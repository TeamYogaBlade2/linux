// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN runtime command/event engine
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include "mtk-wlan.h"
#include "mtk-wlan-hif.h"

#define MT6628_CMD_TIMEOUT_MS	1000
#define MT6628_CMD_DIAG_RX_LEN	64

static bool mt6628_cmd_tc_available(struct mt6628_wlan *wl)
{
	unsigned long flags;
	bool available;

	spin_lock_irqsave(&wl->tx_lock, flags);
	available = wl->tx_free[MT6628_TX_TC_CMD] != 0;
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	return available;
}

static int mt6628_cmd_acquire_tc(struct mt6628_wlan *wl)
{
	unsigned long flags;
	u32 wtsr0 = 0, wtsr1 = 0;
	unsigned int credits, credits_max;
	int poll_ret;

	if (!wait_event_timeout(wl->tx_wait,
				!wl->runtime_started ||
				mt6628_cmd_tc_available(wl),
				msecs_to_jiffies(MT6628_CMD_TIMEOUT_MS))) {
		poll_ret = mt6628_wlan_poll_tx_status(wl, &wtsr0, &wtsr1);
		spin_lock_irqsave(&wl->tx_lock, flags);
		credits = wl->tx_free[MT6628_TX_TC_CMD];
		credits_max = wl->tx_max[MT6628_TX_TC_CMD];
		spin_unlock_irqrestore(&wl->tx_lock, flags);
		dev_warn_ratelimited(&wl->func->dev,
			"command TX credit timeout: TC=%u free=%u/%u "
			"runtime=%u pm_idle=%u driver_owned=%u "
			"poll=%d WTSR0=%#010x WTSR1=%#010x\n",
			MT6628_TX_TC_CMD, credits, credits_max,
			(unsigned int)wl->runtime_started,
			(unsigned int)wl->pm_idle,
			(unsigned int)wl->driver_owned,
			poll_ret, wtsr0, wtsr1);
		if (!credits)
			return -EBUSY;
	}

	if (!wl->runtime_started)
		return -ESHUTDOWN;

	spin_lock_irqsave(&wl->tx_lock, flags);
	if (!wl->tx_free[MT6628_TX_TC_CMD]) {
		credits = wl->tx_free[MT6628_TX_TC_CMD];
		credits_max = wl->tx_max[MT6628_TX_TC_CMD];
		spin_unlock_irqrestore(&wl->tx_lock, flags);
		dev_warn_ratelimited(&wl->func->dev,
			"command TX credit raced empty: TC=%u free=%u/%u\n",
			MT6628_TX_TC_CMD, credits, credits_max);
		return -EBUSY;
	}
	wl->tx_free[MT6628_TX_TC_CMD]--;
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	return 0;
}

static void mt6628_cmd_release_tc(struct mt6628_wlan *wl)
{
	unsigned long flags;

	spin_lock_irqsave(&wl->tx_lock, flags);
	wl->tx_free[MT6628_TX_TC_CMD] = min_t(unsigned int,
					      wl->tx_free[MT6628_TX_TC_CMD] + 1,
					      wl->tx_max[MT6628_TX_TC_CMD]);
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	wake_up_all(&wl->tx_wait);
}

