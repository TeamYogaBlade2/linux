// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN firmware control helpers
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/mmc/sdio_func.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

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
#define MT6628_HT_CAP_SUP_WIDTH_20_40	BIT(1)
#define MT6628_HT_MCS_SET		0xff
#define MT6628_HT_AMPDU_PARAM		3

#define MT6628_PS_PROFILE_CAM		0
#define MT6628_PS_PROFILE_FAST_PSP	2

/*
 * Unsolicited RX Block Ack session notifications.  These are the two event
 * ids the downstream enum marks "(obsolete)"; the annotation is wrong, both
 * are dispatched to live code (nic_rx.c:1715-1722).
 */
#define MT6628_EVENT_ID_RX_ADDBA	0x11
#define MT6628_EVENT_ID_RX_DELBA	0x12

/*
 * CFG_STA_REC_NUM, the bound the downstream STA_REC lookup applies to the
 * index carried in these events (include/nic/wlan_def.h:287).  The two
 * reserved index values, STA_REC_INDEX_BMCAST and STA_REC_INDEX_NOT_FOUND
 * (include/mgmt/cnm_mem.h:501-502), are 0xff and 0xfe and therefore fall
 * outside this range as well, so one test rejects all three.
 */
#define MT6628_STA_REC_NUM		20

/*
 * Block Ack Parameter Set field and BAR Start Sequence Control, as the
 * firmware decodes them (include/nic/mac.h:465, include/nic/mac.h:681-684).
 * BITS(2,5) and BITS(6,15) there are inclusive of both ends.
 */
#define MT6628_BA_PARAM_SET_TID_MASK		GENMASK(5, 2)
#define MT6628_BA_PARAM_SET_TID_OFFSET		2
#define MT6628_BA_PARAM_SET_WIN_SIZE_MASK	GENMASK(15, 6)
#define MT6628_BA_PARAM_SET_WIN_SIZE_OFFSET	6
#define MT6628_BAR_SSC_SN_OFFSET		4

#define MT6628_KEY_INDEX_MAX		MT6628_WLAN_KEY_INDEX_MAX
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

struct mt6628_event_activate_sta_rec {
	u8 mac_addr[ETH_ALEN];
	u8 sta_rec_idx;
	u8 network_type_index;
	u8 is_qos;
	u8 is_ap;
	u8 reserved[2];
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

/*
 * Body of EVENT_ID_RX_ADDBA: EVENT_RX_ADDBA_T minus the eight-byte event
 * header the dispatcher has already consumed (include/nic/que_mgt.h:532).
 * The fields the firmware copied out of the peer's ADDBA request are the
 * ones the host acted on downstream.
 */
struct mt6628_event_rx_addba {
	u8 sta_rec_idx;
	u8 dialog_token;
	__le16 ba_parameter_set;
	__le16 ba_timeout_value;
	__le16 ba_start_seq_ctrl;
} __packed;

/* Body of EVENT_ID_RX_DELBA: EVENT_RX_DELBA_T minus the event header. */
struct mt6628_event_rx_delba {
	u8 sta_rec_idx;
	u8 tid;
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
static_assert(sizeof(struct mt6628_event_activate_sta_rec) == 12);
static_assert(sizeof(struct mt6628_cmd_bss_activate_ctrl) == 4);
static_assert(sizeof(struct mt6628_cmd_remove_sta_record) == 8);
static_assert(sizeof(struct mt6628_cmd_ps_profile) == 4);
static_assert(sizeof(struct mt6628_cmd_add_remove_key) == 64);
static_assert(sizeof(struct mt6628_hif_mgmt_tx_hdr) == 16);
static_assert(sizeof(struct mt6628_event_rx_addba) == 8);
static_assert(sizeof(struct mt6628_event_rx_delba) == 2);

int mt6628_wlan_request_channel(struct mt6628_wlan *wl,
				const struct ieee80211_channel *channel,
				const u8 *bssid)
{
	return mt6628_wlan_ch_privilege(wl, channel, bssid,
					MT6628_CH_REQ_TYPE_JOIN, true);
}

int mt6628_wlan_ch_privilege(struct mt6628_wlan *wl,
			     const struct ieee80211_channel *channel,
			     const u8 *bssid, u8 req_type, bool require_grant)
{
	struct mt6628_cmd_ch_privilege cmd = {};
	struct mt6628_event_ch_privilege event = {};
	size_t response_len;
	u8 rf_band;
	u8 token;
	int ret;

	if (!channel)
		return -EINVAL;

	/*
	 * A JOIN has to name the BSS it is joining.  The listen-class
	 * requests only need the channel, and the firmware treats a zero
	 * BSSID as "no particular BSS" for those.
	 */
	if (req_type == MT6628_CH_REQ_TYPE_JOIN) {
		if (!bssid || is_multicast_ether_addr(bssid) ||
		    is_zero_ether_addr(bssid))
			return -EINVAL;
	} else if (bssid && !is_multicast_ether_addr(bssid) &&
		   !is_zero_ether_addr(bssid)) {
		/* Unicast BSSID is accepted but unused for listen requests. */
		bssid = NULL;
	}

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
	if (req_type != MT6628_CH_REQ_TYPE_JOIN &&
	    req_type != MT6628_CH_REQ_TYPE_P2P_LISTEN)
		return -EINVAL;

	mutex_lock(&wl->channel_mutex);
	if (wl->channel_token) {
		mutex_unlock(&wl->channel_mutex);
		return -EBUSY;
	}

	token = ++wl->channel_token_next;
	if (!token)
		token = ++wl->channel_token_next;

	cmd.net_type_index = 0;
	cmd.token_id = token;
	cmd.action = MT6628_CMD_CH_ACTION_REQ;
	cmd.primary_channel = channel->hw_value;
	cmd.rf_sco = 0;
	cmd.rf_band = rf_band;
	cmd.req_type = req_type;
	cmd.max_interval = cpu_to_le32(MT6628_CH_MAX_INTERVAL_MS);
	if (bssid)
		ether_addr_copy(cmd.bssid, bssid);

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_CH_PRIVILEGE, 1,
				   &cmd, sizeof(cmd), &event, sizeof(event),
				   &response_len, MT6628_EVENT_ID_CH_PRIVILEGE, 2000);
	if (ret) {
		mutex_unlock(&wl->channel_mutex);
		return ret;
	}

