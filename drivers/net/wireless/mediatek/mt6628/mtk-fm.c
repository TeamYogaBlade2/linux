// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6628 FM radio driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The FM receiver lives inside the MT6628 combo chip and is controlled
 * through the STP control channel (SDIO function 2) with the chip's
 * "basic operation" (BOP) command language: each BOP is a small
 * [opcode][size][args...] program executed by the on-chip firmware.
 *
 * Firmware: the DSP runs from mt6628_fm_rom.bin, optionally patched by
 * mt6628_fm_v<N>_patch.bin and calibrated with
 * mt6628_fm_v<N>_coeff.bin (all under /lib/firmware/mediatek/).
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/mfd/mt6628.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

/* FM BOP opcodes */
#define FM_BOP_BASE			0x80
#define FM_BOP_WRITE			(FM_BOP_BASE + 0x00)
#define FM_BOP_UDELAY			(FM_BOP_BASE + 0x01)
#define FM_BOP_RD_UNTIL			(FM_BOP_BASE + 0x02)
#define FM_BOP_MODIFY			(FM_BOP_BASE + 0x03)
#define FM_BOP_MSLEEP			(FM_BOP_BASE + 0x04)

/* FM packet types on the STP control channel */
#define FM_TASK_COMMAND_PKT_TYPE	0x1
#define FM_ENABLE_OPCODE		0x07

struct mtk_fm {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct mt6628_wmt *wmt;
	u32 freq;			/* in 10 kHz units */
};

/* ---- BOP buffer construction ---- */

static int fm_bop_write(u8 addr, u16 val, u8 *buf, int size)
{
	if (size < 5)
		return -1;

	buf[0] = FM_BOP_WRITE;
	buf[1] = 3;
	buf[2] = addr;
	buf[3] = val & 0xff;
	buf[4] = val >> 8;

	return 5;
}

static int fm_bop_modify(u8 addr, u16 mask_and, u16 mask_or,
			 u8 *buf, int size)
{
	if (size < 7)
		return -1;

	buf[0] = FM_BOP_MODIFY;
	buf[1] = 5;
	buf[2] = addr;
	buf[3] = mask_and & 0xff;
	buf[4] = mask_and >> 8;
	buf[5] = mask_or & 0xff;
	buf[6] = mask_or >> 8;

	return 7;
}

static int fm_bop_udelay(u32 us, u8 *buf, int size)
{
	if (size < 6)
		return -1;

	buf[0] = FM_BOP_UDELAY;
	buf[1] = 4;
	buf[2] = us & 0xff;
	buf[3] = (us >> 8) & 0xff;
	buf[4] = (us >> 16) & 0xff;
	buf[5] = (us >> 24) & 0xff;

	return 6;
}

/* Power-up step 1: enable the FM digital clock. */
static int fm_pwrup_clock_on(struct mtk_fm *fm, u8 *buf, int bufsize)
{
	/* Match the MT6628 downstream defaults. */
	const u16 de_emphasis = 0;
	const u16 osc_freq = 0;
	int pkt = 4;

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_ENABLE_OPCODE;
	buf[2] = 0;			/* length filled in at the end */
	buf[3] = 0;

	pkt += fm_bop_write(0x60, 0x0000, buf + pkt, bufsize - pkt);
	pkt += fm_bop_write(0x60, 0x0001, buf + pkt, bufsize - pkt);
	pkt += fm_bop_udelay(3000, buf + pkt, bufsize - pkt);
	pkt += fm_bop_write(0x60, 0x0003, buf + pkt, bufsize - pkt);
	pkt += fm_bop_write(0x60, 0x0007, buf + pkt, bufsize - pkt);
	pkt += fm_bop_modify(0x70, 0xffbf, 0x0040, buf + pkt, bufsize - pkt);
	/* no low-power mode, analog line-in, long antenna */
	pkt += fm_bop_modify(0x61, 0xff63, 0x0000, buf + pkt, bufsize - pkt);
	pkt += fm_bop_modify(0x61, 0xefff, de_emphasis << 12,
			     buf + pkt, bufsize - pkt);
	pkt += fm_bop_modify(0x60, 0xff8f, osc_freq << 4,
			     buf + pkt, bufsize - pkt);

	/* payload length for the packet header */
	buf[2] = (pkt - 4) & 0xff;
	buf[3] = (pkt - 4) >> 8;

	return pkt;
}

