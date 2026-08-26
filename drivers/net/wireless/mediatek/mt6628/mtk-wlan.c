// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * Brings the WLAN function of the MT6628 combo chip (SDIO function 1)
 * up to the point where its firmware is running.  The chip is a full
 * MAC: the firmware generates 802.11 headers, the host hands it plain
 * ethernet frames.
 *
 * Bring-up sequence (from the downstream wlanAdapterStart()):
 *   1. wait for WCIR_WLAN_READY
 *   2. take driver ownership through WHLPCR
 *   3. push the firmware image with DOWNLOAD_BUF commands (address,
 *      length, CRC32 per chunk), then issue WIFI_START
 *   4. query pending errors
 *   5. enable interrupts
 *
 * cfg80211/netdev wiring is not implemented yet; this driver only owns
 * the chip bring-up and exposes the state for the data-path work that
 * follows.
 */

#include <linux/bitfield.h>
#include <linux/crc32.h>
#include <linux/iopoll.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/unaligned.h>

#include "mtk-wlan-hif.h"

#define MT6628_FW_NAME			"mediatek/mt6628_wifi_fw.bin"
#define MT6628_FW_DL_CHUNK		4096

struct mt6628_wlan {
	struct sdio_func *func;
	u8 seq_num;
	bool fw_running;
};

static int mt6628_read32(struct mt6628_wlan *wl, u32 reg, u32 *val)
{
	int ret;

	sdio_claim_host(wl->func);
	ret = sdio_memcpy_fromio(wl->func, val, reg, sizeof(*val));
	sdio_release_host(wl->func);

	return ret;
}

static int mt6628_write32(struct mt6628_wlan *wl, u32 reg, u32 val)
{
	int ret;

	sdio_claim_host(wl->func);
	ret = sdio_memcpy_toio(wl->func, reg, &val, sizeof(val));
	sdio_release_host(wl->func);

	return ret;
}

static int mt6628_poll_ready(struct mt6628_wlan *wl)
{
	unsigned int tries = 50;
	u32 val = 0;
	int ret;

	while (tries--) {
		ret = mt6628_read32(wl, MT6628_MCR_WCIR, &val);
		if (ret)
			return ret;
		if (val & MT6628_WCIR_WLAN_READY) {
			dev_info(&wl->func->dev, "WLAN ready\n");
			return 0;
		}
		msleep(20);
	}

	dev_err(&wl->func->dev, "timed out waiting for WLAN_READY\n");
	return -ETIMEDOUT;
}

static int mt6628_driver_own(struct mt6628_wlan *wl)
{
	u32 val;
	int ret, i;

	ret = mt6628_write32(wl, MT6628_MCR_WHLPCR, MT6628_FW_OWN_REQ_CLR);
	if (ret)
		return ret;

	for (i = 0; i < 100; i++) {
		ret = mt6628_read32(wl, MT6628_MCR_WHLPCR, &val);
		if (ret)
			return ret;
		if (val & MT6628_IS_DRIVER_OWN)
			return 0;
		usleep_range(10, 50);
	}

	dev_err(&wl->func->dev, "timed out waiting for driver ownership\n");
	return -ETIMEDOUT;
}

