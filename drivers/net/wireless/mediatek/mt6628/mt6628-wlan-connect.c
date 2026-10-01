// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 cfg80211 open-system connection state machine
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "mtk-wlan-hif.h"
#include "mtk-wlan.h"

#define MT6628_CONNECT_TIMEOUT_MS       5000

#define MT6628_CONNECT_MAX_IE_LEN       600

/* Bounded retries so an unavailable AP cannot leave us looping. */
#define MT6628_CONN_RETRY_MAX	5
#define MT6628_CONN_RETRY_DELAY_MS	2000

#define MT6628_ASSOC_CAPABILITY         (WLAN_CAPABILITY_ESS | \
					     WLAN_CAPABILITY_SHORT_PREAMBLE | \
					     WLAN_CAPABILITY_SHORT_SLOT_TIME)
#define MT6628_ASSOC_LISTEN_INTERVAL    10

static const u8 mt6628_supported_rates[] = {
	0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
};

static const u8 mt6628_extended_rates[] = {
	0x30, 0x48, 0x60, 0x6c,
};

/*
 * 5 GHz OFDM rates. 6/12/24 Mbps are basic rates, matching
 * BASIC_RATE_SET_OFDM in the MT6628 downstream driver.
 */
static const u8 mt6628_assoc_5ghz_rates[] = {
	0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
};

static int mt6628_send_deauth_to(struct mt6628_wlan *wl, const u8 *da,
				 const u8 *sa, const u8 *bssid, u16 reason);

static bool mt6628_deauth_rate_limit(struct mt6628_wlan *wl, const u8 *da);
static void mt6628_conn_retry_work(struct work_struct *work);
static void mt6628_conn_clear_retry(struct mt6628_wlan *wl);
int mt6628_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
			       struct cfg80211_connect_params *sme);

static void mt6628_conn_free_ies(struct mt6628_wlan *wl)
{
	kfree(wl->conn_req_ie);
	wl->conn_req_ie = NULL;
	wl->conn_req_ie_len = 0;
	kfree(wl->conn_resp_ie);
	wl->conn_resp_ie = NULL;
	wl->conn_resp_ie_len = 0;
	kfree(wl->conn_key);
	wl->conn_key = NULL;
	wl->conn_key_len = 0;
}

static void mt6628_conn_put_bss(struct mt6628_wlan *wl)
{
	if (!wl->conn_bss)
		return;

	cfg80211_put_bss(wl->wiphy, wl->conn_bss);
	wl->conn_bss = NULL;
}

static void mt6628_conn_fw_cleanup(struct mt6628_wlan *wl)
{
	int ret;

	if (!wl->runtime_started || !wl->fw_running || !wl->netdev)
		return;

	/* Keys are indexed by peer, so drop them before the STA-REC goes. */
	mt6628_wlan_flush_keys(wl);

	if (wl->sta_rec_idx != MT6628_STA_REC_INDEX_NOT_FOUND) {
		ret = mt6628_wlan_activate_bss(wl, false);
		if (ret)
			dev_warn(&wl->func->dev,
				 "failed to deactivate BSS during cleanup: %d\n",
				 ret);

		ret = mt6628_wlan_remove_sta_record(wl, wl->conn_bssid);
		if (ret) {
			dev_warn(&wl->func->dev,
				 "failed to remove STA-REC %u: %d\n",
				 wl->sta_rec_idx, ret);
		} else {
			wl->sta_rec_idx = MT6628_STA_REC_INDEX_NOT_FOUND;
		}
	}

	/*
	 * The channel privilege may outlive STA-REC setup failures, so
	 * release the JOIN grant.  A listen-class grant is owned by
	 * remain-on-channel and must be left alone here.
	 */
	if (wl->channel_req_type == MT6628_CH_REQ_TYPE_JOIN) {
		ret = mt6628_wlan_release_channel(wl);
		if (ret)
			dev_warn(&wl->func->dev,
				 "failed to release channel privilege: %d\n",
				 ret);
	}
}

static void mt6628_conn_set_disconnected(struct mt6628_wlan *wl)
{
	wl->conn_state = MT6628_CONN_DISCONNECTED;
	wl->connected = false;
	wl->conn_secure = false;
	wl->conn_aid = 0;
	wl->conn_auth_mode = MT6628_AUTH_MODE_OPEN;
	wl->conn_enc_status = MT6628_ENCRYPTION_DISABLED;
	wl->conn_key_idx = 0;
	wl->conn_rf_sco = 0;
	if (wl->netdev)
		netif_carrier_off(wl->netdev);
}

/*
 * The firmware received a frame it considers illegal while we were not
 * associated, and asks the host to answer it with a deauthentication.
 * The event body is a verbatim copy of that frame's MAC header, so the
 * addresses have to be echoed back the way nicRx.c/authSendDeauthFrame()
 * does it in the downstream driver: the deauth goes back to the sender
 * of the offending frame (aucAddr2), sourced from the address the
 * offending frame was addressed to (aucAddr1).
 *
 * This is a reply, not a teardown: the association stays up.
 */
void mt6628_cfg80211_fw_send_deauth(struct mt6628_wlan *wl,
				    __le16 frame_ctrl, const u8 *addr1,
				    const u8 *addr2)
{
	u8 da[ETH_ALEN], sa[ETH_ALEN], bssid[ETH_ALEN];
	u16 fc;

	/*
	 * Only answer frames that came through the AP. Downstream bails out
	 * for anything with both DS bits clear, since then there is no BSS
	 * that could legitimately own the frame.
	 */
	fc = le16_to_cpu(frame_ctrl);
	if (!(fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)))
		return;
	if (fc & IEEE80211_FCTL_TODS)	/* ToDS: BSSID is aucAddr1 */
		ether_addr_copy(bssid, addr1);
	else				/* otherwise: BSSID is aucAddr2 */
		ether_addr_copy(bssid, addr2);

	ether_addr_copy(da, addr2);
	ether_addr_copy(sa, addr1);

	mutex_lock(&wl->cfg_mutex);
	if (wl->conn_state != MT6628_CONN_CONNECTED ||
	    !ether_addr_equal(wl->conn_bssid, bssid)) {
		mutex_unlock(&wl->cfg_mutex);
		return;
	}
	mutex_unlock(&wl->cfg_mutex);

	/*
	 * Rate limit replies per peer: the firmware raises this event for
	 * every offending frame, and a peer stuck in the offending state
	 * would otherwise make us emit a deauth per received frame.
	 */
	if (!mt6628_deauth_rate_limit(wl, da))
		return;

	mt6628_send_deauth_to(wl, da, sa, bssid,
			      WLAN_REASON_CLASS3_FRAME_FROM_NONASSOC_STA);
}

