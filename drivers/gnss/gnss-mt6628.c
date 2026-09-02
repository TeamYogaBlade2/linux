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
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/slab.h>

#include <linux/gnss.h>

/* Common HIF register addresses (SDIO function 2). */
#define MTK_SDIO_CTDR			0x18
#define MTK_SDIO_CRDR			0x1c

/* STP channel type for GNSS (downstream stp_exp.h numbering). */
#define STP_TASK_GPS			2

struct mtk_gnss {
	struct gnss_device *gdev;
	struct sdio_func *func;
};

static int mtk_gnss_write(struct mtk_gnss *gdev_priv, const void *buf,
			  size_t len)
{
	int ret;

	sdio_claim_host(gdev_priv->func);
	ret = sdio_memcpy_toio(gdev_priv->func, MTK_SDIO_CTDR, (void *)buf,
			       len);
	sdio_release_host(gdev_priv->func);
	return ret;
}

static int mtk_gnss_write_raw(struct gnss_device *gdev, const u8 *buf,
			      size_t len)
{
	struct mtk_gnss *priv = gnss_get_drvdata(gdev);
	u8 *frame;
	size_t frame_len = 4 + len + 2;
	int sent;

	frame = kzalloc(frame_len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	frame[0] = 0x80;
	frame[1] = (STP_TASK_GPS << 4) | ((len >> 8) & 0x0f);
	frame[2] = len & 0xff;
	frame[3] = 0x00;
	memcpy(frame + 4, buf, len);

	sent = mtk_gnss_write(priv, frame, frame_len);
	kfree(frame);

	if (sent < 0)
		return sent;
	return sent == frame_len ? len : -EIO;
}

/*
 * The gnss core calls ops->open/close unconditionally on first/last
 * file open.  The chip is powered up by the WMT owner before this
 * device is probed, so there is nothing to do here yet; the hooks are
 * where WMT FUNC_ON/OFF will be wired once the arbitration between the
 * BT/FM/GPS functions sharing this control channel is settled.
 */
static int mtk_gnss_open(struct gnss_device *gdev)
{
	return 0;
}

static void mtk_gnss_close(struct gnss_device *gdev)
{
}

static const struct gnss_operations mtk_gnss_ops = {
	.open		= mtk_gnss_open,
	.close		= mtk_gnss_close,
	.write_raw	= mtk_gnss_write_raw,
};

static int mtk_gnss_sdio_probe(struct sdio_func *func,
			       const struct sdio_device_id *id)
{
	struct mtk_gnss *priv;
	struct gnss_device *gdev;
	int ret;

	if (func->num != 2) {
		dev_dbg(&func->dev, "ignoring function %d\n", func->num);
		return -ENODEV;
	}

	priv = devm_kzalloc(&func->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->func = func;

	gdev = gnss_allocate_device(&func->dev);
	if (IS_ERR(gdev))
		return PTR_ERR(gdev);

	priv->gdev = gdev;
	gdev->ops = &mtk_gnss_ops;
	gnss_set_drvdata(gdev, priv);

	ret = gnss_register_device(gdev);
	if (ret)
		goto err_put;

	sdio_set_drvdata(func, priv);

	/*
	 * The receive path (draining STP frames of the GPS task into
	 * gnss_insert_raw) is not implemented yet: the control channel
	 * has no out-of-band interrupt wired to this driver and the
	 * polling design still needs to be validated on hardware.
	 */
	dev_info(&func->dev, "MT6628 GNSS registered\n");

	return 0;

err_put:
	gnss_put_device(gdev);
	return ret;
}

static void mtk_gnss_sdio_remove(struct sdio_func *func)
{
	struct mtk_gnss *priv = sdio_get_drvdata(func);

	if (!priv)
		return;

	gnss_deregister_device(priv->gdev);
	gnss_put_device(priv->gdev);
}

static const struct sdio_device_id mtk_gnss_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_MEDIATEK,
		      SDIO_DEVICE_ID_MEDIATEK_MT6628) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, mtk_gnss_sdio_ids);

static struct sdio_driver mtk_gnss_driver = {
	.name = KBUILD_MODNAME,
	.probe = mtk_gnss_sdio_probe,
	.remove = mtk_gnss_sdio_remove,
	.id_table = mtk_gnss_sdio_ids,
};
module_sdio_driver(mtk_gnss_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 GNSS driver");
MODULE_LICENSE("GPL");
