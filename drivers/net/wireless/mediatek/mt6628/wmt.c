// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6628 WMT/STP driver for the Lenovo B8000-H.
 *
 * The command stream, patch format and power timing are derived from the
 * GPL-2.0 MediaTek 3.4 driver (stp_core.c, wmt_core.c, wmt_ic_6628.c and
 * wmt_func.c). The tty line discipline and 6620_launcher are replaced by
 * serdev and firmware_class.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/slab.h>

#include "mt6628.h"

#define MT6628_FW_PATCH_E1       "mt6628_patch_e1_hdr.bin"
#define MT6628_FW_PATCH_E2_0     "mt6628_patch_e2_0_hdr.bin"
#define MT6628_FW_PATCH_E2_1     "mt6628_patch_e2_1_hdr.bin"
#define MT6628_FW_CONFIG         "WMT.cfg"

#define MT6628_WMT_TYPE          4
#define MT6628_PATCH_HEADER_SIZE 28
#define MT6628_PATCH_FRAGMENT    1000
#define MT6628_RX_MAX            2048
#define MT6628_XFER_TIMEOUT      msecs_to_jiffies(3000)

enum mt6628_stp_mode {
	MT6628_STP_MANDATORY,
	MT6628_STP_FULL,
};

struct mt6628_stp_parser {
	u8 hdr[4];
	u8 payload[MT6628_RX_MAX];
	u8 crc[2];
	u16 payload_len;
	u16 pos;
	u8 hdr_pos;
	u8 crc_pos;
};

struct mt6628_wmt {
	struct serdev_device *serdev;
	struct gpio_desc *pmu;
	struct gpio_desc *reset;
	struct clk *clk32k;
	struct mmc_host *mmc;
	struct mutex lock;
	spinlock_t rx_lock;
	struct completion response;
	struct mt6628_stp_parser parser;
	u8 response_buf[MT6628_RX_MAX];
	size_t response_len;
	unsigned int users;
	unsigned int operational_baud;
	u32 off_stable_ms;
	u32 rtc_stable_ms;
	u32 reset_stable_ms;
	u32 on_stable_ms;
	u16 hw_ver;
	u16 fw_ver;
	u8 tx_seq;
	u8 rx_ack;
	enum mt6628_stp_mode stp_mode;
	bool powered;
	bool initialized;
};

static DEFINE_MUTEX(mt6628_global_lock);
static struct mt6628_wmt *mt6628_global;

static bool wmt_wifi_func_ctrl;
module_param(wmt_wifi_func_ctrl, bool, 0644);
MODULE_PARM_DESC(wmt_wifi_func_ctrl, "send WMT_FUNC_CTRL(Wi-Fi) (vendor does not)");