	if (require_grant &&
	    (response_len != sizeof(event) ||
	     event.net_type_index != 0 || event.token_id != token ||
	     event.status != MT6628_EVENT_CH_STATUS_GRANT ||
	     event.primary_channel != channel->hw_value ||
	     event.rf_band != rf_band)) {
		mutex_unlock(&wl->channel_mutex);
		return -EPROTO;
	}

	wl->channel_token = token;
	wl->channel_req_type = req_type;
	if (require_grant && response_len >= sizeof(event))
		wl->channel_grant_ms = le32_to_cpu(event.grant_interval);
	mutex_unlock(&wl->channel_mutex);

	return 0;
}

static int mt6628_wlan_release_channel_locked(struct mt6628_wlan *wl,
					      bool check_type, u8 req_type)
{
	struct mt6628_cmd_ch_privilege cmd = {};
	u8 token = wl->channel_token;
	int ret;

	if (!token)
		return check_type ? -ENOENT : 0;
	if (check_type && wl->channel_req_type != req_type)
		return -ENOENT;

	cmd.net_type_index = 0;
	cmd.token_id = token;
	cmd.action = MT6628_CMD_CH_ACTION_ABORT;
	cmd.req_type = wl->channel_req_type;

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_CH_PRIVILEGE, 1,
				   &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);
	if (!ret)
		wl->channel_token = 0;

	return ret;
}

int mt6628_wlan_release_channel(struct mt6628_wlan *wl)
{
	int ret;

	mutex_lock(&wl->channel_mutex);
	ret = mt6628_wlan_release_channel_locked(wl, false, 0);
	mutex_unlock(&wl->channel_mutex);
	return ret;
}

int mt6628_wlan_release_channel_type(struct mt6628_wlan *wl, u8 req_type)
{
	int ret;

	mutex_lock(&wl->channel_mutex);
	ret = mt6628_wlan_release_channel_locked(wl, true, req_type);
	mutex_unlock(&wl->channel_mutex);
	return ret;
}

bool mt6628_wlan_channel_active(struct mt6628_wlan *wl, u8 req_type)
{
	bool active;

	mutex_lock(&wl->channel_mutex);
	active = wl->channel_token && wl->channel_req_type == req_type;
	mutex_unlock(&wl->channel_mutex);
	return active;
}

struct mt6628_peer_caps {
	bool has_ht;
	bool is_qos;
	bool uapsd;
	bool sup_mcs32;
	__le16 ht_cap_info;
	__le16 ht_extended_cap;
	__le32 tx_beamforming_cap;
	u8 mcs_set;
	u8 ampdu_param;
	u8 asel_cap;
};

/*
 * The STA-REC describes the peer AP, not our own advertised capabilities.
 * The downstream driver builds these fields from the AP's scan record and
 * association response before syncing STA_STATE_3 to firmware. Without the
 * QoS flag, firmware can activate the record but reject the AP's QoS Data
 * frames (including EAPOL).
 */
static void mt6628_wlan_parse_peer_caps(struct mt6628_wlan *wl,
					struct mt6628_peer_caps *caps)
{
	const u8 *ie = wl->conn_resp_ie;
	size_t len = wl->conn_resp_ie_len;

	memset(caps, 0, sizeof(*caps));

	while (len >= 2) {
		size_t ie_len = ie[1];

		if (ie_len + 2 > len)
			break;

		switch (ie[0]) {
		case WLAN_EID_HT_CAPABILITY:
			if (ie_len >= sizeof(struct ieee80211_ht_cap)) {
				caps->has_ht = true;
				caps->is_qos = true;
				caps->ht_cap_info =
					cpu_to_le16(get_unaligned_le16(ie + 2));
				caps->ampdu_param = ie[4];
				caps->mcs_set = ie[5];
				/* MCS 32 is bit 0 of the fifth RX MCS mask octet. */
				caps->sup_mcs32 = !!(ie[9] & BIT(0));
				caps->ht_extended_cap =
					cpu_to_le16(get_unaligned_le16(ie + 21));
				caps->tx_beamforming_cap =
					cpu_to_le32(get_unaligned_le32(ie + 23));
				caps->asel_cap = ie[27];
			}
			break;
		case WLAN_EID_VENDOR_SPECIFIC:
			/* WMM Information or Parameter IE, OUI 00:50:f2, type 2. */
			if (ie_len >= 7 && ie[2] == 0x00 && ie[3] == 0x50 &&
			    ie[4] == 0xf2 && ie[5] == 0x02 &&
			    (ie[6] == 0x00 || ie[6] == 0x01) && ie[7] == 0x01) {
				caps->is_qos = true;
				if (ie[8] & BIT(7))
					caps->uapsd = true;
			}
			break;
		default:
			break;
		}

		ie += ie_len + 2;
		len -= ie_len + 2;
	}

