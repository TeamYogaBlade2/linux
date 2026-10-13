// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6628 shared WMT/STP transport over UART.
 *
 * This is the UART backend for the existing MT6628 child-device API.  It
 * implements the mandatory STP framing used during WMT bootstrap and the
 * sequenced/CRC-protected STP framing used after the UART-full handoff.
 *
 * The SDIO backend remains a separate driver; exactly one backend should be
 * selected by Kconfig for a given board.
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/kfifo.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include <linux/mfd/mt6628.h>

#include "mtk-stp-internal.h"

#define MT6628_STP_HEADER_SIZE             4
#define MT6628_STP_TRAILER_SIZE            2
#define MT6628_STP_MAX_PAYLOAD             1999
#define MT6628_STP_RX_FIFO_SIZE            16384
#define MT6628_STP_TX_TIMEOUT_MS            1000
#define MT6628_WMT_RESPONSE_MAX             32
#define MT6628_WMT_CRYSTAL_TRIM_OPCODE      0x12
#define MT6628_WMT_GEN_HVR                  0x80000000
#define MT6628_WMT_GEN_FVR                  0x80000004
#define MT6628_WMT_GEN_VER_MASK             0x0000ffff
#define MT6628_WMT_PATCH_HDR_SIZE           28
/*
 * The vendor launcher matches the low byte of the 16-bit firmware
 * version stored at bytes 22..23 of the on-disk patch header.
 */
#define MT6628_WMT_PATCH_FWVER_LSB_OFFSET   23
#define MT6628_WMT_PATCH_INFO_OFFSET        24
#define MT6628_WMT_PATCH_FRAG_SIZE          1000
#define MT6628_WMT_UART_BOOT_BAUD            115200
#define MT6628_WMT_UART_DEFAULT_BAUD         4000000
#define MT6628_STP_DELIMITER                 0x55
#define MT6628_STP_RESYNC                    0x7f

/* WMT command/event bodies are little-endian and carried inside STP task 4. */
static const u8 mt6628_wmt_query_baud_cmd[] = {
	0x01, 0x04, 0x01, 0x00, 0x02,
};
static const u8 mt6628_wmt_query_baud_boot_evt[] = {
	0x02, 0x04, 0x06, 0x00, 0x00, 0x02, 0x00, 0xc2, 0x01, 0x00,
};
static const u8 mt6628_wmt_query_stp_cmd[] = {
	0x01, 0x04, 0x01, 0x00, 0x04,
};
static const u8 mt6628_wmt_query_stp_boot_evt[] = {
	0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0x11, 0x00, 0x00, 0x00,
};
static const u8 mt6628_wmt_set_baud_evt[] = {
	0x02, 0x04, 0x02, 0x00, 0x00, 0x01,
};
static const u8 mt6628_wmt_wakeup_evt[] = {
	0x02, 0x03, 0x02, 0x00, 0x00, 0x03,
};
static const u8 mt6628_wmt_patch_addr_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
};
static const u8 mt6628_wmt_patch_evt[] = {
	0x02, 0x01, 0x01, 0x00, 0x00,
};
static const u8 mt6628_wmt_reset_cmd[] = {
	0x01, 0x07, 0x01, 0x00, 0x04,
};
static const u8 mt6628_wmt_coex_evt[] = {
	0x02, 0x10, 0x01, 0x00, 0x00,
};
static const u8 mt6628_wmt_full_stp_evt[] = {
	0x02, 0x04, 0x02, 0x00, 0x00, 0x03,
};
static const u8 mt6628_wmt_full_stp_query_evt[] = {
	0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdf, 0x0e, 0x68, 0x01,
};

static const char * const mt6628_e1_patch_names[] = {
	"mt6628_patch_e1_hdr.bin",
};
static const char * const mt6628_e2_patch_names[] = {
	"mt6628_patch_e2_0_hdr.bin",
	"mt6628_patch_e2_1_hdr.bin",
};

struct mt6628_stp_endpoint {
	mt6628_stp_rx_cb cb;
	void *priv;
};

enum mt6628_uart_rx_state {
	MT6628_UART_RX_SYNC,
	MT6628_UART_RX_HEADER,
	MT6628_UART_RX_PAYLOAD,
	MT6628_UART_RX_CRC_LO,
	MT6628_UART_RX_CRC_HI,
	MT6628_UART_RX_TRAILER,
};

struct mt6628_wmt {
	struct serdev_device *serdev;
	struct work_struct rx_work;
	DECLARE_KFIFO(rx_fifo, u8, MT6628_STP_RX_FIFO_SIZE);
	spinlock_t rx_fifo_lock;
	struct mutex tx_lock;
	struct mutex cmd_lock;
	struct mutex endpoint_lock;
	struct mutex wmt_lock;
	struct completion wmt_done;
	struct completion tx_ack_done;

	struct mt6628_stp_endpoint endpoint[MT6628_STP_TASK_MAX];
	bool stopping;
	bool full_mode;
	bool wmt_waiting;
	bool tx_waiting;
	u8 wmt_wait_opcode;
	int wmt_status;
	u8 wmt_response[MT6628_WMT_RESPONSE_MAX];
	size_t wmt_response_len;

	/* UART-full STP is stop-and-wait here: one unacknowledged TX frame. */
	u8 tx_seq;
	u8 tx_expected_ack;
	u8 rx_expected_seq;
	u8 rx_ack;
	int tx_status;

	/* Incremental RX parser; receive_buf() itself only enqueues bytes. */
	enum mt6628_uart_rx_state rx_state;
	u8 header[MT6628_STP_HEADER_SIZE];
	size_t header_pos;
	u8 payload[MT6628_STP_MAX_PAYLOAD];
	size_t payload_len;
	size_t payload_pos;
	u16 rx_crc;
	u8 rx_trailer_pos;
	u8 resync_count;

	u32 baudrate;
	u8 coex_ant_mode;
	bool coex_configured;
	bool sdio_driving_configured;
	u32 sdio_driving_cfg;
	u8 fm_strap_mode;
	bool crystal_trim_configured;
	u8 crystal_trim;
	bool co_clock_enabled;
	u16 hw_ver;
	u16 rom_ver;
};

static int mt6628_uart_write(struct mt6628_wmt *wmt,
			     const u8 *buf, size_t len)
{
	ssize_t ret;

	if (READ_ONCE(wmt->stopping))
		return -ESHUTDOWN;

	ret = serdev_device_write(wmt->serdev, buf, len,
				  msecs_to_jiffies(MT6628_STP_TX_TIMEOUT_MS));
	if (!READ_ONCE(wmt->full_mode))
		dev_info(&wmt->serdev->dev,
			 "UART TX request=%zu accepted=%zd data=%*ph\n",
			 len, ret, (int)min_t(size_t, len, 32), buf);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EIO;

	return 0;
}

static void mt6628_uart_send_ack(struct mt6628_wmt *wmt)
{
	/* Aquaris-5 defaults to NAK-disabled operation. */
	u8 frame[MT6628_STP_HEADER_SIZE] = {
		0x80 | (wmt->rx_ack & 0x07),
		0x00,
		0x00,
		0x00,
	};

	frame[3] = (frame[0] + frame[1] + frame[2]) & 0xff;
	mt6628_uart_write(wmt, frame, sizeof(frame));
}

