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

#include "mtk-wlan-hif.h"
#include "mtk-wlan.h"

#define MT6628_SCAN_MAX_SSIDS		4
#define MT6628_SCAN_MAX_CHANNELS	32
#define MT6628_SCAN_MAX_IE_LEN		600
#define MT6628_SCAN_DWELL_TIME_TU	20
#define MT6628_EVENT_SCAN_DONE_LEN	4
#define MT6628_SCAN_SSID_WILDCARD	BIT(0)
#define MT6628_SCAN_SSID_SPECIFIC	BIT(2)
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
	{ .center_freq = 2484, .hw_value = 14, .max_power = 20 },
};

static struct ieee80211_supported_band mt6628_2ghz_band = {
	.channels = mt6628_2ghz_channels,
	.n_channels = ARRAY_SIZE(mt6628_2ghz_channels),
	.bitrates = mt6628_2ghz_rates,
	.n_bitrates = ARRAY_SIZE(mt6628_2ghz_rates),
};

static struct ieee80211_channel mt6628_5ghz_channels[] = {
	{ .center_freq = 5170, .hw_value = 34, .max_power = 30 },
	{ .center_freq = 5180, .hw_value = 36, .max_power = 30 },
	{ .center_freq = 5190, .hw_value = 38, .max_power = 30 },
	{ .center_freq = 5200, .hw_value = 40, .max_power = 30 },
	{ .center_freq = 5210, .hw_value = 42, .max_power = 30 },
	{ .center_freq = 5220, .hw_value = 44, .max_power = 30 },
	{ .center_freq = 5230, .hw_value = 46, .max_power = 30 },
	{ .center_freq = 5240, .hw_value = 48, .max_power = 30 },
	{ .center_freq = 5260, .hw_value = 52, .max_power = 30 },
	{ .center_freq = 5280, .hw_value = 56, .max_power = 30 },
	{ .center_freq = 5300, .hw_value = 60, .max_power = 30 },
	{ .center_freq = 5320, .hw_value = 64, .max_power = 30 },
	{ .center_freq = 5500, .hw_value = 100, .max_power = 30 },
	{ .center_freq = 5520, .hw_value = 104, .max_power = 30 },
	{ .center_freq = 5540, .hw_value = 108, .max_power = 30 },
	{ .center_freq = 5560, .hw_value = 112, .max_power = 30 },
	{ .center_freq = 5580, .hw_value = 116, .max_power = 30 },
	{ .center_freq = 5600, .hw_value = 120, .max_power = 30 },
	{ .center_freq = 5620, .hw_value = 124, .max_power = 30 },
	{ .center_freq = 5640, .hw_value = 128, .max_power = 30 },
	{ .center_freq = 5660, .hw_value = 132, .max_power = 30 },
	{ .center_freq = 5680, .hw_value = 136, .max_power = 30 },
	{ .center_freq = 5700, .hw_value = 140, .max_power = 30 },
	{ .center_freq = 5745, .hw_value = 149, .max_power = 30 },
	{ .center_freq = 5765, .hw_value = 153, .max_power = 30 },
	{ .center_freq = 5785, .hw_value = 157, .max_power = 30 },
	{ .center_freq = 5805, .hw_value = 161, .max_power = 30 },
	{ .center_freq = 5825, .hw_value = 165, .max_power = 30 },
	{ .center_freq = 5845, .hw_value = 169, .max_power = 30 },
	{ .center_freq = 5865, .hw_value = 173, .max_power = 30 },
	{ .center_freq = 5920, .hw_value = 184, .max_power = 30 },
	{ .center_freq = 5940, .hw_value = 188, .max_power = 30 },
	{ .center_freq = 5960, .hw_value = 192, .max_power = 30 },
	{ .center_freq = 5980, .hw_value = 196, .max_power = 30 },
	{ .center_freq = 6000, .hw_value = 200, .max_power = 30 },
	{ .center_freq = 6020, .hw_value = 204, .max_power = 30 },
	{ .center_freq = 6040, .hw_value = 208, .max_power = 30 },
	{ .center_freq = 6060, .hw_value = 212, .max_power = 30 },
	{ .center_freq = 6080, .hw_value = 216, .max_power = 30 },
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
};

static const u32 mt6628_cipher_suites[] = {
	WLAN_CIPHER_SUITE_CCMP,
};

