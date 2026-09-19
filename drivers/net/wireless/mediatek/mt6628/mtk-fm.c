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
#include <linux/module.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

/* STP task type for FM traffic */
#define STP_TASK_FM		1

/* Common HIF register addresses (SDIO function 2) */
#define MTK_SDIO_CTDR			0x18
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
	struct sdio_func *func;		/* STP control function (2) */
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
	u8 *frame;
	int pkt, ret;
	size_t frame_len;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pkt = fm_pwrup_clock_on(fm, buf, 512);

	/*
	 * SDIO STP transport:
	 *   byte 0: STP signature
	 *   byte 1: task + payload length[11:8]
	 *   byte 2: payload length[7:0]
	 *   byte 3: zero on SDIO
	 *   payload
	 *   two zero CRC bytes
	 *
	 * The downstream stp_core uses exactly this format for SDIO.
	 */
	if (pkt > 0xfff) {
		kfree(buf);
		return -EMSGSIZE;
	}

	frame_len = 4 + pkt + 2;
	frame = kzalloc(frame_len, GFP_KERNEL);
	if (!frame) {
		kfree(buf);
		return -ENOMEM;
	}

	frame[0] = 0x80;
	frame[1] = (STP_TASK_FM << 4) | ((pkt >> 8) & 0x0f);
	frame[2] = pkt & 0xff;
	frame[3] = 0x00;
	memcpy(frame + 4, buf, pkt);

	sdio_claim_host(fm->func);
	ret = sdio_writesb(fm->func, MTK_SDIO_CTDR, frame, frame_len);
	sdio_release_host(fm->func);

	kfree(frame);
	kfree(buf);

	if (ret)
		return ret;

	return pkt;
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

static int mtk_fm_sdio_probe(struct sdio_func *func,
			     const struct sdio_device_id *id)
{
	struct mtk_fm *fm;
	int ret;

	if (func->num != 2) {
		dev_dbg(&func->dev, "ignoring function %d\n", func->num);
		return -ENODEV;
	}

	fm = devm_kzalloc(&func->dev, sizeof(*fm), GFP_KERNEL);
	if (!fm)
		return -ENOMEM;

	fm->func = func;
	sdio_set_drvdata(func, fm);

	fm->freq = 87500;	/* 87.5 MHz in 10kHz units */

	ret = v4l2_device_register(&func->dev, &fm->v4l2_dev);
	if (ret)
		return dev_err_probe(&func->dev, ret,
				     "failed to register v4l2 device\n");

	fm->vdev.v4l2_dev = &fm->v4l2_dev;
	fm->vdev.fops = &mtk_fm_fops;
	fm->vdev.ioctl_ops = &mtk_fm_ioctl_ops;
	fm->vdev.device_caps = V4L2_CAP_RADIO | V4L2_CAP_TUNER;
	fm->vdev.release = video_device_release_empty;
	video_set_drvdata(&fm->vdev, fm);

	ret = video_register_device(&fm->vdev, VFL_TYPE_RADIO, -1);
	if (ret) {
		v4l2_err(&fm->v4l2_dev, "failed to register radio: %d\n", ret);
		v4l2_device_unregister(&fm->v4l2_dev);
		return ret;
	}

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	sdio_release_host(func);
	if (ret) {
		dev_err_probe(&func->dev, ret, "failed to enable func\n");
		goto err_video;
	}

	ret = mtk_fm_power_up(fm);
	if (ret) {
		dev_err(&func->dev, "FM power-up failed: %d\n", ret);
		goto err_disable;
	}

	return 0;

err_disable:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
err_video:
	sdio_set_drvdata(func, NULL);
	video_unregister_device(&fm->vdev);
	v4l2_device_unregister(&fm->v4l2_dev);
	return ret;
}

static void mtk_fm_sdio_remove(struct sdio_func *func)
{
	struct mtk_fm *fm = sdio_get_drvdata(func);

	if (!fm)
		return;

	video_unregister_device(&fm->vdev);
	v4l2_device_unregister(&fm->v4l2_dev);

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	sdio_set_drvdata(func, NULL);
}

static const struct sdio_device_id mtk_fm_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_MEDIATEK, 0x6628) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, mtk_fm_sdio_ids);

static struct sdio_driver mtk_fm_driver = {
	.name = KBUILD_MODNAME,
	.probe = mtk_fm_sdio_probe,
	.remove = mtk_fm_sdio_remove,
	.id_table = mtk_fm_sdio_ids,
};
module_sdio_driver(mtk_fm_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 FM radio driver");
MODULE_LICENSE("GPL");
