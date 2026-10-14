// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN runtime HIF/data path
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mtk-wlan.h"
#include "mtk-wlan-hif.h"

#define MT6628_HIF_TX_HEADER_LEN		16
#define MT6628_HIF_TX_BYTE_COUNT_MASK	GENMASK(11, 0)
#define MT6628_HIF_TX_USER_PRIO_OFFSET	12
#define MT6628_HIF_TX_RESOURCE_OFFSET	2
#define MT6628_HIF_TX_PACKET_TYPE_OFFSET	6
#define MT6628_HIF_TX_PKT_TYPE_DATA	0
#define MT6628_HIF_TX_BURST_END		BIT(5)
#define MT6628_HIF_RX_PKT_TYPE_DATA	0
#define MT6628_HIF_RX_PKT_TYPE_EVENT	1
#define MT6628_HIF_RX_PKT_TYPE_MANAGEMENT	3
#define MT6628_STA_REC_INDEX_NOT_FOUND	0xfe
#define MT6628_RX_MAX_PACKET		(28 + 2312 + 12)
#define MT6628_RUNTIME_RX_LOOPS		32
#define MT6628_RUNTIME_QUEUE_LIMIT	256
#define MT6628_RUNTIME_EVENT_RESERVED	16
#define MT6628_TX_QUEUE_LIMIT		128

/*
 * How long the driver may stay idle before handing the chip to the
 * firmware.  Downstream enters the sleepy state from the idle hook; keep
 * this short enough that an idle interface actually releases the bus.
 */
#define MT6628_PM_IDLE_DELAY_MS		200
#define MT6628_IRQ_RETRY_DELAY_MS	100

struct mt6628_hif_tx_hdr {
	__le16 tx_byte_count_user_priority;
	u8 ether_type_offset;
	u8 resource_pkt_type_csflags;
	u8 wlan_header_length;
	u8 pkt_format_id_flags;
	__le16 llh;
	__le16 seq_no;
	u8 sta_rec_idx;
	u8 forwarding_type_session_id_reserved;
	u8 packet_seq_no;
	u8 ack_bip_basic_rate;
	u8 reserved[2];
} __packed;

static_assert(sizeof(struct mt6628_hif_tx_hdr) == MT6628_HIF_TX_HEADER_LEN);

static const u8 mt6628_tx_default_resources[MT6628_WLAN_TX_TC_NUM] = {
	1, 20, 1, 1, 4, 1,
};

static int mt6628_runtime_read32(struct mt6628_wlan *wl, u32 reg, u32 *val)
{
	u32 value;
	int ret;

	/*
	 * Every register access goes through here, so this is the single
	 * place that has to take Driver Own back when the firmware has been
	 * allowed to power the chip down.  A failed reclaim aborts the
	 * transfer: issuing it anyway would poke a chip the firmware owns.
	 */
	ret = mt6628_wlan_pm_busy(wl);
	if (ret)
		return ret;

	/*
	 * sdio_readl() uses the SDIO function's temporary buffer. Do not
	 * pass a stack object to sdio_memcpy_fromio(): the MTK MSDC host
	 * transfers SDIO data through DMA.
	 */
	value = sdio_readl(wl->func, reg, &ret);
	if (!ret)
		*val = value;

	return ret;
}

static int mt6628_runtime_write32(struct mt6628_wlan *wl, u32 reg, u32 val)
{
	int ret;

	ret = mt6628_wlan_pm_busy(wl);
	if (ret)
		return ret;

	/*
	 * Match sdio_readl() above: sdio_writel() uses the SDIO function's
	 * temporary buffer instead of a stack object for the DMA transfer.
	 */
	sdio_writel(wl->func, val, reg, &ret);
	return ret;
}

/*
 * WHLPCR global interrupt enable/disable is a CMD52 byte write in the
 * downstream driver. Callers of this helper already hold the SDIO host
 * where required by their context, just as they do for runtime_write32().
 */
static int mt6628_runtime_write8(struct mt6628_wlan *wl, u32 reg, u8 val)
{
	int ret;

	ret = mt6628_wlan_pm_busy(wl);
	if (ret)
		return ret;

	sdio_writeb(wl->func, val, reg, &ret);
	return ret;
}

static void mt6628_runtime_free_queues(struct mt6628_wlan *wl)
{
	struct sk_buff *skb;

	skb_queue_purge(&wl->tx_queue);
	skb_queue_purge(&wl->rx_queue);
	skb_queue_purge(&wl->event_queue);
	skb_queue_purge(&wl->async_event_queue);

	/*
	 * mgmt_pending counts the frames in mgmt_queue, and it is only
	 * decremented by mt6628_runtime_mgmt_work().  That work has already
	 * been cancelled by the time the queue is purged here, so every frame
	 * dropped now needs its count given back by hand.  Otherwise the
	 * counter stays above zero, and mt6628_cfg80211_mgmt_rx_done() only
	 * reports a finished scan once it reaches zero, so that scan would
	 * never be completed.
	 */
	while ((skb = skb_dequeue(&wl->mgmt_queue))) {
		kfree_skb(skb);
		if (atomic_dec_and_test(&wl->mgmt_pending))
			mt6628_cfg80211_mgmt_rx_done(wl);
	}
}

