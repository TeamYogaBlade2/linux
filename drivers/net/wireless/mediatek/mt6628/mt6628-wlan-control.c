// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN firmware control helpers
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/mmc/sdio_func.h>
#include <linux/slab.h>

#include "mtk-wlan-hif.h"
#include "mtk-wlan.h"

#define MT6628_HIF_TX_HEADER_LEN	16
#define MT6628_HIF_TX_RESOURCE_OFFSET	2
#define MT6628_HIF_TX_PACKET_TYPE_OFFSET	6
#define MT6628_HIF_TX_BURST_END	BIT(5)
#define MT6628_HIF_TX_BASIC_RATE	BIT(2)

#define MT6628_CMD_CH_ACTION_REQ		0
#define MT6628_CMD_CH_ACTION_ABORT		1
#define MT6628_EVENT_CH_STATUS_GRANT	0
#define MT6628_CH_REQ_TYPE_JOIN		0
#define MT6628_CH_MAX_INTERVAL_MS	5000

#define MT6628_STA_REC_INDEX_NOT_FOUND	0xfe
#define MT6628_STA_TYPE_LEGACY_AP	0x41

#define MT6628_PHY_TYPE_SET_11BG	0x03
#define MT6628_PHY_TYPE_SET_11BGN	0x0b
#define MT6628_RATE_SET_11BG		0x3fcf
#define MT6628_BASIC_RATE_SET_11BG	0x000f
#define MT6628_BASIC_PHY_TYPE_ERP	1
#define MT6628_PHY_TYPE_SET_11A		0x08
#define MT6628_PHY_TYPE_SET_11AN	0x0c
#define MT6628_RATE_SET_11A		0x3fc0
#define MT6628_BASIC_RATE_SET_11A	0x0540
#define MT6628_BASIC_PHY_TYPE_OFDM	3
#define MT6628_HT_CAP_SGI_20		BIT(5)
#define MT6628_HT_MCS_SET		0xff
#define MT6628_HT_AMPDU_PARAM		3

#define MT6628_AUTH_MODE_OPEN		0
#define MT6628_AUTH_MODE_WPA2_PSK	7
#define MT6628_ENCRYPTION_DISABLED	1
#define MT6628_ENCRYPTION3_KEY_ABSENT	7
#define MT6628_CIPHER_SUITE_CCMP	4
#define MT6628_PS_PROFILE_CAM		0
#define MT6628_PS_PROFILE_FAST_PSP	2

#define MT6628_KEY_INDEX_MAX		3
#define MT6628_KEY_MATERIAL_LEN		32
#define MT6628_KEY_RSC_LEN		16

struct mt6628_cmd_ch_privilege {
	u8 net_type_index;
	u8 token_id;
	u8 action;
	u8 primary_channel;
	u8 rf_sco;
	u8 rf_band;
	u8 req_type;
	u8 reserved;
	__le32 max_interval;
	u8 bssid[ETH_ALEN];
	u8 reserved_tail[2];
} __packed;

struct mt6628_event_ch_privilege {
	u8 net_type_index;
	u8 token_id;
	u8 status;
	u8 primary_channel;
	u8 rf_sco;
	u8 rf_band;
	u8 req_type;
	u8 reserved;
	__le32 grant_interval;
} __packed;

struct mt6628_cmd_set_bss_rlm_param {
	u8 net_type_index;
	u8 rf_band;
	u8 primary_channel;
	u8 rf_sco;
	u8 erp_protect_mode;
	u8 ht_protect_mode;
	u8 gf_operation_mode;
	u8 tx_rifs_mode;
	__le16 ht_op_info3;
	__le16 ht_op_info2;
	u8 ht_op_info1;
	u8 use_short_preamble;
	u8 use_short_slot_time;
	u8 check_id;
} __packed;

