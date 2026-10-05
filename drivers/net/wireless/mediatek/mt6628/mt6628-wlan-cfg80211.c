// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 cfg80211 control plane
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include <net/regulatory.h>

#include "mtk-wlan-hif.h"
#include "mtk-wlan.h"

#define MT6628_SCAN_MAX_SSIDS		4
#define MT6628_SCAN_MAX_CHANNELS	32
#define MT6628_SCAN_MAX_IE_LEN		600
#define MT6628_SCAN_DWELL_TIME_TU	20
#define MT6628_SCAN_SSID_WILDCARD	BIT(0)
#define MT6628_SCAN_SSID_SPECIFIC	BIT(2)
#define MT6628_SCAN_CHANNEL_SPECIFIED	4
#define MT6628_SCAN_TYPE_PASSIVE	0
#define MT6628_SCAN_TYPE_ACTIVE		1

struct mt6628_scan_ssid {
	__le32 len;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
} __packed;

struct mt6628_scan_channel {
	u8 band;
	u8 channel;
} __packed;

struct mt6628_scan_cmd {
	u8 seq_num;
	u8 network_type;
	u8 scan_type;
	u8 ssid_type;
	struct mt6628_scan_ssid ssids[MT6628_SCAN_MAX_SSIDS];
	__le16 probe_delay_time;
	__le16 channel_dwell_time;
	u8 channel_type;
	u8 channel_list_num;
	struct mt6628_scan_channel channels[MT6628_SCAN_MAX_CHANNELS];
	__le16 ie_len;
	u8 ie[MT6628_SCAN_MAX_IE_LEN];
} __packed;

struct mt6628_scan_cancel_cmd {
	u8 seq_num;
	u8 is_ext_channel;
	u8 reserved[2];
} __packed;

struct mt6628_event_scan_done {
	u8 seq_num;
	u8 sparse_channel_valid;
	u8 sparse_channel_band;
	u8 sparse_channel;
} __packed;

static const struct ieee80211_sta_ht_cap mt6628_ht_cap = {
	.ht_supported = true,
	.cap = IEEE80211_HT_CAP_SGI_20 |
	       IEEE80211_HT_CAP_SUP_WIDTH_20_40,
	.ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K,
	.ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE,
	.mcs = {
		.rx_mask = { 0xff },
		.tx_params = IEEE80211_HT_MCS_TX_DEFINED,
	},
};

static struct ieee80211_rate mt6628_2ghz_rates[] = {
	{ .bitrate = 10, .hw_value = 0, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 20, .hw_value = 1, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55, .hw_value = 2, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .hw_value = 3, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60, .hw_value = 4 },
	{ .bitrate = 90, .hw_value = 5 },
	{ .bitrate = 120, .hw_value = 6 },
	{ .bitrate = 180, .hw_value = 7 },
	{ .bitrate = 240, .hw_value = 8 },
	{ .bitrate = 360, .hw_value = 9 },
	{ .bitrate = 480, .hw_value = 10 },
	{ .bitrate = 540, .hw_value = 11 },
};

static struct ieee80211_channel mt6628_2ghz_channels[] = {
	{ .center_freq = 2412, .hw_value = 1, .max_power = 20 },
	{ .center_freq = 2417, .hw_value = 2, .max_power = 20 },
	{ .center_freq = 2422, .hw_value = 3, .max_power = 20 },
	{ .center_freq = 2427, .hw_value = 4, .max_power = 20 },
	{ .center_freq = 2432, .hw_value = 5, .max_power = 20 },
	{ .center_freq = 2437, .hw_value = 6, .max_power = 20 },
	{ .center_freq = 2442, .hw_value = 7, .max_power = 20 },
	{ .center_freq = 2447, .hw_value = 8, .max_power = 20 },
	{ .center_freq = 2452, .hw_value = 9, .max_power = 20 },
	{ .center_freq = 2457, .hw_value = 10, .max_power = 20 },
	{ .center_freq = 2462, .hw_value = 11, .max_power = 20 },
	{ .center_freq = 2467, .hw_value = 12, .max_power = 20 },
	{ .center_freq = 2472, .hw_value = 13, .max_power = 20 },
	/*
	 * Channel 14 is 802.11b only: no HT (so no 40 MHz bonding on either
	 * side of it) and no OFDM.  That is a property of the channel itself,
	 * not of any regulatory domain, so it is asserted here.
	 *
	 * Channels 12 and 13 are deliberately left with no flags.  They are
	 * listed because the hardware can tune them, but whether they may be
	 * scanned on, radiated on or bonded is a regulatory domain decision -
	 * the world fallback marks both NO_IR and most domains restrict them
	 * further.  reg_process_ht_flags() derives their NO_HT40PLUS/
	 * NO_HT40MINUS pair from the active domain's bandwidth and adjacent
	 * channel rules at registration, so a guess here would be overwritten.
	 */
	{ .center_freq = 2484, .hw_value = 14, .max_power = 20,
	  .flags = IEEE80211_CHAN_NO_HT40 | IEEE80211_CHAN_NO_OFDM },
};

static struct ieee80211_supported_band mt6628_2ghz_band = {
	.channels = mt6628_2ghz_channels,
	.n_channels = ARRAY_SIZE(mt6628_2ghz_channels),
	.bitrates = mt6628_2ghz_rates,
	.n_bitrates = ARRAY_SIZE(mt6628_2ghz_rates),
	.ht_cap = mt6628_ht_cap,
};

/*
 * UNII-2 (5260-5725 MHz) and UNII-2 extended are deliberately absent from
 * this table.  See mt6628_reg_dfs_guard() below for why a DFS channel must
 * not be usable here even though the hardware itself can tune one.
 */
