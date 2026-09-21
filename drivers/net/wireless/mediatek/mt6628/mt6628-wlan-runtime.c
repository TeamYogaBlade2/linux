// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN runtime HIF/data path
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "mtk-wlan.h"
#include "mtk-wlan-hif.h"

#define MT6628_HIF_TX_HEADER_LEN		16
#define MT6628_HIF_RX_HEADER_LEN		12
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
#define MT6628_TX_QUEUE_LIMIT		128

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

struct mt6628_hif_rx_hdr {
	__le16 packet_len;
	__le16 packet_type;
	u8 header_len_offset;
	u8 reorder_pal_tcl;
	__le16 seq_no_tid;
	u8 sta_rec_idx;
	u8 rcpi;
	u8 hw_channel_num;
	u8 reserved;
} __packed;

static_assert(sizeof(struct mt6628_hif_tx_hdr) == MT6628_HIF_TX_HEADER_LEN);
static_assert(sizeof(struct mt6628_hif_rx_hdr) == MT6628_HIF_RX_HEADER_LEN);

static const u8 mt6628_tx_default_resources[MT6628_WLAN_TX_TC_NUM] = {
	1, 20, 1, 1, 4, 1,
};

static int mt6628_runtime_read32(struct mt6628_wlan *wl, u32 reg, u32 *val)
{
	__le32 tmp;
	int ret;

	ret = sdio_memcpy_fromio(wl->func, &tmp, reg, sizeof(tmp));
	if (!ret)
		*val = le32_to_cpu(tmp);

	return ret;
}

static int mt6628_runtime_write32(struct mt6628_wlan *wl, u32 reg, u32 val)
{
	__le32 tmp = cpu_to_le32(val);

	return sdio_memcpy_toio(wl->func, reg, &tmp, sizeof(tmp));
}

static void mt6628_runtime_free_queues(struct mt6628_wlan *wl)
{
	skb_queue_purge(&wl->tx_queue);
	skb_queue_purge(&wl->rx_queue);
	skb_queue_purge(&wl->event_queue);
	skb_queue_purge(&wl->mgmt_queue);
	skb_queue_purge(&wl->async_event_queue);
	skb_queue_purge(&wl->async_mgmt_queue);
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
		if (wl->cmd_pending && event->seq_num == wl->cmd_pending_seq &&
		    (event->eid != MT6628_EVENT_ID_CMD_RESULT || body_len >= 2) &&
		    (event->eid != MT6628_EVENT_ID_CMD_RESULT ||
		     skb->data[MT6628_WIFI_EVENT_HEADER_LEN] == wl->cmd_pending_id)) {
			if (event->eid == MT6628_EVENT_ID_CMD_RESULT && body_len < 2) {
				wl->cmd_status = -EPROTO;
			} else {
				copy_len = min(body_len, wl->cmd_response_capacity);
				if (copy_len && wl->cmd_response)
					memcpy(wl->cmd_response,
					       skb->data + MT6628_WIFI_EVENT_HEADER_LEN,
					       copy_len);
				wl->cmd_response_len = copy_len;
				if (event->eid == MT6628_EVENT_ID_CMD_RESULT &&
				    skb->data[MT6628_WIFI_EVENT_HEADER_LEN + 1])
					status = -EIO;
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

		if (wl->event_handler) {
			wl->event_handler(wl, skb);
			continue;
		}
		if (skb_queue_len(&wl->async_event_queue) >=
		    256)
			goto drop;
		skb_queue_tail(&wl->async_event_queue, skb);
		continue;

drop:
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
			continue;
		}
		if (skb_queue_len(&wl->async_mgmt_queue) >= 256) {
			kfree_skb(skb);
			continue;
		}
		skb_queue_tail(&wl->async_mgmt_queue, skb);
	}
}

static int mt6628_napi_poll(struct napi_struct *napi, int budget)
{
	struct mt6628_wlan *wl = container_of(napi, struct mt6628_wlan, napi);
	struct sk_buff *skb;
	int work_done = 0;

	while (work_done < budget && (skb = skb_dequeue(&wl->rx_queue))) {
		if (!netif_carrier_ok(wl->netdev)) {
			dev_kfree_skb_any(skb);
			continue;
		}

		skb->dev = wl->netdev;
		skb->protocol = eth_type_trans(skb, wl->netdev);
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
	    netif_carrier_ok(wl->netdev) && !skb_queue_empty(&wl->rx_queue))
		napi_schedule(&wl->napi);
}

