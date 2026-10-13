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
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>

#include <linux/gnss.h>

struct mtk_gnss {
	struct gnss_device *gdev;
	struct mt6628_wmt *wmt;
	struct mutex lock;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_default;
	struct pinctrl_state *gps_sync;
	bool open;
	bool resume_pending;
};

static void mtk_gnss_rx(void *priv, const u8 *buf, size_t len)
{
	struct mtk_gnss *gnss = priv;

	mutex_lock(&gnss->lock);
	if (gnss->open) {
		int ret;

		dev_dbg(&gnss->gdev->dev, "STP GPS RX: %zu bytes\n", len);
		ret = gnss_insert_raw(gnss->gdev, buf, len);
		if (ret >= 0 && (size_t)ret < len)
			dev_warn_ratelimited(&gnss->gdev->dev,
					     "GNSS RX FIFO full; dropped %zu of %zu bytes\n",
					     len - (size_t)ret, len);
	} else {
		dev_dbg(&gnss->gdev->dev,
			"discarding STP GPS data while GNSS is closed: %zu bytes\n",
			len);
	}
	mutex_unlock(&gnss->lock);
}

static int mtk_gnss_write_raw(struct gnss_device *gdev, const u8 *buf,
			      size_t len)
{
	struct mtk_gnss *priv = gnss_get_drvdata(gdev);
	int ret;

	if (!READ_ONCE(priv->open))
		return -EHOSTDOWN;

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
	mutex_unlock(&priv->lock);

	if (priv->gps_sync) {
		ret = pinctrl_select_state(priv->pinctrl,
					   priv->gps_sync);
		if (ret)
			return ret;
	}

	ret = mt6628_wmt_gps_sync_ctrl(priv->wmt, true);
	if (ret)
		goto err_restore_pinctrl;

	ret = mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, true);
	if (ret) {
		mt6628_wmt_gps_sync_ctrl(priv->wmt, false);
		goto err_restore_pinctrl;
	}

	mutex_lock(&priv->lock);
	WRITE_ONCE(priv->open, true);
	priv->resume_pending = false;
	mutex_unlock(&priv->lock);

	return 0;

err_restore_pinctrl:
	if (priv->pinctrl_default)
		pinctrl_select_state(priv->pinctrl, priv->pinctrl_default);

	return ret;
}

static void mtk_gnss_close(struct gnss_device *gdev)
{
	struct mtk_gnss *priv = gnss_get_drvdata(gdev);
	bool was_open;

	mutex_lock(&priv->lock);
	was_open = priv->open;
	WRITE_ONCE(priv->open, false);
	priv->resume_pending = false;
	mutex_unlock(&priv->lock);

	if (was_open) {
		mt6628_wmt_gps_sync_ctrl(priv->wmt, false);
		if (priv->pinctrl_default)
			pinctrl_select_state(priv->pinctrl,
					     priv->pinctrl_default);
		mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, false);
	}
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

	priv->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(priv->pinctrl)) {
		ret = PTR_ERR(priv->pinctrl);
		if (ret == -ENODEV)
			priv->pinctrl = NULL;
		else
			return ret;
	}

	if (priv->pinctrl) {
		priv->pinctrl_default = pinctrl_lookup_state(priv->pinctrl,
						     PINCTRL_STATE_DEFAULT);
		if (IS_ERR(priv->pinctrl_default))
			priv->pinctrl_default = NULL;

		priv->gps_sync = pinctrl_lookup_state(priv->pinctrl,
						      "gps-sync");
		if (IS_ERR(priv->gps_sync)) {
			dev_dbg(&pdev->dev,
				"GPS_SYNC pinctrl state not provided\n");
			priv->gps_sync = NULL;
		}
	}

	gdev = gnss_allocate_device(&pdev->dev);
	if (!gdev)
		return -ENOMEM;

	priv->gdev = gdev;
	gdev->ops = &mtk_gnss_ops;
	gdev->type = GNSS_TYPE_MTK;
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
	bool was_open;

	if (!priv)
		return;

	mutex_lock(&priv->lock);
	was_open = priv->open;
	WRITE_ONCE(priv->open, false);
	if (was_open)
		priv->resume_pending = true;
	mutex_unlock(&priv->lock);

	if (was_open) {
		mt6628_wmt_gps_sync_ctrl(priv->wmt, false);
		if (priv->pinctrl_default)
			pinctrl_select_state(priv->pinctrl,
					     priv->pinctrl_default);
		mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, false);
	}

	mt6628_stp_unregister_rx(priv->wmt, MT6628_STP_TASK_GPS,
					 mtk_gnss_rx, priv);
	gnss_deregister_device(priv->gdev);
	gnss_put_device(priv->gdev);
}