static struct ieee80211_channel mt6628_5ghz_channels[] = {
	{ .center_freq = 5170, .hw_value = 34, .max_power = 30 },
	{ .center_freq = 5180, .hw_value = 36, .max_power = 30 },
	{ .center_freq = 5190, .hw_value = 38, .max_power = 30 },
	{ .center_freq = 5200, .hw_value = 40, .max_power = 30 },
	{ .center_freq = 5210, .hw_value = 42, .max_power = 30 },
	{ .center_freq = 5220, .hw_value = 44, .max_power = 30 },
	{ .center_freq = 5230, .hw_value = 46, .max_power = 30 },
	{ .center_freq = 5240, .hw_value = 48, .max_power = 30 },
	{ .center_freq = 5745, .hw_value = 149, .max_power = 30 },
	{ .center_freq = 5765, .hw_value = 153, .max_power = 30 },
	{ .center_freq = 5785, .hw_value = 157, .max_power = 30 },
	{ .center_freq = 5805, .hw_value = 161, .max_power = 30 },
	{ .center_freq = 5825, .hw_value = 165, .max_power = 30 },
	{ .center_freq = 5845, .hw_value = 169, .max_power = 30 },
	{ .center_freq = 5865, .hw_value = 173, .max_power = 30 },
};

static struct ieee80211_rate mt6628_5ghz_rates[] = {
	{ .bitrate = 60, .hw_value = 4 },
	{ .bitrate = 90, .hw_value = 5 },
	{ .bitrate = 120, .hw_value = 6 },
	{ .bitrate = 180, .hw_value = 7 },
	{ .bitrate = 240, .hw_value = 8 },
	{ .bitrate = 360, .hw_value = 9 },
	{ .bitrate = 480, .hw_value = 10 },
	{ .bitrate = 540, .hw_value = 11 },
};

static struct ieee80211_supported_band mt6628_5ghz_band = {
	.channels = mt6628_5ghz_channels,
	.n_channels = ARRAY_SIZE(mt6628_5ghz_channels),
	.bitrates = mt6628_5ghz_rates,
	.n_bitrates = ARRAY_SIZE(mt6628_5ghz_rates),
	.ht_cap = mt6628_ht_cap,
};

/* UNII-2 and UNII-2 extended, the bands that require radar detection. */
#define MT6628_DFS_FIRST_FREQ	5260
#define MT6628_DFS_LAST_FREQ	5725

/*
 * cfg80211 does not hold a station back from a DFS channel:
 * cfg80211_chandef_dfs_required() returns 0 for station mode, so a channel
 * flagged IEEE80211_CHAN_RADAR is still something a station can associate
 * to, and no CAC is ever run on this driver's behalf.  This driver has no
 * radar detection at all - the RDD commands the downstream tree defines
 * (CMD_ID_SET_RDD_CH, EVENT_ID_UPDATE_RDD_STATUS) are not implemented here -
 * so a DFS channel must never be usable.  The 5 GHz table above contains
 * none; this turns a future careless addition into a hard failure rather
 * than a channel this radio would radiate on without CAC.
 */
static void mt6628_reg_dfs_guard(struct ieee80211_supported_band *sband)
{
	unsigned int i;

	if (!sband)
		return;

	for (i = 0; i < sband->n_channels; i++) {
		struct ieee80211_channel *chan = &sband->channels[i];

		if (chan->center_freq < MT6628_DFS_FIRST_FREQ ||
		    chan->center_freq > MT6628_DFS_LAST_FREQ)
			continue;

		pr_warn("mt6628: disabling DFS channel %u (%d MHz), this driver performs no radar detection\n",
			chan->hw_value, chan->center_freq);
		chan->flags |= IEEE80211_CHAN_DISABLED;
	}
}

static const u32 mt6628_cipher_suites[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
};

static struct mt6628_wlan *mt6628_wlan_from_wdev(struct wireless_dev *wdev)
{
	return *(struct mt6628_wlan **)netdev_priv(wdev->netdev);
}

static int mt6628_channel_to_hif(const struct ieee80211_channel *channel,
					struct mt6628_scan_channel *dst)
{
	if (!channel || !dst)
		return -EINVAL;

	switch (channel->band) {
	case NL80211_BAND_2GHZ:
		if (channel->hw_value < 1 || channel->hw_value > 14)
			return -EINVAL;
		dst->band = MT6628_BAND_2GHZ;
		break;
	case NL80211_BAND_5GHZ:
		dst->band = MT6628_BAND_5GHZ;
		break;
	default:
		return -EOPNOTSUPP;
	}

	dst->channel = channel->hw_value;
	return 0;
}

static u16 mt6628_scan_dwell_time_tu(
	const struct cfg80211_scan_request *request)
{
	u64 tu;

	if (request->duration <= 0)
		return MT6628_SCAN_DWELL_TIME_TU;

	/* Firmware uses TU (1024 us), cfg80211 uses milliseconds. */
	tu = DIV_ROUND_UP_ULL((u64)request->duration * 1000, 1024);
	tu = clamp_t(u64, tu, 1, U16_MAX);

	return (u16)tu;
}

static int mt6628_scan_send_chunk(struct mt6628_wlan *wl,
				  struct cfg80211_scan_request *request)
{
	struct mt6628_scan_cmd *cmd;
	unsigned int i;
	unsigned int start;
	unsigned int n_channels;
	unsigned int max_channels;
	unsigned int ssid_type;
	size_t cmd_len;
	bool passive;
	u8 seq;
	int ret;

