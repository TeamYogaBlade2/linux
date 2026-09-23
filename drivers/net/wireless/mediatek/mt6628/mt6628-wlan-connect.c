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
#define MT6628_CONN_DISCONNECTED        0
#define MT6628_CONN_AUTH                1
#define MT6628_CONN_ASSOC               2
#define MT6628_CONN_CONNECTED           3

#define MT6628_CONNECT_MAX_IE_LEN       600

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

static void mt6628_conn_free_ies(struct mt6628_wlan *wl)
{
	kfree(wl->conn_req_ie);
	wl->conn_req_ie = NULL;
	wl->conn_req_ie_len = 0;
	kfree(wl->conn_resp_ie);
	wl->conn_resp_ie = NULL;
	wl->conn_resp_ie_len = 0;
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

	/* The channel privilege may outlive STA-REC setup failures. */
	ret = mt6628_wlan_release_channel(wl);
	if (ret)
		dev_warn(&wl->func->dev,
			 "failed to release channel privilege: %d\n", ret);
}

static void mt6628_conn_set_disconnected(struct mt6628_wlan *wl)
{
	wl->conn_state = MT6628_CONN_DISCONNECTED;
	wl->connected = false;
	wl->conn_secure = false;
	wl->conn_aid = 0;
	if (wl->netdev)
		netif_carrier_off(wl->netdev);
}

static bool mt6628_connect_is_wpa2_psk(
	const struct cfg80211_connect_params *sme)
{
	const struct cfg80211_crypto_settings *crypto = &sme->crypto;

	if (crypto->wpa_versions != NL80211_WPA_VERSION_2)
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
	.cap_info = cpu_to_le16(IEEE80211_HT_CAP_SGI_20),
	.ampdu_params_info = IEEE80211_HT_MAX_AMPDU_64K,
	.mcs = {
		.rx_mask = { 0xff },
		.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	},
};

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
	mgmt->u.auth.auth_alg = cpu_to_le16(WLAN_AUTH_OPEN);
	mgmt->u.auth.auth_transaction = cpu_to_le16(1);
	mgmt->u.auth.status_code = cpu_to_le16(WLAN_STATUS_SUCCESS);

	ret = mt6628_wlan_mgmt_tx(wl, (u8 *)mgmt, frame_len, true);
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

	ret = mt6628_wlan_mgmt_tx(wl, (u8 *)mgmt, frame_len, true);
	kfree(mgmt);
	return ret;
}

static int mt6628_send_deauth(struct mt6628_wlan *wl, u16 reason)
{
	struct ieee80211_mgmt *mgmt;
	size_t frame_len = offsetof(struct ieee80211_mgmt, u.deauth.reason_code) +
			sizeof(mgmt->u.deauth.reason_code);
	int ret;

	mgmt = kzalloc(frame_len, GFP_KERNEL);
	if (!mgmt)
		return -ENOMEM;

	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
						  IEEE80211_STYPE_DEAUTH);
	ether_addr_copy(mgmt->da, wl->conn_bssid);
	ether_addr_copy(mgmt->sa, wl->netdev->dev_addr);
	ether_addr_copy(mgmt->bssid, wl->conn_bssid);
	mgmt->u.deauth.reason_code = cpu_to_le16(reason);

	ret = mt6628_wlan_mgmt_tx(wl, (u8 *)mgmt, frame_len, false);
	kfree(mgmt);
	return ret;
}

static int mt6628_wait_mgmt_tx(struct mt6628_wlan *wl)
{
	unsigned long flags;
	long timeout;
	int status;

	spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
	if (!wl->mgmt_tx_pending) {
		status = wl->mgmt_tx_status;
		spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);
		return status;
	}
	spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);

	timeout = wait_for_completion_timeout(&wl->mgmt_tx_done,
					      msecs_to_jiffies(1000));
	if (!timeout)
		return -ETIMEDOUT;

	spin_lock_irqsave(&wl->mgmt_tx_lock, flags);
	status = wl->mgmt_tx_status;
	spin_unlock_irqrestore(&wl->mgmt_tx_lock, flags);

	return status;
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
	wl->conn_state = MT6628_CONN_DISCONNECTED;
	wl->conn_bss = NULL;
	wl->conn_req_ie = NULL;
	wl->conn_resp_ie = NULL;
}

void mt6628_cfg80211_connect_deinit(struct mt6628_wlan *wl)
{
	cancel_delayed_work_sync(&wl->conn_timeout_work);
	mt6628_conn_fw_cleanup(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
	mt6628_conn_set_disconnected(wl);
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
	int ret;

	if (!wl || !wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (!sme->ssid || !sme->ssid_len ||
	    sme->ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;
	if (sme->auth_type != NL80211_AUTHTYPE_OPEN_SYSTEM &&
	    sme->auth_type != NL80211_AUTHTYPE_AUTOMATIC)
		return -EOPNOTSUPP;

	secure = sme->privacy || sme->crypto.wpa_versions ||
		sme->crypto.cipher_group ||
		sme->crypto.n_ciphers_pairwise ||
		sme->crypto.n_akm_suites ||
		sme->crypto.control_port ||
		sme->crypto.control_port_over_nl80211;

	if (secure) {
		if (!mt6628_connect_is_wpa2_psk(sme))
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
	if (!wl->conn_bss)
		return -ENOENT;

	if (!requested_bssid)
		requested_bssid = wl->conn_bss->bssid;

	mutex_lock(&wl->cfg_mutex);
	ether_addr_copy(wl->conn_bssid, requested_bssid);
	memcpy(wl->conn_ssid, sme->ssid, sme->ssid_len);
	wl->conn_ssid_len = sme->ssid_len;
	wl->conn_band = wl->conn_bss->channel->band;
	wl->conn_channel = wl->conn_bss->channel->hw_value;
	wl->conn_secure = secure;
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

	ret = mt6628_send_auth(wl);
	if (ret)
		goto err_reset;

	mod_delayed_work(system_wq, &wl->conn_timeout_work,
			msecs_to_jiffies(MT6628_CONNECT_TIMEOUT_MS));
	return 0;

err_reset:
	cancel_delayed_work_sync(&wl->conn_timeout_work);
	mt6628_conn_fw_cleanup(wl);
	mutex_lock(&wl->cfg_mutex);
	mt6628_conn_set_disconnected(wl);
	mt6628_conn_put_bss(wl);
	mt6628_conn_free_ies(wl);
	mutex_unlock(&wl->cfg_mutex);
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

	ret = mt6628_wait_mgmt_tx(wl);
	if (ret)
		goto timeout;

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
		if (frame_len < offsetof(struct ieee80211_mgmt,
					 u.auth.variable) ||
		    le16_to_cpu(mgmt->u.auth.auth_transaction) != 2) {
			mutex_unlock(&wl->cfg_mutex);
			return true;
		}
		relevant = true;
		mt6628_connect_auth_result(wl,
				le16_to_cpu(mgmt->u.auth.status_code));
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