	/* U-APSD support is advertised by the AP's beacon/probe response. */
	if (wl->conn_bss) {
		const struct cfg80211_bss_ies *bss_ies;

		rcu_read_lock();
		bss_ies = rcu_dereference(wl->conn_bss->ies);
		if (bss_ies) {
			ie = bss_ies->data;
			len = bss_ies->len;
			while (len >= 2) {
				size_t ie_len = ie[1];

				if (ie_len + 2 > len)
					break;
				if (ie[0] == WLAN_EID_VENDOR_SPECIFIC && ie_len >= 7 &&
				    ie[2] == 0x00 && ie[3] == 0x50 && ie[4] == 0xf2 &&
				    ie[5] == 0x02 &&
				    (ie[6] == 0x00 || ie[6] == 0x01) && ie[7] == 0x01) {
					caps->uapsd |= !!(ie[8] & BIT(7));
					break;
				}
				ie += ie_len + 2;
				len -= ie_len + 2;
			}
		}
		rcu_read_unlock();
	}
}
 
int mt6628_wlan_update_sta_record(struct mt6628_wlan *wl,
				enum mt6628_sta_state state,
				u16 assoc_id, const u8 *bssid)
{
	struct mt6628_cmd_update_sta_record cmd = {};
	struct mt6628_event_activate_sta_rec event;
	struct mt6628_peer_caps caps;
	size_t response_len = 0;
	int ret;

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
	cmd.ht_cap_info = cpu_to_le16(MT6628_HT_CAP_SGI_20 |
					       MT6628_HT_CAP_SUP_WIDTH_20_40);
	if (state == MT6628_STA_STATE_3) {
		mt6628_wlan_parse_peer_caps(wl, &caps);
		cmd.is_qos = caps.is_qos;
		cmd.is_uapsd_supported = caps.uapsd;
		cmd.desired_phy_type_set = caps.has_ht ?
			(wl->conn_band == NL80211_BAND_2GHZ ?
			 MT6628_PHY_TYPE_SET_11BGN : MT6628_PHY_TYPE_SET_11AN) :
			(wl->conn_band == NL80211_BAND_2GHZ ?
			 MT6628_PHY_TYPE_SET_11BG : MT6628_PHY_TYPE_SET_11A);
		cmd.mcs_set = caps.mcs_set;
		cmd.sup_mcs32 = caps.sup_mcs32;
		cmd.ampdu_param = caps.ampdu_param;
		cmd.ht_cap_info = caps.ht_cap_info;
		cmd.ht_extended_cap = caps.ht_extended_cap;
		cmd.tx_beamforming_cap = caps.tx_beamforming_cap;
		cmd.asel_cap = caps.asel_cap;
	}
	/* STA_STATE_3 activates the record and must be acknowledged by firmware. */
	cmd.need_resp = state == MT6628_STA_STATE_3;
	if (!cmd.need_resp)
		return mt6628_wlan_send_cmd(wl,
					    MT6628_CMD_ID_UPDATE_STA_RECORD, 1,
					    &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_UPDATE_STA_RECORD, 1,
				   &cmd, sizeof(cmd), &event, sizeof(event),
				   &response_len,
				   MT6628_EVENT_ID_ACTIVATE_STA_REC, 1000);
	if (ret) {
		dev_err(&wl->func->dev,
			"STA-REC activation response failed: index=%u peer=%pM ret=%d\n",
			wl->sta_rec_idx, bssid, ret);
		return ret;
	}

	if (response_len != sizeof(event)) {
		dev_err(&wl->func->dev,
			"invalid STA-REC activation response length: %zu\n",
			response_len);
		return -EPROTO;
	}

	if (!ether_addr_equal(event.mac_addr, bssid) ||
	    event.sta_rec_idx != wl->sta_rec_idx ||
	    event.network_type_index != 0 ||
	    event.is_qos != cmd.is_qos || !event.is_ap) {
		dev_err(&wl->func->dev,
			"STA-REC activation mismatch: peer=%pM index=%u bss=%u qos=%u ap=%u; expected peer=%pM index=%u bss=0 qos=%u ap=1\n",
			event.mac_addr, event.sta_rec_idx, event.network_type_index,
			event.is_qos, event.is_ap,
			bssid, wl->sta_rec_idx, cmd.is_qos);
		return -EPROTO;
	}

	dev_info(&wl->func->dev,
		 "STA-REC activated: peer=%pM index=%u bss=%u qos=%u ap=%u\n",
		 event.mac_addr, event.sta_rec_idx, event.network_type_index,
		 event.is_qos, event.is_ap);
	return 0;
}