static void mt6628_stp_dispatch(struct mt6628_wmt *wmt,
				enum mt6628_stp_task task,
				const u8 *buf, size_t len)
{
	if (task == MT6628_STP_TASK_WMT) {
		u16 payload_len;

		dev_info(&wmt->serdev->dev,
			 "STP WMT packet len=%zu data=%*ph\n",
			 len, (int)min_t(size_t, len, 32), buf);
		if (len < 4 || buf[0] != 0x02)
			return;
		payload_len = get_unaligned_le16(buf + 2);
		if (payload_len != len - 4) {
			dev_dbg(&wmt->serdev->dev,
				"invalid WMT event length: opcode=%#x field=%u packet=%zu\n",
				buf[1], payload_len, len);
			return;
		}

		mutex_lock(&wmt->wmt_lock);
		if (wmt->wmt_waiting && buf[1] == wmt->wmt_wait_opcode) {
			if (len < 5) {
				wmt->wmt_status = -EPROTO;
				wmt->wmt_response_len = 0;
			} else {
				wmt->wmt_status = buf[4] ? -EIO : 0;
				wmt->wmt_response_len =
					min_t(size_t, len, sizeof(wmt->wmt_response));
				memcpy(wmt->wmt_response, buf,
				       wmt->wmt_response_len);
			}
			wmt->wmt_waiting = false;
			complete(&wmt->wmt_done);
		}
		mutex_unlock(&wmt->wmt_lock);
		return;
	}

	if (task >= MT6628_STP_TASK_MAX)
		return;

	mutex_lock(&wmt->endpoint_lock);
	if (wmt->endpoint[task].cb)
		wmt->endpoint[task].cb(wmt->endpoint[task].priv, buf, len);
	mutex_unlock(&wmt->endpoint_lock);
}

static void mt6628_uart_parser_reset(struct mt6628_wmt *wmt)
{
	wmt->rx_state = MT6628_UART_RX_SYNC;
	wmt->header_pos = 0;
	wmt->payload_len = 0;
	wmt->payload_pos = 0;
	wmt->rx_crc = 0;
	wmt->rx_trailer_pos = 0;
	wmt->resync_count = 0;
}

static void mt6628_uart_process_tx_ack(struct mt6628_wmt *wmt)
{
	u8 ack = wmt->header[0] & 0x07;

	if (!READ_ONCE(wmt->tx_waiting))
		return;

	if (wmt->header[1] & BIT(7))
		WRITE_ONCE(wmt->tx_status, -EAGAIN);
	else if (ack == wmt->tx_expected_ack)
		WRITE_ONCE(wmt->tx_status, 0);
	else
		return;

	WRITE_ONCE(wmt->tx_waiting, false);
	complete(&wmt->tx_ack_done);
}

static void mt6628_uart_process_full_frame(struct mt6628_wmt *wmt)
{
	u8 seq = (wmt->header[0] >> 3) & 0x07;
	u8 task = (wmt->header[1] >> 4) & 0x07;
	u16 crc;

	/* ACK-only frames have no payload CRC. */
	if (!wmt->payload_len) {
		mt6628_uart_process_tx_ack(wmt);
		mt6628_uart_parser_reset(wmt);
		return;
	}

	crc = crc16(0, wmt->payload, wmt->payload_len);
	if (crc != wmt->rx_crc) {
		dev_dbg(&wmt->serdev->dev,
			"STP UART CRC mismatch: got %#x expected %#x\n",
			wmt->rx_crc, crc);
		mt6628_uart_parser_reset(wmt);
		return;
	}

	/*
	 * Only a frame in the expected RX sequence advances either the RX
	 * sequence or the peer's TX acknowledgement. The downstream queues
	 * a valid packet before accepting its piggy-backed ACK.
	 */
	if (seq != wmt->rx_expected_seq) {
		/* NAK is disabled by default in the shipping downstream. */
		mt6628_uart_send_ack(wmt);
		mt6628_uart_parser_reset(wmt);
		return;
	}

	mt6628_stp_dispatch(wmt, task, wmt->payload, wmt->payload_len);
	mt6628_uart_process_tx_ack(wmt);
	wmt->rx_ack = seq;
	wmt->rx_expected_seq = (seq + 1) & 0x07;
	mt6628_uart_send_ack(wmt);
	mt6628_uart_parser_reset(wmt);
}

static void mt6628_uart_parse_byte(struct mt6628_wmt *wmt, u8 byte)
{
	if (!READ_ONCE(wmt->full_mode)) {
		switch (wmt->rx_state) {
		case MT6628_UART_RX_SYNC:
			if (byte == 0x80) {
				wmt->header[0] = byte;
				wmt->header_pos = 1;
				wmt->rx_state = MT6628_UART_RX_HEADER;
			}
			break;
		case MT6628_UART_RX_HEADER:
			wmt->header[wmt->header_pos++] = byte;
			if (wmt->header_pos == MT6628_STP_HEADER_SIZE) {
				wmt->payload_len =
					((wmt->header[1] & 0x0f) << 8) |
					wmt->header[2];
				if (wmt->header[3] != 0 ||
				    wmt->payload_len > MT6628_STP_MAX_PAYLOAD) {
					mt6628_uart_parser_reset(wmt);
				} else if (!wmt->payload_len) {
					mt6628_uart_parser_reset(wmt);
				} else {
					wmt->payload_pos = 0;
					wmt->rx_state = MT6628_UART_RX_PAYLOAD;
				}
			}
			break;
		case MT6628_UART_RX_PAYLOAD:
			wmt->payload[wmt->payload_pos++] = byte;
			if (wmt->payload_pos == wmt->payload_len) {
				wmt->rx_trailer_pos = 0;
				wmt->rx_state = MT6628_UART_RX_TRAILER;
			}
			break;
		case MT6628_UART_RX_TRAILER:
			/* UART mandatory mode uses a zero, two-byte trailer. */
			if (byte) {
				mt6628_uart_parser_reset(wmt);
				break;
			}
			if (++wmt->rx_trailer_pos == MT6628_STP_TRAILER_SIZE) {
				u8 task = (wmt->header[1] >> 4) & 0x07;

				if (task < MT6628_STP_TASK_MAX)
					mt6628_stp_dispatch(wmt, task,
							    wmt->payload,
							    wmt->payload_len);
				mt6628_uart_parser_reset(wmt);
			}
			break;
		default:
			mt6628_uart_parser_reset(wmt);
			break;
		}
		return;
	}

	/* UART-full mode uses delimiters and a checksummed four-byte header. */
	switch (wmt->rx_state) {
	case MT6628_UART_RX_SYNC:
		if (byte == MT6628_STP_DELIMITER) {
			wmt->resync_count = 0;
			break;
		}
		if (byte == MT6628_STP_RESYNC) {
			if (++wmt->resync_count == 4)
				mt6628_uart_parser_reset(wmt);
			break;
		}
		wmt->resync_count = 0;
		if ((byte & 0xc0) == 0x80) {
			wmt->header[0] = byte;
			wmt->header_pos = 1;
			wmt->rx_state = MT6628_UART_RX_HEADER;
		}
		break;
	case MT6628_UART_RX_HEADER:
		wmt->header[wmt->header_pos++] = byte;
		if (wmt->header_pos == MT6628_STP_HEADER_SIZE) {
			u8 task = (wmt->header[1] >> 4) & 0x07;
			u16 len = ((wmt->header[1] & 0x0f) << 8) |
				  wmt->header[2];

			if (((wmt->header[0] + wmt->header[1] +
			      wmt->header[2]) & 0xff) != wmt->header[3] ||
			    task >= MT6628_STP_TASK_MAX ||
			    len > MT6628_STP_MAX_PAYLOAD) {
				mt6628_uart_parser_reset(wmt);
			} else {
				wmt->payload_len = len;
				wmt->payload_pos = 0;
				if (!len) {
					mt6628_uart_process_full_frame(wmt);
				} else {
					wmt->rx_state = MT6628_UART_RX_PAYLOAD;
				}
			}
		}
		break;
	case MT6628_UART_RX_PAYLOAD:
		wmt->payload[wmt->payload_pos++] = byte;
		if (wmt->payload_pos == wmt->payload_len)
			wmt->rx_state = MT6628_UART_RX_CRC_LO;
		break;
	case MT6628_UART_RX_CRC_LO:
		wmt->rx_crc = byte;
		wmt->rx_state = MT6628_UART_RX_CRC_HI;
		break;
	case MT6628_UART_RX_CRC_HI:
		wmt->rx_crc |= (u16)byte << 8;
		mt6628_uart_process_full_frame(wmt);
		break;
	case MT6628_UART_RX_TRAILER:
	default:
		mt6628_uart_parser_reset(wmt);
		break;
	}
}