	mutex_lock(&wl->cfg_mutex);
	if (wl->scan_req != request) {
		mutex_unlock(&wl->cfg_mutex);
		return -ECANCELED;
	}
	start = wl->scan_chan_idx;
	if (start >= request->n_channels) {
		mutex_unlock(&wl->cfg_mutex);
		return -EINVAL;
	}
	max_channels = min_t(unsigned int, request->n_channels - start,
					     MT6628_SCAN_MAX_CHANNELS);
	passive = !!(request->channels[start]->flags & IEEE80211_CHAN_NO_IR);
	n_channels = 0;
	while (n_channels < max_channels) {
		bool channel_passive =
			(request->channels[start + n_channels]->flags &
			 IEEE80211_CHAN_NO_IR);

		if (n_channels && channel_passive != passive)
			break;

		n_channels++;
	}
	mutex_unlock(&wl->cfg_mutex);

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	for (i = 0; i < n_channels; i++) {
		ret = mt6628_channel_to_hif(request->channels[start + i],
					    &cmd->channels[i]);
		if (ret)
			goto out_free;
	}

	ssid_type = request->n_ssids ? MT6628_SCAN_SSID_SPECIFIC :
		MT6628_SCAN_SSID_WILDCARD;
	cmd->network_type = 0;
	cmd->scan_type = passive ? MT6628_SCAN_TYPE_PASSIVE :
		MT6628_SCAN_TYPE_ACTIVE;
	cmd->ssid_type = ssid_type;
	cmd->probe_delay_time = cpu_to_le16(0);
	cmd->channel_dwell_time = cpu_to_le16(
		mt6628_scan_dwell_time_tu(request));
	cmd->channel_type = MT6628_SCAN_CHANNEL_SPECIFIED;
	cmd->channel_list_num = n_channels;
	cmd->ie_len = cpu_to_le16(request->ie_len);

	for (i = 0; i < request->n_ssids; i++) {
		cmd->ssids[i].len = cpu_to_le32(request->ssids[i].ssid_len);
		memcpy(cmd->ssids[i].ssid, request->ssids[i].ssid,
		       request->ssids[i].ssid_len);
	}
	if (request->ie_len)
		memcpy(cmd->ie, request->ie, request->ie_len);

	cmd_len = offsetof(struct mt6628_scan_cmd, ie) + request->ie_len;

	mutex_lock(&wl->cfg_mutex);
	if (wl->scan_req != request) {
		mutex_unlock(&wl->cfg_mutex);
		ret = -ECANCELED;
		goto out_free;
	}

	seq = ++wl->scan_seq;
	if (!seq)
		seq = ++wl->scan_seq;
	wl->scan_chan_idx = start + n_channels;
	mutex_unlock(&wl->cfg_mutex);

	cmd->seq_num = seq;
	ret = mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_SCAN_REQ_V2, 1,
					   cmd, cmd_len, NULL, 0, NULL, 0, 0);

out_free:
	kfree(cmd);
	return ret;
}

static void mt6628_scan_work(struct work_struct *work)
{
	struct mt6628_wlan *wl = container_of(work, struct mt6628_wlan,
						   scan_work);
	struct cfg80211_scan_request *request;
	struct cfg80211_scan_info info = {
		.aborted = true,
	};
	bool report = false;
	int ret;

	mutex_lock(&wl->cfg_mutex);
	request = wl->scan_req;
	mutex_unlock(&wl->cfg_mutex);
	if (!request)
		return;

	ret = mt6628_scan_send_chunk(wl, request);
	if (!ret)
		return;

	mutex_lock(&wl->cfg_mutex);
	if (wl->scan_req == request) {
		wl->scan_req = NULL;
		wl->scan_done_pending = false;
		report = true;
	}
	mutex_unlock(&wl->cfg_mutex);

	if (report)
		cfg80211_scan_done(request, &info);
}

static int mt6628_scan_start(struct wiphy *wiphy,
				     struct cfg80211_scan_request *request)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_wdev(request->wdev);
	int ret;

	if (!wl->runtime_started || !wl->fw_running)
		return -ENODEV;
	if (request->n_ssids > MT6628_SCAN_MAX_SSIDS)
		return -E2BIG;
	if (!request->n_channels)
		return -EINVAL;
	if (request->ie_len > MT6628_SCAN_MAX_IE_LEN)
		return -E2BIG;

	mutex_lock(&wl->cfg_mutex);
	if (wl->scan_req) {
		mutex_unlock(&wl->cfg_mutex);
		ret = -EBUSY;
		return ret;
	}

	wl->scan_req = request;
	wl->scan_chan_idx = 0;
	wl->scan_done_pending = false;
	mutex_unlock(&wl->cfg_mutex);

	ret = mt6628_scan_send_chunk(wl, request);
	if (ret) {
		mutex_lock(&wl->cfg_mutex);
		if (wl->scan_req == request)
			wl->scan_req = NULL;
		mutex_unlock(&wl->cfg_mutex);
	}

	return ret;
}

static void mt6628_abort_scan_locked(struct mt6628_wlan *wl,
					struct cfg80211_scan_request **request,
					u8 *seq)
{
	*request = wl->scan_req;
	*seq = wl->scan_seq;
	wl->scan_req = NULL;
	wl->scan_done_pending = false;
}

void mt6628_cfg80211_abort_scan(struct mt6628_wlan *wl)
{
	struct cfg80211_scan_request *request;
	struct mt6628_scan_cancel_cmd cmd = {};
	struct cfg80211_scan_info info = {
		.aborted = true,
	};
	u8 seq;

	mutex_lock(&wl->cfg_mutex);
	mt6628_abort_scan_locked(wl, &request, &seq);
	mutex_unlock(&wl->cfg_mutex);
	cancel_work_sync(&wl->scan_work);

	if (!request)
		return;

	cmd.seq_num = seq;
	if (wl->runtime_started && wl->fw_running)
		mt6628_wlan_send_cmd(wl, MT6628_CMD_ID_SCAN_CANCEL, 1,
					 &cmd, sizeof(cmd), NULL, 0, NULL, 0, 0);

	cfg80211_scan_done(request, &info);
}