static void mt6628_runtime_event_work(struct work_struct *work)
{
	struct mt6628_wlan *wl =
		container_of(work, struct mt6628_wlan, event_work);
	struct mt6628_wifi_event_hdr *event;
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wl->event_queue))) {
		unsigned long flags;
		size_t packet_len, body_len, copy_len;
		bool matched = false;
		int status = 0;

		if (skb->len < MT6628_WIFI_EVENT_HEADER_LEN)
			goto drop;

		event = (struct mt6628_wifi_event_hdr *)skb->data;
		packet_len = le16_to_cpu(event->packet_len);
		if (packet_len < MT6628_WIFI_EVENT_HEADER_LEN ||
						packet_len > skb->len)
			goto drop;

		body_len = packet_len - MT6628_WIFI_EVENT_HEADER_LEN;
		spin_lock_irqsave(&wl->cmd_lock, flags);
		if (wl->cmd_pending && event->eid == wl->cmd_pending_eid &&
						(event->eid == MT6628_EVENT_ID_CH_PRIVILEGE ?
							(body_len >= 2 &&
								skb->data[MT6628_WIFI_EVENT_HEADER_LEN + 1] ==
				wl->cmd_pending_token) :
							event->seq_num == wl->cmd_pending_seq) &&
						(event->eid != MT6628_EVENT_ID_CMD_RESULT ||
							(body_len == 4 &&
								skb->data[MT6628_WIFI_EVENT_HEADER_LEN] ==
				wl->cmd_pending_id))) {
			if (event->eid == MT6628_EVENT_ID_CMD_RESULT) {
				wl->cmd_response_len = 0;
				if (skb->data[MT6628_WIFI_EVENT_HEADER_LEN + 1])
					status = -EIO;
				wl->cmd_status = status;
			} else {
				copy_len = min(body_len, wl->cmd_response_capacity);
				if (copy_len && wl->cmd_response)
					memcpy(wl->cmd_response,
						skb->data + MT6628_WIFI_EVENT_HEADER_LEN,
						copy_len);
				wl->cmd_response_len = copy_len;
				if (copy_len < body_len && !status)
					status = -EMSGSIZE;
				wl->cmd_status = status;
			}
			wl->cmd_pending = false;
			complete(&wl->cmd_done);
			matched = true;
		}
		spin_unlock_irqrestore(&wl->cmd_lock, flags);

		if (matched)
			goto drop;

		if (event->eid == MT6628_EVENT_ID_CMD_RESULT && body_len == 4)
			dev_info_ratelimited(&wl->func->dev,
				"unmatched CMD_RESULT: seq=%u cid=%u status=%u\n",
				event->seq_num,
				skb->data[MT6628_WIFI_EVENT_HEADER_LEN],
				skb->data[MT6628_WIFI_EVENT_HEADER_LEN + 1]);

		if (event->eid == MT6628_EVENT_ID_TX_DONE) {
			const struct mt6628_event_tx_done *tx_done;

			/*
			 * The reference firmware defines a 16-byte TX_DONE body,
			 * but the firmware on this board sends a compact 4-byte
			 * body. The fields we use are present in both formats.
			 */
			if (body_len != 4 && body_len != sizeof(*tx_done)) {
				dev_warn_ratelimited(&wl->func->dev,
					"unexpected TX_DONE body length: %zu\n",
					body_len);
				goto drop;
			}

			tx_done = (const struct mt6628_event_tx_done *)
				(skb->data + MT6628_WIFI_EVENT_HEADER_LEN);
			if (body_len == 4)
				dev_info_ratelimited(&wl->func->dev,
					"compact TX_DONE: packet_seq=%u status=%u\n",
					tx_done->packet_seq, tx_done->status);

			spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
			if (wl->mgmt_tx_pending &&
			    tx_done->packet_seq == wl->mgmt_tx_packet_seq) {
				/*
				 * Any non-success result means the frame was
				 * not acknowledged, which is what the caller
				 * and cfg80211 need to know.
				 */
				wl->mgmt_tx_status =
					tx_done->status ==
					MT6628_TX_RESULT_SUCCESS ? 0 : -EIO;
				if (tx_done->status != MT6628_TX_RESULT_SUCCESS)
					dev_warn_ratelimited(&wl->func->dev,
							     "management frame TX failed: result %u\n",
							     tx_done->status);
				wl->mgmt_tx_pending = false;
				complete(&wl->mgmt_tx_done);
			}
			spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
			goto drop;
		}

		/*
		 * EVENT_ID_SLEEPY_NOTIFY: the firmware has no more work queued
		 * and the host may take the radio.  Its body is a single byte
		 * saying which side is entering the sleepy state
		 * (nic_cmd_event.h:1176-1179).
		 *
		 * Downstream uses exactly this as the condition for handing the
		 * chip over: with CFG_ENABLE_FULL_PM=1, RECLAIM_POWER_CONTROL_TO_PM
		 * calls nicpmSetFWOwn() while fgWiFiInSleepyState is set
		 * (pwr_mgt.h:122-131), and nicpmSetFWOwn is the WHLPCR
		 * set/read-back/clear sequence mt6628_wlan_give_firmware_own()
		 * already performs (nic_pwr_mgt.c:265-301).  Recording it and
		 * letting the idle path act on it makes our hand-off happen
		 * when the firmware says it can, rather than only when our own
		 * timer happens to expire.
		 *
		 * A byte of 0 means the firmware is the one going to sleep, not
		 * that it is ours to take, so only a non-zero value grants it.
		 */
		if (event->eid == MT6628_EVENT_ID_SLEEPY_NOTIFY) {
			if (body_len < 1)
				goto drop;

			wl->fw_sleepy = skb->data[MT6628_WIFI_EVENT_HEADER_LEN];
			if (wl->fw_sleepy) {
				/*
				 * The firmware has said it is idle, so take
				 * the radio now rather than waiting for the
				 * idle timer to expire.  This refuses while
				 * any queue is non-empty, which is the same
				 * condition downstream's block count
				 * enforces, so it is a cheap no-op in the
				 * common case and never a wait.
				 *
				 * pm_idle is left alone: it belongs to the
				 * armed timer, and clearing it here would
				 * make the timer's own reclaim path run
				 * instead of this one.
				 */
				mt6628_wlan_give_firmware_own(wl);
			}
			goto drop;
		}

		/*
		 * Unsolicited-event handlers may block on cfg_mutex or issue
		 * synchronous firmware commands. Keep them off event_work:
		 * that worker must remain able to consume the CMD_RESULT or
		 * TX_DONE event which releases a waiter.
		 */
		if (wl->event_handler &&
		    skb_queue_len(&wl->async_event_queue) <
				MT6628_RUNTIME_QUEUE_LIMIT) {
			skb_queue_tail(&wl->async_event_queue, skb);
			schedule_work(&wl->event_dispatch_work);
			continue;
		}

		if (wl->event_handler)
			dev_warn_ratelimited(&wl->func->dev,
				"async event queue full, dropping eid=%u\n",
				event->eid);
		goto drop;

drop:
		kfree_skb(skb);
	}
}

static void mt6628_runtime_event_dispatch_work(struct work_struct *work)
{
	struct mt6628_wlan *wl =
		container_of(work, struct mt6628_wlan, event_dispatch_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wl->async_event_queue))) {
		if (!wl->event_handler || !wl->event_handler(wl, skb))
			kfree_skb(skb);
	}
}

static void mt6628_runtime_mgmt_work(struct work_struct *work)
{
	struct mt6628_wlan *wl =
		container_of(work, struct mt6628_wlan, mgmt_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wl->mgmt_queue))) {
		if (wl->mgmt_handler) {
			wl->mgmt_handler(wl, skb);
		} else {
			/* No handler installed: see the event path above. */
			kfree_skb(skb);
		}

		if (atomic_dec_and_test(&wl->mgmt_pending))
			mt6628_cfg80211_mgmt_rx_done(wl);
	}
}

/*
 * When userspace controls the 802.1X port, only EAPOL may pass until
 * userspace marks the associated AP authorized.
 */
static bool mt6628_control_port_allows(struct mt6628_wlan *wl,
				       const struct sk_buff *skb)
{
	return !READ_ONCE(wl->control_port) ||
	       READ_ONCE(wl->port_authorized) ||
	       skb->protocol == htons(ETH_P_PAE);
}

static int mt6628_napi_poll(struct napi_struct *napi, int budget)
{
	struct mt6628_wlan *wl = container_of(napi, struct mt6628_wlan, napi);
	struct sk_buff *skb;
	int work_done = 0;

	while (work_done < budget && (skb = skb_dequeue(&wl->rx_queue))) {
		if (!netif_carrier_ok(wl->netdev) ||
		    !READ_ONCE(wl->connected)) {
			dev_kfree_skb_any(skb);
			work_done++;
			continue;
		}

		skb->dev = wl->netdev;
		skb->protocol = eth_type_trans(skb, wl->netdev);
		if (unlikely(!mt6628_control_port_allows(wl, skb))) {
			dev_kfree_skb_any(skb);
			work_done++;
			continue;
		}
		skb->ip_summed = CHECKSUM_NONE;
		napi_gro_receive(napi, skb);
		work_done++;
	}

	if (work_done < budget)
		napi_complete_done(napi, work_done);

	return work_done;
}

static void mt6628_runtime_schedule_rx(struct mt6628_wlan *wl)
{
	if (wl->netdev && netif_running(wl->netdev) &&
	    (netif_carrier_ok(wl->netdev) ||
	     READ_ONCE(wl->conn_state) == MT6628_CONN_DISCONNECTED) &&
	    !skb_queue_empty(&wl->rx_queue))
		napi_schedule(&wl->napi);
}

static void mt6628_runtime_recovery_work(struct work_struct *work)
{
	struct mt6628_wlan *wl =
		container_of(work, struct mt6628_wlan, recovery_work);
	struct cfg80211_connect_params retry = {};
	bool notify_disconnect;
	bool retry_saved;
	int ret;

	if (!wl->runtime_started || !wl->driver_owned) {
		atomic_set(&wl->recovery_pending, 0);
		return;
	}

	mutex_lock(&wl->cfg_mutex);
	notify_disconnect = wl->connected;
	wl->fw_running = false;
	wl->connected = false;
	wl->conn_state = MT6628_CONN_DISCONNECTED;
	wl->conn_secure = false;
	wl->conn_aid = 0;
	/*
	 * The firmware is about to be reset, so every key it held disappears
	 * with it.  Drop the bookkeeping instead of leaving stale masks that
	 * a later connection would try to remove from a fresh firmware.
	 */
	wl->pairwise_key_mask = 0;
	wl->group_key_mask = 0;
	if (wl->netdev)
		netif_carrier_off(wl->netdev);
	mutex_unlock(&wl->cfg_mutex);

	if (notify_disconnect)
		cfg80211_disconnected(wl->netdev, WLAN_REASON_UNSPECIFIED,
				      NULL, 0, true, GFP_KERNEL);

	/*
	 * fw_running is cleared before runtime_stop() so the connection
	 * cleanup path does not try to send commands to the asserted
	 * firmware.
	 */
	/*
	 * Tearing the runtime down also tears down the cfg80211 state, which
	 * drops the saved connect parameters.  Keep a copy so the association
	 * can be restored once the firmware is back; runtime_start() rebuilds
	 * everything else from scratch.
	 */
	retry_saved = mt6628_conn_take_retry(wl, &retry);

	mt6628_wlan_runtime_stop(wl);

	ret = mt6628_wlan_force_firmware_reset(wl);
	if (!ret)
		ret = mt6628_wlan_take_driver_own(wl);
	if (!ret)
		ret = mt6628_wlan_reload_firmware(wl);
	if (!ret)
		ret = mt6628_wlan_runtime_start(wl);

	if (retry_saved && !ret)
		mt6628_conn_restore_retry(wl, &retry);

	if (ret) {
		/* Recovery failed: nothing will retry, so release the copy. */
		if (retry_saved) {
			kfree(retry.ssid);
			kfree(retry.bssid);
			kfree(retry.ie);
			kfree(retry.key);
		}

		dev_err(&wl->func->dev,
			"MT6628 firmware recovery failed: %d\n", ret);
	} else {
		dev_info(&wl->func->dev, "MT6628 firmware recovery complete\n");

		/*
		 * The firmware came back without any association, so try to
		 * restore the one we had.  This is best effort: userspace is
		 * free to reconnect on its own.
		 */
		mt6628_conn_schedule_retry(wl);
	}

	atomic_set(&wl->recovery_pending, 0);
}

