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
#include <linux/fs.h>
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
#define FM_TASK_EVENT_PKT_TYPE		0x4
#define FM_ENABLE_OPCODE		0x07
#define FM_FSPI_READ_OPCODE		0x03
#define FM_FSPI_WRITE_OPCODE		0x04
#define FM_TUNE_OPCODE			0x09
#define FM_SEEK_OPCODE			0x0a
#define FM_PATCH_DOWNLOAD_OPCODE	0x12
#define FM_COEFF_DOWNLOAD_OPCODE	0x13

#define FM_REG_CHIP_ID			0x62
#define FM_REG_ROM_VERSION		0x83
#define FM_REG_ROM_CTRL			0x61
#define FM_REG_CG_CTRL			0x60
#define FM_REG_RSSI_IND			0x6c
#define FM_REG_FORCE_MS			0x75
#define FM_FORCE_MS			0x0008
#define FM_STEREO_IND			BIT(12)

#define FM_PATCH_SEG_LEN		512
#define FM_CMD_TIMEOUT_MS		3000

struct mtk_fm {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct mt6628_wmt *wmt;
	struct completion cmd_done;
	struct mutex cmd_lock;
	struct mutex power_lock;
	u8 waiting_opcode;
	int cmd_status;
	u8 cmd_data[4];
	size_t cmd_data_len;
	u32 freq;			/* in 10 kHz units */
	unsigned int users;
	bool powered;
};

static void mtk_fm_rx(void *priv, const u8 *buf, size_t len)
{
	struct mtk_fm *fm = priv;
	u16 payload_len;

	if (len < 4 || buf[0] != FM_TASK_EVENT_PKT_TYPE)
		return;

	payload_len = get_unaligned_le16(buf + 2);
	if (payload_len != len - 4)
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

	ret = request_firmware(&fw, name, fm->dev);
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

static int mtk_fm_download_versioned(struct mtk_fm *fm, u8 opcode,
				     unsigned int rom, const char *kind)
{
	const struct firmware *fw;
	char name[64];
	unsigned int version;
	int ret;

	/*
	 * Match the downstream selection policy:
	 * use the ROM-specific image when present, otherwise fall back
	 * to the newest available image.
	 */
	version = rom + 1;
	snprintf(name, sizeof(name),
		 "mediatek/mt6628/mt6628_fm_v%u_%s.bin",
		 version, kind);

	ret = request_firmware(&fw, name, fm->dev);
	if (!ret) {
		release_firmware(fw);
		return mtk_fm_download(fm, opcode, name);
	}

	if (ret != -ENOENT)
		return ret;

	for (version = 5; version > 0; version--) {
		if (version == rom + 1)
			continue;

		snprintf(name, sizeof(name),
			 "mediatek/mt6628/mt6628_fm_v%u_%s.bin",
			 version, kind);

		ret = request_firmware(&fw, name, fm->dev);
		if (!ret) {
			release_firmware(fw);
			dev_warn(fm->dev,
				 "ROM v%u %s firmware missing, using v%u\n",
				 rom + 1, kind, version);
			return mtk_fm_download(fm, opcode, name);
		}

		if (ret != -ENOENT)
			return ret;
	}

	return -ENOENT;
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

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_CTRL, &val);
	if (ret)
		return ret;

	val &= ~BIT(15);
	ret = mtk_fm_write_reg(fm, FM_REG_ROM_CTRL, val);
	if (ret)
		return ret;

	ret = mtk_fm_read_reg(fm, FM_REG_ROM_CTRL, &val);
	if (ret)
		return ret;

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
	u16 chip_id;
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

	ret = mtk_fm_download_versioned(fm, FM_PATCH_DOWNLOAD_OPCODE,
					rom, "patch");
	if (ret)
		goto out_free;

	ret = mtk_fm_download_versioned(fm, FM_COEFF_DOWNLOAD_OPCODE,
					rom, "coeff");
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
		fm->freq = 8750;

out_free:
	kfree(buf);
	return ret;
}