static void mt6628_uart_rx_work(struct work_struct *work)
{
	struct mt6628_wmt *wmt = container_of(work, struct mt6628_wmt,
					     rx_work);
	u8 data[512];
	unsigned int count, i;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&wmt->rx_fifo_lock, flags);
		count = kfifo_out(&wmt->rx_fifo, data, sizeof(data));
		spin_unlock_irqrestore(&wmt->rx_fifo_lock, flags);
		if (!count)
			break;

		for (i = 0; i < count; i++) {
			mt6628_uart_parse_byte(wmt, data[i]);
		}
	}
}

static size_t mt6628_uart_receive_buf(struct serdev_device *serdev,
				      const unsigned char *data,
				      size_t count)
{
	struct mt6628_wmt *wmt = serdev_device_get_drvdata(serdev);
	unsigned int copied;

	if (!wmt || READ_ONCE(wmt->stopping))
		return 0;

	copied = kfifo_in_spinlocked(&wmt->rx_fifo, data, count,
				     &wmt->rx_fifo_lock);
	if (copied != count)
		dev_warn_ratelimited(&serdev->dev,
				     "UART RX FIFO overflow; dropped %zu bytes\n",
				     count - copied);
	if (copied) {
		if (!READ_ONCE(wmt->full_mode))
			dev_info(&serdev->dev,
				 "UART RX copied=%u input=%zu data=%*ph\n",
				 copied, count,
				 (int)min_t(size_t, copied, 32), data);
		schedule_work(&wmt->rx_work);
	}

	return copied;
}

static void mt6628_uart_write_wakeup(struct serdev_device *serdev)
{
	serdev_device_write_wakeup(serdev);
}

static const struct serdev_device_ops mt6628_uart_serdev_ops = {
	.receive_buf = mt6628_uart_receive_buf,
	.write_wakeup = mt6628_uart_write_wakeup,
};