static void mt6628_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_wdev(wdev);

	mt6628_cfg80211_abort_scan(wl);
}

static int mt6628_cfg80211_add_key(struct wiphy *wiphy,
				    struct wireless_dev *wdev, int link_id,
				    u8 key_index, bool pairwise,
				    const u8 *mac_addr,
				    struct key_params *params)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_wdev(wdev);

	if (link_id != -1)
		return -EOPNOTSUPP;

	return mt6628_wlan_add_key(wl, key_index, pairwise,
				   pairwise && params->mode != NL80211_KEY_NO_TX,
				   mac_addr, params);
}

static int mt6628_cfg80211_del_key(struct wiphy *wiphy,
				    struct wireless_dev *wdev, int link_id,
				    u8 key_index, bool pairwise,
				    const u8 *mac_addr)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_wdev(wdev);

	if (link_id != -1)
		return -EOPNOTSUPP;

	return mt6628_wlan_del_key(wl, key_index, pairwise, mac_addr);
}

static int mt6628_cfg80211_set_power_mgmt(struct wiphy *wiphy,
					struct net_device *dev,
					bool enabled, int timeout)
{
	struct mt6628_wlan *wl;

	if (timeout < -1)
		return -EINVAL;

	wl = netdev_priv(dev) ? *(struct mt6628_wlan **)netdev_priv(dev) : NULL;
	if (!wl)
		return -ENODEV;

	return mt6628_wlan_set_power_mgmt(wl, enabled);
}

static int mt6628_cfg80211_mgmt_tx(struct wiphy *wiphy,
				   struct wireless_dev *wdev,
				   struct cfg80211_mgmt_tx_params *params,
				   u64 *cookie)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_wdev(wdev);
	u64 tx_cookie;
	int ret;

	if (!params->buf || params->len < sizeof(struct ieee80211_hdr))
		return -EINVAL;

	tx_cookie = ++wl->mgmt_tx_cookie;
	if (!tx_cookie)
		tx_cookie = ++wl->mgmt_tx_cookie;
	*cookie = tx_cookie;

	ret = mt6628_wlan_mgmt_tx(wl, params->buf, params->len,
				  !params->dont_wait_for_ack,
				  !params->dont_wait_for_ack);
	if (ret)
		return ret;

	/*
	 * Report the real outcome.  mgmt_tx() has already waited for the
	 * firmware to confirm the frame by the time it returns zero, and
	 * the firmware reports the 802.11 ACK result in the TX_DONE event,
	 * so an unacknowledged frame is a genuine transmission failure and
	 * must not be reported as a success.
	 */
	if (!params->dont_wait_for_ack)
		cfg80211_mgmt_tx_status(wdev, tx_cookie, params->buf,
					params->len, ret == 0, GFP_KERNEL);

	return 0;
}

/*
 * Convert a firmware RCPI to dBm, matching RCPI_TO_dBm() in the
 * downstream driver: (min(rcpi, 220) >> 1) - 110.
 */
static int mt6628_rcpi_to_dbm(u8 rcpi)
{
	return min_t(int, rcpi, 220) / 2 - 110;
}

static int mt6628_rcpi_to_mbm(u8 rcpi)
{
	return mt6628_rcpi_to_dbm(rcpi) * 100;
}

/*
 * How long to wait before trying again to hand back a listen window's
 * channel after the firmware refused the abort.  The abort is a plain
 * command, so the ordinary command timeout already bounds each attempt.
 */
#define MT6628_ROC_RELEASE_RETRY_MS	1000

/*
 * A listen window that has been retired but whose channel the firmware has
 * not released.  mt6628_roc_finish() has already dropped its cookie by the
 * time this can be true, so cfg80211 can no longer ask for that window: only
 * a retry of our own gives the grant back.
 */
static bool mt6628_roc_release_pending(struct mt6628_wlan *wl)
{
	return !wl->roc_cookie && wl->channel_token &&
		wl->channel_req_type == MT6628_CH_REQ_TYPE_P2P_LISTEN;
}

/*
 * Retire a listen window whose cookie the caller has already matched:
 * hand the channel back, then drop the window's state, then tell cfg80211
 * the window it was tracking is over.
 *
 * The ordering here is the fix for a stale cancel tearing down a newer ROC.
 * cfg80211 hands our own cookie straight back to us and this counter is the
 * only source of new ones, so dropping the cookie before the release finished
 * would let a remain_on_channel() arriving during that release take the very
 * same cookie -- and then have the cancel that was still in flight release
 * that new window's channel and expire it.  Keeping the cookie across the
 * release turns such a request into a plain -EBUSY, and it also keeps the
 * firmware channel token from being aborted twice.
 *
 * The cookie is dropped only once the channel has actually been released, so a
 * window is always retired exactly once and the timer that expires it is
 * never left with nothing to expire.
 */
static void mt6628_roc_finish(struct mt6628_wlan *wl, u64 cookie,
			      struct ieee80211_channel *chan)
{
	unsigned long retry;
	int ret;

	mutex_lock(&wl->cfg_mutex);
	if (wl->roc_cookie != cookie) {
		mutex_unlock(&wl->cfg_mutex);
		return;
	}
	mutex_unlock(&wl->cfg_mutex);

	/*
	 * Give the channel back.  Without this the firmware would still be
	 * holding the grant and the next remain_on_channel request, or a
	 * reconnect, would be refused.  The cookie stays set across this
	 * firmware command on purpose, see above.
	 */
	ret = mt6628_wlan_release_channel(wl);