void mt6628_cfg80211_fw_beacon_timeout(struct mt6628_wlan *wl)
{
	bool connected;

	mutex_lock(&wl->cfg_mutex);
	connected = wl->conn_state == MT6628_CONN_CONNECTED;
	if (connected)
		mt6628_conn_set_disconnected(wl);
	mutex_unlock(&wl->cfg_mutex);

	if (!connected)
		return;

	/*
	 * The firmware has already declared beacon loss.  Do not transmit
	 * a deauthentication frame here: the firmware event is precisely
	 * the indication that the AP is no longer reachable.
	 */
	mt6628_conn_fw_cleanup(wl);
	cfg80211_disconnected(wl->netdev, WLAN_REASON_UNSPECIFIED,
			      NULL, 0, false, GFP_KERNEL);

	mutex_lock(&wl->cfg_mutex);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
	mutex_unlock(&wl->cfg_mutex);
}

static bool mt6628_connect_is_wpa_psk(
	const struct cfg80211_connect_params *sme,
	u8 *auth_mode, u8 *enc_status)
{
	const struct cfg80211_crypto_settings *crypto = &sme->crypto;

	if (!crypto->wpa_versions ||
	    crypto->wpa_versions & ~(NL80211_WPA_VERSION_1 |
				     NL80211_WPA_VERSION_2))
		return false;
	if (crypto->cipher_group != WLAN_CIPHER_SUITE_CCMP &&
	    crypto->cipher_group != WLAN_CIPHER_SUITE_TKIP)
		return false;
	if (crypto->n_ciphers_pairwise != 1 ||
	    (crypto->ciphers_pairwise[0] != WLAN_CIPHER_SUITE_CCMP &&
	     crypto->ciphers_pairwise[0] != WLAN_CIPHER_SUITE_TKIP))
		return false;
	if (crypto->n_akm_suites != 1 ||
	    crypto->akm_suites[0] != WLAN_AKM_SUITE_PSK)
		return false;
	if (crypto->control_port || crypto->control_port_over_nl80211)
		return false;
	if (sme->mfp != NL80211_MFP_NO)
		return false;
	if (sme->key_len || sme->key)
		return false;

	if (crypto->wpa_versions & NL80211_WPA_VERSION_2) {
		*auth_mode = MT6628_AUTH_MODE_WPA2_PSK;
		*enc_status = MT6628_ENCRYPTION3_KEY_ABSENT;
	} else {
		*auth_mode = MT6628_AUTH_MODE_WPA_PSK;
		*enc_status = MT6628_ENCRYPTION2_ENABLED;
	}

	return true;
}

static bool mt6628_connect_is_wep(
	const struct cfg80211_connect_params *sme,
	u8 *auth_mode, u8 *enc_status)
{
	const struct cfg80211_crypto_settings *crypto = &sme->crypto;

	if (!sme->privacy || !sme->key)
		return false;
	if (crypto->wpa_versions ||
	    crypto->n_ciphers_pairwise ||
	    crypto->n_akm_suites ||
	    crypto->control_port ||
	    crypto->control_port_over_nl80211)
		return false;
	if (crypto->cipher_group != 0 &&
	    crypto->cipher_group != WLAN_CIPHER_SUITE_WEP40 &&
	    crypto->cipher_group != WLAN_CIPHER_SUITE_WEP104)
		return false;
	if (sme->mfp != NL80211_MFP_NO)
		return false;
	if (sme->key_len != WLAN_KEY_LEN_WEP40 &&
	    sme->key_len != WLAN_KEY_LEN_WEP104)
		return false;

	switch (sme->auth_type) {
	case NL80211_AUTHTYPE_SHARED_KEY:
		*auth_mode = MT6628_AUTH_MODE_SHARED;
		break;
	case NL80211_AUTHTYPE_AUTOMATIC:
		*auth_mode = MT6628_AUTH_MODE_AUTO_SWITCH;
		break;
	case NL80211_AUTHTYPE_OPEN_SYSTEM:
		*auth_mode = MT6628_AUTH_MODE_OPEN;
		break;
	default:
		return false;
	}

	*enc_status = MT6628_ENCRYPTION_WEP_ENABLED;
	return true;
}

static bool mt6628_assoc_has_ie(const u8 *ies, size_t len, u8 eid)
{
	while (len >= 2) {
		size_t ie_len = ies[1];

		if (ie_len + 2 > len)
			return false;
		if (ies[0] == eid)
			return true;
		ies += ie_len + 2;
		len -= ie_len + 2;
	}

	return false;
}

static const struct ieee80211_ht_cap mt6628_assoc_ht_cap = {
	.cap_info = cpu_to_le16(IEEE80211_HT_CAP_SGI_20 |
				IEEE80211_HT_CAP_SUP_WIDTH_20_40),
	.ampdu_params_info = IEEE80211_HT_MAX_AMPDU_64K,
	.mcs = {
		.rx_mask = { 0xff },
		.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	},
};

static u8 mt6628_assoc_ht_sco(const u8 *ies, size_t len)
{
	while (len >= 2) {
		size_t ie_len = ies[1];

		if (ie_len + 2 > len)
			return 0;

		if (ies[0] == WLAN_EID_HT_OPERATION) {
			u8 sco;

			if (ie_len < 2)
				return 0;

			sco = ies[3] & 0x3;
			if (sco == 1 || sco == 3)
				return sco;
			return 0;
		}

		ies += ie_len + 2;
		len -= ie_len + 2;
	}

	return 0;
}

