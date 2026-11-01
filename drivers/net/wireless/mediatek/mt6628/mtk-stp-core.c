// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6628 transport-independent STP API.
 *
 * SDIO function 2 and UART register their operations against the opaque WMT
 * context passed to MFD children.  Keeping this dispatch layer common lets
 * both transports be built at once without duplicate exported symbols.
 */

#include <linux/list.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/slab.h>

#include <linux/mfd/mt6628.h>

#include "mtk-stp-internal.h"

struct mt6628_stp_transport {
	struct list_head node;
	void *context;
	const struct mt6628_stp_transport_ops *ops;
};

static LIST_HEAD(mt6628_stp_transports);
static DECLARE_RWSEM(mt6628_stp_transports_lock);

static struct mt6628_stp_transport *
mt6628_stp_find_transport(const void *context)
{
	struct mt6628_stp_transport *transport;

	list_for_each_entry(transport, &mt6628_stp_transports, node) {
		if (transport->context == context)
			return transport;
	}

	return NULL;
}

int mt6628_stp_register_transport(struct device *dev, void *context,
				  const struct mt6628_stp_transport_ops *ops)
{
	struct mt6628_stp_transport *transport;
	int ret = 0;

	if (!dev || !context || !ops || !ops->send || !ops->register_rx ||
	    !ops->unregister_rx || !ops->wmt_func_ctrl ||
	    !ops->wmt_gps_sync_ctrl || !ops->wmt_dsns_ctrl ||
	    (ops->hif != MT6628_STP_HIF_SDIO &&
	     ops->hif != MT6628_STP_HIF_UART))
		return -EINVAL;

	transport = kzalloc(sizeof(*transport), GFP_KERNEL);
	if (!transport)
		return -ENOMEM;

	transport->context = context;
	transport->ops = ops;

	down_write(&mt6628_stp_transports_lock);
	if (mt6628_stp_find_transport(context)) {
		ret = -EEXIST;
	} else {
		list_add_tail(&transport->node, &mt6628_stp_transports);
	}
	up_write(&mt6628_stp_transports_lock);

	if (ret) {
		dev_err(dev, "MT6628 transport context already registered\n");
		kfree(transport);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_stp_register_transport);

void mt6628_stp_unregister_transport(void *context)
{
	struct mt6628_stp_transport *transport;

	if (!context)
		return;

	/* The write lock drains in-flight API calls before the backend is freed. */
	down_write(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(context);
	if (transport) {
		list_del(&transport->node);
		kfree(transport);
	}
	up_write(&mt6628_stp_transports_lock);
}
EXPORT_SYMBOL_GPL(mt6628_stp_unregister_transport);

int mt6628_stp_send(struct mt6628_wmt *wmt, enum mt6628_stp_task task,
		    const void *buf, size_t len)
{
	struct mt6628_stp_transport *transport;
	int ret = -ENODEV;

	if (!wmt || (!buf && len))
		return -EINVAL;

	down_read(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(wmt);
	if (transport)
		ret = transport->ops->send(transport->context, task, buf, len);
	up_read(&mt6628_stp_transports_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_stp_send);

int mt6628_stp_register_rx(struct mt6628_wmt *wmt,
			   enum mt6628_stp_task task,
			   mt6628_stp_rx_cb cb, void *priv)
{
	struct mt6628_stp_transport *transport;
	int ret = -ENODEV;

	if (!wmt || !cb)
		return -EINVAL;

	down_read(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(wmt);
	if (transport)
		ret = transport->ops->register_rx(transport->context, task,
						 cb, priv);
	up_read(&mt6628_stp_transports_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_stp_register_rx);

void mt6628_stp_unregister_rx(struct mt6628_wmt *wmt,
			      enum mt6628_stp_task task,
			      mt6628_stp_rx_cb cb, void *priv)
{
	struct mt6628_stp_transport *transport;

	if (!wmt)
		return;

	down_read(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(wmt);
	if (transport)
		transport->ops->unregister_rx(transport->context, task, cb, priv);
	up_read(&mt6628_stp_transports_lock);
}
EXPORT_SYMBOL_GPL(mt6628_stp_unregister_rx);

int mt6628_wmt_func_ctrl(struct mt6628_wmt *wmt,
			 enum mt6628_wmt_func func, bool on)
{
	struct mt6628_stp_transport *transport;
	int ret = -ENODEV;

	if (!wmt)
		return -EINVAL;

	down_read(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(wmt);
	if (transport)
		ret = transport->ops->wmt_func_ctrl(transport->context, func, on);
	up_read(&mt6628_stp_transports_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_wmt_func_ctrl);

int mt6628_wmt_gps_sync_ctrl(struct mt6628_wmt *wmt, bool on)
{
	struct mt6628_stp_transport *transport;
	int ret = -ENODEV;

	if (!wmt)
		return -EINVAL;

	down_read(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(wmt);
	if (transport)
		ret = transport->ops->wmt_gps_sync_ctrl(transport->context, on);
	up_read(&mt6628_stp_transports_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_wmt_gps_sync_ctrl);

int mt6628_wmt_dsns_ctrl(struct mt6628_wmt *wmt,
			 enum mt6628_wmt_dsns type)
{
	struct mt6628_stp_transport *transport;
	int ret = -ENODEV;

	if (!wmt)
		return -EINVAL;

	down_read(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(wmt);
	if (transport)
		ret = transport->ops->wmt_dsns_ctrl(transport->context, type);
	up_read(&mt6628_stp_transports_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_wmt_dsns_ctrl);

int mt6628_stp_get_hif(struct mt6628_wmt *wmt, enum mt6628_stp_hif *hif)
{
	struct mt6628_stp_transport *transport;
	int ret = -ENODEV;

	if (!wmt || !hif)
		return -EINVAL;

	down_read(&mt6628_stp_transports_lock);
	transport = mt6628_stp_find_transport(wmt);
	if (transport) {
		*hif = transport->ops->hif;
		ret = 0;
	}
	up_read(&mt6628_stp_transports_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_stp_get_hif);

MODULE_DESCRIPTION("MediaTek MT6628 transport-independent STP API");
MODULE_LICENSE("GPL");