	mutex_lock(&wl->cfg_mutex);
	if (wl->roc_cookie == cookie) {
		wl->roc_cookie = 0;
		wl->roc_n_chans = 0;
		wl->roc_duration = 0;

		/*
		 * mt6628_wlan_release_channel() only clears the firmware
		 * token when the abort was accepted, so a failure here leaves
		 * the grant live.  Re-arm the expiry so the window is retried
		 * rather than waiting on a cfg80211 cancel that may never
		 * arrive: the cookie is 0 again, so if that cancel does come it
		 * is rejected with -ENOENT and this timer is what gives the
		 * channel back.
		 */
		if (ret) {
			retry = msecs_to_jiffies(MT6628_ROC_RELEASE_RETRY_MS);
			mod_delayed_work(system_wq, &wl->roc_work, retry);
		}
	}
	mutex_unlock(&wl->cfg_mutex);

	if (ret)
		dev_warn(&wl->func->dev,
			 "remain-on-channel %llu still holds its channel: %d\n",
			 cookie, ret);

	cfg80211_remain_on_channel_expired(&wl->wdev, cookie, chan,
					   GFP_KERNEL);
}

static void mt6628_roc_expire(struct mt6628_wlan *wl, u64 cookie)
{
	struct ieee80211_channel *chan = NULL;

	mutex_lock(&wl->cfg_mutex);
	if (wl->roc_cookie != cookie) {
		mutex_unlock(&wl->cfg_mutex);
		return;
	}
	if (wl->roc_n_chans)
		chan = wl->roc_chans[0];
	mutex_unlock(&wl->cfg_mutex);

	mt6628_roc_finish(wl, cookie, chan);
}

static int mt6628_roc_cancel(struct mt6628_wlan *wl, u64 cookie)
{
	struct ieee80211_channel *chan = NULL;
	bool active;

	mutex_lock(&wl->cfg_mutex);
	/*
	 * cfg80211 passes back the cookie it was given, so retire the window
	 * only if that cookie is still the live one: a stale cancel must not
	 * tear down a newer listen window.  The state is deliberately left
	 * alone here and dropped by mt6628_roc_finish() once the channel has
	 * been handed back, which is what keeps this cookie from being handed
	 * out again while the cancel is still in flight.
	 */
	active = wl->roc_cookie && wl->roc_cookie == cookie;
	if (active && wl->roc_n_chans)
		chan = wl->roc_chans[0];
	mutex_unlock(&wl->cfg_mutex);

	if (!active)
		return -ENOENT;

	mt6628_roc_finish(wl, cookie, chan);
	return 0;
}

static void mt6628_roc_work(struct work_struct *work)
{
	struct mt6628_wlan *wl = container_of(to_delayed_work(work),
					     struct mt6628_wlan, roc_work);
	u64 cookie;

	mutex_lock(&wl->cfg_mutex);
	/*
	 * Nothing to do if the window has already been handed back.  Copying
	 * the cookie out here and matching it again later would race a cancel
	 * that is between its match and its release; dropping the event is
	 * safe because that window is fully retired by then either way.
	 */
	if (!wl->roc_cookie) {
		/*
		 * The other reason to be here is the retry that
		 * mt6628_roc_finish() armed because the firmware refused to
		 * release the channel.  Its cookie is 0 already, so there is no
		 * window left for cfg80211 to cancel and no other path back to
		 * the abort.
		 */
		if (mt6628_roc_release_pending(wl)) {
			unsigned long retry;

			mutex_unlock(&wl->cfg_mutex);
			mt6628_wlan_release_channel(wl);

			mutex_lock(&wl->cfg_mutex);
			if (mt6628_roc_release_pending(wl)) {
				retry = msecs_to_jiffies(MT6628_ROC_RELEASE_RETRY_MS);
				mod_delayed_work(system_wq, &wl->roc_work,
						 retry);
			}
			mutex_unlock(&wl->cfg_mutex);
			return;
		}

		mutex_unlock(&wl->cfg_mutex);
		return;
	}

	cookie = wl->roc_cookie;
	mutex_unlock(&wl->cfg_mutex);

	mt6628_roc_expire(wl, cookie);
}

static int mt6628_cfg80211_remain_on_channel(struct wiphy *wiphy,
					     struct wireless_dev *wdev,
					     struct ieee80211_channel *chan,
					     unsigned int duration,
					     u64 *cookie)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_wdev(wdev);
	unsigned long timeout_ms;
	u64 new_cookie;
	int ret;

	if (!wl || !chan)
		return -EINVAL;

	if (duration > wiphy->max_remain_on_channel_duration)
		return -EINVAL;

	/*
	 * Only one listen-class channel can be granted at a time on this
	 * hardware; cfg80211 only ever asks for one.  The check and the cookie
	 * allocation are both under cfg_mutex, so two requests racing through
	 * nl80211 cannot observe the same free cookie value and hand it out
	 * twice.
	 */
	mutex_lock(&wl->cfg_mutex);
	if (wl->roc_cookie) {
		mutex_unlock(&wl->cfg_mutex);
		return -EBUSY;
	}

	new_cookie = wl->roc_cookie + 1;
	if (!new_cookie)
		new_cookie = 1;
	mutex_unlock(&wl->cfg_mutex);

	ret = mt6628_wlan_ch_privilege(wl, chan, NULL,
					MT6628_CH_REQ_TYPE_P2P_LISTEN, true);
	if (ret)
		return ret;

	mutex_lock(&wl->cfg_mutex);
	wl->roc_cookie = new_cookie;
	wl->roc_chans[0] = chan;
	wl->roc_n_chans = 1;
	wl->roc_duration = duration;
	*cookie = new_cookie;
	mutex_unlock(&wl->cfg_mutex);

	cfg80211_ready_on_channel(&wl->wdev, *cookie, chan, duration, GFP_KERNEL);

	/*
	 * The firmware grants the channel for an interval of its own
	 * choosing, which may be shorter than the time we asked for, so
	 * expire on what it actually granted.
	 */
	timeout_ms = wl->channel_grant_ms ? wl->channel_grant_ms :
		     (duration ? duration : 1000);
	mod_delayed_work(system_wq, &wl->roc_work,
			 msecs_to_jiffies(timeout_ms));
	return 0;
}