static int mt6628_stp_send_mandatory(struct mt6628_wmt *wmt,
				     enum mt6628_stp_task task,
				     const void *buf, size_t len)
{
	u8 *frame;
	size_t frame_len = MT6628_STP_HEADER_SIZE + len +
			   MT6628_STP_TRAILER_SIZE;
	int ret;

	if (len > MT6628_STP_MAX_PAYLOAD || task >= MT6628_STP_TASK_MAX)
		return -EMSGSIZE;

	frame = kzalloc(frame_len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	frame[0] = 0x80;
	frame[1] = (task << 4) | ((len >> 8) & 0x0f);
	frame[2] = len & 0xff;
	frame[3] = 0x00;
	memcpy(frame + MT6628_STP_HEADER_SIZE, buf, len);
	ret = mt6628_uart_write(wmt, frame, frame_len);
	kfree(frame);
	return ret;
}

static int mt6628_stp_send_full(struct mt6628_wmt *wmt,
				 enum mt6628_stp_task task,
				 const void *buf, size_t len)
{
	u8 *frame;
	size_t frame_len = MT6628_STP_HEADER_SIZE + len +
			   MT6628_STP_TRAILER_SIZE;
	u8 seq, expected_ack;
	u16 crc;
	int retry, ret = -ETIMEDOUT;

	if (len > MT6628_STP_MAX_PAYLOAD || task >= MT6628_STP_TASK_MAX)
		return -EMSGSIZE;

	frame = kmalloc(frame_len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	mutex_lock(&wmt->tx_lock);
	seq = wmt->tx_seq;
	expected_ack = seq;
	/* The shipping downstream defaults to no 0x55 delimiter. */
	frame[0] = 0x80 | (seq << 3) | (wmt->rx_ack & 0x07);
	frame[1] = (task << 4) | ((len >> 8) & 0x0f);
	frame[2] = len & 0xff;
	frame[3] = (frame[0] + frame[1] + frame[2]) & 0xff;
	memcpy(frame + MT6628_STP_HEADER_SIZE, buf, len);
	crc = crc16(0, buf, len);
	put_unaligned_le16(crc, frame + MT6628_STP_HEADER_SIZE + len);

	wmt->tx_expected_ack = expected_ack;
	for (retry = 0; retry < 3; retry++) {
		reinit_completion(&wmt->tx_ack_done);
		WRITE_ONCE(wmt->tx_status, -ETIMEDOUT);
		WRITE_ONCE(wmt->tx_waiting, true);
		ret = mt6628_uart_write(wmt, frame, frame_len);
		if (ret) {
			WRITE_ONCE(wmt->tx_waiting, false);
			break;
		}
		if (wait_for_completion_timeout(&wmt->tx_ack_done,
						msecs_to_jiffies(MT6628_STP_TX_TIMEOUT_MS))) {
			ret = READ_ONCE(wmt->tx_status);
			if (!ret)
				break;
		} else {
			ret = -ETIMEDOUT;
		}
		WRITE_ONCE(wmt->tx_waiting, false);
	}
	WRITE_ONCE(wmt->tx_waiting, false);
	if (!ret)
		wmt->tx_seq = (seq + 1) & 0x07;
	mutex_unlock(&wmt->tx_lock);
	kfree(frame);
	return ret;
}

static int mt6628_uart_send(struct mt6628_wmt *wmt, enum mt6628_stp_task task,
		    const void *buf, size_t len)
{
	int ret;

	if (!wmt || (!buf && len))
		return -EINVAL;
	if (READ_ONCE(wmt->stopping))
		return -ESHUTDOWN;

	if (READ_ONCE(wmt->full_mode))
		ret = mt6628_stp_send_full(wmt, task, buf, len);
	else
		ret = mt6628_stp_send_mandatory(wmt, task, buf, len);

	return ret ? ret : len;
}

static int mt6628_uart_register_rx(struct mt6628_wmt *wmt,
			   enum mt6628_stp_task task,
			   mt6628_stp_rx_cb cb, void *priv)
{
	if (!wmt || task >= MT6628_STP_TASK_MAX || !cb)
		return -EINVAL;

	mutex_lock(&wmt->endpoint_lock);
	if (wmt->endpoint[task].cb) {
		mutex_unlock(&wmt->endpoint_lock);
		return -EBUSY;
	}
	wmt->endpoint[task].cb = cb;
	wmt->endpoint[task].priv = priv;
	mutex_unlock(&wmt->endpoint_lock);
	return 0;
}

static void mt6628_uart_unregister_rx(struct mt6628_wmt *wmt,
			      enum mt6628_stp_task task,
			      mt6628_stp_rx_cb cb, void *priv)
{
	if (!wmt || task >= MT6628_STP_TASK_MAX)
		return;

	mutex_lock(&wmt->endpoint_lock);
	if (wmt->endpoint[task].cb == cb &&
	    wmt->endpoint[task].priv == priv) {
		wmt->endpoint[task].cb = NULL;
		wmt->endpoint[task].priv = NULL;
	}
	mutex_unlock(&wmt->endpoint_lock);
}

static int __mt6628_wmt_cmd(struct mt6628_wmt *wmt, const u8 *cmd,
			  size_t len, u8 opcode, unsigned int timeout_ms,
			  u8 *response, size_t *response_len)
{
	unsigned long timeout;
	int ret;

	reinit_completion(&wmt->wmt_done);
	mutex_lock(&wmt->wmt_lock);
	wmt->wmt_waiting = true;
	wmt->wmt_wait_opcode = opcode;
	wmt->wmt_status = -ETIMEDOUT;
	wmt->wmt_response_len = 0;
	mutex_unlock(&wmt->wmt_lock);

	ret = mt6628_uart_send(wmt, MT6628_STP_TASK_WMT, cmd, len);
	if (ret < 0) {
		dev_err(&wmt->serdev->dev,
			"WMT opcode %#x transmit failed: %d\n", opcode, ret);
		mutex_lock(&wmt->wmt_lock);
		wmt->wmt_waiting = false;
		mutex_unlock(&wmt->wmt_lock);
		return ret;
	}

	timeout = wait_for_completion_timeout(&wmt->wmt_done,
					     msecs_to_jiffies(timeout_ms));
	mutex_lock(&wmt->wmt_lock);
	if (!timeout && wmt->wmt_waiting) {
		wmt->wmt_waiting = false;
		ret = -ETIMEDOUT;
		dev_err(&wmt->serdev->dev,
			"WMT opcode %#x response timeout; request len=%zu\n",
			opcode, len);
	} else {
		ret = wmt->wmt_status;
	}
	if (!ret && response && response_len) {
		size_t n = min_t(size_t, wmt->wmt_response_len, *response_len);

		memcpy(response, wmt->wmt_response, n);
		*response_len = n;
	}
	wmt->wmt_wait_opcode = 0xff;
	mutex_unlock(&wmt->wmt_lock);
	return ret;
}

static int mt6628_wmt_cmd(struct mt6628_wmt *wmt, const u8 *cmd,
			  size_t len, u8 opcode, unsigned int timeout_ms,
			  u8 *response, size_t *response_len)
{
	int ret;

	mutex_lock(&wmt->cmd_lock);
	ret = __mt6628_wmt_cmd(wmt, cmd, len, opcode, timeout_ms,
			       response, response_len);
	mutex_unlock(&wmt->cmd_lock);
	return ret;
}

static int mt6628_wmt_reg_write(struct mt6628_wmt *wmt,
				u32 addr, u32 value, u32 mask)
{
	u8 cmd[20] = { 0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01 };

	put_unaligned_le32(addr, cmd + 8);
	put_unaligned_le32(value, cmd + 12);
	put_unaligned_le32(mask, cmd + 16);
	return mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x08, 1000, NULL, NULL);
}

static int mt6628_wmt_reg_read(struct mt6628_wmt *wmt,
			       u32 addr, u32 mask, u32 *value)
{
	u8 cmd[20] = { 0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01 };
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len = sizeof(response);
	u16 payload_len;
	int ret;

	put_unaligned_le32(addr, cmd + 8);
	put_unaligned_le32(0, cmd + 12);
	put_unaligned_le32(mask, cmd + 16);
	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x08, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != 16 ||
	    response[0] != 0x02 || response[1] != 0x08)
		return -EPROTO;

	payload_len = get_unaligned_le16(response + 2);
	if (payload_len != response_len - 4 ||
	    response[4] || response[5] != 0 || response[6] != 0 ||
	    response[7] != 1 ||
	    get_unaligned_le32(response + 8) != addr)
		return -EPROTO;
	*value = get_unaligned_le32(response + 12) & mask;
	return 0;
}

static int mt6628_wmt_read_versions(struct mt6628_wmt *wmt)
{
	u32 value;
	int ret;

	ret = mt6628_wmt_reg_read(wmt, MT6628_WMT_GEN_HVR,
				  MT6628_WMT_GEN_VER_MASK, &value);
	if (ret)
		return ret;
	wmt->hw_ver = value;
	ret = mt6628_wmt_reg_read(wmt, MT6628_WMT_GEN_FVR,
				  MT6628_WMT_GEN_VER_MASK, &value);
	if (ret)
		return ret;
	wmt->rom_ver = value;
	dev_info(&wmt->serdev->dev, "MT6628 WMT hardware %#x ROM %#x\n",
		 wmt->hw_ver, wmt->rom_ver);
	return 0;
}

static int mt6628_wmt_reset(struct mt6628_wmt *wmt)
{
	return mt6628_wmt_cmd(wmt, mt6628_wmt_reset_cmd,
			      sizeof(mt6628_wmt_reset_cmd), 0x07, 1000,
			      NULL, NULL);
}