struct mt6628_cmd_set_bss_info {
	u8 net_type_index;
	u8 connection_state;
	u8 current_op_mode;
	u8 ssid_len;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 bssid[ETH_ALEN];
	u8 is_qbss;
	u8 reserved1;
	__le16 operational_rate_set;
	__le16 bss_basic_rate_set;
	u8 sta_rec_idx_of_ap;
	u8 reserved2;
	u8 reserved3;
	u8 non_ht_basic_phy_type;
	u8 auth_mode;
	u8 enc_status;
	u8 phy_type_set;
	u8 own_mac[ETH_ALEN];
	u8 wapi_mode;
	u8 is_ap_mode;
	u8 reserved4;
	struct mt6628_cmd_set_bss_rlm_param rlm;
} __packed;

struct mt6628_cmd_update_sta_record {
	u8 index;
	u8 sta_type;
	u8 mac_addr[ETH_ALEN];
	__le16 assoc_id;
	__le16 listen_interval;
	u8 net_type_index;
	u8 desired_phy_type_set;
	__le16 desired_non_ht_rate_set;
	__le16 bss_basic_rate_set;
	u8 is_qos;
	u8 is_uapsd_supported;
	u8 sta_state;
	u8 mcs_set;
	u8 sup_mcs32;
	u8 ampdu_param;
	__le16 ht_cap_info;
	__le16 ht_extended_cap;
	__le32 tx_beamforming_cap;
	u8 asel_cap;
	u8 rcpi;
	u8 need_resp;
	u8 uapsd_ac;
	u8 uapsd_sp;
	u8 reserved[3];
} __packed;

struct mt6628_cmd_bss_activate_ctrl {
	u8 net_type_index;
	u8 active;
	u8 reserved[2];
} __packed;

struct mt6628_cmd_remove_sta_record {
	u8 index;
	u8 reserved;
	u8 mac_addr[ETH_ALEN];
} __packed;

struct mt6628_cmd_ps_profile {
	u8 net_type_index;
	u8 ps_profile;
	u8 reserved[2];
} __packed;

struct mt6628_hif_mgmt_tx_hdr {
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

struct mt6628_cmd_add_remove_key {
	u8 add_remove;
	u8 tx_key;
	u8 key_type;
	u8 is_authenticator;
	u8 peer_addr[ETH_ALEN];
	u8 net_type_index;
	u8 algorithm_id;
	u8 key_id;
	u8 key_len;
	u8 reserved[2];
	u8 key_material[MT6628_KEY_MATERIAL_LEN];
	u8 key_rsc[MT6628_KEY_RSC_LEN];
} __packed;

static_assert(sizeof(struct mt6628_cmd_ch_privilege) == 20);
static_assert(sizeof(struct mt6628_event_ch_privilege) == 12);
static_assert(sizeof(struct mt6628_cmd_set_bss_rlm_param) == 16);
static_assert(sizeof(struct mt6628_cmd_set_bss_info) == 80);
static_assert(sizeof(struct mt6628_cmd_update_sta_record) == 40);
static_assert(sizeof(struct mt6628_cmd_bss_activate_ctrl) == 4);
static_assert(sizeof(struct mt6628_cmd_remove_sta_record) == 8);
static_assert(sizeof(struct mt6628_cmd_ps_profile) == 4);
static_assert(sizeof(struct mt6628_cmd_add_remove_key) == 64);
static_assert(sizeof(struct mt6628_hif_mgmt_tx_hdr) == 16);

int mt6628_wlan_request_channel(struct mt6628_wlan *wl,
				const struct ieee80211_channel *channel,
				const u8 *bssid)
{
	struct mt6628_cmd_ch_privilege cmd = {};
	struct mt6628_event_ch_privilege event = {};
	size_t response_len;
	u8 rf_band;
	u8 token;
	int ret;

	if (!channel)
		return -EINVAL;
	if (!bssid || is_multicast_ether_addr(bssid) ||
	    is_zero_ether_addr(bssid))
		return -EINVAL;

	switch (channel->band) {
	case NL80211_BAND_2GHZ:
		if (channel->hw_value < 1 || channel->hw_value > 14)
			return -EINVAL;
		rf_band = MT6628_BAND_2GHZ;
		break;
	case NL80211_BAND_5GHZ:
		if (!channel->hw_value || channel->hw_value > 216)
			return -EINVAL;
		rf_band = MT6628_BAND_5GHZ;
		break;
	default:
		return -EOPNOTSUPP;
	}

	token = wl->channel_token + 1;
	if (!token)
		token = 1;

	cmd.net_type_index = 0;
	cmd.token_id = token;
	cmd.action = MT6628_CMD_CH_ACTION_REQ;
	cmd.primary_channel = channel->hw_value;
	cmd.rf_sco = 0;
	cmd.rf_band = rf_band;
	cmd.req_type = MT6628_CH_REQ_TYPE_JOIN;
	cmd.max_interval = cpu_to_le32(MT6628_CH_MAX_INTERVAL_MS);
	ether_addr_copy(cmd.bssid, bssid);

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_CH_PRIVILEGE, 1,
				   &cmd, sizeof(cmd), &event, sizeof(event),
				   &response_len, MT6628_EVENT_ID_CH_PRIVILEGE, 2000);
	if (ret)
		return ret;
	if (response_len != sizeof(event) ||
	    event.net_type_index != 0 || event.token_id != token ||
	    event.status != MT6628_EVENT_CH_STATUS_GRANT ||
	    event.primary_channel != channel->hw_value ||
	    event.rf_band != rf_band)
		return -EPROTO;