static int mt6628_cfg80211_cancel_remain_on_channel(struct wiphy *wiphy,
						    struct wireless_dev *wdev,
						    u64 cookie)
{
	struct mt6628_wlan *wl = mt6628_wlan_from_wdev(wdev);

	if (!wl)
		return -EINVAL;

	cancel_delayed_work_sync(&wl->roc_work);
	return mt6628_roc_cancel(wl, cookie);
}

static int mt6628_cfg80211_get_station(struct wiphy *wiphy,
				       struct wireless_dev *wdev,
				       const u8 *mac,
				       struct station_info *sinfo)
{
	struct mt6628_wlan *wl =
		*(struct mt6628_wlan **)netdev_priv(wdev->netdev);
	struct mt6628_event_sta_statistics stats;
	u32 scaled;
	__le16 link_speed;
	bool connected;

	if (!wl || !wdev->netdev)
		return -ENODEV;

	mutex_lock(&wl->cfg_mutex);
	connected = wl->conn_state == MT6628_CONN_CONNECTED;

	/* Station mode has exactly one peer: the AP we are associated with. */
	if (connected && mac && !is_zero_ether_addr(mac))
		connected = ether_addr_equal(wl->conn_bssid, mac);
	mutex_unlock(&wl->cfg_mutex);

	if (!connected)
		return -ENOLINK;

	memset(&stats, 0, sizeof(stats));
	if (mt6628_wlan_get_sta_statistics(wl, &stats))
		return -EOPNOTSUPP;

	sinfo->filled = BIT_ULL(NL80211_STA_INFO_SIGNAL) |
			 BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_TX_RETRIES) |
			 BIT_ULL(NL80211_STA_INFO_TX_FAILED);

	/* The firmware reports RCPI, which is not a dBm value. */
	sinfo->signal = mt6628_rcpi_to_dbm(stats.rcpi);

	sinfo->tx_packets = le32_to_cpu(stats.tx_count);
	sinfo->tx_retries = le32_to_cpu(stats.tx_life_timeout_count);

	/*
	 * tx_fail_count is the firmware's own count of frames that exhausted
	 * their retries and were never acknowledged.  It is a separate counter
	 * from tx_life_timeout_count above and is reported independently by
	 * the vendor's own reader (nic_cmd_event.c:1737), so the two are not
	 * double-counting the same frames.
	 */
	sinfo->tx_failed = le32_to_cpu(stats.tx_fail_count);

	/*
	 * TX bitrate.  link_speed is documented "unit is 0.5 Mbits"
	 * (nic_cmd_event.h:1690); the vendor scales it by 5000 to reach bps
	 * (nic_cmd_event.c:598) and then divides by 1000 to reach the 100
	 * kbit/s unit cfg80211 wants (gl_cfg80211.c:429-433).  Those two steps
	 * cancel to a factor of 5, so 0.5 Mbit/s per unit becomes 500 kbit/s
	 * per unit here.  Reported as a plain 802.11abg rate: u4PhyMode is an
	 * ENUM_PHY_MODE_T index (wlan_lib.h:391-427) but the vendor ships no
	 * table converting it to a rate, so there is no honest MCS/BW
	 * description to publish.
	 */
	link_speed = le16_to_cpu(stats.link_speed);
	if (link_speed) {
		/*
		 * txrate.legacy is a u16 in 100 kbit/s units, so the scaled
		 * value has to be clamped rather than allowed to wrap.  The
		 * fastest rate this radio can negotiate is far below the
		 * clamp; it is a guard against a corrupt firmware field,
		 * not a reachable limit.
		 */
		scaled = (u32)link_speed * 5;
		if (scaled > U16_MAX)
			scaled = U16_MAX;
		sinfo->txrate.legacy = scaled;
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
	}

	/*
	 * rx_bytes and tx_bytes are not filled: EVENT_ID_STA_STATISTICS
	 * carries no byte counters at all (it holds u4TxCount, u4TxFailCount,
	 * u4TxLifeTimeoutCount and u4TxDoneAirTime - all packet or airtime
	 * counts, nic_cmd_event.h:1690-1723), and no command in the downstream
	 * Query set returns one for a station.  Likewise connected_time: the
	 * event has no field for it, and the vendor's own get_station() does
	 * not report it either.
	 */
	return 0;
}

static const struct cfg80211_ops mt6628_cfg80211_ops = {
	.scan = mt6628_scan_start,
	.abort_scan = mt6628_abort_scan,
	.connect = mt6628_cfg80211_connect,
	.disconnect = mt6628_cfg80211_disconnect,
	.add_key = mt6628_cfg80211_add_key,
	.del_key = mt6628_cfg80211_del_key,
	.set_power_mgmt = mt6628_cfg80211_set_power_mgmt,
	.mgmt_tx = mt6628_cfg80211_mgmt_tx,
	.get_station = mt6628_cfg80211_get_station,
	.remain_on_channel = mt6628_cfg80211_remain_on_channel,
	.cancel_remain_on_channel = mt6628_cfg80211_cancel_remain_on_channel,
};