static int mt6628_wmt_set_baud(struct mt6628_wmt *wmt)
{
	u8 cmd[] = { 0x01, 0x04, 0x05, 0x00, 0x01, 0, 0, 0, 0x00 };
	u8 response[MT6628_WMT_RESPONSE_MAX];
	u8 expected_baud_evt[] = {
		0x02, 0x04, 0x06, 0x00, 0x00, 0x02, 0, 0, 0, 0,
	};
	size_t response_len = sizeof(response);
	unsigned long timeout;
	int ret;

	ret = mt6628_wmt_cmd(wmt, mt6628_wmt_query_baud_cmd,
			     sizeof(mt6628_wmt_query_baud_cmd), 0x04, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != sizeof(mt6628_wmt_query_baud_boot_evt) ||
	    memcmp(response, mt6628_wmt_query_baud_boot_evt,
		   sizeof(mt6628_wmt_query_baud_boot_evt)))
		return -EPROTO;

	response_len = sizeof(response);
	ret = mt6628_wmt_cmd(wmt, mt6628_wmt_query_stp_cmd,
			     sizeof(mt6628_wmt_query_stp_cmd), 0x04, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != sizeof(mt6628_wmt_query_stp_boot_evt) ||
	    memcmp(response, mt6628_wmt_query_stp_boot_evt,
		   sizeof(mt6628_wmt_query_stp_boot_evt)))
		return -EPROTO;

	put_unaligned_le32(wmt->baudrate, cmd + 5);
	cmd[8] = 0; /* The Yoga schematic straps UART_RTS for mode selection. */
	response_len = sizeof(response);
	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x04, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != sizeof(mt6628_wmt_set_baud_evt) ||
	    memcmp(response, mt6628_wmt_set_baud_evt,
		   sizeof(mt6628_wmt_set_baud_evt)))
		return -EPROTO;

	if (serdev_device_set_baudrate(wmt->serdev, wmt->baudrate) != wmt->baudrate)
		return -EINVAL;
	msleep(5);

	/*
	 * The wakeup trigger is a raw 0xff byte, but the observed
	 * wakeup event is carried in a mandatory-mode STP frame.
	 * Leave RX parsing enabled and wait for WMT opcode 0x03.
	 */
	reinit_completion(&wmt->wmt_done);
	mutex_lock(&wmt->wmt_lock);
	wmt->wmt_waiting = true;
	wmt->wmt_wait_opcode = 0x03;
	wmt->wmt_status = -ETIMEDOUT;
	wmt->wmt_response_len = 0;
	mutex_unlock(&wmt->wmt_lock);

	ret = mt6628_uart_write(wmt, (const u8[]){ 0xff }, 1);
	if (ret) {
		mutex_lock(&wmt->wmt_lock);
		wmt->wmt_waiting = false;
		wmt->wmt_wait_opcode = 0xff;
		mutex_unlock(&wmt->wmt_lock);
		return ret;
	}

	timeout = wait_for_completion_timeout(&wmt->wmt_done,
					      msecs_to_jiffies(1000));
	mutex_lock(&wmt->wmt_lock);
	if (!timeout && wmt->wmt_waiting) {
		wmt->wmt_waiting = false;
		ret = -ETIMEDOUT;
		dev_err(&wmt->serdev->dev,
			"WMT wake event timeout after raw 0xff\n");
	} else {
		ret = wmt->wmt_status;
	}
	if (!ret) {
		response_len = min_t(size_t, wmt->wmt_response_len,
				     sizeof(response));
		memcpy(response, wmt->wmt_response, response_len);
	}
	wmt->wmt_wait_opcode = 0xff;
	mutex_unlock(&wmt->wmt_lock);

	if (ret)
		return ret;
	if (response_len != sizeof(mt6628_wmt_wakeup_evt) ||
	    memcmp(response, mt6628_wmt_wakeup_evt,
		   sizeof(mt6628_wmt_wakeup_evt)))
		return -EPROTO;

	/*
	 * The downstream performs a baud-rate query after waking the chip.
	 * Verify that both ends are operating at the requested rate before
	 * beginning firmware download.
	 */
	put_unaligned_le32(wmt->baudrate, expected_baud_evt + 6);
	response_len = sizeof(response);
	ret = mt6628_wmt_cmd(wmt, mt6628_wmt_query_baud_cmd,
			     sizeof(mt6628_wmt_query_baud_cmd), 0x04, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != sizeof(expected_baud_evt) ||
	    memcmp(response, expected_baud_evt, sizeof(expected_baud_evt))) {
		dev_err(&wmt->serdev->dev,
			"unexpected baud-rate event after switching to %u baud\n",
			wmt->baudrate);
		return -EPROTO;
	}

	return 0;
}

static int mt6628_wmt_patch_download_one(struct mt6628_wmt *wmt,
					 const struct firmware *fw)
{
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len, offset;
	u8 patch_num, patch_seq, address[4];
	int ret;

	if (fw->size <= MT6628_WMT_PATCH_HDR_SIZE) {
		dev_err(&wmt->serdev->dev,
			"patch image is too small: %zu bytes\n", fw->size);
		return -EINVAL;
	}
	if (fw->data[MT6628_WMT_PATCH_FWVER_LSB_OFFSET] !=
	    (u8)wmt->rom_ver) {
		dev_err(&wmt->serdev->dev,
			"patch version mismatch: header bytes[22:23]=%02x %02x, ROM=%#x\n",
			fw->data[22], fw->data[23], wmt->rom_ver);
		return -EINVAL;
	}
	patch_num = (fw->data[MT6628_WMT_PATCH_INFO_OFFSET] >> 4) & 0x0f;
	patch_seq = fw->data[MT6628_WMT_PATCH_INFO_OFFSET] & 0x0f;
	if (!patch_num || !patch_seq || patch_seq > patch_num) {
		dev_err(&wmt->serdev->dev,
			"invalid patch sequence: patch=%u/%u\n",
			patch_seq, patch_num);
		return -EPROTO;
	}
	memcpy(address, fw->data + MT6628_WMT_PATCH_INFO_OFFSET, sizeof(address));
	address[0] = 0;

	{
		u8 cmd[] = {
			0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
			0xd4, 0x01, 0x09, 0xf0, 0x00, 0x00, 0x00, 0x00,
			0xff, 0xff, 0xff, 0xff,
		};
		response_len = sizeof(response);
		ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x08, 1000,
				     response, &response_len);
		if (ret)
			return ret;
		if (response_len != sizeof(mt6628_wmt_patch_addr_evt) ||
		    memcmp(response, mt6628_wmt_patch_addr_evt,
			   sizeof(mt6628_wmt_patch_addr_evt)))
			return -EPROTO;
	}
	{
		u8 cmd[] = {
			0x01, 0x08, 0x10, 0x00, 0x01, 0x01, 0x00, 0x01,
			0x48, 0x03, 0x09, 0xf0, 0xff, 0xff, 0xff, 0xff,
			0xff, 0xff, 0xff, 0xff,
		};
		memcpy(cmd + 12, address, sizeof(address));
		response_len = sizeof(response);
		ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x08, 1000,
				     response, &response_len);
		if (ret)
			return ret;
		if (response_len != sizeof(mt6628_wmt_patch_addr_evt) ||
		    memcmp(response, mt6628_wmt_patch_addr_evt,
			   sizeof(mt6628_wmt_patch_addr_evt)))
			return -EPROTO;
	}

	offset = MT6628_WMT_PATCH_HDR_SIZE;
	while (offset < fw->size) {
		size_t frag = min_t(size_t, MT6628_WMT_PATCH_FRAG_SIZE,
				    fw->size - offset);
		u8 *cmd = kmalloc(frag + 5, GFP_KERNEL);

		if (!cmd)
			return -ENOMEM;
		cmd[0] = 0x01;
		cmd[1] = 0x01;
		put_unaligned_le16(frag + 1, cmd + 2);
		if (offset + frag == fw->size)
			cmd[4] = 0x03;	/* LAST */
		else if (offset == MT6628_WMT_PATCH_HDR_SIZE)
			cmd[4] = 0x01;	/* FIRST */
		else
			cmd[4] = 0x02;	/* MIDDLE */
		memcpy(cmd + 5, fw->data + offset, frag);
		response_len = sizeof(response);
		ret = mt6628_wmt_cmd(wmt, cmd, frag + 5, 0x01, 1000,
				     response, &response_len);
		kfree(cmd);
		if (ret)
			return ret;
		if (response_len != sizeof(mt6628_wmt_patch_evt) ||
		    memcmp(response, mt6628_wmt_patch_evt,
			   sizeof(mt6628_wmt_patch_evt)))
			return -EPROTO;
		offset += frag;
	}

	return 0;
}

