// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6628 WMT (Wireless Management Tool) driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6628 is a Wi-Fi / Bluetooth / FM / GPS combo chip attached over
 * SDIO.  Function 1 is the WLAN data path; function 2 carries the STP
 * control channel shared by WMT/Bluetooth (and FM/GPS on this
 * generation).
 *
 * Control traffic uses STP (Serial Transport Protocol) framing over the
 * SDIO packet ports CTDR/CRDR:
 *
 *   STP header (4 bytes)
 *     [0] = 0x80
 *     [1] = (type << 4) | ((len >> 8) & 0x0f)
 *     [2] = len & 0xff
 *     [3] = 0x00
 *   payload (len bytes)
 *   CRC (2 bytes, zero on this generation)
 *
 * The WMT protocol runs as type 4 (WMT_TASK) payloads: a 4-byte WMT
 * header [dir][opcode][len_lo][len_hi] followed by parameters.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include "mtk-wmt.h"

/* Common HIF register addresses (same layout as btmtksdio). */
#define MTK_SDIO_CTDR			0x18	/* host -> chip data */
#define MTK_SDIO_CRDR			0x1c	/* chip -> host data */
#define MTK_SDIO_CHISR			0x10	/* interrupt status */
#define RX_PKT_LEN_MASK			GENMASK(31, 16)

/* STP channel types (downstream stp_exp.h numbering). */
#define STP_TASK_BT			0
#define STP_TASK_FM			1
#define STP_TASK_GPS			2
#define STP_TASK_WIFI			3
#define STP_TASK_WMT			4

#define STP_HEADER_SIZE			4
#define STP_CRC_SIZE			2
#define WMT_HDR_LEN			4

/* WMT packet direction bytes. */
#define WMT_DIR_HOST_TO_CHIP		0x01
#define WMT_DIR_CHIP_TO_HOST		0x02

/* WMT opcodes (downstream ENUM_OPCODE). */
enum wmt_opcode {
	WMT_OP_HOST_AWAKE		= 0x03,
	WMT_OP_PATCH			= 0x01,
	WMT_OP_FUNC_CTRL		= 0x06,
	WMT_OP_RESET			= 0x07,
	WMT_OP_REG_RW			= 0x08,
};

/*
 * In-band reset payload from mtk_wcn_stp_inband_reset(); sent as a plain
 * STP frame after eight sync bytes.
 */
static const u8 inband_reset_payload[] = {
	0xc0, 0x01, 0xc0, 0xde, 0x3e, 0xd1, 0xa7, 0xef
};

struct mtk_wmt {
	struct sdio_func *func;
	struct regulator *vmmc;
	bool fw_ready;
};

/* ---- low level SDIO ---- */

static int mtk_wmt_write(struct mtk_wmt *wmt, const void *buf, size_t len)
{
	int ret;

	sdio_claim_host(wmt->func);
	ret = sdio_memcpy_toio(wmt->func, MTK_SDIO_CTDR, (void *)buf, len);
	sdio_release_host(wmt->func);
	return ret;
}

static int mtk_wmt_read(struct mtk_wmt *wmt, void *buf, size_t len)
{
	int ret;

	sdio_claim_host(wmt->func);
	ret = sdio_memcpy_fromio(wmt->func, buf, MTK_SDIO_CRDR, len);
	sdio_release_host(wmt->func);
	return ret;
}

static int mtk_wmt_rx_len(struct mtk_wmt *wmt, u16 *len)
{
	u32 isr;
	int ret;

	ret = sdio_memcpy_fromio(wmt->func, &isr, MTK_SDIO_CHISR, sizeof(isr));
	if (ret)
		return ret;

	*len = FIELD_GET(RX_PKT_LEN_MASK, isr);
	return 0;
}

/* ---- STP framing ---- */

