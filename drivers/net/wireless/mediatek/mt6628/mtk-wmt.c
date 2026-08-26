// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6628 WMT (Wireless Management Tool) driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6628 is a Wi-Fi / Bluetooth / FM / GPS combo chip attached over
 * SDIO (two functions: func 1 carries the WMT/BT/FM/GPS control channel,
 * func 2 the WLAN data path).
 *
 * Control traffic uses STP (Serial Transport Protocol) framing over the
 * SDIO packet ports:
 *
 *   STP header (4 bytes)
 *     [0] = 0x80
 *     [1] = (type << 4) | ((len >> 8) & 0x0f)   type: 0=wmt,1=bt,...
 *     [2] = len & 0xff
 *     [3] = 0x00
 *   payload (len bytes)
 *   CRC (2 bytes, always zero on this generation)
 *
 * The WMT protocol runs as type 0 payloads: a 4-byte WMT header
 * [direction][opcode][length_lo][length_hi] followed by parameters.
 *
 * Bring-up sequence:
 *   1. optional vmmc regulator + ramp delay
 *   2. in-band reset (5 x 0x7f) to sync the STP state machines
 *   3. HOST_AWAKE so the chip accepts commands
 *   4. patch download from "mediatek/mt6628_patch_e2_hdr.bin"
 */

#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

/* SDIO packet ports of the control function */
#define MTK_SDIO_FMR_BASE		0x000	/* host -> chip */
#define MTK_SDIO_RFR_BASE		0x100	/* chip -> host */

/* STP channel types */
#define STP_TYPE_WMT			0
#define STP_TYPE_BT			1

#define STP_HEADER_SIZE			4
#define STP_CRC_SIZE			2
#define STP_SYNC_BYTE			0x7f

/* WMT opcodes */
enum wmt_opcode {
	WMT_OP_SLEEP		= 0x01,	/* sub-opcode in payload */
	WMT_OP_HOST_AWAKE	= 0x02,
	WMT_OP_PATCH_DL		= 0x0d,
};

struct mtk_wmt {
	struct sdio_func *func;
	struct regulator *vmmc;
	u8 tx_seq;
	bool fw_ready;
};

/* ---- low level SDIO ---- */

static int mtk_wmt_write(struct mtk_wmt *wmt, const void *buf, size_t len)
{
	int ret;

	sdio_claim_host(wmt->func);
	ret = sdio_memcpy_toio(wmt->func, MTK_SDIO_FMR_BASE,
			       (void *)buf, len);
	sdio_release_host(wmt->func);
	return ret;
}

static int mtk_wmt_read(struct mtk_wmt *wmt, void *buf, size_t len)
{
	int ret;

	sdio_claim_host(wmt->func);
	ret = sdio_memcpy_fromio(wmt->func, buf, MTK_SDIO_RFR_BASE, len);
	sdio_release_host(wmt->func);
	return ret;
}

/* ---- STP framing ---- */

static int mtk_stp_send(struct mtk_wmt *wmt, u8 type,
			const u8 *payload, size_t len)
{
	u8 frame[STP_HEADER_SIZE + 512];
	size_t frame_len = STP_HEADER_SIZE + len + STP_CRC_SIZE;

	if (len > sizeof(frame) - STP_HEADER_SIZE - STP_CRC_SIZE)
		return -EINVAL;

	frame[0] = 0x80;
	frame[1] = (type << 4) | ((len >> 8) & 0x0f);
	frame[2] = len & 0xff;
	frame[3] = 0x00;
	memcpy(frame + STP_HEADER_SIZE, payload, len);
	memset(frame + STP_HEADER_SIZE + len, 0, STP_CRC_SIZE);

	return mtk_wmt_write(wmt, frame, frame_len);
}

static int mtk_stp_recv(struct mtk_wmt *wmt, u8 type,
			u8 *payload, size_t max_len)
{
	u8 hdr[STP_HEADER_SIZE];
	u16 plen;
	int ret;

	ret = mtk_wmt_read(wmt, hdr, sizeof(hdr));
	if (ret)
		return ret;

	if (hdr[0] != 0x80 || ((hdr[1] >> 4) & 0xf) != type)
		return -EAGAIN;		/* not our packet */

	plen = ((u16)(hdr[1] & 0x0f) << 8) | hdr[2];
	if (plen > max_len)
		return -EOVERFLOW;

	return mtk_wmt_read(wmt, payload, plen);
}

/* ---- WMT command/event ---- */

static int mtk_wmt_cmd(struct mtk_wmt *wmt, u8 opcode,
		       const u8 *param, size_t plen,
		       u8 *evt, size_t elen_max)
{
	u8 wmt_pkt[4 + 16];
	size_t wmt_len = 4 + plen;
	unsigned int tries = 10;
	int ret;

	if (plen > sizeof(wmt_pkt) - 4)
		return -EINVAL;

	wmt_pkt[0] = 0x01;	/* direction: host -> chip */
	wmt_pkt[1] = opcode;
	wmt_pkt[2] = plen & 0xff;
	wmt_pkt[3] = plen >> 8;
	memcpy(wmt_pkt + 4, param, plen);

	while (tries--) {
		ret = mtk_stp_send(wmt, STP_TYPE_WMT, wmt_pkt, wmt_len);
		if (ret)
			return ret;

		msleep(20);

		ret = mtk_stp_recv(wmt, STP_TYPE_WMT, evt, elen_max);
		if (ret == -EAGAIN)
			continue;
		if (ret < 0)
			return ret;

		/* evt[0]=dir evt[1]=opcode evt[2..3]=len */
		if (elen_max >= 2 && evt[0] == 0x02 && evt[1] == opcode &&
		    evt[(evt[2] | (evt[3] << 8)) >= 6 ? 6 : elen_max - 1])
			return 0;
	}

	dev_err(&wmt->func->dev, "no valid event for WMT opcode 0x%02x\n",
		opcode);
	return -EIO;
}