static int mt6628_wmt_patch_download(struct mt6628_wmt *wmt)
{
	const char * const *names;
	unsigned int count, i, expected_seq;
	const struct firmware *fw[ARRAY_SIZE(mt6628_e2_patch_names)] = { };
	int ret = 0;

	switch (wmt->hw_ver) {
	case 0x8a00:
		names = mt6628_e1_patch_names;
		count = ARRAY_SIZE(mt6628_e1_patch_names);
		break;
	case 0x8a10:
	case 0x8b10:
	case 0x8b11:
	case 0x8a11:
		names = mt6628_e2_patch_names;
		count = ARRAY_SIZE(mt6628_e2_patch_names);
		break;
	default:
		dev_err(&wmt->serdev->dev,
			"unsupported MT6628 HW version %#x\n", wmt->hw_ver);
		return -ENODEV;
	}

	for (i = 0; i < count; i++) {
		u8 file_patch_num;

		ret = request_firmware(&fw[i], names[i], &wmt->serdev->dev);
		if (ret) {
			dev_err(&wmt->serdev->dev, "failed to load %s: %d\n",
				names[i], ret);
			goto out;
		}

		if (fw[i]->size <= MT6628_WMT_PATCH_HDR_SIZE) {
			dev_err(&wmt->serdev->dev,
				"%s: invalid patch size %zu\n",
				names[i], fw[i]->size);
			ret = -EINVAL;
			goto out;
		}
		if (fw[i]->data[MT6628_WMT_PATCH_FWVER_LSB_OFFSET] !=
		    (u8)wmt->rom_ver) {
			dev_err(&wmt->serdev->dev,
				"%s: patch version mismatch: bytes[22:23]=%02x %02x, ROM=%#x\n",
				names[i], fw[i]->data[22], fw[i]->data[23],
				wmt->rom_ver);
			ret = -EINVAL;
			goto out;
		}

		file_patch_num =
			(fw[i]->data[MT6628_WMT_PATCH_INFO_OFFSET] >> 4) & 0x0f;
		if (file_patch_num != count) {
			dev_err(&wmt->serdev->dev,
				"%s: patch set declares %u patches, expected %u\n",
				names[i], file_patch_num, count);
			ret = -EINVAL;
			goto out;
		}
	}

	for (expected_seq = 1; expected_seq <= count; expected_seq++) {
		unsigned int found = count;

		for (i = 0; i < count; i++) {
			if ((fw[i]->data[MT6628_WMT_PATCH_INFO_OFFSET] & 0x0f) ==
			    expected_seq) {
				found = i;
				break;
			}
		}
		if (found == count) {
			ret = -EPROTO;
			goto out;
		}
		ret = mt6628_wmt_patch_download_one(wmt, fw[found]);
		if (ret)
			goto out;
		/* Aquaris-5 resets WMT after each patch in the multi-patch set. */
		ret = mt6628_wmt_reset(wmt);
		if (ret)
			goto out;
	}

out:
	for (i = 0; i < ARRAY_SIZE(fw); i++)
		if (fw[i])
			release_firmware(fw[i]);
	return ret;
}

static int mt6628_wmt_crystal_trim_get(struct mt6628_wmt *wmt, u8 *timing)
{
	static const u8 cmd[] = { 0x01, 0x12, 0x02, 0x00, 0x00, 0x00 };
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len = sizeof(response);
	int ret;

	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x12, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != 6 || response[4] != 0x00)
		return -EPROTO;
	*timing = response[5] & 0x7f;
	return 0;
}

static int mt6628_wmt_crystal_trim_set(struct mt6628_wmt *wmt, u8 timing)
{
	u8 cmd[] = { 0x01, 0x12, 0x02, 0x00, 0x01, timing };
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len = sizeof(response);
	int ret;

	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x12, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != 6 || response[4] != 0x01 || response[5] != timing)
		return -EPROTO;
	return 0;
}

static int mt6628_wmt_crystal_trim_init(struct mt6628_wmt *wmt)
{
	u8 old, new;
	int offset, ret;

	if (!wmt->crystal_trim_configured || !(wmt->crystal_trim & BIT(7)))
		return 0;
	ret = mt6628_wmt_crystal_trim_get(wmt, &old);
	if (ret)
		return ret;
	offset = wmt->crystal_trim & 0x7f;
	if (offset & BIT(6))
		offset -= 128;
	new = clamp_t(int, old + offset, 0, 0x7f);
	ret = mt6628_wmt_crystal_trim_set(wmt, new);
	if (ret)
		return ret;
	ret = mt6628_wmt_crystal_trim_get(wmt, &old);
	if (ret)
		return ret;
	return old == new ? 0 : -EIO;
}

static int mt6628_wmt_coex_init(struct mt6628_wmt *wmt)
{
	u8 cmd[] = { 0x01, 0x10, 0x02, 0x00, 0x01, 0x00 };
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len = sizeof(response);
	int ret;

	if (!wmt->coex_configured)
		return 0;
	cmd[5] = wmt->coex_ant_mode;
	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x10, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != sizeof(mt6628_wmt_coex_evt) ||
	    memcmp(response, mt6628_wmt_coex_evt, sizeof(mt6628_wmt_coex_evt)))
		return -EPROTO;
	return 0;
}

/*
 * The downstream applies this setting even when BGF uses UART: SDIO
 * function 1 still carries WLAN on the same combo chip.
 */
static int mt6628_wmt_set_sdio_driving(struct mt6628_wmt *wmt)
{
	u8 cmd[] = {
		0x01, 0x08, 0x10, 0x00,
		0x01, 0x01, 0x00, 0x01,
		0x50, 0x00, 0x05, 0x80,
		0x00, 0x00, 0x00, 0x00,
		0x77, 0x77, 0x07, 0x00,
	};

	if (!wmt->sdio_driving_configured)
		return 0;

	cmd[12] = wmt->sdio_driving_cfg & 0x77;
	cmd[13] = (wmt->sdio_driving_cfg >> 8) & 0x77;
	cmd[14] = (wmt->sdio_driving_cfg >> 16) & 0x07;

	return mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x08, 1000,
			      NULL, NULL);
}