static int mt6628_runtime_read_rx_packet(struct mt6628_wlan *wl,
						 unsigned int port, u16 packet_len)
{
	struct mt6628_hif_rx_hdr *rx_hdr;
	struct sk_buff *skb;
	size_t read_len;
	u8 *buf;
	u16 packet_type;
	unsigned int payload_len;
	int ret;

	if (packet_len < 8 || packet_len > MT6628_RX_MAX_PACKET)
		return -EMSGSIZE;

	/* MT6628 downstream reads ALIGN_4(packet_len) + one HW DWORD. */
	read_len = ALIGN(packet_len, 4) + 4;
	buf = kmalloc(read_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = sdio_readsb(wl->func, buf,
			  port ? MT6628_MCR_WRDR1 : MT6628_MCR_WRDR0,
			  read_len);
	if (ret)
		goto out_free;

	rx_hdr = (struct mt6628_hif_rx_hdr *)buf;
	if (le16_to_cpu(rx_hdr->packet_len) != packet_len) {
		ret = -EPROTO;
		goto out_free;
	}

	packet_type = le16_to_cpu(rx_hdr->packet_type) & GENMASK(1, 0);
	switch (packet_type) {
	case MT6628_HIF_RX_PKT_TYPE_DATA:
		if (packet_len < MT6628_HIF_RX_HEADER_LEN)
			goto bad_packet;

		payload_len = packet_len - MT6628_HIF_RX_HEADER_LEN;
		if (skb_queue_len(&wl->rx_queue) >= MT6628_RUNTIME_QUEUE_LIMIT)
			goto drop_packet;

		skb = netdev_alloc_skb_ip_align(wl->netdev, payload_len);
		if (!skb) {
			ret = -ENOMEM;
			goto out_free;
		}
		memcpy(skb_put(skb, payload_len),
		       buf + MT6628_HIF_RX_HEADER_LEN, payload_len);
		skb_queue_tail(&wl->rx_queue, skb);
		ret = 0;
		break;

	case MT6628_HIF_RX_PKT_TYPE_EVENT:
		/* Event packets use the downstream 8-byte WIFI_EVENT_T overlay. */
		if (skb_queue_len(&wl->event_queue) >=
		    MT6628_RUNTIME_QUEUE_LIMIT)
			goto drop_packet;

		skb = alloc_skb(packet_len, GFP_KERNEL);
		if (!skb) {
			ret = -ENOMEM;
			goto out_free;
		}
		memcpy(skb_put(skb, packet_len), buf, packet_len);
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
	spin_unlock_irqrestore(&wl->tx_lock, flags);

	if (wake)
		wake_up_all(&wl->tx_wait);
	if (wake && wl->netdev)
		netif_wake_queue(wl->netdev);
}

static unsigned int mt6628_runtime_tc_from_skb(struct sk_buff *skb)
{
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
	size_t packet_len, xfer_len;
	u8 *buf;
	int ret;

	if (!wl->connected) {
		dev_kfree_skb_any(skb);
		return 0;
	}

	tc = mt6628_runtime_tc_from_skb(skb);
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

	xfer_len = ALIGN(packet_len, 4);
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
	hdr.seq_no = cpu_to_le16(wl->tx_seq++);

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), skb->data, skb->len);

	sdio_claim_host(wl->func);
	ret = sdio_writesb(wl->func, MT6628_MCR_WTDR0, buf, xfer_len);
	sdio_release_host(wl->func);

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
	struct mt6628_wlan *wl = container_of(work, struct mt6628_wlan, tx_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&wl->tx_queue))) {
		int ret;

		ret = mt6628_runtime_tx_frame(wl, skb);
		if (ret == -EAGAIN) {
			skb_queue_head(&wl->tx_queue, skb);
			break;
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

	if (skb_queue_len(&wl->tx_queue) >= MT6628_TX_QUEUE_LIMIT) {
		netif_stop_queue(ndev);
		return NETDEV_TX_BUSY;
	}

	skb_queue_tail(&wl->tx_queue, skb);
	if (skb_queue_len(&wl->tx_queue) >= MT6628_TX_QUEUE_LIMIT)
		netif_stop_queue(ndev);
	schedule_work(&wl->tx_work);
	return NETDEV_TX_OK;
}

static int mt6628_ndo_open(struct net_device *ndev)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_ndev(ndev);

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;

	netif_carrier_off(ndev);
	napi_enable(&wl->napi);
	netif_start_queue(ndev);
	return 0;
}