static struct mt6628_wlan *mt6628_wlan_from_wdev(struct wireless_dev *wdev)
{
	return *(struct mt6628_wlan **)netdev_priv(wdev->netdev);
}

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
	cmd->channel_type = 0;
	cmd->channel_list_num = n_channels;
	cmd->ie_len = cpu_to_le16(request->ie_len);

	for (i = 0; i < request->n_ssids; i++) {
		cmd->ssids[i].len = cpu_to_le32(request->ssids[i].ssid_len);
		memcpy(cmd->ssids[i].ssid, request->ssids[i].ssid,
		       request->ssids[i].ssid_len);
	}
	if (request->ie_len)
		memcpy(cmd->ie, request->ie, request->ie_len);

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
					   cmd, sizeof(*cmd), NULL, 0, NULL, 0, 0);

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

	return mt6628_wlan_add_key(wl, key_index, pairwise, mac_addr, params);
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

static const struct cfg80211_ops mt6628_cfg80211_ops = {
	.scan = mt6628_scan_start,
	.abort_scan = mt6628_abort_scan,
	.connect = mt6628_cfg80211_connect,
	.disconnect = mt6628_cfg80211_disconnect,
	.add_key = mt6628_cfg80211_add_key,
	.del_key = mt6628_cfg80211_del_key,
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

static int mt6628_rcpi_to_mbm(u8 rcpi)
{
	int dbm = min_t(int, rcpi, 220) / 2 - 110;

	return dbm * 100;
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
	mutex_lock(&wl->cfg_mutex);
	if (wl->scan_done_pending && skb_queue_empty(&wl->mgmt_queue)) {
		request = wl->scan_req;
		wl->scan_req = NULL;
		wl->scan_done_pending = false;
	} else {
		request = NULL;
	}
	mutex_unlock(&wl->cfg_mutex);

	if (request) {
		struct cfg80211_scan_info info = {
			.aborted = false,
		};

		cfg80211_scan_done(request, &info);
	}

	kfree_skb(skb);
}

void mt6628_cfg80211_event_handler(struct mt6628_wlan *wl,
					   struct sk_buff *skb)
{
	struct mt6628_wifi_event_hdr *event;
	struct cfg80211_scan_request *request;
	struct cfg80211_scan_info info = {
		.aborted = false,
	};
	bool next_chunk = false;
	size_t packet_len, body_len;

	if (skb->len < MT6628_WIFI_EVENT_HEADER_LEN)
		goto out;

	event = (struct mt6628_wifi_event_hdr *)skb->data;
	packet_len = le16_to_cpu(event->packet_len);
	if (packet_len < MT6628_WIFI_EVENT_HEADER_LEN ||
	    packet_len > skb->len)
		goto out;

	body_len = packet_len - MT6628_WIFI_EVENT_HEADER_LEN;
	if (event->eid != MT6628_EVENT_ID_SCAN_DONE)
		goto out;
	if (body_len != MT6628_EVENT_SCAN_DONE_LEN)
		goto out;

	mutex_lock(&wl->cfg_mutex);
	request = wl->scan_req;
	if (request && event->seq_num == wl->scan_seq) {
		if (wl->scan_chan_idx < request->n_channels) {
			next_chunk = true;
		} else {
			wl->scan_done_pending = true;
			if (skb_queue_empty(&wl->mgmt_queue)) {
				wl->scan_req = NULL;
				wl->scan_done_pending = false;
			} else {
				request = NULL;
			}
		}
	} else {
		request = NULL;
	}
	mutex_unlock(&wl->cfg_mutex);

	if (next_chunk) {
		schedule_work(&wl->scan_work);
		goto out;
	}

	if (request)
		cfg80211_scan_done(request, &info);

out:
	kfree_skb(skb);
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
	wl->wiphy->max_scan_ssids = MT6628_SCAN_MAX_SSIDS;
	wl->wiphy->max_scan_ie_len = MT6628_SCAN_MAX_IE_LEN;
	wl->wiphy->bands[NL80211_BAND_2GHZ] = &mt6628_2ghz_band;
	wl->wiphy->bands[NL80211_BAND_5GHZ] = &mt6628_5ghz_band;
	wl->wiphy->cipher_suites = mt6628_cipher_suites;
	wl->wiphy->n_cipher_suites = ARRAY_SIZE(mt6628_cipher_suites);

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