static int mt6628_build_assoc_ies(struct mt6628_wlan *wl,
				   const struct cfg80211_connect_params *sme)
{
	size_t len = 0;
	const u8 *supported_rates;
	size_t supported_rates_len;
	bool use_extended_rates;
	void *p;

	switch (wl->conn_band) {
	case NL80211_BAND_2GHZ:
		supported_rates = mt6628_supported_rates;
		supported_rates_len = sizeof(mt6628_supported_rates);
		use_extended_rates = true;
		break;
	case NL80211_BAND_5GHZ:
		supported_rates = mt6628_assoc_5ghz_rates;
		supported_rates_len = sizeof(mt6628_assoc_5ghz_rates);
		use_extended_rates = false;
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (sme->ie_len > MT6628_CONNECT_MAX_IE_LEN - 64)
		return -E2BIG;

	len += 2 + sme->ssid_len;
	len += 2 + supported_rates_len;
	if (use_extended_rates)
		len += 2 + sizeof(mt6628_extended_rates);
	if (!mt6628_assoc_has_ie(sme->ie, sme->ie_len,
				 WLAN_EID_HT_CAPABILITY))
		len += 2 + sizeof(mt6628_assoc_ht_cap);
	len += sme->ie_len;

	if (len > MT6628_CONNECT_MAX_IE_LEN)
		return -E2BIG;

	wl->conn_req_ie = kmalloc(len, GFP_KERNEL);
	if (!wl->conn_req_ie)
		return -ENOMEM;

	p = wl->conn_req_ie;
	*(u8 *)p = WLAN_EID_SSID;
	((u8 *)p)[1] = sme->ssid_len;
	p += 2;
	memcpy(p, sme->ssid, sme->ssid_len);
	p += sme->ssid_len;

	*(u8 *)p = WLAN_EID_SUPP_RATES;
	((u8 *)p)[1] = supported_rates_len;
	p += 2;
	memcpy(p, supported_rates, supported_rates_len);
	p += supported_rates_len;

	if (use_extended_rates) {
		*(u8 *)p = WLAN_EID_EXT_SUPP_RATES;
		((u8 *)p)[1] = sizeof(mt6628_extended_rates);
		p += 2;
		memcpy(p, mt6628_extended_rates,
		       sizeof(mt6628_extended_rates));
		p += sizeof(mt6628_extended_rates);
	}

	if (!mt6628_assoc_has_ie(sme->ie, sme->ie_len,
				WLAN_EID_HT_CAPABILITY)) {
		*(u8 *)p = WLAN_EID_HT_CAPABILITY;
		((u8 *)p)[1] = sizeof(mt6628_assoc_ht_cap);
		p += 2;
		memcpy(p, &mt6628_assoc_ht_cap, sizeof(mt6628_assoc_ht_cap));
		p += sizeof(mt6628_assoc_ht_cap);
	}

	if (sme->ie_len)
		memcpy(p, sme->ie, sme->ie_len);

	wl->conn_req_ie_len = len;
	return 0;
}

static int mt6628_send_auth(struct mt6628_wlan *wl)
{
	struct ieee80211_mgmt *mgmt;
	size_t frame_len = offsetof(struct ieee80211_mgmt, u.auth.variable);
	int ret;

	mgmt = kzalloc(frame_len, GFP_KERNEL);
	if (!mgmt)
		return -ENOMEM;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_AUTH);
	ether_addr_copy(mgmt->da, wl->conn_bssid);
	ether_addr_copy(mgmt->sa, wl->netdev->dev_addr);
	ether_addr_copy(mgmt->bssid, wl->conn_bssid);
	mgmt->u.auth.auth_alg = cpu_to_le16(
		wl->conn_auth_mode == MT6628_AUTH_MODE_SHARED ?
		WLAN_AUTH_SHARED_KEY : WLAN_AUTH_OPEN);
	mgmt->u.auth.auth_transaction = cpu_to_le16(1);
	mgmt->u.auth.status_code = cpu_to_le16(WLAN_STATUS_SUCCESS);

	ret = mt6628_wlan_mgmt_tx(wl, (u8 *)mgmt, frame_len, true, true);
	kfree(mgmt);
	return ret;
}

static int mt6628_send_shared_auth_response(struct mt6628_wlan *wl,
					    const struct ieee80211_mgmt *rx,
					    size_t frame_len)
{
	struct ieee80211_mgmt *mgmt;
	const u8 *ies = rx->u.auth.variable;
	size_t ie_len;
	size_t challenge_len = 0;
	const u8 *challenge = NULL;
	size_t out_len;
	int ret;

	if (frame_len < offsetof(struct ieee80211_mgmt, u.auth.variable))
		return -EINVAL;

	ie_len = frame_len - offsetof(struct ieee80211_mgmt, u.auth.variable);
	while (ie_len >= 2) {
		size_t len = ies[1];

		if (len + 2 > ie_len)
			return -EPROTO;
		if (ies[0] == 16) {
			challenge = ies + 2;
			challenge_len = len;
			break;
		}
		ies += len + 2;
		ie_len -= len + 2;
	}

	if (!challenge || challenge_len == 0)
		return -EPROTO;

	out_len = offsetof(struct ieee80211_mgmt, u.auth.variable) +
		2 + challenge_len;
	mgmt = kzalloc(out_len, GFP_KERNEL);
	if (!mgmt)
		return -ENOMEM;

	mgmt->frame_control = cpu_to_le16(
		IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_AUTH |
		IEEE80211_FCTL_PROTECTED);
	ether_addr_copy(mgmt->da, wl->conn_bssid);
	ether_addr_copy(mgmt->sa, wl->netdev->dev_addr);
	ether_addr_copy(mgmt->bssid, wl->conn_bssid);
	mgmt->u.auth.auth_alg = cpu_to_le16(WLAN_AUTH_SHARED_KEY);
	mgmt->u.auth.auth_transaction = cpu_to_le16(3);
	mgmt->u.auth.status_code = cpu_to_le16(WLAN_STATUS_SUCCESS);
	mgmt->u.auth.variable[0] = 16;
	mgmt->u.auth.variable[1] = challenge_len;
	memcpy(&mgmt->u.auth.variable[2], challenge, challenge_len);

	ret = mt6628_wlan_mgmt_tx(wl, (u8 *)mgmt, out_len, true, true);
	kfree(mgmt);
	return ret;
}

static int mt6628_send_assoc(struct mt6628_wlan *wl)
{
	struct ieee80211_mgmt *mgmt;
	size_t head_len = offsetof(struct ieee80211_mgmt, u.assoc_req.variable);
	size_t frame_len = head_len + wl->conn_req_ie_len;
	int ret;

	mgmt = kzalloc(frame_len, GFP_KERNEL);
	if (!mgmt)
		return -ENOMEM;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_ASSOC_REQ);
	ether_addr_copy(mgmt->da, wl->conn_bssid);
	ether_addr_copy(mgmt->sa, wl->netdev->dev_addr);
	ether_addr_copy(mgmt->bssid, wl->conn_bssid);
	mgmt->u.assoc_req.capab_info = cpu_to_le16(MT6628_ASSOC_CAPABILITY);
	mgmt->u.assoc_req.listen_interval =
		cpu_to_le16(MT6628_ASSOC_LISTEN_INTERVAL);
	memcpy(mgmt->u.assoc_req.variable, wl->conn_req_ie,
	       wl->conn_req_ie_len);

	ret = mt6628_wlan_mgmt_tx(wl, (u8 *)mgmt, frame_len, true, true);
	kfree(mgmt);
	return ret;
}