static const u8 wmt_query_baud[] = { 0x01, 0x04, 0x01, 0x00, 0x02 };
static const u8 wmt_query_stp[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
static const u8 wmt_reset[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
static const u8 wmt_reset_evt[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };
static const u8 wmt_patch_evt[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
static const u8 wmt_set_stp[] = {
	0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01
};
static const u8 wmt_set_stp_evt[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
static const u8 wmt_query_stp_full_evt[] = {
	0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdf, 0x0e, 0x68, 0x01
};
/* byte 5 = coex_wmt_ant_mode (stock mt6628_ant_m1.cfg: 1) */
static const u8 wmt_coex[] = { 0x01, 0x10, 0x02, 0x00, 0x01, 0x01 };
static const u8 wmt_coex_evt[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };
static const u8 wmt_sdio_drive[] = {
	0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
	/* 0x80050050 = sdio_driving_cfg 0x00077777 (stock mt6628_ant_m1.cfg) */
	0x50, 0x00, 0x05, 0x80, 0x77, 0x77, 0x07, 0x00,
	0x77, 0x77, 0x07, 0x00
};
static const u8 wmt_sdio_drive_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01
};
static const u8 wmt_fm_strap[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
static const u8 wmt_fm_strap_evt[] = { 0x02, 0x05, 0x02, 0x00, 0x00, 0x02 };

static void mt6628_read_delay(struct device *dev, const char *name,
			      u32 *value, u32 fallback)
{
	if (device_property_read_u32(dev, name, value))
		*value = fallback;
}

static void mt6628_parser_reset(struct mt6628_stp_parser *p)
{
	p->hdr_pos = 0;
	p->payload_len = 0;
	p->pos = 0;
	p->crc_pos = 0;
}

static void mt6628_deliver_packet(struct mt6628_wmt *wmt,
				  const u8 *payload, size_t len)
{
	unsigned long flags;

	spin_lock_irqsave(&wmt->rx_lock, flags);
	if (!completion_done(&wmt->response)) {
		wmt->response_len = min(len, sizeof(wmt->response_buf));
		memcpy(wmt->response_buf, payload, wmt->response_len);
		complete(&wmt->response);
	}
	spin_unlock_irqrestore(&wmt->rx_lock, flags);
}

static void mt6628_parse_byte(struct mt6628_wmt *wmt, u8 byte)
{
	struct mt6628_stp_parser *p = &wmt->parser;
	u16 expected_crc, received_crc;
	u8 type;

	if (p->hdr_pos < sizeof(p->hdr)) {
		if (!p->hdr_pos && !(byte & 0x80))
			return;
		p->hdr[p->hdr_pos++] = byte;
		if (p->hdr_pos != sizeof(p->hdr))
			return;
		p->payload_len = ((p->hdr[1] & 0x0f) << 8) | p->hdr[2];
		if (!p->payload_len || p->payload_len > sizeof(p->payload) ||
		    (wmt->stp_mode == MT6628_STP_FULL &&
		     p->hdr[3] != (u8)(p->hdr[0] + p->hdr[1] + p->hdr[2])))
			mt6628_parser_reset(p);
		return;
	}

	if (p->pos < p->payload_len) {
		p->payload[p->pos++] = byte;
		return;
	}
	p->crc[p->crc_pos++] = byte;
	if (p->crc_pos != sizeof(p->crc))
		return;

	type = p->hdr[1] >> 4;
	received_crc = p->crc[0] | (p->crc[1] << 8);
	expected_crc = crc16(0, p->payload, p->payload_len);
	if (wmt->stp_mode == MT6628_STP_FULL && received_crc != expected_crc) {
		dev_warn_ratelimited(&wmt->serdev->dev,
				     "STP CRC mismatch: got %#x expected %#x\n",
				     received_crc, expected_crc);
		goto out;
	}
	if (wmt->stp_mode == MT6628_STP_FULL)
		wmt->rx_ack = (((p->hdr[0] >> 3) & 7) + 1) & 7;
	if (type == MT6628_WMT_TYPE)
		mt6628_deliver_packet(wmt, p->payload, p->payload_len);
out:
	mt6628_parser_reset(p);
}

static bool debug_io;
module_param(debug_io, bool, 0644);
MODULE_PARM_DESC(debug_io, "hex-dump UART3 traffic");

static size_t mt6628_receive_buf(struct serdev_device *serdev,
				 const unsigned char *data, size_t count)
{
	struct mt6628_wmt *wmt = serdev_device_get_drvdata(serdev);
	size_t i;

	if (debug_io)
		dev_info(&serdev->dev, "rx %zu: %*ph\n", count,
			 (int)min_t(size_t, count, 32), data);
	for (i = 0; i < count; i++)
		mt6628_parse_byte(wmt, data[i]);
	return count;
}

static void mt6628_write_wakeup(struct serdev_device *serdev)
{
	serdev_device_write_wakeup(serdev);
}

static const struct serdev_device_ops mt6628_serdev_ops = {
	.receive_buf = mt6628_receive_buf,
	.write_wakeup = mt6628_write_wakeup,
};

static int mt6628_uart_write(struct mt6628_wmt *wmt, const u8 *buf, size_t len)
{
	ssize_t written;

	if (debug_io)
		dev_info(&wmt->serdev->dev, "tx %zu: %*ph\n", len,
			 (int)min_t(size_t, len, 32), buf);
	written = serdev_device_write(wmt->serdev, buf, len,
				      MT6628_XFER_TIMEOUT);
	if (written < 0)
		return written;
	if (written != len)
		return -EIO;
	serdev_device_wait_until_sent(wmt->serdev, MT6628_XFER_TIMEOUT);
	return 0;
}

static int mt6628_stp_send(struct mt6628_wmt *wmt, const u8 *payload,
			   size_t len)
{
	u8 *packet;
	u16 crc;
	int ret;

	if (!len || len > 0xfff)
		return -EINVAL;
	packet = kmalloc(len + 6, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;

	packet[0] = wmt->stp_mode == MT6628_STP_FULL ?
		0x80 | (wmt->tx_seq << 3) | wmt->rx_ack : 0x80;
	packet[1] = (MT6628_WMT_TYPE << 4) | ((len >> 8) & 0x0f);
	packet[2] = len & 0xff;
	packet[3] = wmt->stp_mode == MT6628_STP_FULL ?
		(u8)(packet[0] + packet[1] + packet[2]) : 0;
	memcpy(packet + 4, payload, len);
	crc = wmt->stp_mode == MT6628_STP_FULL ? crc16(0, payload, len) : 0;
	packet[len + 4] = crc & 0xff;
	packet[len + 5] = crc >> 8;

	ret = mt6628_uart_write(wmt, packet, len + 6);
	if (!ret && wmt->stp_mode == MT6628_STP_FULL)
		wmt->tx_seq = (wmt->tx_seq + 1) & 7;
	kfree(packet);
	return ret;
}

static int mt6628_xfer(struct mt6628_wmt *wmt, const u8 *cmd, size_t cmd_len,
			const u8 *expected, size_t expected_len)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&wmt->rx_lock, flags);
	reinit_completion(&wmt->response);
	wmt->response_len = 0;
	spin_unlock_irqrestore(&wmt->rx_lock, flags);
	ret = mt6628_stp_send(wmt, cmd, cmd_len);
	if (ret)
		return ret;
	if (!wait_for_completion_timeout(&wmt->response, MT6628_XFER_TIMEOUT))
		return -ETIMEDOUT;
	if (wmt->response_len != expected_len)
		return -EPROTO;
	if (expected && memcmp(wmt->response_buf, expected, expected_len)) {
		dev_err(&wmt->serdev->dev,
			"unexpected WMT event for opcode %#x\n", cmd[1]);
		return -EPROTO;
	}
	return 0;
}

static int mt6628_reg_read(struct mt6628_wmt *wmt, u32 address, u32 *value)
{
	u8 cmd[20] = {
		0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01,
	};
	u8 expected_prefix[8] = {
		0x02, 0x08, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x01,
	};
	int ret;

	put_unaligned_le32(address, cmd + 8);
	put_unaligned_le32(0, cmd + 12);
	put_unaligned_le32(0xffffffff, cmd + 16);
	ret = mt6628_xfer(wmt, cmd, sizeof(cmd), NULL, 16);
	if (ret)
		return ret;
	if (memcmp(wmt->response_buf, expected_prefix, sizeof(expected_prefix)) ||
	    get_unaligned_le32(wmt->response_buf + 8) != address)
		return -EPROTO;
	*value = get_unaligned_le32(wmt->response_buf + 12);
	return 0;
}

static int mt6628_set_baud(struct mt6628_wmt *wmt)
{
	u8 cmd[] = { 0x01, 0x04, 0x05, 0x00, 0x01, 0, 0, 0, 0 };
	const u8 evt[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x01 };
	unsigned int actual;
	int ret;

	put_unaligned_le32(wmt->operational_baud, cmd + 5);
	cmd[8] = 0;
	ret = mt6628_xfer(wmt, cmd, sizeof(cmd), evt, sizeof(evt));
	if (ret)
		return ret;
	actual = serdev_device_set_baudrate(wmt->serdev,
					    wmt->operational_baud);
	if (actual != wmt->operational_baud)
		return -EINVAL;
	serdev_device_set_flow_control(wmt->serdev, false);
	usleep_range(2000, 3000);
	return 0;
}

static int mt6628_patch_address(struct mt6628_wmt *wmt, const u8 address[4])
{
	static const u8 part_cmd[] = {
		0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
		0xd4, 0x01, 0x09, 0xf0, 0x00, 0x00, 0x00, 0x00,
		0xff, 0xff, 0xff, 0xff
	};
	static const u8 part_evt[] = {
		0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01
	};
	u8 address_cmd[] = {
		0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
		0x48, 0x03, 0x09, 0xf0, 0, 0, 0, 0,
		0xff, 0xff, 0xff, 0xff
	};
	int ret;

	ret = mt6628_xfer(wmt, part_cmd, sizeof(part_cmd),
			  part_evt, sizeof(part_evt));
	if (ret)
		return ret;
	memcpy(address_cmd + 12, address, 4);
	return mt6628_xfer(wmt, address_cmd, sizeof(address_cmd),
			   part_evt, sizeof(part_evt));
}

static int mt6628_download_patch(struct mt6628_wmt *wmt, const char *name,
				 const u8 address[4])
{
	const struct firmware *fw;
	u8 *cmd;
	size_t offset, body_len, frag;
	u8 hdr_address[4];
	int ret;

	ret = request_firmware(&fw, name, &wmt->serdev->dev);
	if (ret)
		return dev_err_probe(&wmt->serdev->dev, ret,
				     "cannot load %s\n", name);
	if (fw->size <= MT6628_PATCH_HEADER_SIZE) {
		ret = -EINVAL;
		goto out_release;
	}
	/*
	 * Like 6620_launcher: header byte 24 = (parts << 4) | index, bytes
	 * 25..27 = patch address; the first address byte is sent as 0.
	 */
	if (!address) {
		hdr_address[0] = 0;
		memcpy(hdr_address + 1, fw->data + 25, 3);
		address = hdr_address;
		dev_info(&wmt->serdev->dev, "%s: part %u/%u address %*ph\n",
			 name, fw->data[24] & 0xf, fw->data[24] >> 4, 4, address);
	}
	ret = mt6628_patch_address(wmt, address);
	if (ret)
		goto out_release;
	cmd = kmalloc(MT6628_PATCH_FRAGMENT + 5, GFP_KERNEL);
	if (!cmd) {
		ret = -ENOMEM;
		goto out_release;
	}

	body_len = fw->size - MT6628_PATCH_HEADER_SIZE;
	for (offset = 0; offset < body_len; offset += frag) {
		frag = min_t(size_t, MT6628_PATCH_FRAGMENT, body_len - offset);
		cmd[0] = 0x01;
		cmd[1] = 0x01;
		put_unaligned_le16(frag + 1, cmd + 2);
		cmd[4] = offset == 0 ? 1 :
			(offset + frag == body_len ? 3 : 2);
		memcpy(cmd + 5, fw->data + MT6628_PATCH_HEADER_SIZE + offset,
		       frag);
		ret = mt6628_xfer(wmt, cmd, frag + 5,
				  wmt_patch_evt, sizeof(wmt_patch_evt));
		if (ret) {
			dev_err(&wmt->serdev->dev,
				"%s fragment %zu failed: %d\n",
				name, offset / MT6628_PATCH_FRAGMENT, ret);
			break;
		}
	}
	if (!ret)
		dev_info(&wmt->serdev->dev, "downloaded %s (%zu bytes)\n",
			 name, body_len);
	kfree(cmd);
out_release:
	release_firmware(fw);
	return ret;
}

static int mt6628_do_reset(struct mt6628_wmt *wmt)
{
	return mt6628_xfer(wmt, wmt_reset, sizeof(wmt_reset),
			   wmt_reset_evt, sizeof(wmt_reset_evt));
}

static int mt6628_download_selected_patches(struct mt6628_wmt *wmt)
{
	/* E1 single patch: address unverified (no E1 chip seen). */
	static const u8 e1_address[4] = { 0x21, 0x00, 0x0e, 0xf0 };
	int ret;

	if ((wmt->hw_ver & 0xfff0) == 0x8a00) {
		ret = mt6628_download_patch(wmt, MT6628_FW_PATCH_E1,
					    e1_address);
		if (ret)
			return ret;
		return mt6628_do_reset(wmt);
	}
	/* Parts in header index order: _0 is part 1/2, _1 is part 2/2. */
	ret = mt6628_download_patch(wmt, MT6628_FW_PATCH_E2_0, NULL);
	if (ret)
		return ret;
	ret = mt6628_do_reset(wmt);
	if (ret)
		return ret;
	ret = mt6628_download_patch(wmt, MT6628_FW_PATCH_E2_1, NULL);
	if (ret)
		return ret;
	ret = mt6628_do_reset(wmt);
	if (ret)
		return ret;
	return mt6628_do_reset(wmt);
}

static int mt6628_power_on(struct mt6628_wmt *wmt)
{
	int ret;

	if (wmt->powered)
		return 0;
	gpiod_set_value_cansleep(wmt->pmu, 0);
	gpiod_set_value_cansleep(wmt->reset, 0);
	msleep(wmt->off_stable_ms);
	if (wmt->clk32k) {
		ret = clk_prepare_enable(wmt->clk32k);
		if (ret)
			return ret;
	}
	msleep(wmt->rtc_stable_ms);
	gpiod_set_value_cansleep(wmt->pmu, 1);
	msleep(wmt->reset_stable_ms);
	gpiod_set_value_cansleep(wmt->reset, 1);
	msleep(wmt->on_stable_ms);
	wmt->powered = true;
	return 0;
}

static void mt6628_power_off(struct mt6628_wmt *wmt)
{
	if (!wmt->powered)
		return;
	gpiod_set_value_cansleep(wmt->reset, 0);
	gpiod_set_value_cansleep(wmt->pmu, 0);
	if (wmt->clk32k)
		clk_disable_unprepare(wmt->clk32k);
	wmt->powered = false;
	wmt->initialized = false;
}

static int mt6628_chip_init(struct mt6628_wmt *wmt)
{
	const struct firmware *cfg;
	u8 baud_evt[10] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x02 };
	const u8 baud_115200_evt[] = {
		0x02, 0x04, 0x06, 0x00, 0x00, 0x02, 0x00, 0xc2, 0x01, 0x00
	};
	const u8 stp_default_evt[] = {
		0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00
	};
	const u8 wake_evt[] = { 0x02, 0x03, 0x02, 0x00, 0x00, 0x03 };
	const u8 wifi_on[] = { 0x01, 0x06, 0x02, 0x00, 0x03, 0x01 };
	const u8 func_evt[] = { 0x02, 0x06, 0x01, 0x00, 0x00 };
	u32 value;
	int ret;

	wmt->stp_mode = MT6628_STP_MANDATORY;
	wmt->tx_seq = 0;
	wmt->rx_ack = 0;
	mt6628_parser_reset(&wmt->parser);
	serdev_device_set_baudrate(wmt->serdev, 115200);
	serdev_device_set_flow_control(wmt->serdev, false);

	ret = mt6628_reg_read(wmt, 0x80000008, &value);
	if (ret || (value & 0xffff) != 0x6628)
		return ret ?: -ENODEV;
	ret = mt6628_reg_read(wmt, 0x80000000, &value);
	if (ret)
		return ret;
	wmt->hw_ver = value;
	ret = mt6628_reg_read(wmt, 0x80000004, &value);
	if (ret)
		return ret;
	wmt->fw_ver = value;
	dev_info(&wmt->serdev->dev, "MT6628 hw=%#04x rom=%#04x\n",
		 wmt->hw_ver, wmt->fw_ver);

	ret = request_firmware(&cfg, MT6628_FW_CONFIG, &wmt->serdev->dev);
	if (ret)
		return dev_err_probe(&wmt->serdev->dev, ret,
				     "cannot load %s\n", MT6628_FW_CONFIG);
	release_firmware(cfg);
	ret = mt6628_xfer(wmt, wmt_query_baud, sizeof(wmt_query_baud),
			  baud_115200_evt, sizeof(baud_115200_evt));
	if (ret)
		return ret;
	ret = mt6628_xfer(wmt, wmt_query_stp, sizeof(wmt_query_stp),
			  stp_default_evt, sizeof(stp_default_evt));
	if (ret)
		return ret;
	ret = mt6628_set_baud(wmt);
	if (ret)
		return ret;

	reinit_completion(&wmt->response);
	ret = mt6628_uart_write(wmt, (const u8[]){ 0xff }, 1);
	if (ret)
		return ret;
	if (!wait_for_completion_timeout(&wmt->response, MT6628_XFER_TIMEOUT) ||
	    wmt->response_len != sizeof(wake_evt) ||
	    memcmp(wmt->response_buf, wake_evt, sizeof(wake_evt)))
		return -EPROTO;
	put_unaligned_le32(wmt->operational_baud, baud_evt + 6);
	baud_evt[9] = 0;
	ret = mt6628_xfer(wmt, wmt_query_baud, sizeof(wmt_query_baud),
			  baud_evt, sizeof(baud_evt));
	if (ret)
		return ret;

	ret = mt6628_download_selected_patches(wmt);
	if (ret)
		return ret;
	ret = mt6628_xfer(wmt, wmt_coex, sizeof(wmt_coex),
			  wmt_coex_evt, sizeof(wmt_coex_evt));
	if (ret)
		return ret;
	ret = mt6628_xfer(wmt, wmt_sdio_drive, sizeof(wmt_sdio_drive),
			  wmt_sdio_drive_evt, sizeof(wmt_sdio_drive_evt));
	if (ret)
		return ret;
	ret = mt6628_xfer(wmt, wmt_set_stp, sizeof(wmt_set_stp),
			  wmt_set_stp_evt, sizeof(wmt_set_stp_evt));
	if (ret)
		return ret;

	wmt->stp_mode = MT6628_STP_FULL;
	wmt->tx_seq = 0;
	wmt->rx_ack = 0;
	msleep(10);
	ret = mt6628_xfer(wmt, wmt_query_stp, sizeof(wmt_query_stp),
			  wmt_query_stp_full_evt, sizeof(wmt_query_stp_full_evt));
	if (ret)
		return ret;
	ret = mt6628_xfer(wmt, wmt_query_baud, sizeof(wmt_query_baud),
			  baud_evt, sizeof(baud_evt));
	if (ret)
		return ret;
	ret = mt6628_xfer(wmt, wmt_fm_strap, sizeof(wmt_fm_strap),
			  wmt_fm_strap_evt, sizeof(wmt_fm_strap_evt));
	if (ret)
		return ret;
	/*
	 * The vendor WMT never sends WMT_FUNC_CTRL for Wi-Fi (only BT, FM,
	 * GPS): Wi-Fi on = SDIO slot on + WLAN driver FW download over SDIO.
	 * Sending it seems to leave the Wi-Fi CPU not accepting the host FW.
	 */
	if (wmt_wifi_func_ctrl) {
		ret = mt6628_xfer(wmt, wifi_on, sizeof(wifi_on),
				  func_evt, sizeof(func_evt));
		if (ret)
			return ret;
	}

	wmt->initialized = true;
	dev_info(&wmt->serdev->dev,
		 "WMT patch loaded and Wi-Fi function enabled\n");
	if (wmt->mmc) {
		/*
		 * MSDC3 is non-removable and its boot-time scan ran while the
		 * chip was off; re-arm the one-shot rescan.
		 */
		wmt->mmc->rescan_entered = 0;
		mmc_detect_change(wmt->mmc, msecs_to_jiffies(200));
	}
	return 0;
}

int mt6628_wmt_wifi_on(void)
{
	struct mt6628_wmt *wmt;
	int ret;

	mutex_lock(&mt6628_global_lock);
	wmt = mt6628_global;
	if (!wmt) {
		ret = -EPROBE_DEFER;
		goto out_global;
	}
	mutex_lock(&wmt->lock);
	if (wmt->initialized) {
		wmt->users++;
		ret = 0;
		goto out_wmt;
	}
	ret = mt6628_power_on(wmt);
	if (!ret)
		ret = mt6628_chip_init(wmt);
	if (ret) {
		dev_err(&wmt->serdev->dev, "WMT initialization failed: %d\n", ret);
		mt6628_power_off(wmt);
	} else {
		wmt->users++;
	}
out_wmt:
	mutex_unlock(&wmt->lock);
out_global:
	mutex_unlock(&mt6628_global_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6628_wmt_wifi_on);

void mt6628_wmt_wifi_off(void)
{
	struct mt6628_wmt *wmt;

	mutex_lock(&mt6628_global_lock);
	wmt = mt6628_global;
	if (!wmt)
		goto out;
	mutex_lock(&wmt->lock);
	if (wmt->users)
		wmt->users--;
	mutex_unlock(&wmt->lock);
out:
	mutex_unlock(&mt6628_global_lock);
}
EXPORT_SYMBOL_GPL(mt6628_wmt_wifi_off);

static irqreturn_t mt6628_bgf_irq_thread(int irq, void *data)
{
	struct mt6628_wmt *wmt = data;

	pm_wakeup_event(&wmt->serdev->dev, 250);
	return IRQ_HANDLED;
}

static int mt6628_get_mmc_host(struct mt6628_wmt *wmt)
{
	struct device_node *np;
	struct platform_device *pdev;

	np = of_parse_phandle(wmt->serdev->dev.of_node,
			      "mediatek,mmc-host", 0);
	if (!np)
		return -EINVAL;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -EPROBE_DEFER;
	wmt->mmc = platform_get_drvdata(pdev);
	put_device(&pdev->dev);
	return wmt->mmc ? 0 : -EPROBE_DEFER;
}

static bool bgf_irq = true;
module_param(bgf_irq, bool, 0644);
MODULE_PARM_DESC(bgf_irq, "request the BGF EINT (level low)");

static bool probe_only;
module_param(probe_only, bool, 0644);
MODULE_PARM_DESC(probe_only, "open UART3 only, do not power the chip");

static int mt6628_wmt_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct mt6628_wmt *wmt;
	int irq, ret;

	wmt = devm_kzalloc(dev, sizeof(*wmt), GFP_KERNEL);
	if (!wmt)
		return -ENOMEM;
	wmt->serdev = serdev;
	mutex_init(&wmt->lock);
	spin_lock_init(&wmt->rx_lock);
	init_completion(&wmt->response);
	wmt->operational_baud = 2000000;
	device_property_read_u32(dev, "mediatek,operational-speed",
				 &wmt->operational_baud);
	mt6628_read_delay(dev, "mediatek,rtc-stable-ms",
			  &wmt->rtc_stable_ms, 100);
	mt6628_read_delay(dev, "mediatek,off-stable-ms",
			  &wmt->off_stable_ms, 10);
	mt6628_read_delay(dev, "mediatek,reset-stable-ms",
			  &wmt->reset_stable_ms, 30);
	mt6628_read_delay(dev, "mediatek,on-stable-ms",
			  &wmt->on_stable_ms, 100);

	wmt->pmu = devm_gpiod_get(dev, "pmu-en", GPIOD_OUT_LOW);
	if (IS_ERR(wmt->pmu))
		return dev_err_probe(dev, PTR_ERR(wmt->pmu), "PMU GPIO\n");
	wmt->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(wmt->reset))
		return dev_err_probe(dev, PTR_ERR(wmt->reset), "reset GPIO\n");
	wmt->clk32k = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(wmt->clk32k))
		return dev_err_probe(dev, PTR_ERR(wmt->clk32k), "32 kHz clock\n");
	ret = mt6628_get_mmc_host(wmt);
	if (ret)
		return dev_err_probe(dev, ret, "MSDC3 host\n");

	serdev_device_set_drvdata(serdev, wmt);
	serdev_device_set_client_ops(serdev, &mt6628_serdev_ops);
	ret = serdev_device_open(serdev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot open UART3\n");

	dev_info(dev, "probe: UART3 open, bgf_irq=%d probe_only=%d\n", bgf_irq, probe_only);
	irq = bgf_irq ? of_irq_get(dev->of_node, 0) : 0;
	if (irq > 0) {
		dev_info(dev, "probe: BGF irq %d\n", irq);
		ret = devm_request_threaded_irq(dev, irq, NULL,
					mt6628_bgf_irq_thread,
					IRQF_ONESHOT | IRQF_TRIGGER_LOW,
					dev_name(dev), wmt);
		if (ret)
			goto err_close;
	} else if (irq == -EPROBE_DEFER) {
		ret = irq;
		goto err_close;
	}

	device_init_wakeup(dev, true);
	mutex_lock(&mt6628_global_lock);
	if (mt6628_global) {
		mutex_unlock(&mt6628_global_lock);
		ret = -EBUSY;
		goto err_wakeup;
	}
	mt6628_global = wmt;
	mutex_unlock(&mt6628_global_lock);

	if (probe_only) {
		dev_info(dev, "probe_only: chip left off\n");
		return 0;
	}
	ret = mt6628_wmt_wifi_on();
	if (ret)
		goto err_global;
	dev_info(dev, "MT6628 WMT ready at %u baud\n", wmt->operational_baud);
	return 0;

err_global:
	mutex_lock(&mt6628_global_lock);
	mt6628_global = NULL;
	mutex_unlock(&mt6628_global_lock);
err_wakeup:
	device_init_wakeup(dev, false);
err_close:
	serdev_device_close(serdev);
	return ret;
}

static void mt6628_wmt_remove(struct serdev_device *serdev)
{
	struct mt6628_wmt *wmt = serdev_device_get_drvdata(serdev);

	mutex_lock(&mt6628_global_lock);
	if (mt6628_global == wmt)
		mt6628_global = NULL;
	mutex_unlock(&mt6628_global_lock);
	mutex_lock(&wmt->lock);
	wmt->users = 0;
	mt6628_power_off(wmt);
	mutex_unlock(&wmt->lock);
	device_init_wakeup(&serdev->dev, false);
	serdev_device_close(serdev);
}

static const struct of_device_id mt6628_wmt_of_match[] = {
	{ .compatible = "mediatek,mt6628-wmt" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6628_wmt_of_match);

static struct serdev_device_driver mt6628_wmt_driver = {
	.probe = mt6628_wmt_probe,
	.remove = mt6628_wmt_remove,
	.driver = {
		.name = "mt6628-wmt",
		.of_match_table = mt6628_wmt_of_match,
	},
};
module_serdev_device_driver(mt6628_wmt_driver);

MODULE_AUTHOR("Lenovo B8000-H Linux port contributors");
MODULE_DESCRIPTION("MediaTek MT6628 WMT/STP serdev driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(MT6628_FW_PATCH_E1);
MODULE_FIRMWARE(MT6628_FW_PATCH_E2_0);
MODULE_FIRMWARE(MT6628_FW_PATCH_E2_1);
MODULE_FIRMWARE(MT6628_FW_CONFIG);