static int mt6628_rx_channel(const struct mt6628_hif_rx_hdr *hdr)
{
	unsigned int channel = hdr->hw_channel_num;

	if (channel > 241)
		channel -= 241;

	return channel;
}

static int mt6628_rx_frequency(struct mt6628_wlan *wl,
				       const struct mt6628_hif_rx_hdr *hdr)
{
	int channel = mt6628_rx_channel(hdr);
	int band;

	if (hdr->hw_channel_num <= 14)
		band = NL80211_BAND_2GHZ;
	else
		band = NL80211_BAND_5GHZ;

	if (!channel)
		return 0;

	if (!wl->wiphy->bands[band])
		return 0;

	return ieee80211_channel_to_frequency(channel, band);
}

void mt6628_cfg80211_mgmt_rx_done(struct mt6628_wlan *wl)
{
	struct cfg80211_scan_request *request = NULL;

	mutex_lock(&wl->cfg_mutex);
	if (wl->scan_done_pending && wl->scan_req &&
	    atomic_read(&wl->mgmt_pending) == 0) {
		request = wl->scan_req;
		wl->scan_req = NULL;
		wl->scan_done_pending = false;
	}
	mutex_unlock(&wl->cfg_mutex);

	if (request) {
		struct cfg80211_scan_info info = {
			.aborted = false,
		};

		cfg80211_scan_done(request, &info);
	}
}

void mt6628_cfg80211_mgmt_handler(struct mt6628_wlan *wl,
					  struct sk_buff *skb)
{
	struct mt6628_hif_rx_hdr *hdr;
	struct ieee80211_mgmt *mgmt;
	struct cfg80211_scan_request *request;
	struct cfg80211_bss *bss;
	struct ieee80211_channel *channel;
	unsigned int offset;
	unsigned int frame_len;
	int freq;

	if (mt6628_cfg80211_connection_mgmt(wl, skb))
		return;

	mutex_lock(&wl->cfg_mutex);
	request = wl->scan_req;
	mutex_unlock(&wl->cfg_mutex);
	if (!request)
		goto out;

	if (skb->len < MT6628_HIF_RX_HEADER_LEN)
		goto out;

	hdr = (struct mt6628_hif_rx_hdr *)skb->data;
	offset = hdr->header_len_offset & GENMASK(1, 0);
	if (le16_to_cpu(hdr->packet_len) <
	    MT6628_HIF_RX_HEADER_LEN + offset)
		goto out;

	frame_len = le16_to_cpu(hdr->packet_len) -
		MT6628_HIF_RX_HEADER_LEN - offset;
	if (frame_len < sizeof(struct ieee80211_hdr) ||
	    frame_len > skb->len - MT6628_HIF_RX_HEADER_LEN - offset)
		goto out;

	mgmt = (struct ieee80211_mgmt *)
		(skb->data + MT6628_HIF_RX_HEADER_LEN + offset);
	if (!ieee80211_is_beacon(mgmt->frame_control) &&
	    !ieee80211_is_probe_resp(mgmt->frame_control))
		goto out;

	freq = mt6628_rx_frequency(wl, hdr);
	if (!freq)
		goto out;

	channel = ieee80211_get_channel(wl->wiphy, freq);
	if (!channel)
		goto out;

	bss = cfg80211_inform_bss_frame(wl->wiphy, channel, mgmt, frame_len,
					       mt6628_rcpi_to_mbm(hdr->rcpi),
					       GFP_KERNEL);
	if (bss)
		cfg80211_put_bss(wl->wiphy, bss);

out:
	kfree_skb(skb);
}

bool mt6628_cfg80211_event_handler(struct mt6628_wlan *wl,
					   struct sk_buff *skb)
{
	struct mt6628_wifi_event_hdr *event;
	const struct mt6628_event_scan_done *scan_done;
	const struct mt6628_event_bss_beacon_timeout *beacon_timeout;
	struct cfg80211_scan_request *request;
	bool next_chunk = false;
	size_t packet_len, body_len;

	if (skb->len < MT6628_WIFI_EVENT_HEADER_LEN)
		goto drop;

	event = (struct mt6628_wifi_event_hdr *)skb->data;
	packet_len = le16_to_cpu(event->packet_len);
	if (packet_len < MT6628_WIFI_EVENT_HEADER_LEN ||
	    packet_len > skb->len)
		goto drop;

	body_len = packet_len - MT6628_WIFI_EVENT_HEADER_LEN;

	if (event->eid == MT6628_EVENT_ID_BSS_BEACON_TIMEOUT) {
		if (body_len != sizeof(*beacon_timeout))
			goto drop;

		beacon_timeout = (const struct mt6628_event_bss_beacon_timeout *)
			(skb->data + MT6628_WIFI_EVENT_HEADER_LEN);
		if (beacon_timeout->net_type_index == 0)
			mt6628_cfg80211_fw_beacon_timeout(wl);

		kfree_skb(skb);
		return true;
	}

	if (event->eid == MT6628_EVENT_ID_SEND_DEAUTH) {
		const struct mt6628_event_send_deauth *hdr;
		u8 addr1[ETH_ALEN], addr2[ETH_ALEN];
		__le16 frame_ctrl;

		if (body_len != sizeof(*hdr))
			goto drop;

		hdr = (const struct mt6628_event_send_deauth *)
			(skb->data + MT6628_WIFI_EVENT_HEADER_LEN);
		frame_ctrl = hdr->frame_control;
		ether_addr_copy(addr1, hdr->addr1);
		ether_addr_copy(addr2, hdr->addr2);

		kfree_skb(skb);
		mt6628_cfg80211_fw_send_deauth(wl, frame_ctrl, addr1, addr2);
		return true;
	}