static int mt6628_send_deauth_to(struct mt6628_wlan *wl, const u8 *da,
				 const u8 *sa, const u8 *bssid, u16 reason)
{
	struct ieee80211_mgmt *mgmt;
	size_t frame_len = offsetof(struct ieee80211_mgmt, u.deauth.reason_code) +
			sizeof(mgmt->u.deauth.reason_code);
	int ret;

	if (!wl->netdev)
		return -ENODEV;

	mgmt = kzalloc(frame_len, GFP_KERNEL);
	if (!mgmt)
		return -ENOMEM;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_DEAUTH);
	ether_addr_copy(mgmt->da, da);
	ether_addr_copy(mgmt->sa, sa);
	ether_addr_copy(mgmt->bssid, bssid);
	mgmt->u.deauth.reason_code = cpu_to_le16(reason);

	ret = mt6628_wlan_mgmt_tx(wl, (u8 *)mgmt, frame_len, true, false);
	kfree(mgmt);
	return ret;
}

/*
 * Downstream keeps a small ring of the last deauth reply per peer and
 * refuses to send again inside MIN_DEAUTH_INTERVAL_MSEC.  The firmware
 * raises EVENT_ID_SEND_DEAUTH for every offending frame, so without this
 * a peer stuck in the offending state turns into a deauth storm.
 */
static bool mt6628_deauth_rate_limit(struct mt6628_wlan *wl, const u8 *da)
{
	unsigned long now = jiffies;
	unsigned int i;

	mutex_lock(&wl->cfg_mutex);

	for (i = 0; i < MT6628_MAX_DEAUTH_INFO_COUNT; i++) {
		struct mt6628_deauth_info *info = &wl->deauth_info[i];

		if (is_zero_ether_addr(info->da))
			continue;

		if (time_after_eq(now, info->last_send +
				 msecs_to_jiffies(MT6628_MIN_DEAUTH_INTERVAL_MS))) {
			ether_addr_copy(info->da, da);
			info->last_send = now;
			mutex_unlock(&wl->cfg_mutex);
			return true;
		}

		if (ether_addr_equal(info->da, da)) {
			mutex_unlock(&wl->cfg_mutex);
			return false;
		}
	}

	/* Ring full: answer rather than stay silent. */
	mutex_unlock(&wl->cfg_mutex);
	return true;
}

static int mt6628_send_deauth(struct mt6628_wlan *wl, u16 reason)
{
	return mt6628_send_deauth_to(wl, wl->conn_bssid, wl->netdev->dev_addr,
				     wl->conn_bssid, reason);
}