static int mt6628_ndo_stop(struct net_device *ndev)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_ndev(ndev);

	netif_stop_queue(ndev);
	napi_disable(&wl->napi);
	cancel_work_sync(&wl->tx_work);
	skb_queue_purge(&wl->tx_queue);
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

	sdio_claim_host(wl->func);
	for (loops = 0; loops < MT6628_RUNTIME_RX_LOOPS; loops++) {
		u32 whisr, wtsr0, wtsr1, rx_len;
		u16 rx0_len, rx1_len;

		ret = mt6628_runtime_read32(wl, MT6628_MCR_WHISR, &whisr);
		if (ret || !whisr)
			break;

		if (whisr & MT6628_WHISR_TX_DONE) {
			if (!mt6628_runtime_read32(wl, MT6628_MCR_WTSR0, &wtsr0) &&
			    !mt6628_runtime_read32(wl, MT6628_MCR_WTSR1, &wtsr1))
				mt6628_runtime_tx_release(wl, wtsr0, wtsr1);
		}

		if (whisr & MT6628_WHISR_ABNORMAL) {
			u32 wasr;

			if (!mt6628_runtime_read32(wl, MT6628_MCR_WASR, &wasr))
				dev_warn_ratelimited(&wl->func->dev,
						     "abnormal WLAN status %#x\n", wasr);
		}

		if (!(whisr & (MT6628_WHISR_RX0_DONE | MT6628_WHISR_RX1_DONE)))
			continue;

		ret = mt6628_runtime_read32(wl, MT6628_MCR_WRPLR, &rx_len);
		if (ret)
			break;

		rx0_len = rx_len & 0xffff;
		rx1_len = rx_len >> 16;
		if (rx0_len)
			mt6628_runtime_read_rx_packet(wl, 0, rx0_len);
		if (rx1_len)
			mt6628_runtime_read_rx_packet(wl, 1, rx1_len);
	}

	if (wl->runtime_started)
		mt6628_runtime_write32(wl, MT6628_MCR_WHLPCR, MT6628_INT_EN_SET);
	sdio_release_host(wl->func);

	if (!skb_queue_empty(&wl->event_queue))
		schedule_work(&wl->event_work);
	if (!skb_queue_empty(&wl->mgmt_queue))
		schedule_work(&wl->mgmt_work);

	mt6628_runtime_schedule_rx(wl);
}

/* SDIO IRQ callbacks run with the SDIO host already claimed. */
static void mt6628_runtime_irq(struct sdio_func *func)
{
	struct mt6628_wlan *wl = sdio_get_drvdata(func);

	if (!wl || !wl->runtime_started)
		return;

	mt6628_runtime_write32(wl, MT6628_MCR_WHLPCR, MT6628_INT_EN_CLR);
	schedule_work(&wl->irq_work);
}

