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
#include <linux/completion.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6628.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

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
#define FM_TASK_EVENT_PKT_TYPE		0x2
#define FM_ENABLE_OPCODE		0x07
#define FM_FSPI_READ_OPCODE		0x03
#define FM_FSPI_WRITE_OPCODE		0x04
#define FM_PATCH_DOWNLOAD_OPCODE	0x12
#define FM_COEFF_DOWNLOAD_OPCODE	0x13

#define FM_REG_CHIP_ID			0x62
#define FM_REG_ROM_VERSION		0x83
#define FM_REG_ROM_CTRL			0x61

#define FM_PATCH_SEG_LEN		512
#define FM_CMD_TIMEOUT_MS		3000

struct mtk_fm {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct mt6628_wmt *wmt;
	struct completion cmd_done;
	struct mutex cmd_lock;
	u8 waiting_opcode;
	int cmd_status;
	u8 cmd_data[4];
	size_t cmd_data_len;
	u32 freq;			/* in 10 kHz units */
};

static void mtk_fm_rx(void *priv, const u8 *buf, size_t len)
{
	struct mtk_fm *fm = priv;
	u16 payload_len;

	if (len < 4 || buf[0] != FM_TASK_EVENT_PKT_TYPE)
		return;

	payload_len = get_unaligned_le16(buf + 2);
	if (payload_len > len - 4)
		return;

	if (buf[1] != fm->waiting_opcode)
		return;

	fm->cmd_data_len = min_t(size_t, payload_len,
				 sizeof(fm->cmd_data));
	memcpy(fm->cmd_data, buf + 4, fm->cmd_data_len);
	fm->cmd_status = 0;
	complete(&fm->cmd_done);
}

static int mtk_fm_send_cmd(struct mtk_fm *fm, const u8 *buf, size_t len,
			   u8 opcode, unsigned int timeout_ms)
{
	unsigned long timeout;
	int ret;

	mutex_lock(&fm->cmd_lock);
	reinit_completion(&fm->cmd_done);
	fm->waiting_opcode = opcode;
	fm->cmd_status = -ETIMEDOUT;
	fm->cmd_data_len = 0;

	ret = mt6628_stp_send(fm->wmt, MT6628_STP_TASK_FM, buf, len);
	if (ret < 0)
		goto out;

	timeout = wait_for_completion_timeout(&fm->cmd_done,
					     msecs_to_jiffies(timeout_ms));
	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = fm->cmd_status;
out:
	fm->waiting_opcode = 0xff;
	mutex_unlock(&fm->cmd_lock);
	return ret;
}

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

static int fm_bop_rd_until(u8 addr, u16 mask, u16 value,
			   u8 *buf, int size)
{
	if (size < 7)
		return -1;

	buf[0] = FM_BOP_RD_UNTIL;
	buf[1] = 5;
	buf[2] = addr;
	buf[3] = mask & 0xff;
	buf[4] = mask >> 8;
	buf[5] = value & 0xff;
	buf[6] = value >> 8;

	return 7;
}

static int mtk_fm_read_reg(struct mtk_fm *fm, u8 addr, u16 *value)
{
	u8 cmd[5] = {
		FM_TASK_COMMAND_PKT_TYPE,
		FM_FSPI_READ_OPCODE,
		0x01, 0x00, addr,
	};
	int ret;

	ret = mtk_fm_send_cmd(fm, cmd, sizeof(cmd),
			      FM_FSPI_READ_OPCODE, FM_CMD_TIMEOUT_MS);
	if (ret)
		return ret;

	if (fm->cmd_data_len < 2)
		return -EPROTO;

	*value = get_unaligned_le16(fm->cmd_data);
	return 0;
}

static int mtk_fm_write_reg(struct mtk_fm *fm, u8 addr, u16 value)
{
	u8 cmd[7] = {
		FM_TASK_COMMAND_PKT_TYPE,
		FM_FSPI_WRITE_OPCODE,
		0x03, 0x00,
		addr,
		value & 0xff,
		value >> 8,
	};

	return mtk_fm_send_cmd(fm, cmd, sizeof(cmd),
			       FM_FSPI_WRITE_OPCODE, FM_CMD_TIMEOUT_MS);
}

static int mtk_fm_download(struct mtk_fm *fm, u8 opcode,
			   const char *name)
{
	const struct firmware *fw;
	u8 *cmd;
	unsigned int seg_num, seg_id;
	size_t offset = 0;
	int ret;

	ret = request_firmware(&fw, name, &fm->vdev.dev);
	if (ret)
		return ret;

	seg_num = DIV_ROUND_UP(fw->size, FM_PATCH_SEG_LEN);
	if (!seg_num || seg_num > U8_MAX) {
		ret = -EFBIG;
		goto out_release;
	}

	cmd = kmalloc(4 + 2 + FM_PATCH_SEG_LEN, GFP_KERNEL);
	if (!cmd) {
		ret = -ENOMEM;
		goto out_release;
	}

	for (seg_id = 0; seg_id < seg_num; seg_id++) {
		size_t seg_len = min_t(size_t, FM_PATCH_SEG_LEN,
				       fw->size - offset);

		cmd[0] = FM_TASK_COMMAND_PKT_TYPE;
		cmd[1] = opcode;
		put_unaligned_le16(seg_len + 2, cmd + 2);
		cmd[4] = seg_num;
		cmd[5] = seg_id;
		memcpy(cmd + 6, fw->data + offset, seg_len);

		ret = mtk_fm_send_cmd(fm, cmd, 6 + seg_len,
				      opcode, FM_CMD_TIMEOUT_MS);
		if (ret)
			break;

		offset += seg_len;
	}

	kfree(cmd);
out_release:
	release_firmware(fw);
	return ret;
}

