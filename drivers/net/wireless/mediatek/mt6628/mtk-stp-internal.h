/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __MTK_MT6628_STP_INTERNAL_H
#define __MTK_MT6628_STP_INTERNAL_H

#include <linux/device.h>
#include <linux/mfd/mt6628.h>

/* Transport implementation details; public clients use mt6628.h only. */
struct mt6628_stp_transport_ops {
	enum mt6628_stp_hif hif;
	int (*send)(void *context, enum mt6628_stp_task task,
		    const void *buf, size_t len);
	int (*register_rx)(void *context, enum mt6628_stp_task task,
			   mt6628_stp_rx_cb cb, void *priv);
	void (*unregister_rx)(void *context, enum mt6628_stp_task task,
			      mt6628_stp_rx_cb cb, void *priv);
	int (*wmt_func_ctrl)(void *context, enum mt6628_wmt_func func,
			     bool on);
	int (*wmt_gps_sync_ctrl)(void *context, bool on);
	int (*wmt_dsns_ctrl)(void *context, enum mt6628_wmt_dsns type);
};

int mt6628_stp_register_transport(struct device *dev, void *context,
				  const struct mt6628_stp_transport_ops *ops);
void mt6628_stp_unregister_transport(void *context);

/* Returns true only for the backend selected by mediatek,bgf-hif. */
bool mt6628_stp_transport_selected(struct device *dev, const char *transport);

#endif /* __MTK_MT6628_STP_INTERNAL_H */