static int mtk_fm_power_down(struct mtk_fm *fm)
{
	u8 buf[128] = {};
	int pkt = 4;
	int ret;

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_ENABLE_OPCODE;

	/* Disable HW clock control. */
	pkt += fm_bop_write(0x60, 0x330f,
			    buf + pkt, sizeof(buf) - pkt);

	/* Reset ASIP. */
	pkt += fm_bop_write(0x61, 0x0001,
			    buf + pkt, sizeof(buf) - pkt);

	/* Reset the digital core and digital RGF. */
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x6e, 0xfff8, 0x0000,
			     buf + pkt, sizeof(buf) - pkt);

	/* Disable all clocks and reset RGF/RF. */
	pkt += fm_bop_write(0x60, 0x0000,
			    buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_write(0x60, 0x4000,
			    buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_write(0x60, 0x0000,
			    buf + pkt, sizeof(buf) - pkt);

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_ENABLE_OPCODE, 3000);
	return ret;
}

static int mtk_fm_power_get(struct mtk_fm *fm)
{
	int ret;

	mutex_lock(&fm->power_lock);

	if (fm->users) {
		fm->users++;
		mutex_unlock(&fm->power_lock);
		return 0;
	}

	ret = mt6628_wmt_func_ctrl(fm->wmt, MT6628_WMT_FUNC_FM, true);
	if (ret)
		goto out_unlock;

	ret = mtk_fm_power_up(fm);
	if (ret) {
		mtk_fm_power_down(fm);
		mt6628_wmt_func_ctrl(fm->wmt,
				     MT6628_WMT_FUNC_FM, false);
		goto out_unlock;
	}

	fm->powered = true;
	fm->users = 1;
	ret = 0;

out_unlock:
	mutex_unlock(&fm->power_lock);
	return ret;
}

static void mtk_fm_power_put(struct mtk_fm *fm)
{
	int ret;

	mutex_lock(&fm->power_lock);

	if (!fm->users) {
		mutex_unlock(&fm->power_lock);
		return;
	}

	fm->users--;
	if (fm->users) {
		mutex_unlock(&fm->power_lock);
		return;
	}

	if (fm->powered) {
		ret = mtk_fm_power_down(fm);
		if (ret)
			dev_warn(fm->dev,
				 "FM power-down failed: %d\n", ret);
		fm->powered = false;
	}

	ret = mt6628_wmt_func_ctrl(fm->wmt,
				   MT6628_WMT_FUNC_FM, false);
	if (ret)
		dev_warn(fm->dev,
			 "FM function-off failed: %d\n", ret);

	mutex_unlock(&fm->power_lock);
}

static int mtk_fm_open(struct file *file)
{
	struct mtk_fm *fm = video_drvdata(file);
	int ret;

	ret = nonseekable_open(file_inode(file), file);
	if (ret)
		return ret;

	return mtk_fm_power_get(fm);
}

static int mtk_fm_release(struct file *file)
{
	struct mtk_fm *fm = video_drvdata(file);

	mtk_fm_power_put(fm);
	return 0;
}

struct mtk_fm_chan_para {
	u16 freq;
	u8 value;
};

/*
 * MT6628's downstream chan_para_map is indexed in 5 kHz units.
 * Keep only the non-zero entries here; all other entries are zero.
 */
static const struct mtk_fm_chan_para mtk_fm_chan_para_map[] = {
	{  7680, 1 },
	{  7690, 1 },
	{  8000, 8 },
	{  8210, 1 },
	{  8450, 1 },
	{  8460, 1 },
	{  8470, 1 },
	{  9210, 1 },
	{  9220, 1 },
	{  9230, 1 },
	{  9450, 1 },
	{  9460, 1 },
	{  9470, 1 },
	{  9480, 1 },
	{  9500, 1 },
	{  9510, 1 },
	{  9520, 1 },
	{  9550, 2 },
	{  9590, 1 },
	{  9600, 8 },
	{  9840, 2 },
	{  9980, 1 },
	{  9990, 1 },
	{ 10030, 1 },
	{ 10260, 2 },
	{ 10400, 8 },
	{ 10500, 1 },
	{ 10510, 1 },
	{ 10520, 1 },
	{ 10750, 1 },
	{ 10760, 1 },
	{ 10770, 1 },
};