int mt6628_wlan_runtime_start(struct mt6628_wlan *wl)
{
	struct net_device *ndev;
	unsigned long flags;
	unsigned int i;
	int ret;

	INIT_WORK(&wl->irq_work, mt6628_runtime_irq_work);
	INIT_WORK(&wl->tx_work, mt6628_runtime_tx_work);
	INIT_WORK(&wl->event_work, mt6628_runtime_event_work);
	INIT_WORK(&wl->mgmt_work, mt6628_runtime_mgmt_work);
	spin_lock_init(&wl->tx_lock);
	init_waitqueue_head(&wl->tx_wait);
	skb_queue_head_init(&wl->tx_queue);
	skb_queue_head_init(&wl->rx_queue);
	skb_queue_head_init(&wl->event_queue);
	skb_queue_head_init(&wl->mgmt_queue);
	skb_queue_head_init(&wl->async_event_queue);
	skb_queue_head_init(&wl->async_mgmt_queue);
	init_waitqueue_head(&wl->event_wait);
	mutex_init(&wl->cmd_mutex);
	spin_lock_init(&wl->cmd_lock);
	init_completion(&wl->cmd_done);
	wl->sta_rec_idx = MT6628_STA_REC_INDEX_NOT_FOUND;

	spin_lock_irqsave(&wl->tx_lock, flags);
	for (i = 0; i < MT6628_WLAN_TX_TC_NUM; i++) {
		wl->tx_max[i] = mt6628_tx_default_resources[i];
		wl->tx_free[i] = mt6628_tx_default_resources[i];
	}
	spin_unlock_irqrestore(&wl->tx_lock, flags);

	ndev = alloc_etherdev(sizeof(struct mt6628_wlan *));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, &wl->func->dev);
	ndev->netdev_ops = &mt6628_netdev_ops;
	eth_hw_addr_random(ndev);
	netif_carrier_off(ndev);
	*(struct mt6628_wlan **)netdev_priv(ndev) = wl;
	netif_napi_add(ndev, &wl->napi, mt6628_napi_poll);

	ret = register_netdev(ndev);
	if (ret)
		goto err_free_netdev;
	wl->netdev = ndev;

	sdio_claim_host(wl->func);
	ret = sdio_claim_irq(wl->func, mt6628_runtime_irq);
	if (!ret)
		wl->irq_claimed = true;
	if (!ret) {
		wl->runtime_started = true;
		ret = mt6628_runtime_write32(wl, MT6628_MCR_WHIER,
					     MT6628_WHIER_RUNTIME);
	}
	if (!ret)
		ret = mt6628_runtime_write32(wl, MT6628_MCR_WHLPCR,
					     MT6628_INT_EN_SET);
	if (ret && wl->irq_claimed) {
		wl->runtime_started = false;
		sdio_release_irq(wl->func);
		wl->irq_claimed = false;
	}
	sdio_release_host(wl->func);
	if (ret)
		goto err_unregister;

	ret = mt6628_wlan_query_basic_config(wl);
	if (ret)
		dev_warn(&wl->func->dev,
			"failed to read firmware MAC address: %d\n", ret);

	return 0;

err_unregister:
	unregister_netdev(ndev);
	wl->netdev = NULL;
	return ret;
err_free_netdev:
	netif_napi_del(&wl->napi);
	free_netdev(ndev);
	return ret;
}

void mt6628_wlan_runtime_stop(struct mt6628_wlan *wl)
{
	struct net_device *ndev = wl->netdev;
	unsigned long flags;

	wl->runtime_started = false;

	spin_lock_irqsave(&wl->cmd_lock, flags);
	if (wl->cmd_pending) {
		wl->cmd_status = -ESHUTDOWN;
		wl->cmd_pending = false;
		wl->cmd_response_len = 0;
		complete(&wl->cmd_done);
	}
	spin_unlock_irqrestore(&wl->cmd_lock, flags);

	if (wl->irq_claimed) {
		sdio_claim_host(wl->func);
		mt6628_runtime_write32(wl, MT6628_MCR_WHIER, 0);
		mt6628_runtime_write32(wl, MT6628_MCR_WHLPCR, MT6628_INT_EN_CLR);
		sdio_release_irq(wl->func);
		wl->irq_claimed = false;
		sdio_release_host(wl->func);
	}

	cancel_work_sync(&wl->irq_work);
	cancel_work_sync(&wl->tx_work);
	cancel_work_sync(&wl->event_work);
	cancel_work_sync(&wl->mgmt_work);

	if (ndev) {
		unregister_netdev(ndev);
		netif_napi_del(&wl->napi);
		wl->netdev = NULL;
	}

	mt6628_runtime_free_queues(wl);
}

EXPORT_SYMBOL_GPL(mt6628_wlan_runtime_start);
EXPORT_SYMBOL_GPL(mt6628_wlan_runtime_stop);