static int mt6628_runtime_read_rx_packet(struct mt6628_wlan *wl,
							 unsigned int port, u16 packet_len)
{
	struct mt6628_hif_rx_hdr *rx_hdr;
	struct sk_buff *skb;
	size_t read_len, xfer_len;
u8 *buf;
u16 packet_type;
unsigned int payload_len;
unsigned int event_len;
unsigned int header_offset;
int ret;

	if (packet_len < 8 || packet_len > MT6628_RX_MAX_PACKET)
		return -EMSGSIZE;

	/* MT6628 downstream reads ALIGN_4(packet_len) + one HW DWORD. */
	read_len = ALIGN(packet_len, 4) + 4;
	xfer_len = mt6628_sdio_xfer_len(read_len);
	buf = kmalloc(xfer_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Bulk transfer: reclaim Driver Own, the 32-bit helpers are bypassed. */
	ret = mt6628_wlan_pm_busy(wl);
	if (ret)
		goto out_free;

	ret = sdio_readsb(wl->func, buf,
			  port ? MT6628_MCR_WRDR1 : MT6628_MCR_WRDR0,
			  xfer_len);
	if (ret)
		goto out_free;

	rx_hdr = (struct mt6628_hif_rx_hdr *)buf;
	if (le16_to_cpu(rx_hdr->packet_len) != packet_len) {
		ret = -EPROTO;
		goto out_free;
	}

	packet_type = le16_to_cpu(rx_hdr->packet_type) & GENMASK(1, 0);
	if (packet_type == MT6628_HIF_RX_PKT_TYPE_EVENT &&
	    packet_len >= sizeof(struct mt6628_wifi_event_hdr)) {
		const struct mt6628_wifi_event_hdr *event =
			(const struct mt6628_wifi_event_hdr *)buf;

		dev_info_ratelimited(&wl->func->dev,
			"RX event packet: port=%u len=%u eid=%u seq=%u\n",
			port, packet_len, event->eid, event->seq_num);
	}
	header_offset = rx_hdr->header_len_offset & GENMASK(1, 0);
	switch (packet_type) {
	case MT6628_HIF_RX_PKT_TYPE_DATA:
		if (packet_len < MT6628_HIF_RX_HEADER_LEN + header_offset)
			goto bad_packet;

		payload_len = packet_len - MT6628_HIF_RX_HEADER_LEN -
			header_offset;
		if (payload_len < ETH_HLEN)
			goto bad_packet;
		if (!netif_running(wl->netdev)) {
			ret = 0;
			break;
		}
		if (payload_len >= ETH_HLEN) {
			const struct ethhdr *eth = (const struct ethhdr *)
				(buf + MT6628_HIF_RX_HEADER_LEN + header_offset);

			if (eth->h_proto == cpu_to_be16(ETH_P_PAE))
				dev_info_ratelimited(&wl->func->dev,
					"RX EAPOL data: len=%u src=%pM dst=%pM "
					"sta_rec=%u conn_state=%u\n",
					payload_len, eth->h_source, eth->h_dest,
					rx_hdr->sta_rec_idx,
					READ_ONCE(wl->conn_state));
		}
		if (skb_queue_len(&wl->rx_queue) >= MT6628_RUNTIME_QUEUE_LIMIT)
			goto drop_packet;

		skb = netdev_alloc_skb_ip_align(wl->netdev, payload_len);
		if (!skb) {
			ret = -ENOMEM;
			goto out_free;
		}
		memcpy(skb_put(skb, payload_len),
		       buf + MT6628_HIF_RX_HEADER_LEN + header_offset,
		       payload_len);
		skb_queue_tail(&wl->rx_queue, skb);
		ret = 0;
	break;

	case MT6628_HIF_RX_PKT_TYPE_EVENT:
		if (packet_len < MT6628_WIFI_EVENT_HEADER_LEN)
			goto bad_packet;

		/*
		 * WIFI_EVENT_T overlays the first eight bytes of HIF_RX_HEADER_T.
		 * The remaining four HIF bytes are part of the event payload.
		 */
		event_len = packet_len;
		{
			unsigned int limit = MT6628_RUNTIME_QUEUE_LIMIT;

			/* Preserve slots for events which release synchronous waiters. */
			switch (buf[4]) {
			case MT6628_EVENT_ID_CMD_RESULT:
			case MT6628_EVENT_ID_SCAN_DONE:
			case MT6628_EVENT_ID_TX_DONE:
			case MT6628_EVENT_ID_CH_PRIVILEGE:
			case MT6628_EVENT_ID_BSS_BEACON_TIMEOUT:
			case MT6628_EVENT_ID_SEND_DEAUTH:
			case MT6628_EVENT_ID_STA_STATISTICS:
				break;
			default:
				limit -= MT6628_RUNTIME_EVENT_RESERVED;
				break;
			}
			if (skb_queue_len(&wl->event_queue) >= limit)
				goto drop_packet;
		}
		if (packet_len >= MT6628_WIFI_EVENT_HEADER_LEN &&
		    (buf[4] == MT6628_EVENT_ID_CMD_RESULT ||
		     buf[4] == MT6628_EVENT_ID_SCAN_DONE))
			dev_info_ratelimited(&wl->func->dev,
				"firmware event RX: eid=%#x seq=%u len=%u\n",
				buf[4], buf[5], packet_len);

		skb = alloc_skb(event_len, GFP_KERNEL);
		if (!skb) {
			ret = -ENOMEM;
			goto out_free;
		}
		memcpy(skb_put(skb, event_len),
		       buf, event_len);
		skb_queue_tail(&wl->event_queue, skb);
		wake_up_all(&wl->event_wait);
		ret = 0;
		break;

	case MT6628_HIF_RX_PKT_TYPE_MANAGEMENT:
		if (packet_len < MT6628_HIF_RX_HEADER_LEN)
			goto bad_packet;
		if (skb_queue_len(&wl->mgmt_queue) >=
		    MT6628_RUNTIME_QUEUE_LIMIT)
			goto drop_packet;

		/* Keep the HIF header: RCPI/channel are needed by cfg80211. */
		skb = alloc_skb(packet_len, GFP_KERNEL);
		if (!skb) {
			ret = -ENOMEM;
			goto out_free;
		}
		memcpy(skb_put(skb, packet_len), buf, packet_len);
		atomic_inc(&wl->mgmt_pending);
		skb_queue_tail(&wl->mgmt_queue, skb);
		ret = 0;
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	goto out_free;

bad_packet:
	ret = -EPROTO;
	goto out_free;
drop_packet:
	ret = -ENOBUFS;
out_free:
	if (ret == -ENOBUFS)
		dev_warn_ratelimited(&wl->func->dev,
				     "MT6628 RX queue full, dropping packet\n");
	kfree(buf);
	return ret;
}

static void mt6628_runtime_tx_release(struct mt6628_wlan *wl,
					      u32 wtsr0, u32 wtsr1)
{
	unsigned long flags;
	unsigned int i;
	unsigned int cmd_credits;
	u8 released;
	bool wake = false;

	spin_lock_irqsave(&wl->tx_lock, flags);
	for (i = 0; i < MT6628_WLAN_TX_TC_NUM; i++) {
		if (i < 4)
			released = (wtsr0 >> (i * 8)) & 0xff;
		else
			released = (wtsr1 >> ((i - 4) * 8)) & 0xff;

		if (released) {
			wl->tx_free[i] = min_t(unsigned int,
					      wl->tx_free[i] + released,
					      wl->tx_max[i]);
			wake = true;
		}
	}
	cmd_credits = wl->tx_free[MT6628_TX_TC_CMD];
	spin_unlock_irqrestore(&wl->tx_lock, flags);

	if (wake)
		dev_info_ratelimited(&wl->func->dev,
			"TX credits released: WTSR0=%#010x WTSR1=%#010x "
			"CMD TC free=%u/%u\n",
			wtsr0, wtsr1, cmd_credits,
			wl->tx_max[MT6628_TX_TC_CMD]);
	if (wake)
		wake_up_all(&wl->tx_wait);
	if (wake) {
		if (wl->netdev)
			netif_wake_queue(wl->netdev);

		if (!skb_queue_empty(&wl->tx_queue))
			mod_delayed_work(system_wq, &wl->tx_work, 0);
	}
}

/*
 * Recover TX-release counters if a resource wait times out without an SDIO
 * IRQ. WTSR is a read-and-consume release counter, so account the values
 * here instead of merely logging them.
 */
int mt6628_wlan_poll_tx_status(struct mt6628_wlan *wl,
			       u32 *wtsr0_out, u32 *wtsr1_out)
{
	u32 wtsr0, wtsr1;
	u32 whlpcr = 0, whcr = 0, whier = 0, wrplr = 0;
	int diag_ret = 0;
	bool rx_pending;
	int ret;

	ret = mt6628_wlan_pm_busy(wl);
	if (ret)
		return ret;

	sdio_claim_host(wl->func);
	wtsr0 = sdio_readl(wl->func, MT6628_MCR_WTSR0, &ret);
	if (!ret)
		wtsr1 = sdio_readl(wl->func, MT6628_MCR_WTSR1, &ret);
	else
		wtsr1 = 0;
	if (!ret) {
		whlpcr = sdio_readl(wl->func, MT6628_MCR_WHLPCR, &diag_ret);
		if (!diag_ret)
			whcr = sdio_readl(wl->func, MT6628_MCR_WHCR, &diag_ret);
		if (!diag_ret)
			whier = sdio_readl(wl->func, MT6628_MCR_WHIER, &diag_ret);
		if (!diag_ret)
			wrplr = sdio_readl(wl->func, MT6628_MCR_WRPLR, &diag_ret);
	}
	sdio_release_host(wl->func);
	if (ret)
		return ret;
	rx_pending = !diag_ret && wrplr;
	if (!diag_ret)
		dev_warn_ratelimited(&wl->func->dev,
			"TX poll registers: WHLPCR=%#010x WHCR=%#010x "
			"WHIER=%#010x WRPLR=%#010x\n",
			whlpcr, whcr, whier, wrplr);
	else
		dev_warn_ratelimited(&wl->func->dev,
			"failed to read TX poll diagnostic registers: %d\n",
			diag_ret);

	*wtsr0_out = wtsr0;
	*wtsr1_out = wtsr1;
	mt6628_runtime_tx_release(wl, wtsr0, wtsr1);
	if (rx_pending && READ_ONCE(wl->runtime_started)) {
		dev_warn_ratelimited(&wl->func->dev,
			"RX data pending during TX poll; scheduling IRQ worker: WRPLR=%#010x\n",
			wrplr);
		schedule_work(&wl->irq_work);
	}
	return 0;
}

 /*
  * Synchronize firmware status before and after the polled BASIC_CONFIG
  * command, following the downstream startup order:
 *
 *   read WHISR/WTSR -> reset the normal TX resource counters -> query MAC
 *
 * Any RX packet already present is consumed through the ordinary RX parser.
 * In particular, do not discard RX0 just because BASIC_CONFIG is expected
 * on RX1: its packet identity cannot be inferred from WRPLR alone.
 *
 * reset_resources=true is used before BASIC_CONFIG, when INIT/download
 * credits must not leak into the normal TX resource counters. With it
 * false, release counts include completion of the BASIC_CONFIG command and
 * must be added back to the normal counters before interrupts are enabled.
 * The caller holds the SDIO host for this whole operation.
 */
static int mt6628_runtime_sync_startup(struct mt6628_wlan *wl,
				      bool reset_resources)
{
	unsigned long flags;
	u32 whisr, wtsr0, wtsr1, rx_len;
	unsigned int loops, i;
	int ret;

	ret = mt6628_runtime_read32(wl, MT6628_MCR_WHISR, &whisr);
	if (ret)
		return ret;

	ret = mt6628_runtime_read32(wl, MT6628_MCR_WTSR0, &wtsr0);
	if (ret)
		return ret;

	ret = mt6628_runtime_read32(wl, MT6628_MCR_WTSR1, &wtsr1);
	if (ret)
		return ret;

	if (!reset_resources &&
	    ((whisr & MT6628_WHISR_TX_DONE) || wtsr0 || wtsr1))
		mt6628_runtime_tx_release(wl, wtsr0, wtsr1);

	for (loops = 0; loops < MT6628_RUNTIME_RX_LOOPS; loops++) {
		u16 rx0_len, rx1_len;

		ret = mt6628_runtime_read32(wl, MT6628_MCR_WRPLR, &rx_len);
		if (ret)
			return ret;

		rx0_len = rx_len & 0xffff;
		rx1_len = rx_len >> 16;
		if (!rx0_len && !rx1_len)
			break;

		if (rx0_len) {
			ret = mt6628_runtime_read_rx_packet(wl, 0, rx0_len);
			if (ret && ret != -ENOBUFS && ret != -EPROTO &&
			    ret != -EOPNOTSUPP) {
				dev_err(&wl->func->dev,
					"startup RX0 read failed: %d\n", ret);
				return ret;
			}
			if (ret)
				dev_warn_ratelimited(&wl->func->dev,
					"startup RX0 packet rejected: %d\n",
					ret);
		}

		if (rx1_len) {
			ret = mt6628_runtime_read_rx_packet(wl, 1, rx1_len);
			if (ret && ret != -ENOBUFS && ret != -EPROTO &&
			    ret != -EOPNOTSUPP) {
				dev_err(&wl->func->dev,
					"startup RX1 read failed: %d\n", ret);
				return ret;
			}
			if (ret)
				dev_warn_ratelimited(&wl->func->dev,
					"startup RX1 packet rejected: %d\n",
					ret);
		}
	}

	if (reset_resources) {
		spin_lock_irqsave(&wl->tx_lock, flags);
		for (i = 0; i < MT6628_WLAN_TX_TC_NUM; i++)
			wl->tx_free[i] = wl->tx_max[i];
		spin_unlock_irqrestore(&wl->tx_lock, flags);
	}

	dev_dbg(&wl->func->dev,
		"startup status synchronized: WHISR=%#010x reset_resources=%u\n",
		whisr, reset_resources);
	return 0;
}

static unsigned int mt6628_runtime_tc_from_skb(struct sk_buff *skb)
{
	/* EAPOL must use TC4/HIF TX1 even though its destination is multicast. */
	if (skb->protocol == htons(ETH_P_PAE))
		return MT6628_TX_TC_CMD;

	if (is_multicast_ether_addr(eth_hdr(skb)->h_dest))
		return 5;

	switch (skb->priority & 0x7) {
	case 1:
	case 2:
		return 0;
	case 0:
	case 3:
		return 1;
	case 4:
	case 5:
		return 2;
	case 6:
	case 7:
		return 3;
	default:
		return 1;
	}
}

static int mt6628_runtime_tx_frame(struct mt6628_wlan *wl,
					   struct sk_buff *skb)
{
	struct mt6628_hif_tx_hdr hdr = {};
	unsigned long flags;
	unsigned int tc;
	unsigned int port;
	size_t packet_len, xfer_len;
	u8 *buf;
	int ret;

	if (!wl->connected) {
		dev_kfree_skb_any(skb);
		return 0;
	}

	/*
	 * Check again in the worker: the authorization state may have
	 * changed after this packet was queued by ndo_start_xmit().
	 */
	if (unlikely(!mt6628_control_port_allows(wl, skb))) {
		dev_kfree_skb_any(skb);
		return 0;
	}

	tc = mt6628_runtime_tc_from_skb(skb);
	port = tc == MT6628_TX_TC_CMD ?
		MT6628_MCR_WTDR1 : MT6628_MCR_WTDR0;

	spin_lock_irqsave(&wl->tx_lock, flags);
	if (!wl->tx_free[tc]) {
		netif_stop_queue(wl->netdev);
		spin_unlock_irqrestore(&wl->tx_lock, flags);
		return -EAGAIN;
	}
	wl->tx_free[tc]--;
	spin_unlock_irqrestore(&wl->tx_lock, flags);

	packet_len = MT6628_HIF_TX_HEADER_LEN + skb->len;
	if (packet_len > MT6628_HIF_TX_BYTE_COUNT_MASK) {
		ret = -EMSGSIZE;
		goto err_resource;
	}

	xfer_len = mt6628_sdio_xfer_len(ALIGN(packet_len, 4));
	buf = kzalloc(xfer_len, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_resource;
	}

	hdr.tx_byte_count_user_priority = cpu_to_le16(
		packet_len | ((skb->priority & 0x7) << MT6628_HIF_TX_USER_PRIO_OFFSET));
	/* The firmware field is the EtherType offset in 16-bit words. */
	hdr.ether_type_offset = 14;
	hdr.resource_pkt_type_csflags =
		(tc << MT6628_HIF_TX_RESOURCE_OFFSET) |
		(MT6628_HIF_TX_PKT_TYPE_DATA << MT6628_HIF_TX_PACKET_TYPE_OFFSET);
	/*
	 * The netdev path passes one Ethernet frame at a time, so every
	 * submitted frame terminates its HIF burst.
	 */
	hdr.forwarding_type_session_id_reserved = MT6628_HIF_TX_BURST_END;
	hdr.wlan_header_length = ETH_HLEN;
	hdr.sta_rec_idx = wl->sta_rec_idx;
	hdr.seq_no = 0;
	if (skb->protocol == htons(ETH_P_PAE))
		hdr.pkt_format_id_flags |= MT6628_HIF_TX_1X_FRAME;

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), skb->data, skb->len);

	/* Bulk transfer: reclaim Driver Own, the 32-bit helpers are bypassed. */
	mutex_lock(&wl->cmd_mutex);
	ret = mt6628_wlan_pm_busy(wl);
	if (ret) {
		mutex_unlock(&wl->cmd_mutex);
		kfree(buf);
		goto err_resource;
	}

	sdio_claim_host(wl->func);
	ret = sdio_writesb(wl->func, port, buf, xfer_len);
	sdio_release_host(wl->func);
	mutex_unlock(&wl->cmd_mutex);

	kfree(buf);
	if (ret)
		goto err_resource;

	dev_consume_skb_any(skb);
	return 0;

err_resource:
	spin_lock_irqsave(&wl->tx_lock, flags);
	wl->tx_free[tc] = min_t(unsigned int, wl->tx_free[tc] + 1,
				wl->tx_max[tc]);
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	wake_up_all(&wl->tx_wait);
	return ret;
}