int mt6628_wlan_set_bss_info(struct mt6628_wlan *wl, u8 channel,
			     const u8 *ssid, u8 ssid_len,
			     const u8 *bssid, bool connected)
{
	struct mt6628_cmd_set_bss_info cmd = {};
	struct mt6628_peer_caps caps = {};

	if (wl->sta_rec_idx == MT6628_STA_REC_INDEX_NOT_FOUND ||
	    !ssid || !ssid_len || ssid_len > IEEE80211_MAX_SSID_LEN ||
	    !bssid || !channel)
		return -EINVAL;

	if (connected)
		mt6628_wlan_parse_peer_caps(wl, &caps);

	cmd.net_type_index = 0;
	cmd.connection_state = connected;
	cmd.current_op_mode = 0;
	cmd.is_qbss = connected && caps.is_qos;
	cmd.ssid_len = ssid_len;
	memcpy(cmd.ssid, ssid, ssid_len);
	ether_addr_copy(cmd.bssid, bssid);
	cmd.sta_rec_idx_of_ap = wl->sta_rec_idx;
	cmd.auth_mode = wl->conn_auth_mode;
	cmd.enc_status = wl->conn_enc_status;
	ether_addr_copy(cmd.own_mac, wl->netdev->dev_addr);

	cmd.rlm.net_type_index = 0;
	cmd.rlm.primary_channel = channel;
	cmd.rlm.rf_sco = wl->conn_rf_sco;
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
		cmd.phy_type_set = !connected || caps.has_ht ?
			MT6628_PHY_TYPE_SET_11BGN : MT6628_PHY_TYPE_SET_11BG;
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
		cmd.phy_type_set = !connected || caps.has_ht ?
			MT6628_PHY_TYPE_SET_11AN : MT6628_PHY_TYPE_SET_11A;
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

static int __mt6628_wlan_add_key(struct mt6628_wlan *wl, u8 key_index,
				 bool pairwise, bool tx_key,
				 const u8 *mac_addr,
				 const struct key_params *params)
{
	struct mt6628_cmd_add_remove_key cmd = {};
	const u8 *peer;
	u8 broadcast[ETH_ALEN];
	u8 response[4];
	size_t response_len;
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (key_index > MT6628_KEY_INDEX_MAX)
		return -EINVAL;
	if (!params)
		return -EOPNOTSUPP;
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
		eth_broadcast_addr(broadcast);
		peer = broadcast;
	}

	cmd.add_remove = 1;
	/*
	 * A station's GTK is RX-only. The PTK is the station's TX key
	 * unless cfg80211 explicitly requested a receive-only key.
	 */
	cmd.tx_key = tx_key;
	cmd.key_type = pairwise;
	cmd.is_authenticator = 0;
	ether_addr_copy(cmd.peer_addr, peer);
	cmd.net_type_index = 0;
	switch (params->cipher) {
	case WLAN_CIPHER_SUITE_CCMP:
		if (params->key_len != 16)
			return -EINVAL;
		cmd.algorithm_id = MT6628_CIPHER_SUITE_CCMP;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		if (params->key_len != 32)
			return -EINVAL;
		cmd.algorithm_id = MT6628_CIPHER_SUITE_TKIP;
		break;
	case WLAN_CIPHER_SUITE_WEP40:
		if (params->key_len != WLAN_KEY_LEN_WEP40)
			return -EINVAL;
		cmd.algorithm_id = MT6628_CIPHER_SUITE_WEP40;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		if (params->key_len != WLAN_KEY_LEN_WEP104)
			return -EINVAL;
		cmd.algorithm_id = MT6628_CIPHER_SUITE_WEP104;
		break;
	default:
		return -EOPNOTSUPP;
	}
	cmd.key_id = key_index;
	cmd.key_len = params->key_len;
	memcpy(cmd.key_material, params->key, params->key_len);
	if (params->seq_len)
		memcpy(cmd.key_rsc, params->seq, params->seq_len);

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_ADD_REMOVE_KEY, 1,
				   &cmd, sizeof(cmd), response, sizeof(response),
				   &response_len,
				   MT6628_EVENT_ID_CMD_RESULT, 1000);
	if (ret)
		return ret;

	/*
	 * Only remember the key once the firmware has accepted it, so that
	 * teardown never tries to remove something that was never installed.
	 */
	mutex_lock(&wl->cfg_mutex);
	if (pairwise)
		wl->pairwise_key_mask |= BIT(key_index);
	else {
		wl->group_key_mask |= BIT(key_index);

		/*
		 * add_key() is issued with tx_key=false for a group key.
		 * Cache only WEP material, which set_default_key() must replay
		 * with tx_key=true. A replacement invalidates any previous
		 * default-key bookkeeping for this index.
		 */
		if (wl->default_wep_key_valid &&
		    wl->default_wep_key == key_index)
			wl->default_wep_key_valid = false;

		if ((params->cipher == WLAN_CIPHER_SUITE_WEP40 &&
		     params->key_len == 5) ||
		    (params->cipher == WLAN_CIPHER_SUITE_WEP104 &&
		     params->key_len == 13)) {
			struct mt6628_wep_key *wep = &wl->wep_keys[key_index];

			memzero_explicit(wep, sizeof(*wep));
			wep->valid = true;
			wep->cipher = params->cipher;
			wep->key_len = params->key_len;
			memcpy(wep->key, params->key, params->key_len);
		} else {
			memzero_explicit(&wl->wep_keys[key_index],
					 sizeof(wl->wep_keys[key_index]));
		}
	}
	mutex_unlock(&wl->cfg_mutex);

	return 0;
}

int mt6628_wlan_add_key(struct mt6628_wlan *wl, u8 key_index,
			bool pairwise, bool tx_key, const u8 *mac_addr,
			const struct key_params *params)
{
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;

	mutex_lock(&wl->key_mutex);
	mutex_lock(&wl->cfg_mutex);
	if (wl->conn_cleanup_pending)
		ret = -EBUSY;
	else if (!wl->conn_secure)
		ret = -ENOTCONN;
	else
		ret = 0;
	mutex_unlock(&wl->cfg_mutex);

	if (!ret)
		ret = __mt6628_wlan_add_key(wl, key_index, pairwise, tx_key,
					    mac_addr, params);

	mutex_unlock(&wl->key_mutex);
	return ret;
}