/*
 * Release the GPS function when the system suspends.
 *
 * Downstream does exactly this: its suspend path sets GPS_PWRCTL_OFF
 * (gps/gps.c:369-380).  The matching resume deliberately does *not* power
 * the chip back on - the comment there is "don't power on device
 * automatically" (gps/gps.c:382-396) - because the GNSS session belongs to
 * userspace, which reopens the device and reasserts the WMT FUNC_ON
 * itself.  Driving that from a resume callback instead would race that
 * open and leave the function powered with nobody attached to it.
 *
 * So this only tears down, exactly as downstream does.  A GPS fix in
 * progress does not survive a suspend; that is the upstream behaviour and
 * is also what userspace expects.
 */
static int mtk_gnss_suspend(struct device *dev)
{
	struct mtk_gnss *priv = dev_get_drvdata(dev);
	bool was_open;

	if (!priv)
		return 0;

	mutex_lock(&priv->lock);
	was_open = priv->open;
	/*
	 * Stop relaying first: the chip is about to stop answering, and any
	 * bytes that arrive on the way down belong to a session that is
	 * being torn down.
	 */
	WRITE_ONCE(priv->open, false);
	mutex_unlock(&priv->lock);

	if (!was_open)
		return 0;

	/* Same teardown order as close, and as .remove below. */
	mt6628_wmt_gps_sync_ctrl(priv->wmt, false);
	if (priv->pinctrl_default)
		pinctrl_select_state(priv->pinctrl, priv->pinctrl_default);
	mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, false);

	dev_dbg(dev, "released the GPS function for suspend\n");

	return 0;
}

/*
 * GNSS core retains its open count across suspend and does not call .open()
 * again on resume. Restore only a session that was open before suspend;
 * a client that closed its FD is not powered up automatically.
 */
static int mtk_gnss_resume(struct device *dev)
{
	struct mtk_gnss *priv = dev_get_drvdata(dev);
	bool should_resume;
	bool resumed;
	int ret;

	if (!priv)
		return 0;

	mutex_lock(&priv->lock);
	should_resume = priv->resume_pending && !priv->open;
	mutex_unlock(&priv->lock);

	if (!should_resume)
		return 0;

	if (priv->gps_sync) {
		ret = pinctrl_select_state(priv->pinctrl, priv->gps_sync);
		if (ret)
			goto err_restore_pinctrl;
	}

	ret = mt6628_wmt_gps_sync_ctrl(priv->wmt, true);
	if (ret)
		goto err_restore_pinctrl;

	ret = mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, true);
	if (ret) {
		mt6628_wmt_gps_sync_ctrl(priv->wmt, false);
		goto err_restore_pinctrl;
	}

	mutex_lock(&priv->lock);
	if (priv->resume_pending) {
		WRITE_ONCE(priv->open, true);
		priv->resume_pending = false;
	}
	resumed = priv->open;
	mutex_unlock(&priv->lock);

	/*
	 * The session may have been closed while the hardware was starting.
	 * Do not leave GPS enabled without an active GNSS session.
	 */
	if (!resumed) {
		mt6628_wmt_gps_sync_ctrl(priv->wmt, false);
		if (priv->pinctrl_default)
			pinctrl_select_state(priv->pinctrl,
					     priv->pinctrl_default);
		mt6628_wmt_func_ctrl(priv->wmt, MT6628_WMT_FUNC_GPS, false);
		return 0;
	}

	dev_dbg(dev, "restored the GNSS function after resume\n");
	return 0;

err_restore_pinctrl:
	if (priv->pinctrl_default)
		pinctrl_select_state(priv->pinctrl, priv->pinctrl_default);

	dev_err(dev, "failed to restore GNSS after resume: %d\n", ret);
	return ret;
}

static const struct dev_pm_ops mtk_gnss_pm_ops = {
	.suspend = mtk_gnss_suspend,
	.resume = mtk_gnss_resume,
};

static struct platform_driver mtk_gnss_driver = {
	.probe = mtk_gnss_probe,
	.remove = mtk_gnss_remove,
	.driver = {
		.name = "mt6628-gnss",
		.pm = &mtk_gnss_pm_ops,
	},
};
module_platform_driver(mtk_gnss_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 GNSS driver");
MODULE_LICENSE("GPL");