static void mt6628_runtime_tx_work(struct work_struct *work)
{
	struct mt6628_wlan *wl =
		container_of(to_delayed_work(work), struct mt6628_wlan, tx_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wl->tx_queue))) {
		int ret;

		ret = mt6628_runtime_tx_frame(wl, skb);
		if (ret == -EAGAIN) {
			skb_queue_head(&wl->tx_queue, skb);
			mod_delayed_work(system_wq, &wl->tx_work,
					 msecs_to_jiffies(10));
			return;
		}
		if (ret)
			dev_kfree_skb_any(skb);
	}

	if (wl->netdev && skb_queue_len(&wl->tx_queue) < MT6628_TX_QUEUE_LIMIT)
		netif_wake_queue(wl->netdev);
}

static inline struct mt6628_wlan *mt6628_wlan_from_ndev(struct net_device *ndev)
{
	return *(struct mt6628_wlan **)netdev_priv(ndev);
}

static netdev_tx_t mt6628_ndo_start_xmit(struct sk_buff *skb,
						 struct net_device *ndev)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_ndev(ndev);

	if (unlikely(!wl->runtime_started || !wl->fw_running || !wl->connected)) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	if (unlikely(!mt6628_control_port_allows(wl, skb))) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	if (skb_queue_len(&wl->tx_queue) >= MT6628_TX_QUEUE_LIMIT) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	skb_queue_tail(&wl->tx_queue, skb);
	if (skb_queue_len(&wl->tx_queue) >= MT6628_TX_QUEUE_LIMIT)
		netif_stop_queue(ndev);
	mod_delayed_work(system_wq, &wl->tx_work, 0);
	return NETDEV_TX_OK;
}