static const u16 mtk_fm_mcu_desense_channels[] = {
	7630, 7800, 7940, 8320, 9260,
	9600, 9710, 9920, 10400, 10410,
};

static const u16 mtk_fm_gps_desense_channels[] = {
	7850, 7860,
};

static u8 mtk_fm_get_chan_para(u32 freq)
{
	unsigned int i;

	if (freq < 7600 || freq > 10800)
		return 0;

	freq = 7600 + DIV_ROUND_CLOSEST(freq - 7600, 5) * 5;

	for (i = 0; i < ARRAY_SIZE(mtk_fm_chan_para_map); i++)
		if (mtk_fm_chan_para_map[i].freq == freq)
			return mtk_fm_chan_para_map[i].value;

	return 0;
}

static bool mtk_fm_freq_in_list(u32 freq, const u16 *list, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (list[i] == freq)
			return true;

	return false;
}

static void mtk_fm_update_desense(struct mtk_fm *fm, u32 freq)
{
	int ret;

	ret = mt6628_wmt_dsns_ctrl(
		fm->wmt,
		mtk_fm_freq_in_list(freq, mtk_fm_mcu_desense_channels,
				    ARRAY_SIZE(mtk_fm_mcu_desense_channels)) ?
		MT6628_WMT_DSNS_FM_ENABLE :
		MT6628_WMT_DSNS_FM_DISABLE);
	if (ret)
		dev_warn(fm->dev, "failed to update FM MCU desense: %d\n", ret);

	ret = mt6628_wmt_dsns_ctrl(
		fm->wmt,
		mtk_fm_freq_in_list(freq, mtk_fm_gps_desense_channels,
				    ARRAY_SIZE(mtk_fm_gps_desense_channels)) ?
		MT6628_WMT_DSNS_FM_GPS_ENABLE :
		MT6628_WMT_DSNS_FM_GPS_DISABLE);
	if (ret)
		dev_warn(fm->dev, "failed to update FM GPS desense: %d\n", ret);
}

static int mtk_fm_tune(struct mtk_fm *fm, u32 freq)
{
	u8 buf[64] = {};
	u16 tune_value;
	u8 chan_para;
	int pkt = 4;
	int ret;

	if (freq < 7600 || freq > 10800)
		return -EINVAL;

	mtk_fm_update_desense(fm, freq);

	tune_value = (freq - 6400) * 2 / 10;
	chan_para = mtk_fm_get_chan_para(freq);

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_TUNE_OPCODE;

	/*
	 * FM_CHANNEL_SET = 0x65
	 * [9:0]  desired channel
	 * [15:12] ATJ/HL/FA channel parameters
	 */
	pkt += fm_bop_modify(0x65, 0xfc00, tune_value,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x65, 0x0fff, chan_para << 12,
			     buf + pkt, sizeof(buf) - pkt);

	/* Enable the hardware-controlled tuning sequence. */
	pkt += fm_bop_modify(0x63, 0xfff8, 0x0001,
			     buf + pkt, sizeof(buf) - pkt);

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_TUNE_OPCODE, 5000);
	if (ret < 0)
		goto restore_desense;

	/*
	 * MT6628's FM event parser reports TUNE_DONE as a one-byte payload
	 * whose value must be 1.
	 */
	if (fm->cmd_data_len != 1 || fm->cmd_data[0] != 1)
	{
		ret = -EIO;
		goto restore_desense;
	}

	fm->freq = freq;
	return 0;

restore_desense:
	/*
	 * Desense state belongs to the actual tuned channel.  If the
	 * hardware tune fails, restore the state for the previous channel.
	 */
	mtk_fm_update_desense(fm, fm->freq);
	return ret;
}

