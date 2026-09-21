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

#define MT6628_WLAN_TX_TC_NUM		6

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
	bool runtime_started;
	bool irq_claimed;
	bool connected;
	u8 sta_rec_idx;

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
	u8 cmd_seq_num;
	u8 *cmd_response;
	size_t cmd_response_len;
	size_t cmd_response_capacity;
	int cmd_status;
};

int mt6628_wlan_runtime_start(struct mt6628_wlan *wl);
void mt6628_wlan_runtime_stop(struct mt6628_wlan *wl);
int mt6628_wlan_query_basic_config(struct mt6628_wlan *wl);

int mt6628_wlan_send_cmd(struct mt6628_wlan *wl, u8 cid, u8 set_query,
			 const void *payload, size_t payload_len,
			 void *response, size_t response_capacity,
			 size_t *response_len, unsigned int timeout_ms);

#endif /* __MTK6628_WLAN_H */