static int mt6628_ndo_open(struct net_device *ndev)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_ndev(ndev);

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;

	if (READ_ONCE(wl->connected))
		netif_carrier_on(ndev);
	else
		netif_carrier_off(ndev);
	napi_enable(&wl->napi);
	netif_start_queue(ndev);
	return 0;
}

static int mt6628_ndo_stop(struct net_device *ndev)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_ndev(ndev);

	/*
	 * cfg80211 warns if the interface goes DOWN while its scan request
	 * has not been reported complete. Retire any scan before NETDEV_DOWN
	 * is sent by the networking core.
	 */
	mt6628_cfg80211_abort_scan(wl);
	netif_stop_queue(ndev);
	napi_disable(&wl->napi);
	cancel_delayed_work_sync(&wl->tx_work);
	skb_queue_purge(&wl->tx_queue);
	skb_queue_purge(&wl->rx_queue);
	return 0;
}

static const struct net_device_ops mt6628_netdev_ops = {
	.ndo_open = mt6628_ndo_open,
	.ndo_stop = mt6628_ndo_stop,
	.ndo_start_xmit = mt6628_ndo_start_xmit,
};

static void mt6628_runtime_irq_work(struct work_struct *work)
{
	struct mt6628_wlan *wl = container_of(work, struct mt6628_wlan, irq_work);
	unsigned int loops;
	int ret;

	if (!READ_ONCE(wl->runtime_started))
		return;

	/*
	 * The firmware may have been holding the chip in its low power
	 * state, so take Driver Own back before reading anything.  This has
	 * to happen in process context: the transition polls and can sleep.
	 */
	ret = mt6628_wlan_pm_resume(wl);
	if (ret) {
		dev_warn_ratelimited(&wl->func->dev,
			"deferring SDIO IRQ processing until Driver Own is restored: %d\n",
			ret);
		if (READ_ONCE(wl->runtime_started))
			mod_delayed_work(system_wq, &wl->irq_retry_work,
				msecs_to_jiffies(MT6628_IRQ_RETRY_DELAY_MS));
		return;
	}

	if (!READ_ONCE(wl->runtime_started))
		return;

	sdio_claim_host(wl->func);
	for (loops = 0; loops < MT6628_RUNTIME_RX_LOOPS; loops++) {
		u32 whisr, wtsr0, wtsr1, rx_len;
		u16 rx0_len, rx1_len;
		int tx_status_ret;

		ret = mt6628_runtime_read32(wl, MT6628_MCR_WHISR, &whisr);
		if (ret) {
			dev_warn_ratelimited(&wl->func->dev,
				"failed to read WHISR in IRQ worker: %d\n", ret);
			break;
		}

		/*
		 * The MT6628 downstream also treats non-zero WTSR values as a
		 * TX-done indication.  In enhanced SDIO mode the status may be
		 * observed before WHISR_TX_DONE is latched.
		 */
		tx_status_ret = mt6628_runtime_read32(wl, MT6628_MCR_WTSR0,
						     &wtsr0);
		if (!tx_status_ret)
			tx_status_ret = mt6628_runtime_read32(wl,
						MT6628_MCR_WTSR1, &wtsr1);
		if (tx_status_ret) {
			wtsr0 = 0;
			wtsr1 = 0;
			dev_warn_ratelimited(&wl->func->dev,
				"failed to read TX status registers: %d\n",
				tx_status_ret);
		}
		dev_info_ratelimited(&wl->func->dev,
			"runtime SDIO IRQ status: WHISR=%#010x "
			"WTSR0=%#010x WTSR1=%#010x\n",
			whisr, wtsr0, wtsr1);
		if (!tx_status_ret &&
		    ((whisr & MT6628_WHISR_TX_DONE) || wtsr0 || wtsr1))
			mt6628_runtime_tx_release(wl, wtsr0, wtsr1);

		if (whisr & MT6628_WHISR_ABNORMAL) {
			u32 wasr;

			if (!mt6628_runtime_read32(wl, MT6628_MCR_WASR, &wasr))
				dev_warn_ratelimited(&wl->func->dev,
						     "abnormal WLAN status %#x\n", wasr);
		}

		if (whisr & MT6628_WHISR_D2H_SW_ASSERT_INFO) {
			u32 d2hrm0, d2hrm1, d2hrm2;
			int sw_ret;

			sw_ret = mt6628_runtime_read32(wl, MT6628_MCR_D2HRM0R,
						       &d2hrm0);
			if (!sw_ret)
				sw_ret = mt6628_runtime_read32(wl,
						       MT6628_MCR_D2HRM1R, &d2hrm1);
			if (!sw_ret)
				sw_ret = mt6628_runtime_read32(wl,
						       MT6628_MCR_D2HRM2R, &d2hrm2);

			if (!sw_ret)
				dev_err_ratelimited(&wl->func->dev,
					"firmware assert: D2HRM0=%#x D2HRM1=%#x D2HRM2=%#x\n",
					d2hrm0, d2hrm1, d2hrm2);
			else
				dev_err_ratelimited(&wl->func->dev,
					"firmware assert: failed to read D2H mailbox: %d\n",
					sw_ret);

			if (atomic_cmpxchg(&wl->recovery_pending, 0, 1) == 0)
				schedule_work(&wl->recovery_work);
		}

		ret = mt6628_runtime_read32(wl, MT6628_MCR_WRPLR, &rx_len);
		if (ret)
			break;

		rx0_len = rx_len & 0xffff;
		rx1_len = rx_len >> 16;
		if (!whisr && !rx0_len && !rx1_len)
			break;
		if (!(whisr & (MT6628_WHISR_RX0_DONE | MT6628_WHISR_RX1_DONE)) &&
		    (rx0_len || rx1_len))
			dev_warn_ratelimited(&wl->func->dev,
				"RX length pending without WHISR RX_DONE: "
				"WHISR=%#010x WRPLR=%#010x\n", whisr, rx_len);

		if (rx0_len) {
			ret = mt6628_runtime_read_rx_packet(wl, 0, rx0_len);
			if (ret && ret != -ENOBUFS)
				dev_warn_ratelimited(&wl->func->dev,
						     "RX0 packet failed: %d\n",
						     ret);
		}
		if (rx1_len) {
			ret = mt6628_runtime_read_rx_packet(wl, 1, rx1_len);
			if (ret && ret != -ENOBUFS)
				dev_warn_ratelimited(&wl->func->dev,
						     "RX1 packet failed: %d\n",
						     ret);
		}
	}

	if (wl->runtime_started) {
		ret = mt6628_runtime_write8(wl, MT6628_MCR_WHLPCR,
					    MT6628_INT_EN_SET);
		if (ret)
			dev_err_ratelimited(&wl->func->dev,
				"failed to re-enable SDIO interrupts: %d\n", ret);
	}
	sdio_release_host(wl->func);

	if (!skb_queue_empty(&wl->event_queue))
		schedule_work(&wl->event_work);
	if (!skb_queue_empty(&wl->mgmt_queue))
		schedule_work(&wl->mgmt_work);

	mt6628_runtime_schedule_rx(wl);

	/*
	 * Everything that was pending has been handed to the workers, so
	 * the driver can go idle again once they have drained.
	 */
	mt6628_wlan_pm_idle(wl);
}

