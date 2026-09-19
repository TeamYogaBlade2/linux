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
#define MT6628_FW_DL_CHUNK		2048
/* CFG_FW_LOAD_ADDRESS / CFG_FW_START_ADDRESS of the downstream config.h */
#define MT6628_FW_LOAD_ADDRESS		0x00060000
#define MT6628_FW_START_ADDRESS		0x00060000
#define MT6628_FW_SIGNATURE		0x574b544d
#define MT6628_FW_HEADER_SIZE		16
#define MT6628_FW_SECTION_SIZE		16
#define MT6628_INIT_EVENT_CMD_RESULT	1
#define MT6628_INIT_EVENT_SIZE		8

struct mt6628_wlan {
	struct sdio_func *func;
	u8 seq_num;
	bool fw_running;
};

struct mt6628_fw_section {
	__le32 offset;
	__le32 reserved;
	__le32 length;
	__le32 dest_addr;
} __packed;

static u32 mt6628_crc32(const u8 *data, size_t len)
{
	return ~crc32_le(~0, data, len);
}

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
	unsigned int tries = 0;
	unsigned long timeout = jiffies + msecs_to_jiffies(8192);
	u32 val;
	int ret;

	ret = mt6628_write32(wl, MT6628_MCR_WHLPCR,
			     MT6628_FW_OWN_REQ_CLR);
	if (ret)
		return ret;

	while (time_before(jiffies, timeout)) {
		ret = mt6628_read32(wl, MT6628_MCR_WHLPCR, &val);
		if (ret)
			return ret;

		if (val & MT6628_IS_DRIVER_OWN)
			return 0;

		/*
		 * Match nicpmSetDriverOwn(): refresh the ownership request
		 * periodically while the LP engine is completing its transition.
		 */
		if (!(tries++ & 0xff)) {
			ret = mt6628_write32(wl, MT6628_MCR_WHLPCR,
					     MT6628_FW_OWN_REQ_CLR);
			if (ret)
				return ret;
		}

		usleep_range(900, 1100);
	}

	dev_err(&wl->func->dev, "timed out waiting for driver ownership\n");
	return -ETIMEDOUT;
}

static int mt6628_wait_init_cmd_result(struct mt6628_wlan *wl, u8 seq_num)
{
	unsigned int tries = 1000;

	while (tries--) {
		u32 isr;
		u32 rx_len_reg;
		u16 rx_len;
		u8 *resp;
		int ret;

		ret = mt6628_read32(wl, MT6628_MCR_WHISR, &isr);
		if (ret)
			return ret;

		if (isr & MT6628_WHISR_ABNORMAL) {
			dev_err(&wl->func->dev,
				"firmware reported abnormal status (isr %#x)\n",
				isr);
			return -EIO;
		}

		if (!(isr & MT6628_WHISR_RX0_DONE)) {
			usleep_range(1000, 1500);
			continue;
		}

		ret = mt6628_read32(wl, MT6628_MCR_WRPLR, &rx_len_reg);
		if (ret)
			return ret;

		rx_len = (u16)rx_len_reg;
		if (rx_len != MT6628_INIT_EVENT_SIZE) {
			dev_err(&wl->func->dev,
				"invalid init event length: %u bytes\n", rx_len);
			return -EMSGSIZE;
		}

		resp = kzalloc(rx_len, GFP_KERNEL);
		if (!resp)
			return -ENOMEM;

		sdio_claim_host(wl->func);
		ret = sdio_readsb(wl->func, resp, MT6628_MCR_WRDR0, rx_len);
		sdio_release_host(wl->func);
		if (ret) {
			kfree(resp);
			return ret;
		}

		/*
		 * INIT_HIF_RX_HEADER:
		 *   +0: u2RxByteCount
		 *   +2: ucEID
		 *   +3: ucSeqNum
		 *   +4: ucStatus
		 */
		if (resp[2] != MT6628_INIT_EVENT_CMD_RESULT ||
		    resp[3] != seq_num) {
			kfree(resp);
			continue;
		}

		ret = resp[4] ? -EIO : 0;
		kfree(resp);
		return ret;
	}

	dev_err(&wl->func->dev,
		"timeout waiting for init command result\n");
	return -ETIMEDOUT;
}

/* Send one init command with a payload buffer. */
static int mt6628_init_cmd(struct mt6628_wlan *wl, u8 cid,
			   void *extra, size_t extra_len,
			   const u8 *data, size_t data_len,
			   bool wait_result)
{
	struct sdio_func *func = wl->func;
	size_t hdr_len = sizeof(struct mt6628_init_hif_tx_hdr);
	size_t pkt_len = ALIGN(hdr_len + extra_len + data_len, 4);
	u8 seq_num;
	u8 *pkt;
	int ret;

	pkt = kzalloc(pkt_len, GFP_KERNEL);
	if (!pkt)
		return -ENOMEM;

	put_unaligned_le16(pkt_len, pkt);
	pkt[2] = 0;			/* ether type offset */
	pkt[3] = 0;			/* checksum flags: none */
	pkt[4] = cid;
	seq_num = ++wl->seq_num;
	pkt[5] = seq_num;
	put_unaligned_le16(0, pkt + 6);

	if (extra_len)
		memcpy(pkt + hdr_len, extra, extra_len);
	if (data_len)
		memcpy(pkt + hdr_len + extra_len, data, data_len);

	sdio_claim_host(func);
	ret = sdio_writesb(func, MT6628_MCR_WTDR0, pkt, pkt_len);
	sdio_release_host(func);

	kfree(pkt);

	if (ret)
		return ret;

	if (!wait_result)
		return 0;

	return mt6628_wait_init_cmd_result(wl, seq_num);
}