	wl->channel_token = token;
	return 0;
}

int mt6628_wlan_release_channel(struct mt6628_wlan *wl)
{
	struct mt6628_cmd_ch_privilege cmd = {};
	u8 token = wl->channel_token;
	int ret;

	if (!token)
		return 0;

	cmd.net_type_index = 0;
	cmd.token_id = token;
	cmd.action = MT6628_CMD_CH_ACTION_ABORT;
	cmd.req_type = MT6628_CH_REQ_TYPE_JOIN;

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_CH_PRIVILEGE, 1,
				   &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);
	if (!ret)
		wl->channel_token = 0;

	return ret;
}

int mt6628_wlan_update_sta_record(struct mt6628_wlan *wl,
				enum mt6628_sta_state state,
				u16 assoc_id, const u8 *bssid)
{
	struct mt6628_cmd_update_sta_record cmd = {};

	if (wl->sta_rec_idx == MT6628_STA_REC_INDEX_NOT_FOUND || !bssid)
		return -EINVAL;
	if (state > MT6628_STA_STATE_3)
		return -EINVAL;

	cmd.index = wl->sta_rec_idx;
	cmd.sta_type = MT6628_STA_TYPE_LEGACY_AP;
	ether_addr_copy(cmd.mac_addr, bssid);
	cmd.assoc_id = cpu_to_le16(assoc_id);
	cmd.listen_interval = cpu_to_le16(10);
	cmd.net_type_index = 0;

	switch (wl->conn_band) {
	case NL80211_BAND_2GHZ:
		cmd.desired_phy_type_set = MT6628_PHY_TYPE_SET_11BGN;
		cmd.desired_non_ht_rate_set =
			cpu_to_le16(MT6628_RATE_SET_11BG);
		cmd.bss_basic_rate_set =
			cpu_to_le16(MT6628_BASIC_RATE_SET_11BG);
		break;
	case NL80211_BAND_5GHZ:
		cmd.desired_phy_type_set = MT6628_PHY_TYPE_SET_11AN;
		cmd.desired_non_ht_rate_set =
			cpu_to_le16(MT6628_RATE_SET_11A);
		cmd.bss_basic_rate_set =
			cpu_to_le16(MT6628_BASIC_RATE_SET_11A);
		break;
	default:
		return -EINVAL;
	}

	cmd.sta_state = state;
	cmd.mcs_set = MT6628_HT_MCS_SET;
	cmd.sup_mcs32 = 0;
	cmd.ampdu_param = MT6628_HT_AMPDU_PARAM;
	cmd.ht_cap_info = cpu_to_le16(MT6628_HT_CAP_SGI_20);
	cmd.need_resp = 0;

	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_UPDATE_STA_RECORD, 1,
				    &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);
}