static int mtk_fm_power_up(struct mtk_fm *fm)
{
	u8 *buf;
	int pkt, ret;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pkt = fm_pwrup_clock_on(fm, buf, 512);

	ret = mt6628_stp_send(fm->wmt, MT6628_STP_TASK_FM, buf, pkt);
	kfree(buf);

	return ret < 0 ? ret : 0;
}

/* ---- V4L2 ---- */

static int mtk_fm_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, "MediaTek MT6628 FM Radio", sizeof(cap->card));
	return 0;
}

static const struct v4l2_ioctl_ops mtk_fm_ioctl_ops = {
	.vidioc_querycap	= mtk_fm_querycap,
};

static const struct v4l2_file_operations mtk_fm_fops = {
	.owner			= THIS_MODULE,
	.unlocked_ioctl		= video_ioctl2,
};

static int mtk_fm_probe(struct platform_device *pdev)
{
	struct mtk_fm *fm;
	struct mt6628_wmt *wmt;
	int ret;

	wmt = dev_get_drvdata(pdev->dev.parent);
	if (!wmt)
		return -EPROBE_DEFER;

	fm = devm_kzalloc(&pdev->dev, sizeof(*fm), GFP_KERNEL);
	if (!fm)
		return -ENOMEM;

	fm->wmt = wmt;

	fm->freq = 87500;	/* 87.5 MHz in 10kHz units */

	ret = v4l2_device_register(&pdev->dev, &fm->v4l2_dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register v4l2 device\n");

	ret = mt6628_wmt_func_ctrl(wmt, MT6628_WMT_FUNC_FM, true);
	if (ret)
		goto err_v4l2;

	ret = mtk_fm_power_up(fm);
	if (ret) {
		dev_err(&pdev->dev, "FM power-up failed: %d\n", ret);
		mt6628_wmt_func_ctrl(wmt, MT6628_WMT_FUNC_FM, false);
		goto err_v4l2;
	}

	fm->vdev.v4l2_dev = &fm->v4l2_dev;
	fm->vdev.fops = &mtk_fm_fops;
	fm->vdev.ioctl_ops = &mtk_fm_ioctl_ops;
	fm->vdev.device_caps = V4L2_CAP_RADIO | V4L2_CAP_TUNER;
	fm->vdev.release = video_device_release_empty;
	video_set_drvdata(&fm->vdev, fm);

	ret = video_register_device(&fm->vdev, VFL_TYPE_RADIO, -1);
	if (ret) {
		v4l2_err(&fm->v4l2_dev, "failed to register radio: %d\n", ret);
		mt6628_wmt_func_ctrl(wmt, MT6628_WMT_FUNC_FM, false);
		v4l2_device_unregister(&fm->v4l2_dev);
		return ret;
	}

	platform_set_drvdata(pdev, fm);

	return 0;

err_v4l2:
	v4l2_device_unregister(&fm->v4l2_dev);
	return ret;
}

static void mtk_fm_remove(struct platform_device *pdev)
{
	struct mtk_fm *fm = platform_get_drvdata(pdev);

	if (!fm)
		return;

	video_unregister_device(&fm->vdev);
	v4l2_device_unregister(&fm->v4l2_dev);
	mt6628_wmt_func_ctrl(fm->wmt, MT6628_WMT_FUNC_FM, false);
}

static struct platform_driver mtk_fm_driver = {
	.probe = mtk_fm_probe,
	.remove_new = mtk_fm_remove,
	.driver = {
		.name = "mt6628-fm",
	},
};
module_platform_driver(mtk_fm_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 FM radio driver");
MODULE_LICENSE("GPL");