int mt6628_wlan_send_cmd(struct mt6628_wlan *wl, u8 cid, u8 set_query,
				 const void *payload, size_t payload_len,
				 void *response, size_t response_capacity,
				 size_t *response_len, u8 expected_event_id,
				 unsigned int timeout_ms)
{
	struct mt6628_wifi_cmd_hdr *cmd;
	unsigned long flags;
	size_t packet_len, xfer_len;
	unsigned long timeout;
	u8 *buf;
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (payload_len > 4095 - MT6628_WIFI_CMD_HEADER_LEN)
		return -EMSGSIZE;

	mutex_lock(&wl->cmd_mutex);
	ret = mt6628_cmd_acquire_tc(wl);
	if (ret)
		goto out_unlock;

	packet_len = MT6628_WIFI_CMD_HEADER_LEN + payload_len;
	xfer_len = mt6628_sdio_xfer_len(ALIGN(packet_len, 4));
	buf = kzalloc(xfer_len, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_resource;
	}

	cmd = (struct mt6628_wifi_cmd_hdr *)buf;
	cmd->tx_byte_count_user_priority = cpu_to_le16(ALIGN(packet_len, 4));
	cmd->resource_pkt_type_csflags =
		(MT6628_TX_TC_CMD << MT6628_HIF_TX_RESOURCE_OFFSET) |
		(MT6628_HIF_TX_PKT_TYPE_CMD << MT6628_HIF_TX_PACKET_TYPE_OFFSET);
	cmd->cid = cid;
	cmd->set_query = set_query;
	cmd->seq_num = ++wl->cmd_seq_num;

	if (payload_len)
		memcpy(buf + MT6628_WIFI_CMD_HEADER_LEN, payload, payload_len);

	if (response || response_len) {
		spin_lock_irqsave(&wl->cmd_lock, flags);
		reinit_completion(&wl->cmd_done);
		wl->cmd_pending = true;
		wl->cmd_pending_seq = cmd->seq_num;
		wl->cmd_pending_id = cid;
		wl->cmd_pending_eid = expected_event_id;
		wl->cmd_response = response;
		wl->cmd_response_capacity = response_capacity;
		wl->cmd_response_len = 0;
		wl->cmd_status = 0;
		spin_unlock_irqrestore(&wl->cmd_lock, flags);
	}

	/* Bulk transfer: reclaim Driver Own, the 32-bit helpers are bypassed. */
	ret = mt6628_wlan_pm_busy(wl);
	if (ret) {
		kfree(buf);
		/*
		 * Nothing was written, so no CMD_RESULT can arrive for this
		 * command: drop the pending state here too, exactly as the
		 * transfer-failure path below does, or the next command would
		 * wait on a completion that will never come.
		 */
		spin_lock_irqsave(&wl->cmd_lock, flags);
		wl->cmd_pending = false;
		spin_unlock_irqrestore(&wl->cmd_lock, flags);
		goto err_resource;
	}

	sdio_claim_host(wl->func);
	ret = sdio_writesb(wl->func, MT6628_MCR_WTDR1, buf, xfer_len);
	sdio_release_host(wl->func);
	kfree(buf);
	if (ret) {
		spin_lock_irqsave(&wl->cmd_lock, flags);
		wl->cmd_pending = false;
		spin_unlock_irqrestore(&wl->cmd_lock, flags);
		goto err_resource;
	}

	if (!response && !response_len) {
		ret = 0;
		goto out_unlock;
	}

	timeout = msecs_to_jiffies(timeout_ms ?: MT6628_CMD_TIMEOUT_MS);
	if (!wait_for_completion_timeout(&wl->cmd_done, timeout)) {
		spin_lock_irqsave(&wl->cmd_lock, flags);
		wl->cmd_pending = false;
		spin_unlock_irqrestore(&wl->cmd_lock, flags);
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	spin_lock_irqsave(&wl->cmd_lock, flags);
	ret = wl->cmd_status;
	if (response_len)
		*response_len = wl->cmd_response_len;
	spin_unlock_irqrestore(&wl->cmd_lock, flags);
	goto out_unlock;

err_resource:
	mt6628_cmd_release_tc(wl);
out_unlock:
	mutex_unlock(&wl->cmd_mutex);
	return ret;
}

int mt6628_wlan_query_basic_config(struct mt6628_wlan *wl)
{
	struct {
		struct mt6628_wifi_cmd_hdr hdr;
		u8 mac[ETH_ALEN];
		u8 native_80211;
		u8 reserved;
		__le16 rx_checksum;
		__le16 tx_checksum;
	} __packed *request;
	struct {
		u8 mac[ETH_ALEN];
		u8 native_80211;
		u8 reserved;
		__le16 rx_checksum;
		__le16 tx_checksum;
	} __packed response;
	struct mt6628_wifi_event_hdr *event;
	u8 *rx_buf;
	unsigned long timeout;
	u32 rx_len_reg = 0;
	size_t rx_len = 0;
	size_t rx0_diag_len;
	u16 rx0_len;
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;

	/*
	 * sdio_writesb()/sdio_readsb() submit their buffers to the MMC host.
	 * Keep these port-transfer buffers out of the kernel stack because
	 * this platform's MSDC controller uses DMA for data transfers.
	 */
	request = kzalloc(sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	rx_buf = kmalloc(MT6628_CMD_DIAG_RX_LEN, GFP_KERNEL);
	if (!rx_buf) {
		kfree(request);
		return -ENOMEM;
	}

	mutex_lock(&wl->cmd_mutex);

	ret = mt6628_cmd_acquire_tc(wl);
	if (ret)
		goto out_unlock;

	/*
	 * Match the downstream BASIC_CONFIG query format and sequence:
	 * send the complete command, then poll WRPLR and read WRDR1.
	 */
	request->hdr.tx_byte_count_user_priority =
		cpu_to_le16(sizeof(*request));
	request->hdr.resource_pkt_type_csflags =
		(MT6628_TX_TC_CMD << MT6628_HIF_TX_RESOURCE_OFFSET) |
		(MT6628_HIF_TX_PKT_TYPE_CMD <<
		 MT6628_HIF_TX_PACKET_TYPE_OFFSET);
	request->hdr.cid = MT6628_CMD_ID_BASIC_CONFIG;
	request->hdr.set_query = 0;
	request->hdr.seq_num = ++wl->cmd_seq_num;

	ret = mt6628_wlan_pm_busy(wl);
	if (ret)
		goto out_release_tc;

	sdio_claim_host(wl->func);
	ret = sdio_writesb(wl->func, MT6628_MCR_WTDR1, request,
			   mt6628_sdio_xfer_len(sizeof(*request)));
	if (!ret) {
		timeout = jiffies + msecs_to_jiffies(MT6628_CMD_TIMEOUT_MS);

		do {
			rx_len_reg = sdio_readl(wl->func,
						MT6628_MCR_WRPLR, &ret);
			if (ret)
				break;

			rx_len = rx_len_reg >> 16;
			if (rx_len)
				break;

			usleep_range(50, 100);
		} while (time_before(jiffies, timeout));

		if (!ret && !rx_len) {
			u32 whisr;
			u32 wrplr_after;
			int diag_ret;
			int after_ret;
			int packet_ret;

			whisr = sdio_readl(wl->func, MT6628_MCR_WHISR,
					   &diag_ret);
			dev_err(&wl->func->dev,
				"BASIC_CONFIG poll timeout: "
				"seq=%u WRPLR=%#010x WHISR=%#010x "
				"read_err=%d\n",
				request->hdr.seq_num, rx_len_reg, whisr,
				diag_ret);

			/*
			 * Match nicRxWaitResponse() in the downstream driver:
			 * response polling reads exactly the length reported by
			 * WRPLR. The extra hardware DWORD is used by the regular
			 * RX-buffer path, not this response-polling path.
			 *
			 * Do not infer packet type from the reported length.
			 */
			rx0_len = rx_len_reg & 0xffff;
			if (rx0_len >= sizeof(*event) &&
			    rx0_len <= MT6628_CMD_DIAG_RX_LEN) {
				rx0_diag_len = rx0_len;
				packet_ret = sdio_readsb(wl->func, rx_buf,
							 MT6628_MCR_WRDR0,
							 rx0_diag_len);
				if (packet_ret) {
					dev_err(&wl->func->dev,
						"failed to capture RX0 packet: %d\n",
						packet_ret);
				} else {
					dev_err(&wl->func->dev,
						"BASIC_CONFIG timeout: "
						"RX0 packet length=%u\n",
						rx0_len);
					print_hex_dump(KERN_ERR,
					       "mtk_wlan RX0: ",
					       DUMP_PREFIX_OFFSET, 16, 1,
					       rx_buf, rx0_len, false);
				}

				wrplr_after = sdio_readl(wl->func,
							 MT6628_MCR_WRPLR,
							 &after_ret);
				if (!after_ret)
					dev_err(&wl->func->dev,
						"after RX0 read: WRPLR=%#010x "
						"(before=%#010x)\n",
						wrplr_after, rx_len_reg);
				else
					dev_err(&wl->func->dev,
						"failed to reread WRPLR after "
						"RX0 read: %d\n", after_ret);
			}

			ret = -ETIMEDOUT;
		}

		if (!ret && (rx_len < sizeof(*event) ||
			     rx_len > MT6628_WIFI_EVENT_HEADER_LEN +
				      sizeof(response)))
			ret = -EMSGSIZE;

		if (!ret)
			ret = sdio_readsb(wl->func, rx_buf,
					  MT6628_MCR_WRDR1, rx_len);
	}
	sdio_release_host(wl->func);
	if (ret)
		goto out_release_tc;

	event = (struct mt6628_wifi_event_hdr *)rx_buf;
	if (le16_to_cpu(event->packet_len) != rx_len ||
	    (le16_to_cpu(event->packet_type) & GENMASK(1, 0)) != 1 ||
	    event->eid != MT6628_EVENT_ID_BASIC_CONFIG ||
	    event->seq_num != request->hdr.seq_num) {
		ret = -EPROTO;
		goto out_release_tc;
	}

	if (rx_len - sizeof(*event) != sizeof(response)) {
		ret = -EMSGSIZE;
		goto out_release_tc;
	}

	memcpy(&response, rx_buf + sizeof(*event), sizeof(response));
	if (is_multicast_ether_addr(response.mac) ||
	    is_zero_ether_addr(response.mac)) {
		ret = -EPROTO;
		goto out_release_tc;
	}

	eth_hw_addr_set(wl->netdev, response.mac);
	if (wl->wiphy)
		ether_addr_copy(wl->wiphy->perm_addr, response.mac);
	ret = 0;

	/*
	 * Keep the command TX resource reserved until TX_DONE is processed
	 * after runtime interrupts are enabled.
	 */
	goto out_unlock;

out_release_tc:
	mt6628_cmd_release_tc(wl);
out_unlock:
	mutex_unlock(&wl->cmd_mutex);
	kfree(rx_buf);
	kfree(request);
	return ret;
}

EXPORT_SYMBOL_GPL(mt6628_wlan_send_cmd);
EXPORT_SYMBOL_GPL(mt6628_wlan_query_basic_config);