int mt6628_wlan_set_bss_info(struct mt6628_wlan *wl, u8 channel,
			     const u8 *ssid, u8 ssid_len,
			     const u8 *bssid, bool connected)
{
	struct mt6628_cmd_set_bss_info cmd = {};

	if (wl->sta_rec_idx == MT6628_STA_REC_INDEX_NOT_FOUND ||
	    !ssid || !ssid_len || ssid_len > IEEE80211_MAX_SSID_LEN ||
	    !bssid || !channel)
		return -EINVAL;

	cmd.net_type_index = 0;
	cmd.connection_state = connected;
	cmd.current_op_mode = 0;
	cmd.ssid_len = ssid_len;
	memcpy(cmd.ssid, ssid, ssid_len);
	ether_addr_copy(cmd.bssid, bssid);
	cmd.sta_rec_idx_of_ap = wl->sta_rec_idx;
	cmd.auth_mode = wl->conn_secure ? MT6628_AUTH_MODE_WPA2_PSK :
		MT6628_AUTH_MODE_OPEN;
	cmd.enc_status = wl->conn_secure ? MT6628_ENCRYPTION3_KEY_ABSENT :
		MT6628_ENCRYPTION_DISABLED;
	ether_addr_copy(cmd.own_mac, wl->netdev->dev_addr);

	cmd.rlm.net_type_index = 0;
	cmd.rlm.primary_channel = channel;
	cmd.rlm.rf_sco = 0;
	cmd.rlm.use_short_preamble = 1;
	cmd.rlm.use_short_slot_time = 1;
	cmd.rlm.check_id = 0x72;

	switch (wl->conn_band) {
	case NL80211_BAND_2GHZ:
		if (channel > 14)
			return -EINVAL;
		cmd.operational_rate_set =
			cpu_to_le16(MT6628_RATE_SET_11BG);
		cmd.bss_basic_rate_set =
			cpu_to_le16(MT6628_BASIC_RATE_SET_11BG);
		cmd.non_ht_basic_phy_type = MT6628_BASIC_PHY_TYPE_ERP;
		cmd.phy_type_set = MT6628_PHY_TYPE_SET_11BGN;
		cmd.rlm.rf_band = MT6628_BAND_2GHZ;
		break;
	case NL80211_BAND_5GHZ:
		if (channel > 216)
			return -EINVAL;
		cmd.operational_rate_set =
			cpu_to_le16(MT6628_RATE_SET_11A);
		cmd.bss_basic_rate_set =
			cpu_to_le16(MT6628_BASIC_RATE_SET_11A);
		cmd.non_ht_basic_phy_type = MT6628_BASIC_PHY_TYPE_OFDM;
		cmd.phy_type_set = MT6628_PHY_TYPE_SET_11AN;
		cmd.rlm.rf_band = MT6628_BAND_5GHZ;
		break;
	default:
		return -EINVAL;
	}

	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_SET_BSS_INFO, 1,
				    &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);
}

int mt6628_wlan_activate_bss(struct mt6628_wlan *wl, bool active)
{
	struct mt6628_cmd_bss_activate_ctrl cmd = {
		.net_type_index = 0,
		.active = active,
	};

	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_BSS_ACTIVATE_CTRL, 1,
				    &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);
}

int mt6628_wlan_remove_sta_record(struct mt6628_wlan *wl, const u8 *bssid)
{
	struct mt6628_cmd_remove_sta_record cmd = {
		.index = wl->sta_rec_idx,
	};

	if (wl->sta_rec_idx == MT6628_STA_REC_INDEX_NOT_FOUND || !bssid)
		return -EINVAL;

	ether_addr_copy(cmd.mac_addr, bssid);
	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_REMOVE_STA_RECORD, 1,
				    &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);
}

