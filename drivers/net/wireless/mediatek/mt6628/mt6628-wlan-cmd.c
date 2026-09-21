// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN runtime command/event engine
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "mtk-wlan.h"
#include "mtk-wlan-hif.h"

#define MT6628_CMD_TIMEOUT_MS	1000

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

	if (!wait_event_timeout(wl->tx_wait,
				mt6628_cmd_tc_available(wl),
				msecs_to_jiffies(MT6628_CMD_TIMEOUT_MS)))
		return -EBUSY;

	spin_lock_irqsave(&wl->tx_lock, flags);
	if (!wl->tx_free[MT6628_TX_TC_CMD]) {
		spin_unlock_irqrestore(&wl->tx_lock, flags);
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
			 size_t *response_len, unsigned int timeout_ms)
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
	xfer_len = ALIGN(packet_len, 4);
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
		wl->cmd_response = response;
		wl->cmd_response_capacity = response_capacity;
		wl->cmd_response_len = 0;
		wl->cmd_status = 0;
		spin_unlock_irqrestore(&wl->cmd_lock, flags);
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
		u8 mac[ETH_ALEN];
		u8 native_80211;
		u8 reserved;
		__le16 rx_checksum;
		__le16 tx_checksum;
	} __packed response;
	size_t response_len;
	int ret;

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_BASIC_CONFIG, 0,
				   NULL, 0, &response, sizeof(response),
				   &response_len, MT6628_CMD_TIMEOUT_MS);
	if (ret)
		return ret;
	if (response_len != sizeof(response) ||
	    is_multicast_ether_addr(response.mac) ||
	    is_zero_ether_addr(response.mac))
		return -EPROTO;

	eth_hw_addr_set(wl->netdev, response.mac);
	return 0;
}

EXPORT_SYMBOL_GPL(mt6628_wlan_send_cmd);
EXPORT_SYMBOL_GPL(mt6628_wlan_query_basic_config);
