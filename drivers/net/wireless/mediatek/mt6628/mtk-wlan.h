/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * MediaTek MT6628 WLAN driver shared state
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef __MTK6628_WLAN_H
#define __MTK6628_WLAN_H

#include <linux/atomic.h>
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
#define MT6628_STA_REC_INDEX_NOT_FOUND	0xfe

/* ENUM_CH_REQ_TYPE_T from the downstream connection manager. */
#define MT6628_CH_REQ_TYPE_JOIN		0
#define MT6628_CH_REQ_TYPE_P2P_LISTEN	1

/* Connection state machine states, shared with the event paths. */
#define MT6628_CONN_DISCONNECTED	0
#define MT6628_CONN_AUTH		1
#define MT6628_CONN_ASSOC		2
#define MT6628_CONN_CONNECTED		3
#define MT6628_WLAN_KEY_INDEX_MAX	3

/* Size of the EVENT_ID_SEND_DEAUTH reply rate limiter ring. */
#define MT6628_MAX_DEAUTH_INFO_COUNT	4
#define MT6628_MIN_DEAUTH_INTERVAL_MS	500

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
	/*
	 * Runtime power ownership.  When the driver has been idle for a
	 * while it hands the chip to the firmware, which may then enter its
	 * low power state; the driver reclaims ownership on the next
	 * interrupt, transmit or command.
	 */
	bool pm_idle;
	struct delayed_work pm_work;
	/* Remain-on-channel state, including the request we granted it with. */
	u64 roc_cookie;
	struct ieee80211_channel *roc_chans[2];
	unsigned int roc_n_chans;
	unsigned int roc_duration;
	struct delayed_work roc_work;
	u8 channel_req_type;
	unsigned int channel_grant_ms;	/* firmware-granted interval */

	struct work_struct irq_work;
	struct work_struct recovery_work;
	struct delayed_work tx_work;
	struct work_struct scan_work;
	struct napi_struct napi;
	struct work_struct event_work;
	struct work_struct mgmt_work;
	struct net_device *netdev;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	bool runtime_initialized;
	bool runtime_started;
	bool irq_claimed;
	atomic_t recovery_pending;
	bool connected;
	bool conn_secure;
	u8 sta_rec_idx;
	u8 conn_state;
	u8 conn_bssid[ETH_ALEN];
	u8 conn_ssid[IEEE80211_MAX_SSID_LEN];
	u8 conn_ssid_len;
	u8 conn_band;
	u8 conn_channel;
	u16 conn_aid;
	u8 conn_auth_mode;
	u8 conn_enc_status;
	u8 conn_key_idx;
	u8 conn_rf_sco;
	u8 *conn_key;
	size_t conn_key_len;
	/*
	 * Bit per key index, tracked separately for pairwise (PTK) and
	 * group (GTK) keys so that teardown can tell the firmware to
	 * remove exactly the keys that were installed.
	 */
	u8 pairwise_key_mask;
	u8 group_key_mask;
	struct cfg80211_bss *conn_bss;
	u8 *conn_req_ie;
	size_t conn_req_ie_len;
	/*
	 * Parameters of the last requested connection, kept so a firmware
	 * reset can be recovered from by associating again on the driver's
	 * own initiative.  Cleared on an explicit disconnect or whenever the
	 * connection is otherwise abandoned.
	 */
	struct cfg80211_connect_params conn_retry;
	bool conn_retry_valid;
	struct delayed_work conn_retry_work;
	unsigned int conn_retry_count;
	u8 *conn_resp_ie;
	size_t conn_resp_ie_len;
	struct delayed_work conn_timeout_work;

	/*
	 * Rate limiter for EVENT_ID_SEND_DEAUTH replies, one entry per
	 * peer, mirroring the deauth-info ring in the downstream driver.
	 */
	struct mt6628_deauth_info {
		u8 da[ETH_ALEN];
		unsigned long last_send;
	} deauth_info[MT6628_MAX_DEAUTH_INFO_COUNT];

	struct mutex cfg_mutex;
	struct cfg80211_scan_request *scan_req;
	u8 scan_seq;
	unsigned int scan_chan_idx;
	u8 channel_token;
	bool scan_done_pending;

	spinlock_t tx_lock;
	u8 tx_free[MT6628_WLAN_TX_TC_NUM];
	u8 tx_max[MT6628_WLAN_TX_TC_NUM];
	struct sk_buff_head tx_queue;
	wait_queue_head_t tx_wait;

	struct sk_buff_head rx_queue;
	struct sk_buff_head event_queue;
	struct sk_buff_head mgmt_queue;
	struct sk_buff_head async_event_queue;
	struct sk_buff_head async_mgmt_queue;
	atomic_t mgmt_pending;
	wait_queue_head_t event_wait;
	bool (*event_handler)(struct mt6628_wlan *, struct sk_buff *);
	void (*mgmt_handler)(struct mt6628_wlan *, struct sk_buff *);

	spinlock_t mgmt_tx_lock;
	struct completion mgmt_tx_done;
	bool mgmt_tx_pending;
	u8 mgmt_tx_seq;
	u8 mgmt_tx_packet_seq;
	u64 mgmt_tx_cookie;
	int mgmt_tx_status;

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
int mt6628_wlan_force_firmware_reset(struct mt6628_wlan *wl);
int mt6628_wlan_take_driver_own(struct mt6628_wlan *wl);
int mt6628_wlan_give_firmware_own(struct mt6628_wlan *wl);
int mt6628_wlan_pm_resume(struct mt6628_wlan *wl);
void mt6628_wlan_pm_idle(struct mt6628_wlan *wl);
void mt6628_wlan_pm_busy(struct mt6628_wlan *wl);
int mt6628_wlan_reload_firmware(struct mt6628_wlan *wl);

