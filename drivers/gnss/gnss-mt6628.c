// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6628 GNSS driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The GPS receiver of the MT6628 is driven by the on-chip MCU; the host
 * only relays bytes between the STP control channel (SDIO function 2,
 * task type GPS) and userspace.  The downstream stack terminates that
 * stream in a vendor daemon (mnld); here the stream is exposed through
 * the standard GNSS framework so any NMEA consumer works.
 *
 * The chip is powered up by the WMT FUNC_ON command for the GPS driver
 * type, which the WMT/bring-up owner issues before this device sees
 * traffic.
 */

#include <linux/module.h>
#include <linux/mfd/mt6628.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/gnss.h>

struct mtk_gnss {
	struct gnss_device *gdev;
	struct mt6628_wmt *wmt;
	struct mutex lock;
	bool open;
};

static void mtk_gnss_rx(void *priv, const u8 *buf, size_t len)
{
	struct mtk_gnss *gnss = priv;

	mutex_lock(&gnss->lock);
	if (gnss->open)
		gnss_insert_raw(gnss->gdev, buf, len);
	mutex_unlock(&gnss->lock);
}

static int mtk_gnss_write_raw(struct gnss_device *gdev, const u8 *buf,
			      size_t len)
{
	struct mtk_gnss *priv = gnss_get_drvdata(gdev);
	int ret;

	ret = mt6628_stp_send(priv->wmt, MT6628_STP_TASK_GPS, buf, len);

	if (ret)
		return ret;

	return len;
}

static int mtk_gnss_open(struct gnss_device *gdev)
{
	struct mtk_gnss *priv = gnss_get_drvdata(gdev);
	int ret;

	mutex_lock(&priv->lock);
	if (priv->open) {
		mutex_unlock(&priv->lock);
		return 0;
	}

	ret = mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, true);
	if (!ret)
		priv->open = true;
	mutex_unlock(&priv->lock);

	return ret;
}

static void mtk_gnss_close(struct gnss_device *gdev)
{
	struct mtk_gnss *priv = gnss_get_drvdata(gdev);

	mutex_lock(&priv->lock);
	if (priv->open) {
		priv->open = false;
		mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, false);
	}
	mutex_unlock(&priv->lock);
}

static const struct gnss_operations mtk_gnss_ops = {
	.open		= mtk_gnss_open,
	.close		= mtk_gnss_close,
	.write_raw	= mtk_gnss_write_raw,
};

static int mtk_gnss_probe(struct platform_device *pdev)
{
	struct mtk_gnss *priv;
	struct gnss_device *gdev;
	struct mt6628_wmt *wmt;
	int ret;

	wmt = dev_get_drvdata(pdev->dev.parent);
	if (!wmt)
		return -EPROBE_DEFER;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wmt = wmt;
	mutex_init(&priv->lock);

	gdev = gnss_allocate_device(&pdev->dev);
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);

	priv->gdev = gdev;
	gdev->ops = &mtk_gnss_ops;
	gnss_set_drvdata(gdev, priv);

	ret = mt6628_stp_register_rx(wmt, MT6628_STP_TASK_GPS,
					 mtk_gnss_rx, priv);
	if (ret)
		goto err_put;

	ret = gnss_register_device(gdev);
	if (ret) {
		mt6628_stp_unregister_rx(wmt, MT6628_STP_TASK_GPS,
					 mtk_gnss_rx, priv);
		goto err_put;
	}

	platform_set_drvdata(pdev, priv);
	dev_info(&pdev->dev, "MT6628 GNSS registered\n");

	return 0;

err_put:
	gnss_put_device(gdev);
	return ret;
}

static void mtk_gnss_remove(struct platform_device *pdev)
{
	struct mtk_gnss *priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	mutex_lock(&priv->lock);
	if (priv->open) {
		priv->open = false;
		mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, false);
	}
	mutex_unlock(&priv->lock);

	mt6628_stp_unregister_rx(priv->wmt, MT6628_STP_TASK_GPS,
					 mtk_gnss_rx, priv);
	gnss_deregister_device(priv->gdev);
	gnss_put_device(priv->gdev);
}

static struct platform_driver mtk_gnss_driver = {
	.probe = mtk_gnss_probe,
	.remove_new = mtk_gnss_remove,
	.driver = {
		.name = "mt6628-gnss",
	},
};
module_platform_driver(mtk_gnss_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 GNSS driver");
MODULE_LICENSE("GPL");