static int mt6628_download_blob(struct mt6628_wlan *wl, u32 dest_addr,
				const u8 *data, size_t len)
{
	while (len) {
		struct mt6628_init_cmd_download_buf dl;
		size_t chunk = min_t(size_t, len, MT6628_FW_DL_CHUNK);
		u32 crc;
		int ret;

		if (chunk > U32_MAX - dest_addr)
			return -EOVERFLOW;

		/*
		 * The firmware protocol carries the real section length.
		 * mt6628_init_cmd() takes care of the 4-byte HIF packet
		 * alignment and zero padding separately.
		 */
		crc = mt6628_crc32(data, chunk);

		dl.address = cpu_to_le32(dest_addr);
		dl.length = cpu_to_le32(chunk);
		dl.crc32 = cpu_to_le32(crc);
		dl.data_mode = cpu_to_le32(BIT(0) | BIT(31));

		ret = mt6628_init_cmd(wl, MT6628_INIT_CMD_DOWNLOAD_BUF,
				      &dl, sizeof(dl), data, chunk, true);
		if (ret)
			return ret;

		dest_addr += chunk;
		data += chunk;
		len -= chunk;
	}

	return 0;
}

static int mt6628_download_firmware(struct mt6628_wlan *wl)
{
	const struct firmware *fw;
	char fwname[64];
	u8 seq_backup;
	u32 num_sections;
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

	/*
	 * MT6628 uses divided firmware images:
	 *
	 *   0x00 signature
	 *   0x04 whole-file CRC32
	 *   0x08 section count
	 *   0x0c reserved
	 *   0x10 section table
	 *
	 * The downstream checks the CRC over everything from u4NumOfEntries
	 * (offset 8) to the end of the image.
	 */
	if (fw->size >= MT6628_FW_HEADER_SIZE &&
	    get_unaligned_le32(fw->data) == MT6628_FW_SIGNATURE &&
	    get_unaligned_le32(fw->data + 4) ==
			mt6628_crc32(fw->data + 8, fw->size - 8)) {
		num_sections = get_unaligned_le32(fw->data + 8);

		if (num_sections >
		    (fw->size - MT6628_FW_HEADER_SIZE) /
			MT6628_FW_SECTION_SIZE) {
			dev_err(&wl->func->dev,
				"invalid firmware section count: %u\n",
				num_sections);
			ret = -EINVAL;
			goto out_restore_seq;
		}

		for (offset = 0; offset < num_sections; offset++) {
			const struct mt6628_fw_section *section;
			u32 data_offset;
			u32 data_len;
			u32 dest_addr;

			section = (const struct mt6628_fw_section *)
				(fw->data + MT6628_FW_HEADER_SIZE +
				 offset * MT6628_FW_SECTION_SIZE);

			data_offset = le32_to_cpu(section->offset);
			data_len = le32_to_cpu(section->length);
			dest_addr = le32_to_cpu(section->dest_addr);

			if (data_offset > fw->size ||
			    data_len > fw->size - data_offset) {
				dev_err(&wl->func->dev,
					"invalid firmware section %u: offset %#x length %#x\n",
					offset, data_offset, data_len);
				ret = -EINVAL;
				goto out_restore_seq;
			}

			ret = mt6628_download_blob(wl, dest_addr,
						   fw->data + data_offset,
						   data_len);
			if (ret) {
				dev_err(&wl->func->dev,
					"firmware section %u failed: %d\n",
					offset, ret);
				goto out_restore_seq;
			}
		}
	} else {
		/*
		 * Keep the downstream fallback for a legacy/raw RAM-code image.
		 * This path still uses the real unpadded length for CRC/firmware
		 * metadata and only pads the HIF packet itself.
		 */
		ret = mt6628_download_blob(wl, MT6628_FW_LOAD_ADDRESS,
					   fw->data, fw->size);
		if (ret)
			goto out_restore_seq;
	}

	{
		/* INIT_CMD_WIFI_START { u4Override, u4Address } */
		struct {
			__le32 override;
			__le32 address;
		} __packed start = {
			.override = cpu_to_le32(0),	/* no override */
			.address = cpu_to_le32(0),
		};

		ret = mt6628_init_cmd(wl, MT6628_INIT_CMD_WIFI_START,
				      &start, sizeof(start), NULL, 0, false);
		if (ret) {
			dev_err(&wl->func->dev, "WIFI_START failed: %d\n",
				ret);
			goto out_restore_seq;
		}

		ret = mt6628_poll_ready(wl);
		if (ret)
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
	u32 wcir;
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

	/* enable the SDIO function; MMC core does not do it for us */
	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	sdio_release_host(func);
	if (ret)
		return dev_err_probe(&func->dev, ret,
				     "failed to enable function\n");

	msleep(50);		/* let the ROM come up */

	ret = mt6628_read32(wl, MT6628_MCR_WCIR, &wcir);
	if (ret)
		goto err_disable;

	if ((wcir & MT6628_WCIR_CHIP_ID) != 0x6628) {
		dev_err(&func->dev, "unexpected chip ID %#x\n",
			wcir & MT6628_WCIR_CHIP_ID);
		ret = -ENODEV;
		goto err_disable;
	}

	ret = mt6628_driver_own(wl);
	if (ret)
		goto err_disable;

	ret = mt6628_download_firmware(wl);
	if (ret)
		goto err_disable;

	return 0;

err_disable:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	sdio_set_drvdata(func, NULL);

	return dev_err_probe(&func->dev, ret, "MT6628 WLAN probe failed\n");
}

static void mt6628_wlan_sdio_remove(struct sdio_func *func)
{
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
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