int mt6628_wlan_ch_privilege(struct mt6628_wlan *wl,
			     const struct ieee80211_channel *channel,
			     const u8 *bssid, u8 req_type, bool require_grant);
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
int mt6628_wlan_add_key(struct mt6628_wlan *wl, u8 key_index,
			bool pairwise, bool tx_key, const u8 *mac_addr,
			const struct key_params *params);
int mt6628_wlan_del_key(struct mt6628_wlan *wl, u8 key_index,
			bool pairwise, const u8 *mac_addr);
void mt6628_wlan_flush_keys(struct mt6628_wlan *wl);
int mt6628_wlan_set_power_mgmt(struct mt6628_wlan *wl, bool enabled);
struct mt6628_event_sta_statistics;
int mt6628_wlan_get_sta_statistics(struct mt6628_wlan *wl,
				   struct mt6628_event_sta_statistics *stats);
int mt6628_wlan_mgmt_tx(struct mt6628_wlan *wl, const u8 *frame,
			 size_t frame_len, bool wait_for_status, bool need_ack);

int mt6628_cfg80211_init(struct mt6628_wlan *wl);
void mt6628_cfg80211_deinit(struct mt6628_wlan *wl);
void mt6628_cfg80211_connect_init(struct mt6628_wlan *wl);
void mt6628_roc_work(struct work_struct *work);
void mt6628_conn_schedule_retry(struct mt6628_wlan *wl);
void mt6628_cfg80211_connect_deinit(struct mt6628_wlan *wl);
int mt6628_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
				struct cfg80211_connect_params *sme);
int mt6628_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *dev,
				u16 reason_code);
bool mt6628_cfg80211_connection_mgmt(struct mt6628_wlan *wl,
					     struct sk_buff *skb);
bool mt6628_cfg80211_event_handler(struct mt6628_wlan *wl,
					   struct sk_buff *skb);
void mt6628_cfg80211_fw_beacon_timeout(struct mt6628_wlan *wl);
void mt6628_cfg80211_fw_send_deauth(struct mt6628_wlan *wl,
				    __le16 frame_ctrl, const u8 *addr1,
				    const u8 *addr2);
void mt6628_cfg80211_mgmt_handler(struct mt6628_wlan *wl,
					  struct sk_buff *skb);
void mt6628_cfg80211_mgmt_rx_done(struct mt6628_wlan *wl);
void mt6628_cfg80211_abort_scan(struct mt6628_wlan *wl);

int mt6628_wlan_send_cmd(struct mt6628_wlan *wl, u8 cid, u8 set_query,
			 const void *payload, size_t payload_len,
			 void *response, size_t response_capacity,
			 size_t *response_len, u8 expected_event_id,
			 unsigned int timeout_ms);

#endif /* __MTK6628_WLAN_H */