static int
mt6628_wlan_program_wep_key(struct mt6628_wlan *wl, u8 key_index,
			    const struct mt6628_wep_key *key, bool tx_key)
{
	struct mt6628_cmd_add_remove_key cmd = {};
	u8 response[4];
	size_t response_len;

	if (!key->valid || !key->key_len ||
	    key->key_len > sizeof(key->key))
		return -EINVAL;

	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		if (key->key_len != 5)
			return -EINVAL;
		cmd.algorithm_id = MT6628_CIPHER_SUITE_WEP40;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		if (key->key_len != 13)
			return -EINVAL;
		cmd.algorithm_id = MT6628_CIPHER_SUITE_WEP104;
		break;
	default:
		return -EOPNOTSUPP;
	}

	cmd.add_remove = 1;
	cmd.tx_key = tx_key;
	cmd.key_type = 0;
	cmd.is_authenticator = 0;
	eth_broadcast_addr(cmd.peer_addr);
	cmd.net_type_index = 0;
	cmd.key_id = key_index;
	cmd.key_len = key->key_len;
	memcpy(cmd.key_material, key->key, key->key_len);

	return mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_ADD_REMOVE_KEY, 1,
				    &cmd, sizeof(cmd), response,
				    sizeof(response), &response_len,
				    MT6628_EVENT_ID_CMD_RESULT, 1000);
}

static int __mt6628_wlan_set_default_key(struct mt6628_wlan *wl,
					 u8 key_index, bool unicast,
					 bool multicast)
{
	struct mt6628_wep_key target = {}, previous = {};
	u8 previous_index = 0;
	bool previous_valid = false;
	int ret, restore_ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (key_index > MT6628_KEY_INDEX_MAX)
		return -EINVAL;
	if (!unicast && !multicast)
		return -EINVAL;

	/*
	 * The MT6628 key command has a transmit-key selector but no separate
	 * unicast/multicast default-key selectors. Only installed WEP group
	 * keys can therefore be selected through this callback.
	 */
	mutex_lock(&wl->cfg_mutex);
	if (!(wl->group_key_mask & BIT(key_index)) ||
	    !wl->wep_keys[key_index].valid) {
		mutex_unlock(&wl->cfg_mutex);
		return -EOPNOTSUPP;
	}

	target = wl->wep_keys[key_index];
	if (wl->default_wep_key_valid &&
	    wl->default_wep_key != key_index &&
	    wl->default_wep_key <= MT6628_KEY_INDEX_MAX &&
	    wl->wep_keys[wl->default_wep_key].valid) {
		previous_index = wl->default_wep_key;
		previous = wl->wep_keys[previous_index];
		previous_valid = true;
	}
	mutex_unlock(&wl->cfg_mutex);

	/* Stop using the old WEP key for transmission before selecting the new one. */
	if (previous_valid) {
		ret = mt6628_wlan_program_wep_key(wl, previous_index,
						  &previous, false);
		if (ret)
			goto out;
	}

	ret = mt6628_wlan_program_wep_key(wl, key_index, &target, true);
	if (ret) {
		/*
		 * Roll back the previous default if the new key could not be
		 * selected. If rollback also fails, stop claiming that a
		 * particular default key is active.
		 */
		if (previous_valid) {
			restore_ret = mt6628_wlan_program_wep_key(
				wl, previous_index, &previous, true);
			if (restore_ret) {
				mutex_lock(&wl->cfg_mutex);
				wl->default_wep_key_valid = false;
				mutex_unlock(&wl->cfg_mutex);
				dev_warn(&wl->func->dev,
					 "failed to restore default WEP key: %d\n",
					 restore_ret);
			}
		}
		goto out;
	}

	mutex_lock(&wl->cfg_mutex);
	wl->default_wep_key = key_index;
	wl->default_wep_key_valid = true;
	mutex_unlock(&wl->cfg_mutex);

out:
	memzero_explicit(&target, sizeof(target));
	memzero_explicit(&previous, sizeof(previous));
	return ret;
}

int mt6628_wlan_set_default_key(struct mt6628_wlan *wl, u8 key_index,
				bool unicast, bool multicast)
{
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;

	mutex_lock(&wl->key_mutex);
	mutex_lock(&wl->cfg_mutex);
	if (wl->conn_cleanup_pending) {
		mutex_unlock(&wl->cfg_mutex);
		mutex_unlock(&wl->key_mutex);
		return -EBUSY;
	}
	mutex_unlock(&wl->cfg_mutex);

	ret = __mt6628_wlan_set_default_key(wl, key_index,
					    unicast, multicast);
	mutex_unlock(&wl->key_mutex);
	return ret;
}

static int __mt6628_wlan_del_key(struct mt6628_wlan *wl, u8 key_index,
				 bool pairwise, const u8 *mac_addr)
{
	struct mt6628_cmd_add_remove_key cmd = {};
	const u8 *peer;
	u8 broadcast[ETH_ALEN];
	u8 response[4];
	size_t response_len;
	int ret;

	/*
	 * Key deletion is normally part of disconnect teardown.  It is still
	 * allowed once the connection has been marked disconnected, because
	 * teardown removes the keys after dropping that state.
	 */
	if (!wl->runtime_started || !wl->fw_running)
		return 0;
	if (key_index > MT6628_KEY_INDEX_MAX)
		return -EINVAL;

	if (pairwise) {
		if (!mac_addr || is_zero_ether_addr(mac_addr) ||
		    is_multicast_ether_addr(mac_addr))
			return -EINVAL;
		peer = mac_addr;
	} else {
		eth_broadcast_addr(broadcast);
		peer = broadcast;
	}

	cmd.add_remove = 0;
	cmd.is_authenticator = 0;
	ether_addr_copy(cmd.peer_addr, peer);
	cmd.net_type_index = 0;
	cmd.key_id = key_index;

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_ADD_REMOVE_KEY, 1,
				   &cmd, sizeof(cmd), response, sizeof(response),
				   &response_len,
				   MT6628_EVENT_ID_CMD_RESULT, 1000);
	if (!ret) {
		mutex_lock(&wl->cfg_mutex);
		if (pairwise)
			wl->pairwise_key_mask &= ~BIT(key_index);
		else {
			wl->group_key_mask &= ~BIT(key_index);
			memzero_explicit(&wl->wep_keys[key_index],
					 sizeof(wl->wep_keys[key_index]));
			if (wl->default_wep_key_valid &&
			    wl->default_wep_key == key_index)
				wl->default_wep_key_valid = false;
		}
		mutex_unlock(&wl->cfg_mutex);
	}

	return ret;
}