/* Send one init command with a payload buffer. */
static int mt6628_init_cmd(struct mt6628_wlan *wl, u8 cid,
			   void *extra, size_t extra_len,
			   const u8 *data, size_t data_len)
{
	struct sdio_func *func = wl->func;
	size_t hdr_len = sizeof(struct mt6628_init_hif_tx_hdr);
	size_t pkt_len = hdr_len + extra_len + data_len;
	u8 *pkt;
	int ret, sent;

	pkt = kzalloc(pkt_len, GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	put_unaligned_le16(pkt_len, pkt);
	pkt[2] = 0;			/* ether type offset */
	pkt[3] = 0;			/* checksum flags: none */
	pkt[4] = cid;
	pkt[5] = wl->seq_num++;
	put_unaligned_le16(0, pkt + 6);

	if (extra_len)
		memcpy(pkt + hdr_len, extra, extra_len);
	if (data_len)
		memcpy(pkt + hdr_len + extra_len, data, data_len);

	sdio_claim_host(func);
	sent = sdio_memcpy_toio(func, MT6628_MCR_WTDR0, pkt, pkt_len);
	sdio_release_host(func);

	kfree(pkt);

	if (sent < 0)
		return sent;
	if (sent != pkt_len)
		return -EIO;

	/* wait for the command-done interrupt */
	{
		unsigned int tries = 100;
		u32 isr;

		while (tries--) {
			ret = mt6628_read32(wl, MT6628_MCR_WHISR, &isr);
			if (ret)
				return ret;
			if (isr & MT6628_WHISR_TX_DONE)
				return 0;
			usleep_range(10 * USEC_PER_MSEC / 10,
				     15 * USEC_PER_MSEC / 10);
		}
		dev_err(&wl->func->dev, "timeout waiting for cmd done\n");
		return -ETIMEDOUT;
	}
}

static int mt6628_download_firmware(struct mt6628_wlan *wl)
{
	const struct firmware *fw;
	char fwname[64];
	u8 seq_backup;
	unsigned int offset;
	int ret;

	/*
	 * The firmware image name follows the downstream convention; the
	 * actual RAM code image has to be supplied out-of-tree.
	 */
	snprintf(fwname, sizeof(fwname), "%s", MT6628_FW_NAME);

	ret = firmware_request_nowarn(&fw, fwname, &wl->func->dev);
	if (ret) {
		dev_err(&wl->func->dev, "failed to load %s: %d\n",
			fwname, ret);
		return ret;
	}

	seq_backup = wl->seq_num;
	wl->seq_num = 0;

	dev_info(&wl->func->dev, "firmware %s (%zu bytes)\n", fwname,
		 fw->size);

	for (offset = 0; offset < fw->size; offset += MT6628_FW_DL_CHUNK) {
		size_t chunk = min_t(size_t, fw->size - offset,
				     MT6628_FW_DL_CHUNK);
		struct mt6628_init_cmd_download_buf dl;
		u32 crc = crc32(0, fw->data + offset, chunk);

		dl.address = cpu_to_le32(offset);
		dl.length = cpu_to_le32(chunk);
		dl.crc32 = cpu_to_le32(crc);
		/* ACK requested, no encryption */
		dl.data_mode = cpu_to_le32(BIT(31));

		ret = mt6628_init_cmd(wl, MT6628_INIT_CMD_DOWNLOAD_BUF,
				      &dl, sizeof(dl),
				      fw->data + offset, chunk);
		if (ret) {
			dev_err(&wl->func->dev,
				"DOWNLOAD_BUF at %#zx failed: %d\n",
				offset, ret);
			goto out_restore_seq;
		}
	}

	ret = mt6628_init_cmd(wl, MT6628_INIT_CMD_WIFI_START,
			      NULL, 0, NULL, 0);
	if (ret) {
		dev_err(&wl->func->dev, "WIFI_START failed: %d\n", ret);
		goto out_restore_seq;
	}

	wl->fw_running = true;
	dev_info(&wl->func->dev, "firmware started\n");

out_restore_seq:
	wl->seq_num = seq_backup;
	release_firmware(fw);
	return ret;
}

static int mt6628_wlan_sdio_probe(struct sdio_func *func,
				  const struct sdio_device_id *id)
{
	struct mt6628_wlan *wl;
	int ret;

	if (func->num != 1) {
		dev_dbg(&func->dev, "ignoring function %d\n", func->num);
		return -ENODEV;
	}

	wl = devm_kzalloc(&func->dev, sizeof(*wl), GFP_KERNEL);
	if (!wl)
		return -ENOMEM;

	wl->func = func;
	sdio_set_drvdata(func, wl);

	/* wait for the firmware ROM to come up after power-on */
	ret = mt6628_poll_ready(wl);
	if (ret)
		return dev_err_probe(&func->dev, ret,
				     "chip did not report WLAN_READY\n");

	ret = mt6628_driver_own(wl);
	if (ret)
		return dev_err_probe(&func->dev, ret,
				     "failed to take driver ownership\n");

	ret = mt6628_download_firmware(wl);
	if (ret)
		return ret;

	return 0;
}

static void mt6628_wlan_sdio_remove(struct sdio_func *func)
{
	sdio_set_drvdata(func, NULL);
}

static const struct sdio_device_id mt6628_wlan_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_MEDIATEK, SDIO_DEVICE_ID_MEDIATEK_MT6628) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, mt6628_wlan_sdio_ids);

static struct sdio_driver mt6628_wlan_driver = {
	.name = KBUILD_MODNAME,
	.probe = mt6628_wlan_sdio_probe,
	.remove = mt6628_wlan_sdio_remove,
	.id_table = mt6628_wlan_sdio_ids,
};
module_sdio_driver(mt6628_wlan_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 WLAN driver");
MODULE_LICENSE("GPL");