static int mtk_fm_get_rom_version(struct mtk_fm *fm, u8 *rom)
{
	u16 val;
	int ret;

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_CTRL, &val);
	if (ret)
		return ret;

	val |= BIT(15);
	ret = mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
	if (ret)
		return ret;

	val |= BIT(1);
	val &= ~BIT(0);
	ret = mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
	if (ret)
		return ret;

	udelay(1000);

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_VERSION, &val);
	if (ret)
		return ret;

	*rom = val >> 8;

	val &= ~BIT(15);
	val &= ~0x3;
	val |= 0x1;
	return mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
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
	char patch[64];
	char coeff[64];
	u16 chip_id, val;
	u8 rom;
	int pkt, ret;

	buf = kzalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	pkt = fm_pwrup_clock_on(fm, buf, 512);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_ENABLE_OPCODE, 3000);
	if (ret)
		goto out_free;

	ret = mtk_fm_read_reg(fm, FM_REG_CHIP_ID, &chip_id);
	if (ret)
		goto out_free;
	if (chip_id != 0x6628) {
		ret = -ENODEV;
		goto out_free;
	}

	ret = mtk_fm_get_rom_version(fm, &rom);
	if (ret)
		goto out_free;
	if (rom >= 5) {
		ret = -EINVAL;
		goto out_free;
	}

	snprintf(patch, sizeof(patch),
		 "mediatek/mt6628/mt6628_fm_v%u_patch.bin", rom + 1);
	snprintf(coeff, sizeof(coeff),
		 "mediatek/mt6628/mt6628_fm_v%u_coeff.bin", rom + 1);

	ret = mtk_fm_download(fm, FM_PATCH_DOWNLOAD_OPCODE, patch);
	if (ret)
		goto out_free;

	ret = mtk_fm_download(fm, FM_COEFF_DOWNLOAD_OPCODE, coeff);
	if (ret)
		goto out_free;

	ret = mtk_fm_write_reg(fm, 0x90, 0x0040);
	if (ret)
		goto out_free;

	ret = mtk_fm_write_reg(fm, 0x90, 0x0000);
	if (ret)
		goto out_free;

	pkt = 4;
	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_ENABLE_OPCODE;
	buf[2] = 0;
	buf[3] = 0;

	pkt += fm_bop_write(0x6a, 0x2100, buf + pkt, 512 - pkt);
	pkt += fm_bop_write(0x6b, 0x2100, buf + pkt, 512 - pkt);
	pkt += fm_bop_modify(0x60, 0xfff7, 0x0008,
			     buf + pkt, 512 - pkt);
	pkt += fm_bop_modify(0x61, 0xfffd, 0x0002,
			     buf + pkt, 512 - pkt);
	pkt += fm_bop_modify(0x61, 0xfffe, 0x0000,
			     buf + pkt, 512 - pkt);
	pkt += fm_bop_udelay(200000, buf + pkt, 512 - pkt);
	pkt += fm_bop_rd_until(0x64, 0x001f, 0x0002,
			       buf + pkt, 512 - pkt);

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_ENABLE_OPCODE, 5000);
	if (!ret)
		fm->freq = 87500;

out_free:
	kfree(buf);
	return ret;
}

/* ---- V4L2 ---- */

static int mtk_fm_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, "MediaTek MT6628 FM Radio", sizeof(cap->card));
	cap->device_caps = vdev->device_caps;
	cap->capabilities = vdev->device_caps | V4L2_CAP_DEVICE_CAPS;
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
	init_completion(&fm->cmd_done);
	mutex_init(&fm->cmd_lock);
	fm->waiting_opcode = 0xff;

	fm->freq = 87500;	/* 87.5 MHz in 10kHz units */

	ret = mt6628_stp_register_rx(wmt, MT6628_STP_TASK_FM,
				     mtk_fm_rx, fm);
	if (ret)
		return ret;

	ret = v4l2_device_register(&pdev->dev, &fm->v4l2_dev);
	if (ret)
		goto err_unregister_rx;

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
	/*
	 * Tuner/frequency ioctls are not implemented yet.  Do not advertise
	 * V4L2_CAP_TUNER until VIDIOC_{G,S}_TUNER/FREQUENCY exist.
	 */
	fm->vdev.device_caps = V4L2_CAP_RADIO;
	fm->vdev.release = video_device_release_empty;
	video_set_drvdata(&fm->vdev, fm);

	ret = video_register_device(&fm->vdev, VFL_TYPE_RADIO, -1);
	if (ret) {
		v4l2_err(&fm->v4l2_dev, "failed to register radio: %d\n", ret);
		mt6628_wmt_func_ctrl(wmt, MT6628_WMT_FUNC_FM, false);
		v4l2_device_unregister(&fm->v4l2_dev);
		goto err_unregister_rx;
	}

	platform_set_drvdata(pdev, fm);

	return 0;

err_v4l2:
	v4l2_device_unregister(&fm->v4l2_dev);
err_unregister_rx:
	mt6628_stp_unregister_rx(wmt, MT6628_STP_TASK_FM,
				 mtk_fm_rx, fm);
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
	mt6628_stp_unregister_rx(fm->wmt, MT6628_STP_TASK_FM,
				 mtk_fm_rx, fm);
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