/* SDIO IRQ callbacks run with the SDIO host already claimed. */
static void mt6628_runtime_irq(struct sdio_func *func)
{
	struct mt6628_wlan *wl = sdio_get_drvdata(func);
	int ret;

	if (!wl || !wl->runtime_started)
		return;

	dev_info_ratelimited(&wl->func->dev,
		"SDIO IRQ callback: pm_idle=%u driver_owned=%u\n",
		(unsigned int)wl->pm_idle, (unsigned int)wl->driver_owned);

	/*
	 * This runs in hard IRQ context, so it must not sleep.  Reclaiming
	 * Driver Own polls for the LP engine and can take seconds, so that
	 * is deferred to the work handler.  Mask the interrupt first so the
	 * ownership transition and the register reads that follow in the
	 * work handler are not re-entered.
	 *
	 * If a reclaim is still owed the firmware owns the chip, so this
	 * driver must not write to it at all -- and it could not reclaim
	 * ownership here even if it wanted to, since that call sleeps.  The
	 * masking write is therefore skipped rather than forced through.
	 * irq_work re-arms the interrupt as the last thing it does after the
	 * reclaim succeeds, so the chip still ends up masked by exactly the
	 * same code as in every other path.
	 */
	ret = mt6628_wlan_pm_busy_irq(wl);
	if (!ret) {
		ret = mt6628_runtime_write8(wl, MT6628_MCR_WHLPCR,
					    MT6628_INT_EN_CLR);
		if (ret)
			dev_warn_ratelimited(&wl->func->dev,
				"failed to mask SDIO interrupts in callback: %d\n",
				ret);
	} else {
		dev_warn_ratelimited(&wl->func->dev,
			"failed to acquire driver ownership in SDIO IRQ: %d\n",
			ret);
	}
	schedule_work(&wl->irq_work);
}

static void mt6628_runtime_pm_work(struct work_struct *work);

static void mt6628_runtime_irq_retry_work(struct work_struct *work);

