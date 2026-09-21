/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * MediaTek MT6628 WLAN driver shared state
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef __MTK6628_WLAN_H
#define __MTK6628_WLAN_H

#include <linux/completion.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>

#define MT6628_WLAN_TX_TC_NUM		6

/*
 * Wire values used by CMD_UPDATE_STA_RECORD_T.ucStaState.
 * The MT6628 downstream encodes STA_STATE_1/2/3 as 0/1/2.
 */
enum mt6628_sta_state {
	MT6628_STA_STATE_1,
	MT6628_STA_STATE_2,
	MT6628_STA_STATE_3,
};

struct mt6628_wlan {
	struct sdio_func *func;
	u8 seq_num;
	bool fw_running;
	bool driver_owned;

	struct work_struct irq_work;
	struct work_struct tx_work;
	struct napi_struct napi;
	struct work_struct event_work;
	struct work_struct mgmt_work;
	struct net_device *netdev;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	bool runtime_initialized;
	bool runtime_started;
	bool irq_claimed;
	bool connected;
	u8 sta_rec_idx;
	u8 conn_state;
	u8 conn_bssid[ETH_ALEN];
	u8 conn_ssid[IEEE80211_MAX_SSID_LEN];
	u8 conn_ssid_len;
	u8 conn_channel;
	u16 conn_aid;
	struct cfg80211_bss *conn_bss;
	u8 *conn_req_ie;
	size_t conn_req_ie_len;
	u8 *conn_resp_ie;
	size_t conn_resp_ie_len;
	struct delayed_work conn_timeout_work;

	struct mutex cfg_mutex;
	struct cfg80211_scan_request *scan_req;
	u8 scan_seq;
	u8 channel_token;
	bool scan_done_pending;

	spinlock_t tx_lock;
	u8 tx_free[MT6628_WLAN_TX_TC_NUM];
	u8 tx_max[MT6628_WLAN_TX_TC_NUM];
	u16 tx_seq;
	struct sk_buff_head tx_queue;
	wait_queue_head_t tx_wait;

	struct sk_buff_head rx_queue;
	struct sk_buff_head event_queue;
	struct sk_buff_head mgmt_queue;
	struct sk_buff_head async_event_queue;
	struct sk_buff_head async_mgmt_queue;
	wait_queue_head_t event_wait;
	void (*event_handler)(struct mt6628_wlan *, struct sk_buff *);
	void (*mgmt_handler)(struct mt6628_wlan *, struct sk_buff *);

	/* Runtime command/event state, used by later control-plane commits. */
	struct mutex cmd_mutex;
	spinlock_t cmd_lock;
	struct completion cmd_done;
	bool cmd_pending;
	u8 cmd_pending_seq;
	u8 cmd_pending_id;
	u8 cmd_pending_eid;
	u8 cmd_seq_num;
	u8 *cmd_response;
	size_t cmd_response_len;
	size_t cmd_response_capacity;
	int cmd_status;
};

int mt6628_wlan_runtime_start(struct mt6628_wlan *wl);
void mt6628_wlan_runtime_stop(struct mt6628_wlan *wl);
int mt6628_wlan_query_basic_config(struct mt6628_wlan *wl);

int mt6628_wlan_request_channel(struct mt6628_wlan *wl,
				const struct ieee80211_channel *channel,
				const u8 *bssid);
int mt6628_wlan_release_channel(struct mt6628_wlan *wl);
int mt6628_wlan_update_sta_record(struct mt6628_wlan *wl,
					enum mt6628_sta_state state,
					u16 assoc_id,
					const u8 *bssid);
int mt6628_wlan_set_bss_info(struct mt6628_wlan *wl, u8 channel,
				     const u8 *ssid, u8 ssid_len,
				     const u8 *bssid, bool connected);
int mt6628_wlan_activate_bss(struct mt6628_wlan *wl, bool active);
int mt6628_wlan_remove_sta_record(struct mt6628_wlan *wl,
					const u8 *bssid);
int mt6628_wlan_mgmt_tx(struct mt6628_wlan *wl, const u8 *frame,
				 size_t frame_len);

int mt6628_cfg80211_init(struct mt6628_wlan *wl);
void mt6628_cfg80211_deinit(struct mt6628_wlan *wl);
void mt6628_cfg80211_connect_init(struct mt6628_wlan *wl);
void mt6628_cfg80211_connect_deinit(struct mt6628_wlan *wl);
int mt6628_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
				struct cfg80211_connect_params *sme);
int mt6628_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *dev,
				u16 reason_code);
bool mt6628_cfg80211_connection_mgmt(struct mt6628_wlan *wl,
					     struct sk_buff *skb);
void mt6628_cfg80211_event_handler(struct mt6628_wlan *wl,
					   struct sk_buff *skb);
void mt6628_cfg80211_mgmt_handler(struct mt6628_wlan *wl,
					  struct sk_buff *skb);
void mt6628_cfg80211_abort_scan(struct mt6628_wlan *wl);

int mt6628_wlan_send_cmd(struct mt6628_wlan *wl, u8 cid, u8 set_query,
			 const void *payload, size_t payload_len,
			 void *response, size_t response_capacity,
			 size_t *response_len, u8 expected_event_id,
			 unsigned int timeout_ms);

#endif /* __MTK6628_WLAN_H */
