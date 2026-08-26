// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6628 WMT (Wireless Management Tool) driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6628 is a Wi-Fi / Bluetooth / FM / GPS combo chip attached over
 * SDIO (two functions: func 1 carries the WMT/BT/FM/GPS control channel,
 * func 2 the WLAN data path).  Before any of those subsystems can be
 * used, the chip must be brought up:
 *
 *   1. power is enabled through an optional regulator and the chip
 *      comes out of reset;
 *   2. a patch image ("mt6628_patch_*.bin") is pushed over the SDIO
 *      control channel, which contains the firmware for the internal
 *      MCU plus per-chip calibration;
 *   3. individual functions are then switched on with WMT commands.
 *
 * This driver owns steps 1 and 2 and exposes the control channel to
 * the BT/FM/GPS drivers; it does not implement the data paths.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>

#define MTK_WMT_SDIO_FUNC1	1	/* control channel */
#define MTK_WMT_SDIO_FUNC2	2	/* wlan */

/* SDIO registers of the WMT control function */
#define MTK_SDIO_TO_HOST_PORT	0x0	/* chip -> host packet ring */
#define MTK_SDIO_FROM_HOST_PORT	0x4	/* host -> chip packet ring */
#define MTK_SDIO_CTRL_REG	0x8

/*
 * WMT packet framing: [type][opcode][length_lo][length_hi] payload...
 * type 1 = command, 2 = event.
 */
#define WMT_PKT_TYPE_CMD	1
#define WMT_PKT_TYPE_EVT	2

#define WMT_HDR_LEN		4

enum wmt_opcode {
	WMT_OP_SLEEP		= 0x01, /* sub-op in payload */
	WMT_OP_HOST_AWAKE	= 0x02,
	WMT_OP_HIF_CONF		= 0x03,
	WMT_OP_FUNC_CTRL	= 0x08,
	WMT_OP_PATCH_DL		= 0x0d,
};

struct mtk_wmt {
	struct sdio_func *func;		/* control function */
	struct regulator *vmmc;		/* chip supply */
	bool fw_downloaded;
};

static int mtk_wmt_sdio_write(struct mtk_wmt *wmt, const void *buf, size_t len)
{
	sdio_claim_host(wmt->func);
	ret = sdio_memcpy_toio(wmt->func, MTK_SDIO_FROM_HOST_PORT,
			       (void *)buf, len);
	sdio_release_host(wmt->func);
	return ret;
}

static int mtk_wmt_sdio_read(struct mtk_wmt *wmt, void *buf, size_t len)
{
	int ret;

	sdio_claim_host(wmt->func);
	ret = sdio_memcpy_fromio(wmt->func, buf, MTK_SDIO_TO_HOST_PORT, len);
	sdio_release_host(wmt->func);
	return ret;
}

/* Send a WMT command and wait for its event. Caller holds no SDIO lock. */
static int mtk_wmt_cmd(struct mtk_wmt *wmt, u8 opcode,
		       const u8 *param, size_t plen,
		       u8 *evt, size_t elen)
{
	u8 cmd[WMT_HDR_LEN + 16];
	unsigned int tries = 10;
	int ret;

	if (plen > sizeof(cmd) - WMT_HDR_LEN)
		return -EINVAL;

	cmd[0] = WMT_PKT_TYPE_CMD;
	cmd[1] = opcode;
	cmd[2] = plen & 0xff;
	cmd[3] = (plen >> 8) & 0xff;
	memcpy(cmd + WMT_HDR_LEN, param, plen);

	while (tries--) {
		ret = mtk_wmt_sdio_write(wmt, cmd, WMT_HDR_LEN + plen);
		if (ret)
			return ret;

		msleep(20);

		ret = mtk_wmt_sdio_read(wmt, evt, elen);
		if (ret)
			return ret;

		/* got our event? */
		if (elen >= 2 && evt[0] == WMT_PKT_TYPE_EVT && evt[1] == opcode)
			return 0;
	}

	return -EIO;
}

static int mtk_wmt_download_firmware(struct mtk_wmt *wmt)
{
	const struct firmware *fw;
	char name[] = "mediatek/mt6628_patch_e2_hdr.bin";
	u8 evt[64];
	u8 cmd[16];
	int ret;

	ret = firmware_request_nowarn(&fw, name, wmt->dev);
	if (ret) {
		dev_err(wmt->dev, "failed to load %s: %d\n", name, ret);
		return ret;
	}

	/*
	 * The patch download protocol is a sequence of WMT_PATCH_DL
	 * commands carrying chunks of the image followed by a commit;
	 * the exact framing lives in the downstream wmt_core.c.  For now
	 * just announce what we would do so that the bring-up can be
	 * traced on real hardware.
	 */
	dev_info(wmt->dev, "patch %s (%zu bytes) available\n",
		 name, fw->size);

	release_firmware(fw);

	return 0;
}