int mt6628_wlan_runtime_start(struct mt6628_wlan *wl)
{
	struct net_device *ndev = NULL;
	unsigned long flags;
	unsigned int i;
	int ret;

	INIT_WORK(&wl->irq_work, mt6628_runtime_irq_work);
	INIT_DELAYED_WORK(&wl->irq_retry_work,
			  mt6628_runtime_irq_retry_work);
	if (!wl->recovery_work_initialized) {
		INIT_WORK(&wl->recovery_work, mt6628_runtime_recovery_work);
		wl->recovery_work_initialized = true;
	}
	INIT_DELAYED_WORK(&wl->tx_work, mt6628_runtime_tx_work);
	INIT_WORK(&wl->event_work, mt6628_runtime_event_work);
	INIT_WORK(&wl->event_dispatch_work,
		  mt6628_runtime_event_dispatch_work);
	INIT_WORK(&wl->mgmt_work, mt6628_runtime_mgmt_work);
	INIT_DELAYED_WORK(&wl->pm_work, mt6628_runtime_pm_work);
	spin_lock_init(&wl->tx_lock);
	init_waitqueue_head(&wl->tx_wait);
	skb_queue_head_init(&wl->tx_queue);
	skb_queue_head_init(&wl->rx_queue);
	skb_queue_head_init(&wl->event_queue);
	skb_queue_head_init(&wl->async_event_queue);
	skb_queue_head_init(&wl->mgmt_queue);
	init_waitqueue_head(&wl->event_wait);
	atomic_set(&wl->mgmt_pending, 0);
	spin_lock_init(&wl->mgmt_tx_lock);
	init_completion(&wl->mgmt_tx_done);
	wl->mgmt_tx_pending = false;
	wl->mgmt_tx_seq = 0;
	wl->mgmt_tx_packet_seq = 0;
	wl->mgmt_tx_cookie = 0;
	wl->mgmt_tx_status = 0;
	/* A restarted firmware no longer owns the previous channel grant. */
	wl->channel_token = 0;
	wl->channel_req_type = MT6628_CH_REQ_TYPE_JOIN;
	wl->channel_grant_ms = 0;
	wl->roc_cookie = 0;
	wl->roc_starting = false;
	wl->roc_finishing = false;
	wl->roc_chans[0] = NULL;
	wl->roc_chans[1] = NULL;
	wl->roc_n_chans = 0;
	wl->roc_duration = 0;
	mutex_init(&wl->cmd_mutex);
	mutex_init(&wl->channel_mutex);
	mutex_init(&wl->scan_mutex);
	spin_lock_init(&wl->cmd_lock);
	mutex_init(&wl->cfg_mutex);
	mutex_init(&wl->conn_cleanup_mutex);
	mutex_init(&wl->key_mutex);
	wl->conn_cleanup_pending = 0;
	init_completion(&wl->cmd_done);
	atomic_set(&wl->recovery_pending, 0);
	wl->runtime_initialized = true;
	wl->sta_rec_idx = MT6628_STA_REC_INDEX_NOT_FOUND;
	wl->event_handler = mt6628_cfg80211_event_handler;
	wl->mgmt_handler = mt6628_cfg80211_mgmt_handler;

	ret = mt6628_cfg80211_init(wl);
	if (ret)
		goto err_runtime;

	spin_lock_irqsave(&wl->tx_lock, flags);
	for (i = 0; i < MT6628_WLAN_TX_TC_NUM; i++) {
		wl->tx_max[i] = mt6628_tx_default_resources[i];
		wl->tx_free[i] = mt6628_tx_default_resources[i];
	}
	spin_unlock_irqrestore(&wl->tx_lock, flags);

	ndev = alloc_etherdev(sizeof(struct mt6628_wlan *));
	if (!ndev) {
		ret = -ENOMEM;
		goto err_runtime;
	}

	SET_NETDEV_DEV(ndev, &wl->func->dev);
	/* alloc_etherdev() defaults to eth%d, but this is a WLAN interface. */
	strscpy(ndev->name, "wlan%d", sizeof(ndev->name));
	ndev->netdev_ops = &mt6628_netdev_ops;
	eth_hw_addr_random(ndev);
	netif_carrier_off(ndev);
	*(struct mt6628_wlan **)netdev_priv(ndev) = wl;
	ndev->ieee80211_ptr = &wl->wdev;
	wl->wdev.netdev = ndev;
	netif_napi_add(ndev, &wl->napi, mt6628_napi_poll);

	ret = register_netdev(ndev);
	if (ret)
		goto err_netdev;
	wl->netdev = ndev;

	/*
	 * Query BASIC_CONFIG by polling RX1 first, matching the downstream
	 * nicRxWaitResponse() path. First consume any packet left pending by
	 * firmware startup, and synchronize the TX status/resource counters
	 * before issuing that command.
	 */
	wl->runtime_started = true;
	sdio_claim_host(wl->func);
	ret = mt6628_runtime_sync_startup(wl, true);
	sdio_release_host(wl->func);
	if (ret) {
		dev_err(&wl->func->dev,
			"failed to synchronize startup status: %d\n", ret);
		wl->runtime_started = false;
		wl->fw_running = false;
		goto err_unregister;
	}

	ret = mt6628_wlan_query_basic_config(wl);
	if (ret) {
		dev_err(&wl->func->dev,
			"failed to read firmware basic configuration: %d\n",
			ret);
		wl->runtime_started = false;
		wl->fw_running = false;
		goto err_unregister;
	}

	sdio_claim_host(wl->func);
	{
		u32 whcr;

		ret = mt6628_runtime_read32(wl, MT6628_MCR_WHCR, &whcr);
		if (!ret)
			ret = mt6628_runtime_write32(wl, MT6628_MCR_WHCR,
						     whcr & ~MT6628_WHCR_W_INT_CLR_CTRL);
	}
	if (!ret)
		ret = mt6628_runtime_sync_startup(wl, false);
	if (!ret) {
		ret = sdio_claim_irq(wl->func, mt6628_runtime_irq);
		if (!ret)
			wl->irq_claimed = true;
	}
	if (!ret)
		ret = mt6628_runtime_write32(wl, MT6628_MCR_WHIER,
					     MT6628_WHIER_RUNTIME);
	if (!ret)
		ret = mt6628_runtime_write8(wl, MT6628_MCR_WHLPCR,
					    MT6628_INT_EN_SET);
	if (ret && wl->irq_claimed) {
		sdio_release_irq(wl->func);
		wl->irq_claimed = false;
	}
	sdio_release_host(wl->func);
	if (ret) {
		wl->runtime_started = false;
		wl->fw_running = false;
		goto err_unregister;
	}

	/* A successful restart reopens the runtime and PM paths. */
	WRITE_ONCE(wl->pm_suspending, false);
	WRITE_ONCE(wl->runtime_stopping, false);

	if (!skb_queue_empty(&wl->event_queue))
		schedule_work(&wl->event_work);
	if (!skb_queue_empty(&wl->async_event_queue))
		schedule_work(&wl->event_dispatch_work);
	if (!skb_queue_empty(&wl->mgmt_queue))
		schedule_work(&wl->mgmt_work);
	mt6628_runtime_schedule_rx(wl);

	return 0;

err_unregister:
	unregister_netdev(ndev);
	wl->netdev = NULL;
err_netdev:
	ndev->ieee80211_ptr = NULL;
	wl->wdev.netdev = NULL;
	netif_napi_del(&wl->napi);
	free_netdev(ndev);
err_runtime:
	mt6628_runtime_free_queues(wl);
	wl->event_handler = NULL;
	wl->mgmt_handler = NULL;
	mt6628_cfg80211_deinit(wl);
	wl->runtime_initialized = false;
	return ret;
}

void mt6628_wlan_runtime_stop(struct mt6628_wlan *wl)
{
	struct net_device *ndev = wl->netdev;
	unsigned long flags;
	int ret;

	if (!wl->runtime_initialized)
		return;

	WRITE_ONCE(wl->runtime_stopping, true);
	WRITE_ONCE(wl->pm_suspending, true);

	/* Drain outstanding handoffs before tearing down firmware state. */
	cancel_delayed_work_sync(&wl->pm_work);

	if (ndev)
		netif_stop_queue(ndev);
	cancel_delayed_work_sync(&wl->tx_work);

	mt6628_cfg80211_abort_scan(wl);

	spin_lock_irqsave(&wl->cmd_lock, flags);
	if (wl->cmd_pending) {
		wl->cmd_status = -ESHUTDOWN;
		wl->cmd_pending = false;
		wl->cmd_response_len = 0;
		complete(&wl->cmd_done);
	}
	spin_unlock_irqrestore(&wl->cmd_lock, flags);

	/*
	 * These workers can update connection state and its IE buffers.
	 * Drain them before connect_deinit() frees that state. Keep event_work
	 * alive until teardown commands have completed so command responses
	 * can still be dispatched.
	 */
	cancel_work_sync(&wl->event_dispatch_work);
	cancel_work_sync(&wl->mgmt_work);

	/*
	 * Keep the runtime command path and netdev alive while tearing
	 * down firmware-side STA/BSS/channel state.  conn_fw_cleanup()
	 * intentionally rejects cleanup after either one is cleared.
	 */
	mt6628_cfg80211_connect_deinit(wl);

	/*
	 * No more firmware commands should be issued by teardown. Reclaim
	 * Driver Own while runtime_started still permits the ownership path.
	 */
	if (READ_ONCE(wl->pm_idle)) {
		ret = mt6628_wlan_pm_resume(wl);
		if (ret)
			dev_warn(&wl->func->dev,
				 "failed to reclaim Driver Own during stop: %d\n",
				 ret);
	}

	spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
	if (wl->mgmt_tx_pending) {
		wl->mgmt_tx_status = -ESHUTDOWN;
		wl->mgmt_tx_pending = false;
		complete(&wl->mgmt_tx_done);
	}
	spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);

	WRITE_ONCE(wl->runtime_started, false);
	wake_up_all(&wl->tx_wait);

	if (wl->irq_claimed) {
		sdio_claim_host(wl->func);
		mt6628_runtime_write32(wl, MT6628_MCR_WHIER, 0);
		mt6628_runtime_write8(wl, MT6628_MCR_WHLPCR,
				      MT6628_INT_EN_CLR);
		sdio_release_irq(wl->func);
		wl->irq_claimed = false;
		sdio_release_host(wl->func);
	}

	cancel_work_sync(&wl->irq_work);
	cancel_delayed_work_sync(&wl->irq_retry_work);
	cancel_delayed_work_sync(&wl->tx_work);
	cancel_work_sync(&wl->event_work);
	cancel_delayed_work_sync(&wl->pm_work);

	if (ndev) {
		unregister_netdev(ndev);
		netif_napi_del(&wl->napi);
		ndev->ieee80211_ptr = NULL;
		wl->wdev.netdev = NULL;
		wl->netdev = NULL;
	}

	mt6628_runtime_free_queues(wl);
	wl->event_handler = NULL;
	wl->mgmt_handler = NULL;
	mt6628_cfg80211_deinit(wl);
	wl->runtime_initialized = false;
}

EXPORT_SYMBOL_GPL(mt6628_wlan_runtime_start);
EXPORT_SYMBOL_GPL(mt6628_wlan_runtime_stop);

/*
 * Runtime power ownership.
 *
 * Driver Own and 802.11 power save are unrelated: the former decides who
 * may touch the chip, the latter is a link level mode negotiated with the
 * AP.  While the driver has data to move it holds Driver Own; once it has
 * been idle for a while it hands the chip to the firmware, which can then
 * enter its low power state.  The firmware raises an interrupt when it
 * has something for us again, and the interrupt path takes Driver Own
 * back before reading anything.
 *
 * This mirrors nicpmSetFWOwn()/nicpmSetDriverOwn() in the downstream
 * driver, including its refusal to release ownership while interrupts are
 * still pending: otherwise the firmware could take the chip away with an
 * unread WHISR behind us.
 */