int mt6628_wlan_add_key(struct mt6628_wlan *wl, u8 key_index,
			bool pairwise, const u8 *mac_addr,
			const struct key_params *params)
{
	struct mt6628_cmd_add_remove_key cmd = {};
	const u8 *peer;
	u8 response[4];
	size_t response_len;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (!wl->conn_secure)
		return -ENOTCONN;
	if (key_index > MT6628_KEY_INDEX_MAX)
		return -EINVAL;
	if (!params || params->cipher != WLAN_CIPHER_SUITE_CCMP)
		return -EOPNOTSUPP;
	if (params->key_len != 16)
		return -EINVAL;
	if (params->seq_len < 0 || params->seq_len > MT6628_KEY_RSC_LEN)
		return -EINVAL;
	if (!params->key)
		return -EINVAL;

	if (pairwise) {
		if (!mac_addr || is_zero_ether_addr(mac_addr) ||
		    is_multicast_ether_addr(mac_addr))
			return -EINVAL;
		peer = mac_addr;
	} else {
		peer = wl->conn_bssid;
	}

	cmd.add_remove = 1;
	/*
	 * A station's GTK is RX-only. The PTK is the station's TX key
	 * unless cfg80211 explicitly requested a receive-only key.
	 */
	cmd.tx_key = pairwise && params->mode != NL80211_KEY_NO_TX;
	cmd.key_type = pairwise;
	cmd.is_authenticator = 0;
	ether_addr_copy(cmd.peer_addr, peer);
	cmd.net_type_index = 0;
	cmd.algorithm_id = MT6628_CIPHER_SUITE_CCMP;
	cmd.key_id = key_index;
	cmd.key_len = params->key_len;
	memcpy(cmd.key_material, params->key, params->key_len);
	if (params->seq_len)
		memcpy(cmd.key_rsc, params->seq, params->seq_len);

	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_ADD_REMOVE_KEY, 1,
				    &cmd, sizeof(cmd), response, sizeof(response),
				    &response_len,
				    MT6628_EVENT_ID_CMD_RESULT, 1000);
}

int mt6628_wlan_del_key(struct mt6628_wlan *wl, u8 key_index,
			bool pairwise, const u8 *mac_addr)
{
	struct mt6628_cmd_add_remove_key cmd = {};
	const u8 *peer;
	u8 response[4];
	size_t response_len;

	/*
	 * Key deletion is normally part of disconnect teardown. Treat it
	 * as a no-op after the runtime/security state has already gone away.
	 */
	if (!wl->runtime_started || !wl->fw_running || !wl->conn_secure)
		return 0;
	if (key_index > MT6628_KEY_INDEX_MAX)
		return -EINVAL;

	if (pairwise) {
		if (!mac_addr || is_zero_ether_addr(mac_addr) ||
		    is_multicast_ether_addr(mac_addr))
			return -EINVAL;
		peer = mac_addr;
	} else {
		peer = wl->conn_bssid;
	}

	cmd.add_remove = 0;
	cmd.is_authenticator = 0;
	ether_addr_copy(cmd.peer_addr, peer);
	cmd.net_type_index = 0;
	cmd.key_id = key_index;

	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_ADD_REMOVE_KEY, 1,
				    &cmd, sizeof(cmd), response, sizeof(response),
				    &response_len,
				    MT6628_EVENT_ID_CMD_RESULT, 1000);
}

int mt6628_wlan_set_power_mgmt(struct mt6628_wlan *wl, bool enabled)
{
	struct mt6628_cmd_ps_profile cmd = {
		.net_type_index = 0,
		.ps_profile = enabled ? MT6628_PS_PROFILE_FAST_PSP :
			MT6628_PS_PROFILE_CAM,
	};
	u8 response[4];
	size_t response_len;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;

	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_POWER_SAVE_MODE, 1,
					&cmd, sizeof(cmd), response, sizeof(response),
					&response_len, MT6628_EVENT_ID_CMD_RESULT, 1000);
}

static bool mt6628_mgmt_tc_available(struct mt6628_wlan *wl)
{
	unsigned long flags;
	bool available;

	spin_lock_irqsave(&wl->tx_lock, flags);
	available = wl->tx_free[MT6628_TX_TC_MGMT] != 0;
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	return available;
}