int mt6628_wlan_del_key(struct mt6628_wlan *wl, u8 key_index,
			bool pairwise, const u8 *mac_addr)
{
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return 0;

	mutex_lock(&wl->key_mutex);
	mutex_lock(&wl->cfg_mutex);
	if (wl->conn_cleanup_pending) {
		mutex_unlock(&wl->cfg_mutex);
		mutex_unlock(&wl->key_mutex);
		return -EBUSY;
	}
	mutex_unlock(&wl->cfg_mutex);

	ret = __mt6628_wlan_del_key(wl, key_index, pairwise, mac_addr);
	mutex_unlock(&wl->key_mutex);
	return ret;
}

void mt6628_wlan_flush_keys(struct mt6628_wlan *wl)
{
	u8 pairwise_mask;
	u8 group_mask;
	u8 pairwise_bssid[ETH_ALEN];
	u8 broadcast[ETH_ALEN];
	u8 i;

	/*
	 * Take and clear the masks in one step.  add_key()/del_key() can run
	 * concurrently with the teardown that flushes them, and reading then
	 * clearing separately would drop a key that was installed in
	 * between, leaving it installed in the firmware with nothing left to
	 * remove it later.
	 */
	mutex_lock(&wl->key_mutex);
	mutex_lock(&wl->cfg_mutex);
	pairwise_mask = wl->pairwise_key_mask;
	group_mask = wl->group_key_mask;
	ether_addr_copy(pairwise_bssid, wl->conn_bssid);
	wl->pairwise_key_mask = 0;
	wl->group_key_mask = 0;
	for (i = 0; i <= MT6628_KEY_INDEX_MAX; i++)
		memzero_explicit(&wl->wep_keys[i], sizeof(wl->wep_keys[i]));
	wl->default_wep_key_valid = false;
	mutex_unlock(&wl->cfg_mutex);

	/*
	 * Nothing to undo, or the firmware is already gone: either way the
	 * masks above are cleared, because the keys cannot outlive the
	 * connection they belonged to.
	 */
	if (!pairwise_mask && !group_mask)
		goto out_unlock;
	if (!wl->runtime_started || !wl->fw_running)
		goto out_unlock;

	eth_broadcast_addr(broadcast);

	for (i = 0; i <= MT6628_KEY_INDEX_MAX; i++) {
		if (pairwise_mask & BIT(i))
			__mt6628_wlan_del_key(wl, i, true, pairwise_bssid);
		if (group_mask & BIT(i))
			__mt6628_wlan_del_key(wl, i, false, broadcast);
	}

out_unlock:
	mutex_unlock(&wl->key_mutex);
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

int mt6628_wlan_get_sta_statistics(struct mt6628_wlan *wl,
				   struct mt6628_event_sta_statistics *stats)
{
	struct mt6628_cmd_get_sta_statistics cmd = {};
	size_t response_len;
	u8 sta_rec_idx;
	u8 bssid[ETH_ALEN];
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;

	/*
	 * Both the STA-REC index and the peer address are connection state,
	 * so take a consistent snapshot: a concurrent disconnect could
	 * otherwise clear the index while we are still building the command.
	 */
	mutex_lock(&wl->cfg_mutex);
	if (wl->sta_rec_idx == MT6628_STA_REC_INDEX_NOT_FOUND) {
		mutex_unlock(&wl->cfg_mutex);
		return -ENOLINK;
	}
	sta_rec_idx = wl->sta_rec_idx;
	ether_addr_copy(bssid, wl->conn_bssid);
	mutex_unlock(&wl->cfg_mutex);

	cmd.index = sta_rec_idx;
	cmd.flags = 0;
	/*
	 * Do not ask the firmware to clear the counters as it reads them.
	 * The downstream driver sets this for its SIOCSIWSTASTATS ioctl,
	 * which is a deliberate read-and-reset.  get_station() is a passive
	 * query that user space may call repeatedly, and clearing on every
	 * call would destroy the counters just because someone looked at
	 * them.
	 */
	cmd.read_clear = 0;
	ether_addr_copy(cmd.mac_addr, bssid);

	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_GET_STA_STATISTICS, 0,
				   &cmd, sizeof(cmd), stats, sizeof(*stats),
				   &response_len,
				   MT6628_EVENT_ID_STA_STATISTICS, 1000);
	if (ret)
		return ret;

	if (response_len < sizeof(*stats))
		return -EPROTO;

	/* Bit 0 tells us whether the firmware filled the counters in. */
	if (!(le32_to_cpu(stats->flags) & 1))
		return -ENODATA;

	return 0;
}