int mt6628_wlan_give_firmware_own(struct mt6628_wlan *wl)
{
	u32 val;
	unsigned long flags;
	bool mgmt_tx_pending;
	bool tx_pending = false;
	unsigned int i;
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (wl->pm_idle || !wl->driver_owned)
		return 0;

	/*
	 * Do not hand ownership away while a command owns the serialized
	 * command slot. This can run in event_work, so trylock must not wait
	 * for a command whose response this same worker may need to consume.
	 */
	if (!mutex_trylock(&wl->cmd_mutex))
		return -EBUSY;
	if (!wl->runtime_started || !wl->fw_running) {
		ret = -ENODEV;
		goto out_unlock_cmd;
	}
	if (wl->pm_idle || !wl->driver_owned) {
		ret = 0;
		goto out_unlock_cmd;
	}

	/*
	 * Pending traffic means the firmware still wants the host to run.
	 * Keep ownership and let the caller try again later.
	 */
	if (!skb_queue_empty(&wl->rx_queue) ||
	    !skb_queue_empty(&wl->event_queue) ||
	    !skb_queue_empty(&wl->async_event_queue) ||
	    !skb_queue_empty(&wl->mgmt_queue) ||
		!skb_queue_empty(&wl->tx_queue))
		goto out_busy;

	/* TX credits stay consumed until firmware reports completion in WTSR. */
	spin_lock_irqsave(&wl->tx_lock, flags);
	for (i = 0; i < MT6628_WLAN_TX_TC_NUM; i++) {
		if (wl->tx_free[i] < wl->tx_max[i]) {
			tx_pending = true;
			break;
		}
	}
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	if (tx_pending)
		goto out_busy;

	/* A TX_DONE waiter is still using the firmware's completion path. */
	spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
	mgmt_tx_pending = wl->mgmt_tx_pending;
	spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
	if (mgmt_tx_pending)
		goto out_busy;

	sdio_claim_host(wl->func);
	ret = mt6628_runtime_read32(wl, MT6628_MCR_WHISR, &val);
	if (ret)
		goto out_release;

	/* Consume anything already latched before releasing the chip. */
	if (val & (MT6628_WHISR_TX_DONE | MT6628_WHISR_RX0_DONE |
		   MT6628_WHISR_RX1_DONE | MT6628_WHISR_ABNORMAL |
		   MT6628_WHISR_FW_OWN_BACK |
		   MT6628_WHISR_D2H_SW_ASSERT_INFO)) {
		ret = -EBUSY;
		goto out_release;
	}

	ret = mt6628_runtime_write32(wl, MT6628_MCR_WHLPCR,
				     MT6628_FW_OWN_REQ_SET);
	if (ret)
		goto out_release;

	ret = mt6628_runtime_read32(wl, MT6628_MCR_WHLPCR, &val);
	if (ret)
		goto out_release;

	/*
	 * The request bit reads back while the transition is refused or
	 * still in flight.  Roll it back so the driver keeps ownership
	 * and can retry later.
	 */
	if (val & MT6628_FW_OWN_REQ_SET) {
		mt6628_runtime_write32(wl, MT6628_MCR_WHLPCR,
					MT6628_FW_OWN_REQ_CLR);
		ret = -EBUSY;
		goto out_release;
	}

	wl->driver_owned = false;
	wl->pm_idle = true;

out_release:
	sdio_release_host(wl->func);
	mutex_unlock(&wl->cmd_mutex);
	return ret;

out_busy:
	ret = -EBUSY;
out_unlock_cmd:
	mutex_unlock(&wl->cmd_mutex);
	return ret;
}

int mt6628_wlan_pm_resume(struct mt6628_wlan *wl)
{
	int ret;

	/*
	 * The firmware's sleepy notification described the state before we took
	 * the radio, so it is stale the moment ownership comes back.
	 */
	wl->fw_sleepy = false;

	if (!wl->runtime_started || !wl->pm_idle)
		return 0;

	/*
	 * pm_idle means "the firmware owns the chip and the driver still owes
	 * a reclaim".  It is cleared only once the reclaim has actually
	 * succeeded.
	 *
	 * Invariant: pm_idle == false means the driver owns the chip and may
	 * touch it; pm_idle == true means the chip must not be touched at all
	 * until a reclaim has succeeded.  Keeping the flag set across a
	 * failure is what makes a retry possible -- clearing it first would
	 * turn the first failure into a permanent one, because the early
	 * return above keys off the same flag.
	 */
	ret = mt6628_wlan_take_driver_own(wl);
	if (ret) {
		dev_warn_ratelimited(&wl->func->dev,
				     "failed to reclaim driver ownership: %d\n",
				     ret);
		/* pm_idle deliberately untouched: the next access retries. */
		return ret;
	}

	wl->pm_idle = false;
	wl->driver_owned = true;
	return 0;
}

static void mt6628_runtime_irq_retry_work(struct work_struct *work)
{
	struct mt6628_wlan *wl =
		container_of(to_delayed_work(work), struct mt6628_wlan,
			     irq_retry_work);

	if (READ_ONCE(wl->runtime_started))
		schedule_work(&wl->irq_work);
}

static void mt6628_runtime_pm_work(struct work_struct *work)
{
	struct mt6628_wlan *wlan =
		container_of(to_delayed_work(work), struct mt6628_wlan,
			     pm_work);
	int ret;

	if (!wlan->runtime_started)
		return;

	ret = mt6628_wlan_give_firmware_own(wlan);

	/* Traffic still outstanding: back off and try again shortly. */
	if (ret == -EBUSY) {
		schedule_delayed_work(&wlan->pm_work,
				      msecs_to_jiffies(MT6628_PM_IDLE_DELAY_MS));
		return;
	}

	if (ret)
		dev_warn_ratelimited(&wlan->func->dev,
				     "failed to enter idle power state: %d\n",
				     ret);
}

/*
 * True while a Driver Own reclaim is still owed, i.e. while every register
 * and bulk transfer has to be refused.  Exists so the ownership bookkeeping
 * can be kept consistent when a reclaim fails.
 */
static bool mt6628_pm_own_pending(struct mt6628_wlan *wl)
{
	return wl->pm_idle;
}

/*
 * Claim the chip ahead of a register or bulk transfer.
 *
 * Returns 0 when the caller may proceed and -EAGAIN when the reclaim failed:
 * the firmware still owns the chip, so the caller must abort without issuing
 * any SDIO transaction.  A non-zero return must never be ignored.
 *
 * Process context only -- the reclaim polls the LP engine and sleeps.  The
 * hard IRQ path uses mt6628_wlan_pm_busy_irq() instead.
 */
int mt6628_wlan_pm_busy(struct mt6628_wlan *wl)
{
	if (!wl)
		return 0;

	if (!READ_ONCE(wl->runtime_started)) {
		if (READ_ONCE(wl->runtime_stopping) &&
		    READ_ONCE(wl->pm_idle))
			return -EAGAIN;
		return 0;
	}

	/* Any register access starts by taking Driver Own back. */
	return mt6628_wlan_pm_resume(wl);
}

/*
 * Hard IRQ context variant.  Attempting the reclaim here would be fatal,
 * because mt6628_wlan_take_driver_own() calls usleep_range() in its poll
 * loop, so all this can do is report whether a reclaim is still owed.  The
 * IRQ fires exactly because the firmware woke us, so the answer is normally
 * yes; the one caller that cannot tolerate a sleep needs to be able to see
 * that rather than infer it.
 */
int mt6628_wlan_pm_busy_irq(struct mt6628_wlan *wl)
{
	if (!wl || !wl->runtime_started)
		return 0;

	return mt6628_pm_own_pending(wl) ? -EAGAIN : 0;
}

void mt6628_wlan_pm_idle(struct mt6628_wlan *wl)
{
	if (!wl || !wl->runtime_started)
		return;

	/*
	 * The firmware woke us, or a transfer just drained: reclaim ownership
	 * before arming the countdown again.  This is deliberately void and
	 * the reclaim result is dropped.  pm_idle() only schedules the
	 * countdown; if the reclaim fails there is nothing this function can
	 * usefully do about it, and pm_idle is left set so the retry happens.
	 * The next actual access reports the failure through pm_busy() and
	 * aborts, which is where it can be acted on.
	 *
	 * The safest response to a failure here is to leave the handoff armed
	 * anyway: pm_work finds pm_idle already set and returns 0 without
	 * touching the chip, so ownership cannot drift.
	 */
	if (wl->pm_idle)
		mt6628_wlan_pm_resume(wl);

	schedule_delayed_work(&wl->pm_work,
			      msecs_to_jiffies(MT6628_PM_IDLE_DELAY_MS));
}