int mt6628_wlan_mgmt_tx(struct mt6628_wlan *wl, const u8 *frame,
			 size_t frame_len)
{
	struct mt6628_hif_mgmt_tx_hdr hdr = {};
	unsigned long flags;
	size_t packet_len, xfer_len;
	u8 *buf;
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (!frame || frame_len < sizeof(struct ieee80211_hdr))
		return -EINVAL;
	if (frame_len > 4095 - MT6628_HIF_TX_HEADER_LEN)
		return -EMSGSIZE;
	if (wl->sta_rec_idx == MT6628_STA_REC_INDEX_NOT_FOUND)
		return -EINVAL;

	if (!wait_event_timeout(wl->tx_wait,
				!wl->runtime_started ||
				mt6628_mgmt_tc_available(wl),
				msecs_to_jiffies(1000)))
		return -EBUSY;

	if (!wl->runtime_started)
		return -ESHUTDOWN;

	spin_lock_irqsave(&wl->tx_lock, flags);
	if (!wl->tx_free[MT6628_TX_TC_MGMT]) {
		spin_unlock_irqrestore(&wl->tx_lock, flags);
		return -EBUSY;
	}
	wl->tx_free[MT6628_TX_TC_MGMT]--;
	spin_unlock_irqrestore(&wl->tx_lock, flags);

	packet_len = MT6628_HIF_TX_HEADER_LEN + frame_len;
	xfer_len = mt6628_sdio_xfer_len(ALIGN(packet_len, 4));
	buf = kzalloc(xfer_len, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_resource;
	}

	hdr.tx_byte_count_user_priority = cpu_to_le16(packet_len);
	hdr.ether_type_offset = (MT6628_HIF_TX_HEADER_LEN +
					 sizeof(struct ieee80211_hdr)) >> 1;
	hdr.resource_pkt_type_csflags =
		(MT6628_TX_TC_MGMT << MT6628_HIF_TX_RESOURCE_OFFSET) |
		(MT6628_HIF_TX_PKT_TYPE_MANAGEMENT <<
		 MT6628_HIF_TX_PACKET_TYPE_OFFSET);
	hdr.wlan_header_length = sizeof(struct ieee80211_hdr);
	hdr.pkt_format_id_flags = MT6628_HIF_TX_80211_FORMAT;
	hdr.seq_no = 0;
	hdr.sta_rec_idx = wl->sta_rec_idx;
	hdr.forwarding_type_session_id_reserved = MT6628_HIF_TX_BURST_END;
	hdr.ack_bip_basic_rate = MT6628_HIF_TX_NEED_ACK |
		MT6628_HIF_TX_BASIC_RATE;

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), frame, frame_len);

	sdio_claim_host(wl->func);
	ret = sdio_writesb(wl->func, MT6628_MCR_WTDR0, buf, xfer_len);
	sdio_release_host(wl->func);
	kfree(buf);
	if (ret)
		goto err_resource;

	return 0;

err_resource:
	spin_lock_irqsave(&wl->tx_lock, flags);
	wl->tx_free[MT6628_TX_TC_MGMT] = min_t(unsigned int,
					       wl->tx_free[MT6628_TX_TC_MGMT] + 1,
					       wl->tx_max[MT6628_TX_TC_MGMT]);
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	wake_up_all(&wl->tx_wait);
	return ret;
}

EXPORT_SYMBOL_GPL(mt6628_wlan_request_channel);
EXPORT_SYMBOL_GPL(mt6628_wlan_release_channel);
EXPORT_SYMBOL_GPL(mt6628_wlan_update_sta_record);
EXPORT_SYMBOL_GPL(mt6628_wlan_set_bss_info);
EXPORT_SYMBOL_GPL(mt6628_wlan_activate_bss);
EXPORT_SYMBOL_GPL(mt6628_wlan_remove_sta_record);
EXPORT_SYMBOL_GPL(mt6628_wlan_add_key);
EXPORT_SYMBOL_GPL(mt6628_wlan_del_key);
EXPORT_SYMBOL_GPL(mt6628_wlan_set_power_mgmt);
EXPORT_SYMBOL_GPL(mt6628_wlan_mgmt_tx);
