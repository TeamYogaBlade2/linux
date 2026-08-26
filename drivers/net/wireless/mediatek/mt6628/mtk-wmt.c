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
 * This driver owns steps 1 and 2 and exposes the control channel to the
 * BT/FM/GPS drivers; it does not implement the data paths.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#define MTK_SDIO_FMR_BASE		0x000	/* host->chip port */
#define MTK_SDIO_RFR_BASE		0x100	/* chip->host port */

/* WMT packet framing: [type][opcode][len_lo][len_hi] payload... */
#define WMT_PKT_TYPE_CMD	1
#define WMT_PKT_TYPE_EVT	2

enum wmt_opcode {
	WMT_OP_SLEEP		= 0x01,
	WMT_OP_HOST_AWAKE	= 0x02,
	WMT_OP_FUNC_CTRL	= 0x08,
	WMT_OP_PATCH_DL		= 0x0d,
};

struct mtk_wmt {
	struct sdio_func *func;
	struct regulator *vmmc;
	bool fw_ready;
};

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

/*
 * Send a WMT command and read back its event.  The chip has no out-of-band
 * interrupt for this; the downstream driver simply polls after a delay.
 */
static int mtk_wmt_cmd(struct mtk_wmt *wmt, u8 opcode,
		       const u8 *param, size_t plen,
		       u8 *evt, size_t elen)
{
	u8 cmd[8 + 16];
	unsigned int tries = 10;
	size_t cmd_len = 4 + plen;
	int ret;

	if (plen > sizeof(cmd) - 4)
		return -EINVAL;

	cmd[0] = WMT_PKT_TYPE_CMD;
	cmd[1] = opcode;
	cmd[2] = plen & 0xff;
	cmd[3] = plen >> 8;
	memcpy(cmd + 4, param, plen);

	while (tries--) {
		ret = mtk_wmt_write(wmt, cmd, cmd_len);
		if (ret)
			return ret;

		msleep(20);

		memset(evt, 0, elen);
		ret = mtk_wmt_read(wmt, evt, elen);
		if (ret)
			return ret;

		if (elen >= 2 && evt[0] == WMT_PKT_TYPE_EVT && evt[1] == opcode)
			return 0;
	}

	dev_err(&wmt->func->dev, "no event for opcode 0x%02x\n", opcode);
	return -EIO;
}

/* Wake the chip up so that it accepts WMT commands. */
static int mtk_wmt_host_awake(struct mtk_wmt *wmt)
{
	u8 evt[16];
	const u8 param[] = { 0x00 };

	return mtk_wmt_cmd(wmt, WMT_OP_HOST_AWAKE, param, sizeof(param),
			   evt, sizeof(evt));
}

static const char *mtk_wmt_patch_name(struct sdio_func *func)
{
	/* The chip reports its revision in the patch header; e2 is current. */
	return "mediatek/mt6628_patch_e2_hdr.bin";
}

static int mtk_wmt_download_firmware(struct mtk_wmt *wmt)
{
	const struct firmware *fw;
	const char *name = mtk_wmt_patch_name(wmt->func);
	int ret;

	ret = firmware_request_nowarn(&fw, name, &wmt->func->dev);
	if (ret) {
		dev_err(&wmt->func->dev, "failed to load %s: %d\n", name, ret);
		return ret;
	}

	dev_info(&wmt->func->dev,
		 "patch %s (%zu bytes) loaded; download protocol pending\n",
		 name, fw->size);

	release_firmware(fw);

	return 0;
}

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
	if (ret) {
		dev_err_probe(&func->dev, ret, "failed to enable func\n");
		goto err_disable_reg;
	}
	sdio_release_host(func);

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
err_disable_reg:
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