/*
 * Consume EVENT_ID_RX_ADDBA and EVENT_ID_RX_DELBA.
 *
 * The firmware raises these when a peer opens or tears down a Block Ack
 * session on one of our TIDs.  Downstream turns them into per-station,
 * per-TID reorder-queue state (qmAddRxBaEntry()/qmDelRxBaEntry(),
 * nic/que_mgt.c:3468/3550), and the only reader of that state is the host
 * receive reorder path (qmProcessPktWithReordering(), que_mgt.c:2795).
 *
 * That reader does not exist in this port, and cannot be copied blindly:
 * the HIF header field that selects a packet for reordering is parsed inside
 * an "#if 0" block in the downstream driver itself (nic_rx.c:1088-1115), and
 * cfg80211 has no use for a reorder window - the frames arrive already
 * reassembled from the SDIO descriptor queue.  Keeping a window here would be
 * a table nothing ever consults, so this driver decodes the events and
 * reports them instead.  See Downstream-Gaps.md section 2.4.
 *
 * "body" is the event payload after the eight-byte event header, which the
 * dispatcher has already validated; "body_len" its length.  The caller owns
 * the skb.  Returns false only when the event is not one of the two, in
 * which case the caller must leave it alone.
 */
bool mt6628_wlan_handle_rx_ba_event(struct mt6628_wlan *wl, u8 eid,
				    const void *body, size_t body_len)
{
	u8 tid, sta_rec_idx;
	u16 win_size, win_start, param;

	/* Not one of ours: leave the event to the caller's own dispatch. */
	if (eid != MT6628_EVENT_ID_RX_ADDBA && eid != MT6628_EVENT_ID_RX_DELBA)
		return false;

	/*
	 * Count before validating the payload: these are unsolicited, so the
	 * number the firmware raised them is worth keeping even when what
	 * follows turns out to be unusable.
	 *
	 * No lock is taken.  Events are dispatched one at a time from the
	 * single event_dispatch_work item, so this cannot race another event, and the
	 * counters are only ever incremented from that one context.  The
	 * counters saturate rather than wrap, so a long-lived counter cannot
	 * roll back to zero and read as "never happened".
	 */
	if (eid == MT6628_EVENT_ID_RX_ADDBA) {
		if (wl->rx_addba_events != U32_MAX)
			wl->rx_addba_events++;
	} else {
		if (wl->rx_delba_events != U32_MAX)
			wl->rx_delba_events++;
	}

	/*
	 * The firmware sizes these bodies exactly, so a mismatch means the
	 * event is not the one this id names.  Reject on inequality rather
	 * than on a minimum: the structures carry no trailing variable part,
	 * and accepting a longer body would mean reading a field that is not
	 * part of this event.
	 */
	if (eid == MT6628_EVENT_ID_RX_ADDBA) {
		const struct mt6628_event_rx_addba *addba = body;

		if (body_len != sizeof(*addba))
			return true;

		sta_rec_idx = addba->sta_rec_idx;
		param = le16_to_cpu(addba->ba_parameter_set);
		tid = (param & MT6628_BA_PARAM_SET_TID_MASK) >>
			MT6628_BA_PARAM_SET_TID_OFFSET;
		win_size = (param & MT6628_BA_PARAM_SET_WIN_SIZE_MASK) >>
			MT6628_BA_PARAM_SET_WIN_SIZE_OFFSET;
		win_start = le16_to_cpu(addba->ba_start_seq_ctrl) >>
			    MT6628_BAR_SSC_SN_OFFSET;

		/*
		 * Reject an out-of-range station index and discard the event,
		 * exactly as the downstream lookup does (que_mgt.c:3363-3369):
		 * the index selects a STA_REC that does not exist here.  This
		 * test also rejects the two reserved values 0xfe and 0xff,
		 * which are above the table bound.
		 */
		if (sta_rec_idx >= MT6628_STA_REC_NUM)
			return true;

		dev_dbg(&wl->func->dev,
			"RX BA agreement from STA %u: tid %u, window %u, start %u, timeout %u, dialog %u\n",
			sta_rec_idx, tid, win_size, win_start,
			le16_to_cpu(addba->ba_timeout_value),
			addba->dialog_token);
		return true;
	}

	if (eid == MT6628_EVENT_ID_RX_DELBA) {
		const struct mt6628_event_rx_delba *delba = body;

		if (body_len != sizeof(*delba))
			return true;

		sta_rec_idx = delba->sta_rec_idx;
		tid = delba->tid;

		if (sta_rec_idx >= MT6628_STA_REC_NUM)
			return true;

		dev_dbg(&wl->func->dev,
			"RX BA agreement with STA %u torn down: tid %u\n",
			sta_rec_idx, tid);
		return true;
	}

	return false;
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

int mt6628_wlan_mgmt_tx_status(struct mt6628_wlan *wl, const u8 *frame,
			       size_t frame_len, bool wait_for_status,
			       bool need_ack, bool *submitted)
{
	struct mt6628_hif_mgmt_tx_hdr hdr = {};
	unsigned long flags;
	size_t packet_len, xfer_len;
	u8 *buf;
	u8 packet_seq = 0;
	long timeout;
	int ret;

	if (submitted)
		*submitted = false;

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

	if (wait_for_status) {
		spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
		if (wl->mgmt_tx_pending) {
			spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
			ret = -EBUSY;
			goto err_resource;
		}

		packet_seq = ++wl->mgmt_tx_seq;
		if (!packet_seq)
			packet_seq = ++wl->mgmt_tx_seq;

		reinit_completion(&wl->mgmt_tx_done);
		wl->mgmt_tx_packet_seq = packet_seq;
		wl->mgmt_tx_status = -ETIMEDOUT;
		wl->mgmt_tx_pending = true;
		spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
	}

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
	hdr.packet_seq_no = packet_seq;
	hdr.ack_bip_basic_rate = MT6628_HIF_TX_BASIC_RATE;
	if (need_ack)
		hdr.ack_bip_basic_rate |= MT6628_HIF_TX_NEED_ACK;

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), frame, frame_len);

	/* Bulk transfer: reclaim Driver Own, the 32-bit helpers are bypassed. */
	mutex_lock(&wl->cmd_mutex);
	ret = mt6628_wlan_pm_busy(wl);
	if (ret) {
		mutex_unlock(&wl->cmd_mutex);
		/*
		 * Nothing was written, so no EVENT_ID_TX_DONE will arrive.
		 * Retire the wait the same way the transfer-failure path
		 * below does; leaving mgmt_tx_pending set would make every
		 * later mgmt_tx with wait_for_status refuse forever.
		 */
		if (wait_for_status) {
			spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
			if (wl->mgmt_tx_pending &&
			    wl->mgmt_tx_packet_seq == packet_seq) {
				wl->mgmt_tx_status = ret;
				wl->mgmt_tx_pending = false;
				complete(&wl->mgmt_tx_done);
			}
			spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
		}
		kfree(buf);
		goto err_resource;
	}

	sdio_claim_host(wl->func);
	ret = sdio_writesb(wl->func, MT6628_MCR_WTDR1, buf, xfer_len);
	sdio_release_host(wl->func);
	mutex_unlock(&wl->cmd_mutex);
	kfree(buf);
	if (ret) {
		if (wait_for_status) {
			spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
			if (wl->mgmt_tx_pending &&
			    wl->mgmt_tx_packet_seq == packet_seq) {
				wl->mgmt_tx_status = ret;
				wl->mgmt_tx_pending = false;
				complete(&wl->mgmt_tx_done);
			}
			spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
		}
		goto err_resource;
	}

	if (submitted)
		*submitted = true;

	if (wait_for_status) {
		timeout = wait_for_completion_timeout(&wl->mgmt_tx_done,
							      msecs_to_jiffies(1000));

		spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
		if (!timeout && wl->mgmt_tx_pending &&
		    wl->mgmt_tx_packet_seq == packet_seq) {
			wl->mgmt_tx_status = -ETIMEDOUT;
			wl->mgmt_tx_pending = false;
		}
		ret = wl->mgmt_tx_status;
		spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
		if (!timeout && ret == -ETIMEDOUT)
			dev_warn(&wl->func->dev,
				 "management TX timed out: packet_seq=%u sta_rec=%u need_ack=%u\n",
				 (unsigned int)packet_seq, (unsigned int)wl->sta_rec_idx,
				 (unsigned int)need_ack);
		else if (ret)
			dev_warn(&wl->func->dev,
				 "management TX failed: packet_seq=%u status=%d need_ack=%u\n",
				 (unsigned int)packet_seq, ret, (unsigned int)need_ack);

		/*
		 * Do not restore the TC4 resource here.  A timed-out frame
		 * is still in the firmware TX path and its resource is
		 * released later by WTSR0/WTSR1.
		 */
		if (ret)
			return ret;
	}

	return 0;

