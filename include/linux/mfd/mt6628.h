/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MFD_MT6628_H
#define __LINUX_MFD_MT6628_H

#include <linux/types.h>

struct mt6628_wmt;

enum mt6628_stp_task {
	MT6628_STP_TASK_BT = 0,
	MT6628_STP_TASK_FM = 1,
	MT6628_STP_TASK_GPS = 2,
	MT6628_STP_TASK_WIFI = 3,
	MT6628_STP_TASK_WMT = 4,
	MT6628_STP_TASK_STP = 5,
	MT6628_STP_TASK_MAX = 6,
};

enum mt6628_wmt_func {
	MT6628_WMT_FUNC_BT = 0,
	MT6628_WMT_FUNC_FM = 1,
	MT6628_WMT_FUNC_GPS = 2,
};

typedef void (*mt6628_stp_rx_cb)(void *priv, const u8 *buf, size_t len);

int mt6628_stp_send(struct mt6628_wmt *wmt, enum mt6628_stp_task task,
			const void *buf, size_t len);
int mt6628_stp_register_rx(struct mt6628_wmt *wmt,
				 enum mt6628_stp_task task,
				 mt6628_stp_rx_cb cb, void *priv);
void mt6628_stp_unregister_rx(struct mt6628_wmt *wmt,
				    enum mt6628_stp_task task,
				    mt6628_stp_rx_cb cb, void *priv);
int mt6628_wmt_func_ctrl(struct mt6628_wmt *wmt,
				enum mt6628_wmt_func func, bool on);

#endif /* __LINUX_MFD_MT6628_H */