static u16 mtk_fm_seek_spacing_code(u32 spacing)
{
	/*
	 * MT6628 supports 50/100/200 kHz seek spacing.  V4L2 passes
	 * spacing in Hz and permits the driver to select the nearest
	 * supported value.
	 */
	if (!spacing || spacing < 75000)
		return 0x1000;		/* 50 kHz */
	if (spacing < 150000)
		return 0x2000;		/* 100 kHz */

	return 0x4000;			/* 200 kHz */
}

static int mtk_fm_seek(struct mtk_fm *fm, bool seek_upward,
		       bool wrap_around, u32 rangelow, u32 rangehigh,
		       u32 spacing)
{
	u32 min_freq;
	u32 max_freq;
	u32 original_freq = fm->freq;
	u16 min_chan;
	u16 max_chan;
	u16 result;
	u16 spacing_code;
	u8 buf[128] = {};
	int pkt = 4;
	int ret;
	bool repositioned = false;

	/*
	 * With V4L2_TUNER_CAP_LOW the range is expressed in 62.5 Hz
	 * units.  MT6628 uses 10 kHz units internally.
	 */
	if (!rangelow)
		rangelow = 76 * 16000;
	if (!rangehigh)
		rangehigh = 108 * 16000;

	if (rangelow < 76 * 16000 ||
	    rangehigh > 108 * 16000 ||
	    rangelow > rangehigh)
		return -EINVAL;

	min_freq = DIV_ROUND_CLOSEST(rangelow, 160);
	max_freq = DIV_ROUND_CLOSEST(rangehigh, 160);

	if (min_freq < 7600 || max_freq > 10800 ||
	    min_freq > max_freq)
		return -EINVAL;

	/*
	 * VIDIOC_S_HW_FREQ_SEEK requires the current frequency to be
	 * inside the requested band before searching.
	 */
	if (fm->freq < min_freq || fm->freq > max_freq) {
		u32 freq = clamp_t(u32, fm->freq, min_freq, max_freq);

		ret = mtk_fm_tune(fm, freq);
		if (ret)
			return ret;

		repositioned = true;
	}

	min_chan = (min_freq - 6400) * 2 / 10;
	max_chan = (max_freq - 6400) * 2 / 10;
	spacing_code = mtk_fm_seek_spacing_code(spacing);

	buf[0] = FM_TASK_COMMAND_PKT_TYPE;
	buf[1] = FM_SEEK_OPCODE;

	/*
	 * FM_MAIN_CFG1 (0x66):
	 *   bit 10      seek direction
	 *   bits 14:12  channel spacing
	 *   bit 11      wrap
	 *   bits 9:0    upper search bound
	 *
	 * FM_MAIN_CFG2 (0x67):
	 *   bits 9:0    lower search bound
	 */
	pkt += fm_bop_modify(0x66, 0xfbff,
			     seek_upward ? 0x0000 : 0x0400,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x66, 0x8fff, spacing_code,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x66, 0xf7ff,
			     wrap_around ? 0x0800 : 0x0000,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x66, 0xfc00, max_chan,
			     buf + pkt, sizeof(buf) - pkt);
	pkt += fm_bop_modify(0x67, 0xfc00, min_chan,
			     buf + pkt, sizeof(buf) - pkt);

	/* FM_MAIN_CTRL[2:0] = SEEK. */
	pkt += fm_bop_modify(0x63, 0xfff8, 0x0002,
			     buf + pkt, sizeof(buf) - pkt);

	if (pkt > sizeof(buf)) {
		ret = -E2BIG;
		goto restore;
	}

	put_unaligned_le16(pkt - 4, buf + 2);

	ret = mtk_fm_send_cmd(fm, buf, pkt, FM_SEEK_OPCODE, 5000);
	if (ret)
		goto restore;

	if (fm->cmd_data_len < sizeof(result)) {
		ret = -EPROTO;
		goto restore;
	}

	/*
	 * The downstream FM event parser stores seek_result as a
	 * little-endian 16-bit frequency in 10 kHz units.
	 */
	result = get_unaligned_le16(fm->cmd_data);
	if (result < min_freq || result > max_freq) {
		ret = -ENODATA;
		goto restore;
	}

	fm->freq = result;
	return 0;

restore:
	/*
	 * V4L2 requires the original frequency to be restored after
	 * an unsuccessful hardware seek.
	 */
	if (fm->freq != original_freq || repositioned) {
		int restore_ret = mtk_fm_tune(fm, original_freq);

		if (restore_ret)
			dev_warn(fm->dev,
				 "failed to restore FM frequency %u: %d\n",
				 original_freq, restore_ret);
	}

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

static int mtk_fm_g_tuner(struct file *file, void *priv,
			  struct v4l2_tuner *tuner)
{
	struct mtk_fm *fm = video_drvdata(file);
	u16 force_ms;
	u16 rssi_ind;
	int ret;

	if (tuner->index)
		return -EINVAL;

	ret = mtk_fm_read_reg(fm, FM_REG_FORCE_MS, &force_ms);
	if (ret)
		return ret;

	ret = mtk_fm_read_reg(fm, FM_REG_RSSI_IND, &rssi_ind);
	if (ret)
		return ret;

	strscpy(tuner->name, "FM", sizeof(tuner->name));
	tuner->type = V4L2_TUNER_RADIO;
	tuner->capability = V4L2_TUNER_CAP_LOW |
			    V4L2_TUNER_CAP_STEREO |
			    V4L2_TUNER_CAP_HWSEEK_BOUNDED |
			    V4L2_TUNER_CAP_HWSEEK_WRAP |
			    V4L2_TUNER_CAP_HWSEEK_PROG_LIM;
	/*
	 * With V4L2_TUNER_CAP_LOW, frequency units are 62.5 Hz.
	 * MT6628 internally uses 10 kHz units.
	 */
	tuner->rangelow = 76 * 16000;
	tuner->rangehigh = 108 * 16000;
	tuner->rxsubchans = (rssi_ind & FM_STEREO_IND) ?
			    V4L2_TUNER_SUB_STEREO :
			    V4L2_TUNER_SUB_MONO;
	tuner->audmode = (force_ms & FM_FORCE_MS) ?
			 V4L2_TUNER_MODE_MONO :
			 V4L2_TUNER_MODE_STEREO;

	return 0;
}

static int mtk_fm_s_tuner(struct file *file, void *priv,
			  const struct v4l2_tuner *tuner)
{
	struct mtk_fm *fm = video_drvdata(file);
	u16 val;
	int ret;

	if (tuner->index)
		return -EINVAL;

	switch (tuner->audmode) {
	case V4L2_TUNER_MODE_MONO:
	case V4L2_TUNER_MODE_STEREO:
		break;
	default:
		return -EINVAL;
	}

	/*
	 * MT6628's stereo/mono control register is accessed through the
	 * same clock/control window used by the downstream driver.
	 */
	ret = mtk_fm_write_reg(fm, FM_REG_CG_CTRL, 0x3007);
	if (ret)
		return ret;

	ret = mtk_fm_read_reg(fm, FM_REG_FORCE_MS, &val);
	if (ret)
		return ret;

	switch (tuner->audmode) {
	case V4L2_TUNER_MODE_MONO:
		val |= FM_FORCE_MS;
		break;
	case V4L2_TUNER_MODE_STEREO:
		val &= ~FM_FORCE_MS;
		break;
	}

	return mtk_fm_write_reg(fm, FM_REG_FORCE_MS, val);
}

static int mtk_fm_g_frequency(struct file *file, void *priv,
			      struct v4l2_frequency *frequency)
{
	struct mtk_fm *fm = video_drvdata(file);

	if (frequency->tuner)
		return -EINVAL;

	frequency->type = V4L2_TUNER_RADIO;
	frequency->frequency = fm->freq * 160;

	return 0;
}

static int mtk_fm_s_frequency(struct file *file, void *priv,
			      const struct v4l2_frequency *frequency)
{
	struct mtk_fm *fm = video_drvdata(file);
	u32 freq;

	if (frequency->tuner)
		return -EINVAL;

	if (frequency->frequency < 76 * 16000 ||
	    frequency->frequency > 108 * 16000)
		return -ERANGE;

	freq = DIV_ROUND_CLOSEST(frequency->frequency, 160);

	return mtk_fm_tune(fm, freq);
}

static int mtk_fm_enum_freq_bands(struct file *file, void *priv,
				  struct v4l2_frequency_band *band)
{
	if (band->tuner || band->index)
		return -EINVAL;

	if (band->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	band->capability = V4L2_TUNER_CAP_LOW |
			   V4L2_TUNER_CAP_STEREO |
			   V4L2_TUNER_CAP_HWSEEK_BOUNDED |
			   V4L2_TUNER_CAP_HWSEEK_WRAP |
			   V4L2_TUNER_CAP_HWSEEK_PROG_LIM;
	band->rangelow = 76 * 16000;
	band->rangehigh = 108 * 16000;
	band->modulation = V4L2_BAND_MODULATION_FM;

	return 0;
}

static int mtk_fm_s_hw_freq_seek(struct file *file, void *priv,
				 const struct v4l2_hw_freq_seek *seek)
{
	struct mtk_fm *fm = video_drvdata(file);

	if (seek->tuner || seek->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;

	return mtk_fm_seek(fm, !!seek->seek_upward,
			   !!seek->wrap_around, seek->rangelow,
			   seek->rangehigh, seek->spacing);
}

static const struct v4l2_ioctl_ops mtk_fm_ioctl_ops = {
	.vidioc_querycap	= mtk_fm_querycap,
	.vidioc_g_tuner		= mtk_fm_g_tuner,
	.vidioc_s_tuner		= mtk_fm_s_tuner,
	.vidioc_g_frequency	= mtk_fm_g_frequency,
	.vidioc_s_frequency	= mtk_fm_s_frequency,
	.vidioc_enum_freq_bands	= mtk_fm_enum_freq_bands,
	.vidioc_s_hw_freq_seek	= mtk_fm_s_hw_freq_seek,
};

static const struct v4l2_file_operations mtk_fm_fops = {
	.owner			= THIS_MODULE,
	.open			= mtk_fm_open,
	.release		= mtk_fm_release,
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

	fm->dev = &pdev->dev;
	fm->wmt = wmt;
	init_completion(&fm->cmd_done);
	mutex_init(&fm->cmd_lock);
	mutex_init(&fm->power_lock);
	fm->waiting_opcode = 0xff;

	fm->freq = 8750;	/* 87.5 MHz in 10kHz units */

	ret = mt6628_stp_register_rx(wmt, MT6628_STP_TASK_FM,
				     mtk_fm_rx, fm);
	if (ret)
		return ret;

	ret = v4l2_device_register(&pdev->dev, &fm->v4l2_dev);
	if (ret)
		goto err_unregister_rx;

	fm->vdev.v4l2_dev = &fm->v4l2_dev;
	fm->vdev.fops = &mtk_fm_fops;
	fm->vdev.ioctl_ops = &mtk_fm_ioctl_ops;
	fm->vdev.device_caps = V4L2_CAP_RADIO |
			       V4L2_CAP_TUNER |
			       V4L2_CAP_HW_FREQ_SEEK;
	fm->vdev.release = video_device_release_empty;
	video_set_drvdata(&fm->vdev, fm);

	ret = video_register_device(&fm->vdev, VFL_TYPE_RADIO, -1);
	if (ret) {
		v4l2_err(&fm->v4l2_dev, "failed to register radio: %d\n", ret);
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
	mtk_fm_power_put(fm);
	v4l2_device_unregister(&fm->v4l2_dev);
	mt6628_stp_unregister_rx(fm->wmt, MT6628_STP_TASK_FM,
				 mtk_fm_rx, fm);
}

static struct platform_driver mtk_fm_driver = {
	.probe = mtk_fm_probe,
	.remove = mtk_fm_remove,
	.driver = {
		.name = "mt6628-fm",
	},
};
module_platform_driver(mtk_fm_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 FM radio driver");
MODULE_LICENSE("GPL");