static void mt6628_connect_timeout_work(struct work_struct *work)
{
	struct mt6628_wlan *wl = container_of(to_delayed_work(work),
					     struct mt6628_wlan,
					     conn_timeout_work);
	u8 bssid[ETH_ALEN];
	const u8 *req_ie;
	size_t req_ie_len;

	mutex_lock(&wl->cfg_mutex);
	if (wl->conn_state == MT6628_CONN_DISCONNECTED ||
	    wl->conn_state == MT6628_CONN_CONNECTED) {
		mutex_unlock(&wl->cfg_mutex);
		return;
	}

	ether_addr_copy(bssid, wl->conn_bssid);
	req_ie = wl->conn_req_ie;
	req_ie_len = wl->conn_req_ie_len;
	mutex_unlock(&wl->cfg_mutex);

	mt6628_conn_fw_cleanup(wl);
	cfg80211_connect_timeout(wl->netdev, bssid, req_ie, req_ie_len,
					 GFP_KERNEL, NL80211_TIMEOUT_UNSPECIFIED);

	mutex_lock(&wl->cfg_mutex);
	mt6628_conn_set_disconnected(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
	mutex_unlock(&wl->cfg_mutex);
}

void mt6628_cfg80211_connect_init(struct mt6628_wlan *wl)
{
	INIT_DELAYED_WORK(&wl->conn_timeout_work, mt6628_connect_timeout_work);
	INIT_DELAYED_WORK(&wl->conn_retry_work, mt6628_conn_retry_work);
	INIT_DELAYED_WORK(&wl->roc_work, mt6628_roc_work);
	wl->conn_retry_valid = false;
	wl->conn_retry_count = 0;
	wl->conn_state = MT6628_CONN_DISCONNECTED;
	wl->conn_auth_mode = MT6628_AUTH_MODE_OPEN;
	wl->conn_enc_status = MT6628_ENCRYPTION_DISABLED;
	wl->conn_key_idx = 0;
	wl->conn_rf_sco = 0;
	wl->conn_key = NULL;
	wl->conn_key_len = 0;
	wl->conn_bss = NULL;
	wl->conn_req_ie = NULL;
	wl->conn_resp_ie = NULL;
}

void mt6628_cfg80211_connect_deinit(struct mt6628_wlan *wl)
{
	cancel_delayed_work_sync(&wl->roc_work);
	cancel_delayed_work_sync(&wl->conn_timeout_work);
	cancel_delayed_work_sync(&wl->conn_retry_work);
	mt6628_conn_clear_retry(wl);
	mt6628_conn_fw_cleanup(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
	mt6628_conn_set_disconnected(wl);
}


/*
 * Remember the parameters of a requested connection so that a firmware
 * reset can be recovered from without waiting for userspace to notice.
 *
 * Only a copy is kept: the caller owns sme and its crypto/key material
 * for the duration of connect() only.
 */
static void mt6628_conn_save_retry(struct mt6628_wlan *wl,
				   const struct cfg80211_connect_params *sme)
{
	struct cfg80211_connect_params *r = &wl->conn_retry;

	mutex_lock(&wl->cfg_mutex);

	/*
	 * Release the previous copy first.  A reconnect attempt passes
	 * sme parameters that were themselves copied out of this struct, so
	 * dropping the old allocations must not happen after the caller's
	 * buffers have been handed to connect().
	 */
	kfree(r->ssid);
	kfree(r->bssid);
	kfree(r->ie);
	kfree(r->key);
	memset(r, 0, sizeof(*r));

	r->ssid = kmemdup(sme->ssid, sme->ssid_len, GFP_KERNEL);
	if (!r->ssid) {
		mutex_unlock(&wl->cfg_mutex);
		return;
	}
	r->ssid_len = sme->ssid_len;

	r->bssid = NULL;
	if (sme->bssid)
		r->bssid = kmemdup(sme->bssid, ETH_ALEN, GFP_KERNEL);

	r->channel = sme->channel;
	r->channel_hint = sme->channel_hint;
	r->privacy = sme->privacy;
	r->auth_type = sme->auth_type;
	r->key_idx = sme->key_idx;
	r->mfp = sme->mfp;
	/* The crypto settings are a nested struct, not flat members. */
	r->crypto.wpa_versions = sme->crypto.wpa_versions;
	r->crypto.cipher_group = sme->crypto.cipher_group;
	r->crypto.n_ciphers_pairwise = sme->crypto.n_ciphers_pairwise;
	memcpy(r->crypto.ciphers_pairwise, sme->crypto.ciphers_pairwise,
	       sizeof(r->crypto.ciphers_pairwise));
	r->crypto.n_akm_suites = sme->crypto.n_akm_suites;
	memcpy(r->crypto.akm_suites, sme->crypto.akm_suites,
	       sizeof(r->crypto.akm_suites));
	r->crypto.control_port = sme->crypto.control_port;
	r->crypto.control_port_over_nl80211 =
		sme->crypto.control_port_over_nl80211;
	r->ie = sme->ie && sme->ie_len ?
		kmemdup(sme->ie, sme->ie_len, GFP_KERNEL) : NULL;
	r->ie_len = r->ie ? sme->ie_len : 0;

	/* WEP and PMK key material is referenced by pointer upstream. */
	if (sme->key && sme->key_len) {
		r->key = kmemdup(sme->key, sme->key_len, GFP_KERNEL);
		r->key_len = r->key ? sme->key_len : 0;
	} else {
		r->key = NULL;
		r->key_len = 0;
	}

	wl->conn_retry_valid = true;

	mutex_unlock(&wl->cfg_mutex);
}

static void mt6628_conn_clear_retry(struct mt6628_wlan *wl)
{
	struct cfg80211_connect_params *r;

	mutex_lock(&wl->cfg_mutex);
	r = &wl->conn_retry;
	wl->conn_retry_valid = false;
	wl->conn_retry_count = 0;
	kfree(r->ssid);
	kfree(r->bssid);
	kfree(r->ie);
	kfree(r->key);
	memset(r, 0, sizeof(*r));
	mutex_unlock(&wl->cfg_mutex);
}

/*
 * Re-associate after the firmware has been restarted underneath us.
 *
 * wpa_supplicant has been told the link went down and will normally ask
 * us to reconnect, but that only happens once it gets around to it, and
 * after a firmware reset the association can be re-established just as
 * well from here.  Give up after a bounded number of attempts so a
 * genuinely unavailable AP does not leave us looping forever.
 */
void mt6628_conn_schedule_retry(struct mt6628_wlan *wl)
{
	if (!wl || !wl->runtime_started)
		return;

	mutex_lock(&wl->cfg_mutex);
	if (!wl->conn_retry_valid || wl->conn_state != MT6628_CONN_DISCONNECTED) {
		mutex_unlock(&wl->cfg_mutex);
		return;
	}
	wl->conn_retry_count = 0;
	mutex_unlock(&wl->cfg_mutex);

	schedule_delayed_work(&wl->conn_retry_work,
			      msecs_to_jiffies(MT6628_CONN_RETRY_DELAY_MS));
}

static void mt6628_conn_retry_work(struct work_struct *work)
{
	struct mt6628_wlan *wl = container_of(to_delayed_work(work),
					     struct mt6628_wlan,
					     conn_retry_work);
	struct cfg80211_connect_params retry;
	u8 *ssid, *bssid, *ie, *key;
	bool retry_again;
	int ret;

	if (!wl->runtime_started || !wl->fw_running || !wl->netdev) {
		mt6628_conn_clear_retry(wl);
		return;
	}

	mutex_lock(&wl->cfg_mutex);
	if (!wl->conn_retry_valid ||
	    wl->conn_state != MT6628_CONN_DISCONNECTED ||
	    wl->conn_retry_count >= MT6628_CONN_RETRY_MAX) {
		mutex_unlock(&wl->cfg_mutex);
		mt6628_conn_clear_retry(wl);
		return;
	}
	wl->conn_retry_count++;
	mutex_unlock(&wl->cfg_mutex);

	/* Take a private copy: connect() may run against cleared state. */
	mutex_lock(&wl->cfg_mutex);
	retry = wl->conn_retry;
	ssid = kmemdup(retry.ssid, retry.ssid_len, GFP_KERNEL);
	bssid = retry.bssid ? kmemdup(retry.bssid, ETH_ALEN, GFP_KERNEL) : NULL;
	ie = retry.ie ? kmemdup(retry.ie, retry.ie_len, GFP_KERNEL) : NULL;
	key = retry.key ? kmemdup(retry.key, retry.key_len, GFP_KERNEL) : NULL;
	mutex_unlock(&wl->cfg_mutex);

	if (!ssid || (retry.bssid && !bssid) ||
	    (retry.ie && !ie) || (retry.key && !key)) {
		kfree(ssid);
		kfree(bssid);
		kfree(ie);
		kfree(key);
		mt6628_conn_clear_retry(wl);
		return;
	}

	retry.ssid = ssid;
	retry.ssid_len = retry.ssid_len;
	retry.bssid = bssid;
	retry.ie = ie;
	retry.ie_len = ie ? retry.ie_len : 0;
	retry.key = key;
	retry.key_len = key ? retry.key_len : 0;

	ret = mt6628_cfg80211_connect(wl->wiphy, wl->netdev, &retry);

	/*
	 * connect() took its own copy of these parameters on success, or
	 * never consumed them, so release ours either way.  The saved state
	 * was replaced under the lock by save_retry(), so nothing else
	 * refers to them.
	 */
	kfree(ssid);
	kfree(bssid);
	kfree(ie);
	kfree(key);

	if (ret == -EBUSY || ret == -EALREADY) {
		/* A connect is already running; let it take over. */
		mt6628_conn_clear_retry(wl);
		return;
	}

	if (ret) {
		dev_warn(&wl->func->dev,
			 "reconnect after firmware reset failed: %d\n", ret);

		/* Back off and try again while attempts remain. */
		mutex_lock(&wl->cfg_mutex);
		retry_again = wl->conn_retry_valid &&
			      wl->conn_retry_count < MT6628_CONN_RETRY_MAX;
		mutex_unlock(&wl->cfg_mutex);

		if (retry_again)
			schedule_delayed_work(&wl->conn_retry_work,
					      msecs_to_jiffies(
							MT6628_CONN_RETRY_DELAY_MS));
		else
			mt6628_conn_clear_retry(wl);
		return;
	}

	dev_info(&wl->func->dev,
		 "reconnecting after firmware reset (attempt %u)\n",
		 wl->conn_retry_count);
}

int mt6628_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
				struct cfg80211_connect_params *sme)
{
	struct mt6628_wlan *wl = netdev_priv(dev) ?
		*(struct mt6628_wlan **)netdev_priv(dev) : NULL;
	const u8 *requested_bssid;
	struct ieee80211_channel *channel;
	enum ieee80211_privacy privacy;
	bool secure;
	bool wep;
	u8 auth_mode = MT6628_AUTH_MODE_OPEN;
	u8 enc_status = MT6628_ENCRYPTION_DISABLED;
	u8 *conn_key = NULL;
	int ret;

	if (!wl || !wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (!sme->ssid || !sme->ssid_len ||
	    sme->ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;
	if (sme->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
	    sme->auth_type != NL80211_AUTHTYPE_SHARED_KEY &&
	    sme->auth_type != NL80211_AUTHTYPE_AUTOMATIC)
		return -EOPNOTSUPP;

	secure = sme->privacy || sme->crypto.wpa_versions ||
		sme->crypto.cipher_group ||
		sme->crypto.n_ciphers_pairwise ||
		sme->crypto.n_akm_suites ||
		sme->crypto.control_port ||
		sme->crypto.control_port_over_nl80211;

	wep = mt6628_connect_is_wep(sme, &auth_mode, &enc_status);
	if (wep) {
		conn_key = kmemdup(sme->key, sme->key_len, GFP_KERNEL);
		if (!conn_key)
			return -ENOMEM;
	} else if (secure) {
		if (!mt6628_connect_is_wpa_psk(sme, &auth_mode, &enc_status))
			return -EOPNOTSUPP;
	} else if (sme->mfp != NL80211_MFP_NO ||
		   sme->key_len || sme->key)
		return -EOPNOTSUPP;

	privacy = secure ? IEEE80211_PRIVACY_ON : IEEE80211_PRIVACY_OFF;

	requested_bssid = sme->bssid ? sme->bssid : sme->bssid_hint;
	channel = sme->channel ? sme->channel : sme->channel_hint;

	mutex_lock(&wl->cfg_mutex);
	if (wl->conn_state != MT6628_CONN_DISCONNECTED) {
		mutex_unlock(&wl->cfg_mutex);
		kfree(conn_key);
		return -EBUSY;
	}
	if (wl->sta_rec_idx != MT6628_STA_REC_INDEX_NOT_FOUND) {
		mutex_unlock(&wl->cfg_mutex);

		/*
		 * A previous cleanup may have failed after disconnecting the
		 * logical connection.  Retry the firmware-side cleanup before
		 * allocating a new STA-REC.
		 */
		mt6628_conn_fw_cleanup(wl);

		mutex_lock(&wl->cfg_mutex);
		if (wl->sta_rec_idx != MT6628_STA_REC_INDEX_NOT_FOUND) {
			mutex_unlock(&wl->cfg_mutex);
			kfree(conn_key);
			return -EIO;
		}
		mutex_unlock(&wl->cfg_mutex);
	} else {
		mutex_unlock(&wl->cfg_mutex);
	}

	if (requested_bssid && is_zero_ether_addr(requested_bssid))
		requested_bssid = NULL;

	wl->conn_bss = cfg80211_get_bss(wiphy, channel, requested_bssid,
					 sme->ssid, sme->ssid_len,
					 IEEE80211_BSS_TYPE_ESS,
					 privacy);
	if (!wl->conn_bss) {
		kfree(conn_key);
		return -ENOENT;
	}

	if (!requested_bssid)
		requested_bssid = wl->conn_bss->bssid;

	mutex_lock(&wl->cfg_mutex);
	ether_addr_copy(wl->conn_bssid, requested_bssid);
	memcpy(wl->conn_ssid, sme->ssid, sme->ssid_len);
	wl->conn_ssid_len = sme->ssid_len;
	wl->conn_band = wl->conn_bss->channel->band;
	wl->conn_channel = wl->conn_bss->channel->hw_value;
	wl->conn_secure = secure;
	wl->conn_auth_mode = auth_mode;
	wl->conn_enc_status = enc_status;
	wl->conn_key_idx = sme->key_idx;
	wl->conn_rf_sco = 0;
	kfree(wl->conn_key);
	wl->conn_key = conn_key;
	wl->conn_key_len = conn_key ? sme->key_len : 0;
	conn_key = NULL;
	wl->sta_rec_idx = MT6628_STA_REC_INDEX_NOT_FOUND;
	wl->conn_state = MT6628_CONN_AUTH;
	mutex_unlock(&wl->cfg_mutex);

	ret = mt6628_build_assoc_ies(wl, sme);
	if (ret)
		goto err_reset;

	mt6628_cfg80211_abort_scan(wl);

	ret = mt6628_wlan_request_channel(wl, wl->conn_bss->channel,
					  wl->conn_bssid);
	if (ret)
		goto err_reset;

	wl->sta_rec_idx = 0;
	ret = mt6628_wlan_update_sta_record(wl, MT6628_STA_STATE_1, 0,
					    wl->conn_bssid);
	if (ret) {
		wl->sta_rec_idx = MT6628_STA_REC_INDEX_NOT_FOUND;
		goto err_reset;
	}

	ret = mt6628_wlan_set_bss_info(wl, wl->conn_channel, wl->conn_ssid,
					wl->conn_ssid_len, wl->conn_bssid, false);
	if (ret)
		goto err_reset;

	ret = mt6628_wlan_activate_bss(wl, true);
	if (ret)
		goto err_reset;

	if (wl->conn_key) {
		struct key_params key = {
			.cipher = wl->conn_key_len == WLAN_KEY_LEN_WEP40 ?
				WLAN_CIPHER_SUITE_WEP40 :
				WLAN_CIPHER_SUITE_WEP104,
			.key = wl->conn_key,
			.key_len = wl->conn_key_len,
		};

		ret = mt6628_wlan_add_key(wl, wl->conn_key_idx, false, true,
					  NULL, &key);
		if (ret)
			goto err_reset;
	}

	ret = mt6628_send_auth(wl);
	if (ret)
		goto err_reset;

	mod_delayed_work(system_wq, &wl->conn_timeout_work,
			msecs_to_jiffies(MT6628_CONNECT_TIMEOUT_MS));

	/*
	 * Remember how to get back here, so a firmware reset can be
	 * recovered from without waiting for userspace.
	 */
	mt6628_conn_save_retry(wl, sme);

	return 0;

err_reset:
	cancel_delayed_work_sync(&wl->conn_timeout_work);
	mt6628_conn_fw_cleanup(wl);
	mutex_lock(&wl->cfg_mutex);
	mt6628_conn_set_disconnected(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
	mutex_unlock(&wl->cfg_mutex);
	kfree(conn_key);
	return ret;
}

int mt6628_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *dev,
				u16 reason_code)
{
	struct mt6628_wlan *wl = *(struct mt6628_wlan **)netdev_priv(dev);
	bool connected;
	bool in_progress;
	bool stale_sta;
	u8 bssid[ETH_ALEN];
	const u8 *req_ie;
	size_t req_ie_len;

	if (!wl)
		return -ENODEV;

	cancel_delayed_work_sync(&wl->conn_timeout_work);

	/* An explicit disconnect means the user no longer wants to retry. */
	cancel_delayed_work_sync(&wl->conn_retry_work);
	mt6628_conn_clear_retry(wl);

	mutex_lock(&wl->cfg_mutex);
	connected = wl->conn_state == MT6628_CONN_CONNECTED;
	in_progress = !connected &&
		wl->conn_state != MT6628_CONN_DISCONNECTED;
	if (wl->conn_state == MT6628_CONN_DISCONNECTED) {
		stale_sta = wl->sta_rec_idx != MT6628_STA_REC_INDEX_NOT_FOUND;
		mutex_unlock(&wl->cfg_mutex);

		if (stale_sta)
			mt6628_conn_fw_cleanup(wl);

		return 0;
	}

	ether_addr_copy(bssid, wl->conn_bssid);
	req_ie = wl->conn_req_ie;
	req_ie_len = wl->conn_req_ie_len;
	mutex_unlock(&wl->cfg_mutex);

	if (connected)
		mt6628_send_deauth(wl, reason_code);
	mt6628_conn_fw_cleanup(wl);

	if (connected)
		cfg80211_disconnected(dev, reason_code, NULL, 0, true, GFP_KERNEL);
	else if (in_progress)
		cfg80211_connect_timeout(dev, bssid, req_ie, req_ie_len, GFP_KERNEL,
					 NL80211_TIMEOUT_UNSPECIFIED);

	mutex_lock(&wl->cfg_mutex);
	mt6628_conn_set_disconnected(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
	mutex_unlock(&wl->cfg_mutex);
	return 0;
}

static bool mt6628_connection_bssid_matches(struct mt6628_wlan *wl,
						 const u8 *bssid)
{
	return ether_addr_equal(wl->conn_bssid, bssid);
}

static bool mt6628_rx_mgmt_frame(struct sk_buff *skb,
				 struct ieee80211_mgmt **mgmt,
				 unsigned int *frame_len)
{
	const struct mt6628_hif_rx_hdr *hdr;
	unsigned int offset;
	unsigned int len;

	if (skb->len < MT6628_HIF_RX_HEADER_LEN + sizeof(struct ieee80211_hdr))
		return false;

	hdr = (const struct mt6628_hif_rx_hdr *)skb->data;
	offset = hdr->header_len_offset & GENMASK(1, 0);
	if (le16_to_cpu(hdr->packet_len) <
	    MT6628_HIF_RX_HEADER_LEN + offset)
		return false;

	len = le16_to_cpu(hdr->packet_len) -
		MT6628_HIF_RX_HEADER_LEN - offset;
	if (len < sizeof(struct ieee80211_hdr) ||
	    len > skb->len - MT6628_HIF_RX_HEADER_LEN - offset)
		return false;

	*mgmt = (struct ieee80211_mgmt *)
		(skb->data + MT6628_HIF_RX_HEADER_LEN + offset);
	*frame_len = len;
	return true;
}

static void mt6628_connect_auth_result(struct mt6628_wlan *wl,
					       u16 status_code)
{
	int ret;

	if (status_code != WLAN_STATUS_SUCCESS) {
		cfg80211_connect_bss(wl->netdev, wl->conn_bssid, wl->conn_bss,
				     wl->conn_req_ie, wl->conn_req_ie_len, NULL, 0,
				     status_code, GFP_KERNEL,
				     NL80211_TIMEOUT_UNSPECIFIED);
		mt6628_conn_put_bss(wl);
		mt6628_conn_fw_cleanup(wl);
		mt6628_conn_set_disconnected(wl);
		mt6628_conn_free_ies(wl);
		return;
	}

	ret = mt6628_wlan_update_sta_record(wl, MT6628_STA_STATE_2, 0,
					    wl->conn_bssid);
	if (ret)
		goto timeout;

	ret = mt6628_send_assoc(wl);
	if (ret)
		goto timeout;

	wl->conn_state = MT6628_CONN_ASSOC;
	mod_delayed_work(system_wq, &wl->conn_timeout_work,
			msecs_to_jiffies(MT6628_CONNECT_TIMEOUT_MS));
	return;

timeout:
	cancel_delayed_work(&wl->conn_timeout_work);
	cfg80211_connect_timeout(wl->netdev, wl->conn_bssid,
				 wl->conn_req_ie, wl->conn_req_ie_len,
				 GFP_KERNEL, NL80211_TIMEOUT_UNSPECIFIED);
	mt6628_conn_fw_cleanup(wl);
	mt6628_conn_set_disconnected(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
}

static void mt6628_connect_assoc_result(struct mt6628_wlan *wl,
						struct ieee80211_mgmt *mgmt,
						size_t frame_len)
{
	size_t resp_ie_len;
	const u8 *resp_ie;
	int ret;

	if (le16_to_cpu(mgmt->u.assoc_resp.status_code) !=
	    WLAN_STATUS_SUCCESS) {
		cfg80211_connect_bss(wl->netdev, wl->conn_bssid, wl->conn_bss,
				     wl->conn_req_ie, wl->conn_req_ie_len,
				     mgmt->u.assoc_resp.variable,
				     frame_len - offsetof(struct ieee80211_mgmt,
							     u.assoc_resp.variable),
				     le16_to_cpu(mgmt->u.assoc_resp.status_code),
				     GFP_KERNEL, NL80211_TIMEOUT_UNSPECIFIED);
		mt6628_conn_put_bss(wl);
		mt6628_conn_fw_cleanup(wl);
		mt6628_conn_set_disconnected(wl);
		mt6628_conn_free_ies(wl);
		return;
	}

	resp_ie = mgmt->u.assoc_resp.variable;
	resp_ie_len = frame_len - offsetof(struct ieee80211_mgmt,
						 u.assoc_resp.variable);
	wl->conn_resp_ie = kmemdup(resp_ie, resp_ie_len, GFP_KERNEL);
	if (!wl->conn_resp_ie && resp_ie_len)
		goto timeout;
	wl->conn_resp_ie_len = resp_ie_len;
	wl->conn_aid = le16_to_cpu(mgmt->u.assoc_resp.aid) & 0x3fff;
	wl->conn_rf_sco = mt6628_assoc_ht_sco(resp_ie, resp_ie_len);

	ret = mt6628_wlan_update_sta_record(wl, MT6628_STA_STATE_3,
					    wl->conn_aid, wl->conn_bssid);
	if (ret)
		goto timeout;
	ret = mt6628_wlan_set_bss_info(wl, wl->conn_channel, wl->conn_ssid,
					wl->conn_ssid_len, wl->conn_bssid, true);
	if (ret)
		goto timeout;

	cancel_delayed_work(&wl->conn_timeout_work);
	wl->connected = true;
	wl->conn_state = MT6628_CONN_CONNECTED;
	if (wl->netdev)
		netif_carrier_on(wl->netdev);

	cfg80211_connect_bss(wl->netdev, wl->conn_bssid, wl->conn_bss,
			     wl->conn_req_ie, wl->conn_req_ie_len,
			     wl->conn_resp_ie, wl->conn_resp_ie_len,
			     WLAN_STATUS_SUCCESS, GFP_KERNEL,
			     NL80211_TIMEOUT_UNSPECIFIED);
	mt6628_conn_put_bss(wl);
	mt6628_wlan_release_channel(wl);
	mt6628_conn_free_ies(wl);
	return;

timeout:
	cancel_delayed_work(&wl->conn_timeout_work);
	cfg80211_connect_timeout(wl->netdev, wl->conn_bssid,
				 wl->conn_req_ie, wl->conn_req_ie_len,
				 GFP_KERNEL, NL80211_TIMEOUT_UNSPECIFIED);
	mt6628_conn_fw_cleanup(wl);
	mt6628_conn_set_disconnected(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
}

bool mt6628_cfg80211_connection_mgmt(struct mt6628_wlan *wl,
					     struct sk_buff *skb)
{
	struct ieee80211_mgmt *mgmt;
	unsigned int frame_len;
	__le16 fc;
	bool relevant = false;

	if (!mt6628_rx_mgmt_frame(skb, &mgmt, &frame_len))
		return false;

	fc = mgmt->frame_control;
	if (!ieee80211_is_auth(fc) && !ieee80211_is_assoc_resp(fc) &&
	    !ieee80211_is_deauth(fc) && !ieee80211_is_disassoc(fc))
		return false;

	mutex_lock(&wl->cfg_mutex);
	if (wl->conn_state == MT6628_CONN_DISCONNECTED) {
		mutex_unlock(&wl->cfg_mutex);
		return false;
	}

	if (!mt6628_connection_bssid_matches(wl, mgmt->sa)) {
		mutex_unlock(&wl->cfg_mutex);
		return false;
	}

	if (ieee80211_is_auth(fc) && wl->conn_state == MT6628_CONN_AUTH) {
		u16 transaction;

		if (frame_len < offsetof(struct ieee80211_mgmt,
					 u.auth.variable)) {
			mutex_unlock(&wl->cfg_mutex);
			return true;
		}

		transaction = le16_to_cpu(mgmt->u.auth.auth_transaction);

		if (wl->conn_auth_mode == MT6628_AUTH_MODE_SHARED) {
			if (transaction == 2) {
				int ret;

				if (le16_to_cpu(mgmt->u.auth.status_code) !=
				    WLAN_STATUS_SUCCESS) {
					relevant = true;
					mt6628_connect_auth_result(wl,
						le16_to_cpu(
							mgmt->u.auth.status_code));
					goto auth_done;
				}

				ret = mt6628_send_shared_auth_response(wl, mgmt,
								       frame_len);
				if (ret)
					dev_warn(&wl->func->dev,
						 "failed to send WEP challenge response: %d\n",
						 ret);
				relevant = true;
				goto auth_done;
			}

			if (transaction != 4) {
				mutex_unlock(&wl->cfg_mutex);
				return true;
			}
		} else if (transaction != 2) {
			mutex_unlock(&wl->cfg_mutex);
			return true;
		}

		relevant = true;
		mt6628_connect_auth_result(wl,
				le16_to_cpu(mgmt->u.auth.status_code));
auth_done:
		;
	} else if (ieee80211_is_assoc_resp(fc) &&
		   wl->conn_state == MT6628_CONN_ASSOC) {
		if (frame_len < offsetof(struct ieee80211_mgmt,
					 u.assoc_resp.variable)) {
			mutex_unlock(&wl->cfg_mutex);
			return true;
		}
		relevant = true;
		mt6628_connect_assoc_result(wl, mgmt, frame_len);
	} else if ((ieee80211_is_deauth(fc) || ieee80211_is_disassoc(fc)) &&
		   wl->conn_state != MT6628_CONN_DISCONNECTED) {
		u16 reason;
		bool connected = wl->conn_state == MT6628_CONN_CONNECTED;

		if (frame_len < (ieee80211_is_deauth(fc) ?
			offsetof(struct ieee80211_mgmt, u.deauth.reason_code) +
			sizeof(mgmt->u.deauth.reason_code) :
			offsetof(struct ieee80211_mgmt, u.disassoc.reason_code) +
			sizeof(mgmt->u.disassoc.reason_code))) {
			mutex_unlock(&wl->cfg_mutex);
			return true;
		}

		reason = ieee80211_is_deauth(fc) ?
			le16_to_cpu(mgmt->u.deauth.reason_code) :
			le16_to_cpu(mgmt->u.disassoc.reason_code);
		relevant = true;
		if (connected) {
			mt6628_conn_set_disconnected(wl);
			mt6628_conn_fw_cleanup(wl);
			mutex_unlock(&wl->cfg_mutex);
			cfg80211_disconnected(wl->netdev, reason, NULL, 0, false,
				      GFP_KERNEL);
		} else {
			cfg80211_connect_bss(wl->netdev, wl->conn_bssid, wl->conn_bss,
				     wl->conn_req_ie, wl->conn_req_ie_len, NULL, 0,
				     WLAN_STATUS_UNSPECIFIED_FAILURE, GFP_KERNEL,
				     NL80211_TIMEOUT_UNSPECIFIED);
			mt6628_conn_put_bss(wl);
			mt6628_conn_fw_cleanup(wl);
			mt6628_conn_set_disconnected(wl);
			mt6628_conn_free_ies(wl);
			mutex_unlock(&wl->cfg_mutex);
		}
		kfree_skb(skb);
		return true;
	}

	mutex_unlock(&wl->cfg_mutex);
	if (relevant)
		kfree_skb(skb);
	return relevant;
}