	/*
	 * RX_ADDBA / RX_DELBA: unsolicited Block Ack session notifications.
	 * Handled outside the scan path because they arrive on their own,
	 * whether or not a scan is running.  The handler validates the body
	 * before reading it and never touches the skb, so the free stays
	 * here and this is a plain claim, exactly like the cases above.
	 */
	if (mt6628_wlan_handle_rx_ba_event(wl, event->eid,
					    skb->data + MT6628_WIFI_EVENT_HEADER_LEN,
					    body_len)) {
		kfree_skb(skb);
		return true;
	}

	if (event->eid != MT6628_EVENT_ID_SCAN_DONE)
		return false;
	if (body_len != sizeof(*scan_done))
		goto drop;

	scan_done = (const struct mt6628_event_scan_done *)
		(skb->data + MT6628_WIFI_EVENT_HEADER_LEN);

	mutex_lock(&wl->cfg_mutex);
	request = wl->scan_req;
	if (request && scan_done->seq_num == wl->scan_seq) {
		if (wl->scan_chan_idx < request->n_channels) {
			next_chunk = true;
		} else {
			wl->scan_done_pending = true;
			request = NULL;
		}
	} else {
		request = NULL;
	}
	mutex_unlock(&wl->cfg_mutex);

	if (next_chunk) {
		schedule_work(&wl->scan_work);
		kfree_skb(skb);
		return true;
	}

	mt6628_cfg80211_mgmt_rx_done(wl);

	kfree_skb(skb);
	return true;

drop:
	kfree_skb(skb);
	return true;
}

/*
 * Called by the regulatory core whenever the active domain changes.  The
 * channel tables are static and shared, and cfg80211 has already written the
 * resulting flags and max_reg_power into them by the time this runs, so
 * there is nothing to reprogram here - only the DFS guard, which has to run
 * again in case a later domain re-enabled a channel the guard disabled.
 */
static void mt6628_reg_notifier(struct wiphy *wiphy,
				struct regulatory_request *request)
{
	if (WARN_ON(!wiphy))
		return;

	mt6628_reg_dfs_guard(&mt6628_5ghz_band);
}

int mt6628_cfg80211_init(struct mt6628_wlan *wl)
{
	int ret;

	wl->wiphy = wiphy_new_nm(&mt6628_cfg80211_ops, 0, "mt6628");
	if (!wl->wiphy)
		return -ENOMEM;

	set_wiphy_dev(wl->wiphy, &wl->func->dev);
	INIT_WORK(&wl->scan_work, mt6628_scan_work);
	wl->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wl->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wl->wiphy->max_remain_on_channel_duration = 30000;
	wl->wiphy->max_scan_ssids = MT6628_SCAN_MAX_SSIDS;
	wl->wiphy->max_scan_ie_len = MT6628_SCAN_MAX_IE_LEN;
	wl->wiphy->bands[NL80211_BAND_2GHZ] = &mt6628_2ghz_band;
	wl->wiphy->bands[NL80211_BAND_5GHZ] = &mt6628_5ghz_band;
	wl->wiphy->cipher_suites = mt6628_cipher_suites;
	wl->wiphy->n_cipher_suites = ARRAY_SIZE(mt6628_cipher_suites);
	wl->wiphy->reg_notifier = mt6628_reg_notifier;

	/*
	 * No custom regulatory domain and no country code are claimed here, so
	 * cfg80211 and CRDA own the channel set and the transmit power.
	 *
	 * A country IE is ignored: this board carries no verifiable country,
	 * and letting an AP define this radio's regulatory domain would let a
	 * neighbour decide what we are allowed to transmit.
	 *
	 * Beacon hints are deliberately left ENABLED.  They cannot compromise
	 * DFS safety - regulatory_hint_found_beacon() returns early on a
	 * IEEE80211_CHAN_RADAR channel (net/wireless/reg.c:3658) - and on 2.4 GHz
	 * they are restricted to channels 12/13/14 (freq_is_chan_12_13_14()).
	 * Without them, and with the world fallback active whenever no
	 * regulatory.db is present, every 5 GHz channel stays NO_IR and the
	 * 5 GHz band would be unusable on such a system.
	 *
	 * REGULATORY_STRICT_REG is likewise not set: with no driver-set regd,
	 * ignore_reg_update() would discard every later regulatory change and
	 * pin the radio to the world fallback permanently.
	 */
	wl->wiphy->regulatory_flags = REGULATORY_COUNTRY_IE_IGNORE;

	/*
	 * Channel power in the tables above is the hardware ceiling only;
	 * wiphy_register() clamps it to the active domain's max_eirp.  The DFS
	 * guard runs here as well as from the notifier, so a DFS channel is
	 * never usable even in the window before the first domain is applied.
	 */
	mt6628_reg_dfs_guard(&mt6628_5ghz_band);

	wl->wdev.wiphy = wl->wiphy;
	wl->wdev.iftype = NL80211_IFTYPE_STATION;
	mt6628_cfg80211_connect_init(wl);

	ret = wiphy_register(wl->wiphy);
	if (ret) {
		mt6628_cfg80211_connect_deinit(wl);
		wiphy_free(wl->wiphy);
		wl->wiphy = NULL;
	}

	return ret;
}

void mt6628_cfg80211_deinit(struct mt6628_wlan *wl)
{
	if (!wl->wiphy)
		return;

	cancel_work_sync(&wl->scan_work);
	mt6628_cfg80211_connect_deinit(wl);
	wiphy_unregister(wl->wiphy);
	wiphy_free(wl->wiphy);
	wl->wiphy = NULL;
	wl->wdev.wiphy = NULL;
}