static int mt6628_wmt_merge_if_init(struct mt6628_wmt *wmt)
{
	int ret;

	// downstream: MTK_WCN_CMB_MERGE_INTERFACE_SUPPORT

	ret = mt6628_wmt_reg_write(wmt, 0x80050078, 0x11010000, 0x07770000);
	if (ret)
		return ret;
	ret = mt6628_wmt_reg_write(wmt, 0x80050074, 0x00004444, 0x00007777);
	if (ret)
		return ret;
	return mt6628_wmt_reg_write(wmt, 0x800500a0, 0x00000004, 0x00000004);
}

static int mt6628_wmt_co_clock_init(struct mt6628_wmt *wmt)
{
	static const u8 cmd[] = { 0x01, 0x0a, 0x02, 0x00, 0x08, 0x03 };
	static const u8 expected[] = { 0x02, 0x0a, 0x01, 0x00, 0x00 };
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len = sizeof(response);
	int ret;

	if (!wmt->co_clock_enabled)
		return 0;
	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x0a, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	return response_len == sizeof(expected) &&
	       !memcmp(response, expected, sizeof(expected)) ? 0 : -EPROTO;
}

static int mt6628_wmt_set_fm_strap(struct mt6628_wmt *wmt)
{
	u8 cmd[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };

	cmd[5] = wmt->fm_strap_mode;
	return mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x05, 1000, NULL, NULL);
}

static int mt6628_wmt_init(struct mt6628_wmt *wmt)
{
	u8 cmd[] = { 0x01, 0x04, 0x05, 0x00, 0x03, 0xdf, 0x0e, 0x68, 0x01 };
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len;
	int ret;

	ret = mt6628_wmt_set_baud(wmt);
	if (ret)
		return ret;
	ret = mt6628_wmt_patch_download(wmt);
	if (ret)
		return ret;
	ret = mt6628_wmt_reset(wmt);
	if (ret)
		return ret;
	ret = mt6628_wmt_coex_init(wmt);
	if (ret)
		return ret;
	ret = mt6628_wmt_crystal_trim_init(wmt);
	if (ret)
		return ret;
	ret = mt6628_wmt_set_sdio_driving(wmt);
	if (ret)
		return ret;

	/* Configure the chip, then switch both ends to UART-full STP. */
	response_len = sizeof(response);
	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x04, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != sizeof(mt6628_wmt_full_stp_evt) ||
	    memcmp(response, mt6628_wmt_full_stp_evt,
		   sizeof(mt6628_wmt_full_stp_evt)))
		return -EPROTO;

	WRITE_ONCE(wmt->full_mode, true);
	msleep(10);
	response_len = sizeof(response);
	ret = mt6628_wmt_cmd(wmt, mt6628_wmt_query_stp_cmd,
			     sizeof(mt6628_wmt_query_stp_cmd), 0x04, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	if (response_len != sizeof(mt6628_wmt_full_stp_query_evt) ||
	    memcmp(response, mt6628_wmt_full_stp_query_evt,
		   sizeof(mt6628_wmt_full_stp_query_evt)))
		return -EPROTO;

	/* These board-independent WMT controls run after the UART-full handoff. */
	ret = mt6628_wmt_co_clock_init(wmt);
	if (ret)
		return ret;
	ret = mt6628_wmt_merge_if_init(wmt);
	if (ret)
		return ret;
	ret = mt6628_wmt_set_fm_strap(wmt);
	if (ret)
		return ret;

	dev_info(&wmt->serdev->dev, "MT6628 UART STP initialized at %u baud\n",
		 wmt->baudrate);
	return 0;
}

static int mt6628_uart_wmt_func_ctrl(struct mt6628_wmt *wmt,
			 enum mt6628_wmt_func func, bool on)
{
	u8 cmd[] = { 0x01, 0x06, 0x02, 0x00, func, on ? 1 : 0 };

	if (!wmt || (func != MT6628_WMT_FUNC_BT &&
		     func != MT6628_WMT_FUNC_FM && func != MT6628_WMT_FUNC_GPS))
		return -EINVAL;
	return mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x06, 1000, NULL, NULL);
}

static int mt6628_uart_wmt_gps_sync_ctrl(struct mt6628_wmt *wmt, bool on)
{
	if (!wmt)
		return -EINVAL;
	return mt6628_wmt_reg_write(wmt, 0x80050078,
				    on ? (0x1U << 28) : (0x5U << 28),
				    0x7U << 28);
}

static int mt6628_uart_wmt_dsns_ctrl(struct mt6628_wmt *wmt,
			 enum mt6628_wmt_dsns type)
{
	static const u8 expected[] = { 0x02, 0x0e, 0x01, 0x00, 0x00 };
	u8 cmd[] = { 0x01, 0x0e, 0x02, 0x00, MT6628_WMT_FUNC_FM, type };
	u8 response[sizeof(expected)];
	size_t response_len = sizeof(response);
	int ret;

	if (!wmt || type > MT6628_WMT_DSNS_FM_GPS_ENABLE)
		return -EINVAL;
	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x0e, 1000,
			     response, &response_len);
	if (ret)
		return ret;
	return response_len == sizeof(expected) &&
	       !memcmp(response, expected, sizeof(expected)) ? 0 : -EPROTO;
}

static int mt6628_uart_send_op(void *context, enum mt6628_stp_task task,
                               const void *buf, size_t len)
{
	return mt6628_uart_send(context, task, buf, len);
}

static int mt6628_uart_register_rx_op(void *context,
                                      enum mt6628_stp_task task,
                                      mt6628_stp_rx_cb cb, void *priv)
{
	return mt6628_uart_register_rx(context, task, cb, priv);
}

static void mt6628_uart_unregister_rx_op(void *context,
                                          enum mt6628_stp_task task,
                                          mt6628_stp_rx_cb cb, void *priv)
{
	mt6628_uart_unregister_rx(context, task, cb, priv);
}

static int mt6628_uart_wmt_func_ctrl_op(void *context,
                                        enum mt6628_wmt_func func, bool on)
{
	return mt6628_uart_wmt_func_ctrl(context, func, on);
}

static int mt6628_uart_wmt_gps_sync_ctrl_op(void *context, bool on)
{
	return mt6628_uart_wmt_gps_sync_ctrl(context, on);
}

static int mt6628_uart_wmt_dsns_ctrl_op(void *context,
                                       enum mt6628_wmt_dsns type)
{
	return mt6628_uart_wmt_dsns_ctrl(context, type);
}

static const struct mt6628_stp_transport_ops mt6628_uart_transport_ops = {
	.hif = MT6628_STP_HIF_UART,
	.send = mt6628_uart_send_op,
	.register_rx = mt6628_uart_register_rx_op,
	.unregister_rx = mt6628_uart_unregister_rx_op,
	.wmt_func_ctrl = mt6628_uart_wmt_func_ctrl_op,
	.wmt_gps_sync_ctrl = mt6628_uart_wmt_gps_sync_ctrl_op,
	.wmt_dsns_ctrl = mt6628_uart_wmt_dsns_ctrl_op,
};