err_resource:
	spin_lock_irqsave(&wl->tx_lock, flags);
	wl->tx_free[MT6628_TX_TC_MGMT] = min_t(unsigned int,
					       wl->tx_free[MT6628_TX_TC_MGMT] + 1,
					       wl->tx_max[MT6628_TX_TC_MGMT]);
	spin_unlock_irqrestore(&wl->tx_lock, flags);
	/* A pre-submit allocation error must not leave the status slot busy. */
	if (wait_for_status && packet_seq) {
		spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
		if (wl->mgmt_tx_pending &&
		    wl->mgmt_tx_packet_seq == packet_seq) {
			wl->mgmt_tx_status = ret;
			wl->mgmt_tx_pending = false;
		}
		spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
	}
	wake_up_all(&wl->tx_wait);
	return ret;
}

int mt6628_wlan_mgmt_tx(struct mt6628_wlan *wl, const u8 *frame,
			 size_t frame_len, bool wait_for_status, bool need_ack)
{
	return mt6628_wlan_mgmt_tx_status(wl, frame, frame_len,
					  wait_for_status, need_ack, NULL);
}

EXPORT_SYMBOL_GPL(mt6628_wlan_request_channel);
EXPORT_SYMBOL_GPL(mt6628_wlan_ch_privilege);
EXPORT_SYMBOL_GPL(mt6628_wlan_release_channel);
EXPORT_SYMBOL_GPL(mt6628_wlan_release_channel_type);
EXPORT_SYMBOL_GPL(mt6628_wlan_channel_active);
EXPORT_SYMBOL_GPL(mt6628_wlan_update_sta_record);
EXPORT_SYMBOL_GPL(mt6628_wlan_set_bss_info);
EXPORT_SYMBOL_GPL(mt6628_wlan_activate_bss);
EXPORT_SYMBOL_GPL(mt6628_wlan_remove_sta_record);
EXPORT_SYMBOL_GPL(mt6628_wlan_add_key);
EXPORT_SYMBOL_GPL(mt6628_wlan_del_key);
EXPORT_SYMBOL_GPL(mt6628_wlan_flush_keys);
EXPORT_SYMBOL_GPL(mt6628_wlan_set_default_key);
EXPORT_SYMBOL_GPL(mt6628_wlan_set_power_mgmt);
EXPORT_SYMBOL_GPL(mt6628_wlan_mgmt_tx_status);
EXPORT_SYMBOL_GPL(mt6628_wlan_mgmt_tx);