static int mtk_stp_send(struct mtk_wmt *wmt, u8 type,
			const u8 *payload, size_t len)
{
	u8 *frame;
	size_t frame_len = STP_HEADER_SIZE + len + STP_CRC_SIZE;
	int ret;

	frame = kzalloc(frame_len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	frame[0] = 0x80;
	frame[1] = (type << 4) | ((len >> 8) & 0x0f);
	frame[2] = len & 0xff;
	frame[3] = 0x00;
	memcpy(frame + STP_HEADER_SIZE, payload, len);

	ret = mtk_wmt_write(wmt, frame, frame_len);
	kfree(frame);

	return ret;
}

static int mtk_stp_recv(struct mtk_wmt *wmt, u8 type,
			u8 *payload, size_t max_len)
{
	u8 hdr[STP_HEADER_SIZE];
	u16 plen;
	int ret;

	ret = mtk_wmt_rx_len(wmt, &plen);
	if (ret)
		return ret;
	if (!plen)
		return -EAGAIN;
	plen -= STP_CRC_SIZE;		/* header + crc are included in CHISR */

	ret = mtk_wmt_read(wmt, hdr, sizeof(hdr));
	if (ret)
		return ret;

	if ((hdr[1] >> 4) != type)
		return -EAGAIN;

	plen = ((u16)(hdr[1] & 0x0f) << 8) | hdr[2];
	if (plen > max_len)
		return -EOVERFLOW;

	return mtk_wmt_read(wmt, payload, plen);
}

static struct mtk_wmt *g_mtk_wmt;

struct mtk_wmt *mtk_wmt_find(void)
{
	return g_mtk_wmt;
}
EXPORT_SYMBOL_GPL(mtk_wmt_find);

int mtk_wmt_send(struct mtk_wmt *wmt, u8 type,
		 const u8 *payload, size_t len)
{
	return mtk_stp_send(wmt, type, payload, len);
}
EXPORT_SYMBOL_GPL(mtk_wmt_send);

int mtk_wmt_recv(struct mtk_wmt *wmt, u8 type,
		 u8 *payload, size_t max_len)
{
	return mtk_stp_recv(wmt, type, payload, max_len);
}
EXPORT_SYMBOL_GPL(mtk_wmt_recv);

/* ---- WMT command/event ---- */

static int mtk_wmt_cmd(struct mtk_wmt *wmt, u8 opcode,
		       const u8 *param, size_t plen,
		       u8 *evt, size_t elen_max, size_t *elen)
{
	u8 pkt[WMT_HDR_LEN + 24];
	unsigned int tries = 10;
	size_t pkt_len = WMT_HDR_LEN + plen;
	size_t got;
	int ret;

	if (plen > sizeof(pkt) - WMT_HDR_LEN)
		return -EINVAL;

	pkt[0] = WMT_DIR_HOST_TO_CHIP;
	pkt[1] = opcode;
	pkt[2] = plen & 0xff;
	pkt[3] = plen >> 8;
	memcpy(pkt + WMT_HDR_LEN, param, plen);

	while (tries--) {
		ret = mtk_stp_send(wmt, STP_TASK_WMT, pkt, pkt_len);
		if (ret)
			return ret;

		msleep(20);

		memset(evt, 0, elen_max);
		ret = mtk_stp_recv(wmt, STP_TASK_WMT, evt, elen_max - 1);
		if (ret == -EAGAIN || ret == -ENOENT)
			continue;
		if (ret < 0)
			return ret;

		got = ret;
		if (got >= 2 && evt[0] == WMT_DIR_CHIP_TO_HOST &&
		    evt[1] == opcode) {
			*elen = got;
			return 0;
		}
	}

	dev_err(&wmt->func->dev, "no valid event for WMT opcode 0x%02x\n",
		opcode);
	return -EIO;
}

/* ---- bring-up steps ---- */

static int mtk_wmt_inband_reset(struct mtk_wmt *wmt)
{
	u8 sync[8];
	int ret;

	memset(sync, 0x7f, sizeof(sync));
	ret = mtk_wmt_write(wmt, sync, sizeof(sync));
	if (ret)
		return ret;

	usleep_range(10, 50);

	/* resync frame: magic payload inside a plain STP/WMT frame */
	ret = mtk_stp_send(wmt, STP_TASK_WMT, inband_reset_payload,
			   sizeof(inband_reset_payload));
	if (ret)
		return ret;

	msleep(20);

	return 0;
}

/*
 * HOST_AWAKE tells the chip that the host is awake so it accepts WMT
 * commands: [dir=3? no -- opcode 0x03 with sub-op 0x02].
 */
static int mtk_wmt_host_awake(struct mtk_wmt *wmt)
{
	/* downstream WMT_HOST_AWAKE_CMD = {01,03,01,00,02} */
	static const u8 param[] = { 0x02 };
	u8 evt[16];
	size_t elen;
	int ret;

	ret = mtk_wmt_cmd(wmt, WMT_OP_HOST_AWAKE, param, sizeof(param),
			  evt, sizeof(evt), &elen);
	if (ret)
		return ret;

	return 0;
}

/* ---- patch download ---- */

/*
 * Patch image layout (WMT_PATCH):
 *   [0x00] date/time string (16 bytes)
 *   [0x10] platform name ("ALPS", 4 bytes)
 *   [0x14] hardware version (BE swapped on display)
 *   [0x16] software version
 *   [0x18] patch version (4 bytes)
 *   [0x1c] section table / body
 */
#define MT6620_PATCH_HDR_SIZE		28

/* SET_REG command targeting the patch download address register. */
static const u8 patch_address_cmd[] = {
	0x01, 0x08, 0x10, 0x00,		/* dir, op REG_RW, len 16 */
	0x01,				/* write */
	0x01,				/* config space */
	0x00,				/* reserved */
	0x01,				/* one register */
	0xd4, 0x01, 0x09, 0xf0,		/* address 0xF001D4 */
	0x00, 0x00, 0x00, 0x00,		/* value (filled later) */
	0xff, 0xff, 0xff, 0xff,		/* mask */
};

static const u8 patch_frag_cmd_hdr[] = {
	0x01, 0x01, 0x00, 0x00, 0x00,
};

#define PATCH_FRAG_FIRST		1
#define PATCH_FRAG_MID			2
#define PATCH_FRAG_LAST			3

static int mtk_wmt_download_firmware(struct mtk_wmt *wmt)
{
	const struct firmware *fw;
	static const char name[] = "mediatek/mt6628_patch_e2_hdr.bin";
	const u8 *body;
	size_t body_len, frag_size, offset;
	unsigned int frag_seq, frag_num;
	const u16 frag_size_max = 1024;
	u8 evt[64];
	size_t elen;
	u8 cmd[WMT_HDR_LEN + frag_size_max];
	int ret;

	ret = firmware_request_nowarn(&fw, name, &wmt->func->dev);
	if (ret) {
		dev_err(&wmt->func->dev, "failed to load %s: %d\n", name, ret);
		return ret;
	}

	if (fw->size <= MT6620_PATCH_HDR_SIZE) {
		dev_err(&wmt->func->dev, "patch %s too small (%zu)\n",
			name, fw->size);
		release_firmware(fw);
		return -EINVAL;
	}

	dev_info(&wmt->func->dev, "patch %s (%zu bytes)\n", name, fw->size);

	/* point the chip at the patch download buffer */
	BUILD_BUG_ON(sizeof(patch_address_cmd) != 20);
	ret = mtk_wmt_cmd(wmt, WMT_OP_REG_RW, patch_address_cmd + 4,
			  sizeof(patch_address_cmd) - 4, evt, sizeof(evt),
			  &elen);
	if (ret) {
		dev_err(&wmt->func->dev, "patch address command failed\n");
		goto out;
	}

	body = fw->data + MT6620_PATCH_HDR_SIZE;
	body_len = fw->size - MT6620_PATCH_HDR_SIZE;

	frag_num = DIV_ROUND_UP(body_len, frag_size_max);

	for (frag_seq = 0; frag_seq < frag_num; frag_seq++) {
		frag_size = min_t(size_t, body_len - frag_seq * frag_size_max,
				  frag_size_max);
		offset = frag_seq * frag_size_max;

		cmd[0] = WMT_DIR_HOST_TO_CHIP;
		cmd[1] = WMT_OP_PATCH;
		cmd[2] = (frag_size + 1) & 0xff;
		cmd[3] = (frag_size + 1) >> 8;
		cmd[4] = frag_seq == 0 ? PATCH_FRAG_FIRST :
			 frag_seq == frag_num - 1 ? PATCH_FRAG_LAST :
			 PATCH_FRAG_MID;
		memcpy(cmd + WMT_HDR_LEN + 1, body + offset, frag_size);

		ret = mtk_stp_send(wmt, STP_TASK_WMT, cmd,
				   WMT_HDR_LEN + 1 + frag_size);
		if (ret)
			goto out;

		msleep(20);

		ret = mtk_stp_recv(wmt, STP_TASK_WMT, evt, sizeof(evt) - 1);
		if (ret < 0 && ret != -EAGAIN)
			goto out;
	}

	wmt->fw_ready = true;
	dev_info(&wmt->func->dev, "patch download done (%zu bytes)\n",
		 body_len);

out:
	release_firmware(fw);
	return ret;
}

/* ---- probe / remove ---- */

static int mtk_wmt_sdio_probe(struct sdio_func *func,
			      const struct sdio_device_id *id)
{
	struct mtk_wmt *wmt;
	int ret;

	if (func->num != 2) {
		/*
		 * Function 1 is the WLAN data path; the STP control
		 * channel shared by WMT/BT/FM/GPS lives on function 2.
		 */
		dev_dbg(&func->dev, "ignoring function %d\n", func->num);
		return -ENODEV;
	}

	wmt = devm_kzalloc(&func->dev, sizeof(*wmt), GFP_KERNEL);
	if (!wmt)
		return -ENOMEM;

	wmt->func = func;
	sdio_set_drvdata(func, wmt);
	g_mtk_wmt = wmt;

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
	if (ret) {
		dev_warn(&func->dev,
			 "chip did not answer HOST_AWAKE (%d); bring-up incomplete\n",
			 ret);
		goto err_disable;
	}

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

	if (!wmt)
		return;

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);

	if (wmt->vmmc)
		regulator_disable(wmt->vmmc);

	g_mtk_wmt = NULL;
	sdio_set_drvdata(func, NULL);
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