static const struct mfd_cell mt6628_uart_cells[] = {
	{ .name = "mt6628-bt", .of_compatible = "mediatek,mt6628-bt" },
	{ .name = "mt6628-fm", .of_compatible = "mediatek,mt6628-fm" },
	{ .name = "mt6628-gnss", .of_compatible = "mediatek,mt6628-gnss" },
};

static int mt6628_stp_uart_probe(struct serdev_device *serdev)
{
	struct mt6628_wmt *wmt;
	u32 val;
	int ret;

	if (!mt6628_stp_transport_selected(&serdev->dev, "uart"))
		return -ENODEV;

	wmt = devm_kzalloc(&serdev->dev, sizeof(*wmt), GFP_KERNEL);
	if (!wmt)
		return -ENOMEM;
	wmt->serdev = serdev;
	wmt->baudrate = MT6628_WMT_UART_DEFAULT_BAUD;
	wmt->fm_strap_mode = 2;
	mutex_init(&wmt->tx_lock);
	mutex_init(&wmt->cmd_lock);
	mutex_init(&wmt->endpoint_lock);
	mutex_init(&wmt->wmt_lock);
	spin_lock_init(&wmt->rx_fifo_lock);
	INIT_KFIFO(wmt->rx_fifo);
	init_completion(&wmt->wmt_done);
	init_completion(&wmt->tx_ack_done);
	INIT_WORK(&wmt->rx_work, mt6628_uart_rx_work);
	wmt->rx_expected_seq = 0;
	wmt->rx_ack = 7;
	mt6628_uart_parser_reset(wmt);

	if (!device_property_read_u32(&serdev->dev, "mediatek,baudrate", &val))
		wmt->baudrate = val;
	if (wmt->baudrate < 115200 || wmt->baudrate > 4000000)
		return dev_err_probe(&serdev->dev, -EINVAL,
				     "unsupported UART baud rate %u\n", wmt->baudrate);
	if (!device_property_read_u32(&serdev->dev,
				      "mediatek,coex-ant-mode", &val)) {
		wmt->coex_ant_mode = val;
		wmt->coex_configured = true;
	}
	if (!device_property_read_u32(&serdev->dev,
				      "mediatek,sdio-driving-cfg", &val)) {
		wmt->sdio_driving_cfg = val;
		wmt->sdio_driving_configured = true;
	}
	if (!device_property_read_u32(&serdev->dev,
				      "mediatek,fm-strap-mode", &val))
		wmt->fm_strap_mode = val;
	if (!device_property_read_u32(&serdev->dev,
				      "mediatek,crystal-trim", &val)) {
		wmt->crystal_trim = val;
		wmt->crystal_trim_configured = true;
	}
	wmt->co_clock_enabled = device_property_read_bool(&serdev->dev,
							  "mediatek,co-clock");

	serdev_device_set_drvdata(serdev, wmt);
	serdev_device_set_client_ops(serdev, &mt6628_uart_serdev_ops);
	ret = serdev_device_open(serdev);
	if (ret)
		return dev_err_probe(&serdev->dev, ret, "failed to open UART\n");
	serdev_device_set_flow_control(serdev, false);
	if (serdev_device_set_baudrate(serdev, MT6628_WMT_UART_BOOT_BAUD) !=
	    MT6628_WMT_UART_BOOT_BAUD) {
		ret = -EINVAL;
		goto err_close;
	}

	/*
	 * UART3 may probe before MSDC3 has run the board's existing MT6628
	 * power sequence. Wait once for PMU/RTC/reset, but never restart the
	 * complete initialization blindly: after the full-STP handoff the chip
	 * is no longer speaking mandatory-mode framing.
	 */
	msleep(500);
	ret = mt6628_wmt_read_versions(wmt);
	if (ret) {
		dev_err(&serdev->dev,
			"MT6628 WMT version query failed at %u baud: %d\n",
			MT6628_WMT_UART_BOOT_BAUD, ret);
		/* This read-only probe does not change baud or STP framing. */
		ret = -EPROBE_DEFER;
		goto err_close;
	}

	ret = mt6628_wmt_init(wmt);
	if (ret) {
		/* Do not blindly retry after baud or STP mode may have changed. */
		dev_err(&serdev->dev, "UART WMT initialization failed: %d\n", ret);
		goto err_close;
	}

	ret = mt6628_stp_register_transport(&serdev->dev, wmt,
					    &mt6628_uart_transport_ops);
	if (ret)
		goto err_close;

	ret = mfd_add_devices(&serdev->dev, PLATFORM_DEVID_AUTO,
			      mt6628_uart_cells, ARRAY_SIZE(mt6628_uart_cells),
			      NULL, 0, NULL);
	if (ret) {
		mt6628_stp_unregister_transport(wmt);
		goto err_close;
	}
	return 0;

err_close:
	WRITE_ONCE(wmt->stopping, true);
	serdev_device_close(serdev);
	cancel_work_sync(&wmt->rx_work);
	serdev_device_set_drvdata(serdev, NULL);
	return ret;
}

static void mt6628_stp_uart_remove(struct serdev_device *serdev)
{
	struct mt6628_wmt *wmt = serdev_device_get_drvdata(serdev);

	if (!wmt)
		return;
	mfd_remove_devices(&serdev->dev);
	mt6628_stp_unregister_transport(wmt);
	WRITE_ONCE(wmt->stopping, true);
	mutex_lock(&wmt->wmt_lock);
	if (wmt->wmt_waiting) {
		wmt->wmt_status = -ESHUTDOWN;
		wmt->wmt_waiting = false;
		complete(&wmt->wmt_done);
	}
	mutex_unlock(&wmt->wmt_lock);
	if (wmt->tx_waiting) {
		wmt->tx_status = -ESHUTDOWN;
		wmt->tx_waiting = false;
		complete(&wmt->tx_ack_done);
	}
	serdev_device_close(serdev);
	cancel_work_sync(&wmt->rx_work);
	serdev_device_set_drvdata(serdev, NULL);
}

static const struct of_device_id mt6628_stp_uart_of_match[] = {
	{ .compatible = "mediatek,mt6628-stp-uart" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6628_stp_uart_of_match);

static struct serdev_device_driver mt6628_stp_uart_driver = {
	.probe = mt6628_stp_uart_probe,
	.remove = mt6628_stp_uart_remove,
	.driver = {
		.name = "mt6628-stp-uart",
		.of_match_table = mt6628_stp_uart_of_match,
	},
};
module_serdev_device_driver(mt6628_stp_uart_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 shared WMT/STP UART transport");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("mt6628_patch_e1_hdr.bin");
MODULE_FIRMWARE("mt6628_patch_e2_0_hdr.bin");
MODULE_FIRMWARE("mt6628_patch_e2_1_hdr.bin");