/* ---- bring-up steps ---- */

static int mtk_wmt_inband_reset(struct mtk_wmt *wmt)
{
	const u8 sync[5] = { STP_SYNC_BYTE, STP_SYNC_BYTE, STP_SYNC_BYTE,
			     STP_SYNC_BYTE, STP_SYNC_BYTE };
	int ret;

	ret = mtk_wmt_write(wmt, sync, sizeof(sync));
	if (ret)
		return ret;

	usleep_range(10, 50);

	return 0;
}

static int mtk_wmt_host_awake(struct mtk_wmt *wmt)
{
	/*
	 * HOST_AWAKE carries no parameter; the event repeats the opcode and
	 * result byte.  Send an empty-parameter command.
	 */
	static const u8 param[] = { };
	u8 evt[32];

	return mtk_wmt_cmd(wmt, WMT_OP_HOST_AWAKE, param, sizeof(param),
			   evt, sizeof(evt));
}

static int mtk_wmt_download_firmware(struct mtk_wmt *wmt)
{
	const struct firmware *fw;
	static const char name[] = "mediatek/mt6628_patch_e2_hdr.bin";
	int ret;

	ret = firmware_request_nowarn(&fw, name, &wmt->func->dev);
	if (ret) {
		dev_err(&wmt->func->dev, "failed to load %s: %d\n", name, ret);
		return ret;
	}

	dev_info(&wmt->func->dev, "patch %s (%zu bytes)\n", name, fw->size);

	/*
	 * Patch image layout (from the downstream WMT_PATCH struct):
	 *   [0x00] date/time string (16 bytes)
	 *   [0x10] platform name ("ALPS")
	 *   [0x14] hardware version (must match the chip's HVR)
	 *   [0x16] software version
	 *   [0x18] section table, then patch sections
	 *
	 * Sections are pushed with WMT_OP_PATCH_DL commands carrying one
	 * chunk each; the final chunk commits and resets the MCU.
	 * TODO: implement the per-section transfer once it can be verified
	 * against real hardware.
	 */

	release_firmware(fw);

	wmt->fw_ready = false;
	return 0;
}

/* ---- probe/remove ---- */

static int mtk_wmt_sdio_probe(struct sdio_func *func,
			      const struct sdio_device_id *id)
{
	struct mtk_wmt *wmt;
	int ret;

	if (func->num != 1) {
		/* func 2 belongs to the WLAN data path */
		dev_dbg(&func->dev, "ignoring function %d\n", func->num);
		return -ENODEV;
	}

	wmt = devm_kzalloc(&func->dev, sizeof(*wmt), GFP_KERNEL);
	if (!wmt)
		return -ENOMEM;

	wmt->func = func;
	sdio_set_drvdata(func, wmt);

	wmt->vmmc = devm_regulator_get_optional(&func->dev, "vmmc");
	if (IS_ERR(wmt->vmmc)) {
		ret = PTR_ERR(wmt->vmmc);
		if (ret != -ENODEV)
			return dev_err_probe(&func->dev, ret,
					     "failed to get vmmc\n");
		wmt->vmmc = NULL;
	}

	if (wmt->vmmc) {
		ret = regulator_enable(wmt->vmmc);
		if (ret)
			return dev_err_probe(&func->dev, ret,
					     "failed to enable vmmc\n");
		msleep(50);	/* power ramp-up */
	}

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	sdio_release_host(func);
	if (ret) {
		dev_err_probe(&func->dev, ret, "failed to enable func\n");
		goto err_reg;
	}

	ret = mtk_wmt_inband_reset(wmt);
	if (ret)
		dev_warn(&func->dev, "inband reset failed: %d\n", ret);

	ret = mtk_wmt_host_awake(wmt);
	if (ret)
		dev_warn(&func->dev,
			 "chip did not answer HOST_AWAKE (%d); bring-up incomplete\n",
			 ret);

	ret = mtk_wmt_download_firmware(wmt);
	if (ret)
		goto err_disable;

	return 0;

err_disable:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
err_reg:
	if (wmt->vmmc)
		regulator_disable(wmt->vmmc);
	return ret;
}

static void mtk_wmt_sdio_remove(struct sdio_func *func)
{
	struct mtk_wmt *wmt = sdio_get_drvdata(func);

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);

	if (wmt && wmt->vmmc)
		regulator_disable(wmt->vmmc);
}

static const struct sdio_device_id mtk_wmt_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_MEDIATEK, 0x6628) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, mtk_wmt_sdio_ids);

static struct sdio_driver mtk_wmt_driver = {
	.name = KBUILD_MODNAME,
	.probe = mtk_wmt_sdio_probe,
	.remove = mtk_wmt_sdio_remove,
	.id_table = mtk_wmt_sdio_ids,
};
module_sdio_driver(mtk_wmt_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 WMT control driver");
MODULE_LICENSE("GPL");
