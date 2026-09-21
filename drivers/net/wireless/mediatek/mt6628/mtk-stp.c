// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6628 shared WMT/STP SDIO transport
 *
 * SDIO function 2 is a shared transport for BT/FM/GPS/WMT.  It must have
 * exactly one SDIO owner; the functional drivers are MFD/platform children.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/bitfield.h>
#include <linux/firmware.h>
#include <linux/jiffies.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/property.h>

#include <linux/mfd/mt6628.h>

#define MT6628_STP_CHLPCR		0x0004
#define MT6628_STP_CHISR		0x0010
#define MT6628_STP_CHIER		0x0014
#define MT6628_STP_CTDR			0x0018
#define MT6628_STP_CRDR			0x001c

#define MT6628_STP_FW_OWN_REQ_CLR	BIT(9)
#define MT6628_STP_FW_OWN_REQ_SET	BIT(8)
#define MT6628_STP_INT_EN_CLR		BIT(1)
#define MT6628_STP_INT_EN_SET		BIT(0)

#define MT6628_STP_RX_LEN		GENMASK(31, 16)
#define MT6628_STP_FIRMWARE_INT		GENMASK(15, 9)
#define MT6628_STP_TX_FIFO_OVERFLOW	BIT(8)
#define MT6628_STP_FW_INT_IND_INDICATOR	BIT(7)
#define MT6628_STP_RX_DONE		BIT(1)
#define MT6628_STP_TX_UNDER_THOLD	BIT(3)
#define MT6628_STP_TX_EMPTY		BIT(2)
#define MT6628_STP_TX_COMPLETE_COUNT	GENMASK(6, 4)

#define MT6628_STP_TX_FIFO_SIZE		2080
#define MT6628_STP_RX_FIFO_SIZE		2304
#define MT6628_STP_SDIO_HDR_SIZE	4
#define MT6628_STP_BLK_SIZE		512
#define MT6628_STP_HEADER_SIZE		4
#define MT6628_STP_TRAILER_SIZE		2

/* stp_core.c rejects payload lengths >= 2000 on MT6628. */
#define MT6628_STP_MAX_PAYLOAD_LEN	1999

#define MT6628_STP_RX_BUF_SIZE		2560
#define MT6628_STP_TX_MAX_PENDING	7
#define MT6628_STP_TX_TIMEOUT_MS		1000
#define MT6628_WMT_RESPONSE_MAX		32

#define MT6628_WMT_GEN_HVR			0x80000000
#define MT6628_WMT_GEN_FVR			0x80000004
#define MT6628_WMT_GEN_VER_MASK		0x0000ffff

#define MT6628_WMT_PATCH_HDR_SIZE		28
#define MT6628_WMT_PATCH_HWVER_OFFSET		22
#define MT6628_WMT_PATCH_INFO_OFFSET		24
#define MT6628_WMT_PATCH_FRAG_SIZE		1000

#define MT6628_WMT_PATCH_ADDR_CMD_REG		0xf00901d4
#define MT6628_WMT_PATCH_PART_ADDR_REG		0xf0090348

static const u8 mt6628_wmt_patch_addr_evt[] = {
	0x02, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01,
};

static const u8 mt6628_wmt_patch_evt[] = {
	0x02, 0x01, 0x01, 0x00, 0x00,
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

struct mt6628_wmt {
	struct sdio_func *func;
	struct work_struct rx_work;

	struct mutex tx_lock;
	struct mutex rx_lock;
	struct mutex wmt_lock;
	struct completion wmt_done;
	spinlock_t tx_state_lock;
	wait_queue_head_t tx_waitq;
	u16 tx_sizes[MT6628_STP_TX_MAX_PENDING];
	u8 tx_head;
	u8 tx_tail;
	u8 tx_pending;
	u16 tx_fifo_free;

	struct mt6628_stp_endpoint endpoint[MT6628_STP_TASK_MAX];
	u8 rx_buf[MT6628_STP_RX_BUF_SIZE];

	bool driver_owned;
	bool irq_claimed;
	bool stopping;
	bool wmt_waiting;
	u8 wmt_wait_opcode;
	int wmt_status;
	u8 wmt_response[MT6628_WMT_RESPONSE_MAX];
	u8 wmt_response_len;

	bool coex_configured;
	u8 coex_ant_mode;
	bool co_clock_enabled;
	bool sdio_driving_configured;
	u32 sdio_driving_cfg;
	u8 fm_strap_mode;
};

static bool mt6628_stp_tx_ready(struct mt6628_wmt *wmt, size_t frame_len)
{
	unsigned long flags;
	bool ready;

	spin_lock_irqsave(&wmt->tx_state_lock, flags);
	ready = !wmt->stopping &&
		wmt->tx_pending < MT6628_STP_TX_MAX_PENDING &&
		wmt->tx_fifo_free >= frame_len;
	spin_unlock_irqrestore(&wmt->tx_state_lock, flags);

	return ready;
}

static int mt6628_stp_tx_reserve(struct mt6628_wmt *wmt,
				 size_t frame_len, u8 *slot)
{
	unsigned long flags;
	int ret = -EAGAIN;

	spin_lock_irqsave(&wmt->tx_state_lock, flags);
	if (!wmt->stopping &&
	    wmt->tx_pending < MT6628_STP_TX_MAX_PENDING &&
	    wmt->tx_fifo_free >= frame_len) {
		*slot = wmt->tx_tail;
		wmt->tx_sizes[wmt->tx_tail] = frame_len;
		wmt->tx_tail =
			(wmt->tx_tail + 1) % MT6628_STP_TX_MAX_PENDING;
		wmt->tx_pending++;
		wmt->tx_fifo_free -= frame_len;
		ret = 0;
	}
	spin_unlock_irqrestore(&wmt->tx_state_lock, flags);

	return ret;
}

static void mt6628_stp_tx_cancel(struct mt6628_wmt *wmt,
				 size_t frame_len, u8 slot)
{
	unsigned long flags;

	spin_lock_irqsave(&wmt->tx_state_lock, flags);
	if (wmt->tx_pending &&
	    wmt->tx_tail == (u8)((slot + 1) % MT6628_STP_TX_MAX_PENDING)) {
		wmt->tx_tail = slot;
		wmt->tx_pending--;
		wmt->tx_fifo_free += frame_len;
	}
	spin_unlock_irqrestore(&wmt->tx_state_lock, flags);
	wake_up_all(&wmt->tx_waitq);
}

static void mt6628_stp_tx_complete(struct mt6628_wmt *wmt, u32 chisr)
{
	unsigned int count = FIELD_GET(MT6628_STP_TX_COMPLETE_COUNT, chisr);
	unsigned long flags;
	bool changed = false;

	if (!count)
		return;

	spin_lock_irqsave(&wmt->tx_state_lock, flags);
	if (count > wmt->tx_pending) {
		dev_warn(&wmt->func->dev,
			 "invalid STP TX completion count %u (pending %u)\n",
			 count, wmt->tx_pending);
		count = wmt->tx_pending;
	}

	while (count--) {
		wmt->tx_fifo_free += wmt->tx_sizes[wmt->tx_head];
		wmt->tx_head =
			(wmt->tx_head + 1) % MT6628_STP_TX_MAX_PENDING;
		wmt->tx_pending--;
		changed = true;
	}
	spin_unlock_irqrestore(&wmt->tx_state_lock, flags);

	if (changed)
		wake_up_all(&wmt->tx_waitq);
}

static int mt6628_stp_write32(struct mt6628_wmt *wmt, unsigned int reg,
			      u32 val)
{
	int ret;

	sdio_claim_host(wmt->func);
	sdio_writel(wmt->func, val, reg, &ret);
	sdio_release_host(wmt->func);

	return ret;
}

/*
 * MT6628 requires CMD52 accesses for CHLPCR ownership/interrupt
 * control.  The downstream driver carries this workaround as
 * COHEC_00006052.
 */
static int mt6628_stp_write8(struct mt6628_wmt *wmt, unsigned int reg,
			     u8 val)
{
	int ret;

	sdio_claim_host(wmt->func);
	sdio_writeb(wmt->func, val, reg, &ret);
	sdio_release_host(wmt->func);

	return ret;
}

static int mt6628_stp_driver_own(struct mt6628_wmt *wmt)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);
	int ret;
	u32 val;

	ret = mt6628_stp_write8(wmt, MT6628_STP_CHLPCR + 1,
				MT6628_STP_FW_OWN_REQ_CLR >> 8);
	if (ret)
		return ret;

	while (time_before(jiffies, timeout)) {
		sdio_claim_host(wmt->func);
		val = sdio_readl(wmt->func, MT6628_STP_CHLPCR, &ret);
		sdio_release_host(wmt->func);
		if (ret)
			return ret;

		if (val & MT6628_STP_FW_OWN_REQ_SET)
			return 0;

		usleep_range(500, 1000);
	}

	return -ETIMEDOUT;
}

static int mt6628_stp_fw_own(struct mt6628_wmt *wmt)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(1000);
	int ret;
	u32 val;

	ret = mt6628_stp_write8(wmt, MT6628_STP_CHLPCR + 1,
				MT6628_STP_FW_OWN_REQ_SET >> 8);
	if (ret)
		return ret;

	while (time_before(jiffies, timeout)) {
		sdio_claim_host(wmt->func);
		val = sdio_readl(wmt->func, MT6628_STP_CHLPCR, &ret);
		sdio_release_host(wmt->func);
		if (ret)
			return ret;

		if (!(val & MT6628_STP_FW_OWN_REQ_SET))
			return 0;

		usleep_range(500, 1000);
	}

	return -ETIMEDOUT;
}

static int mt6628_stp_irq_enable(struct mt6628_wmt *wmt)
{
	return mt6628_stp_write8(wmt, MT6628_STP_CHLPCR,
				 MT6628_STP_INT_EN_SET);
}

static void mt6628_stp_irq_disable_in_irq(struct mt6628_wmt *wmt)
{
	int ret;

	/* SDIO invokes the callback with the host already claimed. */
	sdio_writeb(wmt->func, MT6628_STP_INT_EN_CLR,
		    MT6628_STP_CHLPCR, &ret);
}

static void mt6628_stp_dispatch(struct mt6628_wmt *wmt,
				 enum mt6628_stp_task task,
				 const u8 *buf, size_t len)
{
	mt6628_stp_rx_cb cb;
	void *priv;

	if (task == MT6628_STP_TASK_WMT) {
		u16 payload_len;

		if (len < 4 || buf[0] != 0x02)
			return;

		payload_len = get_unaligned_le16(buf + 2);
		if (!payload_len || payload_len != len - 4)
			return;

		mutex_lock(&wmt->wmt_lock);
		if (wmt->wmt_waiting &&
		    buf[0] == 0x02 &&
		    buf[1] == wmt->wmt_wait_opcode) {
			if (len < 5) {
				wmt->wmt_status = -EPROTO;
				wmt->wmt_response_len = 0;
			} else {
				wmt->wmt_status = buf[4] ? -EIO : 0;
				wmt->wmt_response_len =
					min_t(size_t, len,
					      MT6628_WMT_RESPONSE_MAX);
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

	mutex_lock(&wmt->rx_lock);
	cb = wmt->endpoint[task].cb;
	priv = wmt->endpoint[task].priv;
	if (cb)
		cb(priv, buf, len);
	mutex_unlock(&wmt->rx_lock);
}

static void mt6628_stp_parse_rx(struct mt6628_wmt *wmt, u16 bus_len)
{
	size_t pos = MT6628_STP_SDIO_HDR_SIZE;
	size_t end = bus_len;

	while (pos + MT6628_STP_HEADER_SIZE + MT6628_STP_TRAILER_SIZE <= end) {
		u16 len;
		u8 task;
		size_t frame_len;
		size_t padded_len;

		if (!(wmt->rx_buf[pos] & BIT(7)))
			break;

		task = (wmt->rx_buf[pos + 1] >> 4) & 0x07;
		len = ((wmt->rx_buf[pos + 1] & 0x0f) << 8) |
			wmt->rx_buf[pos + 2];
		if (len >= 2000)
			break;

		frame_len = MT6628_STP_HEADER_SIZE + len +
			MT6628_STP_TRAILER_SIZE;
		if (frame_len > end - pos)
			break;

		mt6628_stp_dispatch(wmt, task, wmt->rx_buf + pos +
				    MT6628_STP_HEADER_SIZE, len);

		padded_len = ALIGN(frame_len, 4);
		if (padded_len > end - pos)
			break;
		pos += padded_len;
	}
}

static void mt6628_stp_rx_work(struct work_struct *work)
{
	struct mt6628_wmt *wmt = container_of(work, struct mt6628_wmt,
					     rx_work);
	u32 chisr;
	u16 rx_len;
	size_t bus_len;
	bool have_rx = false;
	int ret;

	sdio_claim_host(wmt->func);
	chisr = sdio_readl(wmt->func, MT6628_STP_CHISR, &ret);
	if (!ret && (chisr & (MT6628_STP_TX_EMPTY |
			     MT6628_STP_TX_UNDER_THOLD)))
		mt6628_stp_tx_complete(wmt, chisr);
	if (!ret && (chisr & MT6628_STP_RX_DONE)) {
		rx_len = FIELD_GET(MT6628_STP_RX_LEN, chisr);
		if (rx_len >= MT6628_STP_SDIO_HDR_SIZE &&
			rx_len <= MT6628_STP_RX_FIFO_SIZE) {
			bus_len = ALIGN(rx_len, 4);
			if (bus_len > MT6628_STP_BLK_SIZE)
				bus_len = ALIGN(bus_len, MT6628_STP_BLK_SIZE);
			if (bus_len <= sizeof(wmt->rx_buf)) {
				ret = sdio_readsb(wmt->func, wmt->rx_buf,
						  MT6628_STP_CRDR, bus_len);
				if (!ret &&
				    get_unaligned_le16(wmt->rx_buf) == rx_len &&
				    !wmt->rx_buf[2] && !wmt->rx_buf[3])
					have_rx = true;
			}
		}
	}
	sdio_release_host(wmt->func);

	if (have_rx)
		mt6628_stp_parse_rx(wmt, rx_len);

	if (!READ_ONCE(wmt->stopping) && mt6628_stp_irq_enable(wmt))
		dev_warn(&wmt->func->dev, "failed to re-enable STP IRQ\n");
}

static void mt6628_stp_irq(struct sdio_func *func)
{
	struct mt6628_wmt *wmt = sdio_get_drvdata(func);

	if (!wmt)
		return;
	if (READ_ONCE(wmt->stopping))
		return;

	mt6628_stp_irq_disable_in_irq(wmt);
	schedule_work(&wmt->rx_work);
}

static int __mt6628_stp_send(struct mt6628_wmt *wmt,
				    enum mt6628_stp_task task,
				    const void *buf, size_t len)
{
	u8 *frame;
	size_t stp_len;
	size_t bus_len;
	size_t fifo_len;
	size_t frame_len;
	int ret;

	if (task >= MT6628_STP_TASK_MAX || len > MT6628_STP_MAX_PAYLOAD_LEN)
		return -EMSGSIZE;

	stp_len = MT6628_STP_HEADER_SIZE + len + MT6628_STP_TRAILER_SIZE;
	bus_len = MT6628_STP_SDIO_HDR_SIZE + stp_len;
	fifo_len = ALIGN(bus_len, 4);
	frame_len = fifo_len;
	if (frame_len > MT6628_STP_BLK_SIZE)
		frame_len = ALIGN(frame_len, MT6628_STP_BLK_SIZE);
	if (fifo_len > MT6628_STP_TX_FIFO_SIZE)
		return -EMSGSIZE;

	frame = kzalloc(frame_len, GFP_KERNEL);
	if (!frame)
		return -ENOMEM;

	put_unaligned_le16(bus_len, frame);
	frame[2] = 0;
	frame[3] = 0;
	frame[4] = 0x80;
	frame[5] = (task << 4) | ((len >> 8) & 0x0f);
	frame[6] = len & 0xff;
	frame[7] = 0x00;
	memcpy(frame + MT6628_STP_SDIO_HDR_SIZE + MT6628_STP_HEADER_SIZE,
	       buf, len);

	ret = wait_event_interruptible_timeout(
		wmt->tx_waitq,
		READ_ONCE(wmt->stopping) || mt6628_stp_tx_ready(wmt, fifo_len),
		msecs_to_jiffies(MT6628_STP_TX_TIMEOUT_MS));
	if (ret < 0)
		goto out_free;
	if (READ_ONCE(wmt->stopping)) {
		ret = -ESHUTDOWN;
		goto out_free;
	}
	if (!ret) {
		ret = -ETIMEDOUT;
		goto out_free;
	}

	{
		u8 tx_slot;

		ret = mt6628_stp_tx_reserve(wmt, fifo_len, &tx_slot);
		if (ret)
			goto out_free;

		sdio_claim_host(wmt->func);
		ret = sdio_writesb(wmt->func, MT6628_STP_CTDR, frame,
					frame_len);
		sdio_release_host(wmt->func);

		if (ret)
			mt6628_stp_tx_cancel(wmt, fifo_len, tx_slot);
	}

out_free:
	kfree(frame);
	return ret;
}

int mt6628_stp_send(struct mt6628_wmt *wmt, enum mt6628_stp_task task,
			const void *buf, size_t len)
{
	int ret;

	if (!wmt || (!buf && len))
		return -EINVAL;

	mutex_lock(&wmt->tx_lock);
	ret = __mt6628_stp_send(wmt, task, buf, len);
	mutex_unlock(&wmt->tx_lock);

	return ret ? ret : len;
}
EXPORT_SYMBOL_GPL(mt6628_stp_send);

int mt6628_stp_register_rx(struct mt6628_wmt *wmt,
				 enum mt6628_stp_task task,
				 mt6628_stp_rx_cb cb, void *priv)
{
	if (!wmt || task >= MT6628_STP_TASK_MAX || !cb)
		return -EINVAL;

	mutex_lock(&wmt->rx_lock);
	if (wmt->endpoint[task].cb) {
		mutex_unlock(&wmt->rx_lock);
		return -EBUSY;
	}
	wmt->endpoint[task].cb = cb;
	wmt->endpoint[task].priv = priv;
	mutex_unlock(&wmt->rx_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6628_stp_register_rx);

void mt6628_stp_unregister_rx(struct mt6628_wmt *wmt,
				      enum mt6628_stp_task task,
				      mt6628_stp_rx_cb cb, void *priv)
{
	if (!wmt || task >= MT6628_STP_TASK_MAX)
		return;

	mutex_lock(&wmt->rx_lock);
	if (wmt->endpoint[task].cb == cb &&
	    wmt->endpoint[task].priv == priv) {
		wmt->endpoint[task].cb = NULL;
		wmt->endpoint[task].priv = NULL;
	}
	mutex_unlock(&wmt->rx_lock);
}
EXPORT_SYMBOL_GPL(mt6628_stp_unregister_rx);

static int mt6628_wmt_cmd(struct mt6628_wmt *wmt,
			  const u8 *cmd, size_t len, u8 opcode,
			  unsigned int timeout_ms,
			  u8 *response, size_t *response_len)
{
	unsigned long timeout;
	int ret;

	mutex_lock(&wmt->tx_lock);
	reinit_completion(&wmt->wmt_done);

	mutex_lock(&wmt->wmt_lock);
	wmt->wmt_waiting = true;
	wmt->wmt_wait_opcode = opcode;
	wmt->wmt_status = -ETIMEDOUT;
	wmt->wmt_response_len = 0;
	mutex_unlock(&wmt->wmt_lock);

	ret = __mt6628_stp_send(wmt, MT6628_STP_TASK_WMT, cmd, len);
	if (ret) {
		mutex_lock(&wmt->wmt_lock);
		wmt->wmt_waiting = false;
		mutex_unlock(&wmt->wmt_lock);
		mutex_unlock(&wmt->tx_lock);
		return ret;
	}

	timeout = wait_for_completion_timeout(&wmt->wmt_done,
					     msecs_to_jiffies(timeout_ms));

	mutex_lock(&wmt->wmt_lock);
	if (!timeout && wmt->wmt_waiting) {
		wmt->wmt_waiting = false;
		ret = -ETIMEDOUT;
	} else {
		ret = wmt->wmt_status;
	}

	if (!ret && response && response_len) {
		size_t copy_len = min_t(size_t, wmt->wmt_response_len,
					*response_len);

		memcpy(response, wmt->wmt_response, copy_len);
		*response_len = copy_len;
	}

	wmt->wmt_wait_opcode = 0xff;
	mutex_unlock(&wmt->wmt_lock);

	mutex_unlock(&wmt->tx_lock);
	return ret;
}

static int mt6628_wmt_reg_write(struct mt6628_wmt *wmt,
				u32 addr, u32 value, u32 mask)
{
	u8 cmd[20] = {
		0x01, 0x08, 0x10, 0x00,
		0x01, 0x01, 0x00, 0x01,
	};

	put_unaligned_le32(addr, cmd + 8);
	put_unaligned_le32(value, cmd + 12);
	put_unaligned_le32(mask, cmd + 16);

	return mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x08, 1000,
			      NULL, NULL);
}

static int mt6628_wmt_reg_read(struct mt6628_wmt *wmt,
			       u32 addr, u32 mask, u32 *value)
{
	u8 cmd[20] = {
		0x01, 0x08, 0x10, 0x00,
		0x00, 0x01, 0x00, 0x01,
	};
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len = sizeof(response);
	u16 payload_len;
	int ret;

	if (!value)
		return -EINVAL;

	put_unaligned_le32(addr, cmd + 8);
	put_unaligned_le32(0, cmd + 12);
	put_unaligned_le32(mask, cmd + 16);

	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x08, 1000,
			     response, &response_len);
	if (ret)
		return ret;

	if (response_len < 16 ||
	    response[0] != 0x02 ||
	    response[1] != 0x08) {
		return -EPROTO;
	}

	payload_len = get_unaligned_le16(response + 2);
	if (payload_len < 12 ||
	    payload_len + 4 > response_len ||
	    response[4] ||
	    response[5] != 0 ||
	    response[6] != 0 ||
	    response[7] != 1 ||
	    get_unaligned_le32(response + 8) != addr) {
		return -EPROTO;
	}

	*value = get_unaligned_le32(response + 12) & mask;
	return 0;
}

static int mt6628_wmt_read_versions(struct mt6628_wmt *wmt,
				    u16 *hw_ver, u16 *rom_ver)
{
	u32 value;
	int ret;

	ret = mt6628_wmt_reg_read(wmt, MT6628_WMT_GEN_HVR,
				  MT6628_WMT_GEN_VER_MASK, &value);
	if (ret)
		return ret;
	*hw_ver = value;

	ret = mt6628_wmt_reg_read(wmt, MT6628_WMT_GEN_FVR,
				  MT6628_WMT_GEN_VER_MASK, &value);
	if (ret)
		return ret;
	*rom_ver = value;

	return 0;
}

static int mt6628_wmt_reset(struct mt6628_wmt *wmt);
static int mt6628_wmt_merge_if_init(struct mt6628_wmt *wmt);
static int mt6628_wmt_coex_init(struct mt6628_wmt *wmt);

static int mt6628_wmt_patch_download_one(struct mt6628_wmt *wmt,
					 const struct firmware *fw,
					 u16 rom_ver)
{
	u8 address[4];
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len;
	size_t offset;
	u8 patch_num;
	u8 patch_seq;
	u16 frag_size;
	u16 cmd_len;
	int ret;

	if (fw->size < MT6628_WMT_PATCH_HDR_SIZE)
		return -EINVAL;

	/*
	 * The launcher compares the low byte of the patch HW version
	 * (header byte 22) with the low byte of FVR.
	 */
	if (fw->data[MT6628_WMT_PATCH_HWVER_OFFSET] !=
	    (rom_ver & 0xff))
		return -EINVAL;

	/*
	 * combo_tool reads the four bytes immediately following the
	 * HW-version field as patchInfo:
	 *
	 *   [7:4] patch count
	 *   [3:0] download sequence
	 *   [31:8] patch address
	 *
	 * The first byte is explicitly cleared before it is used as
	 * the partial-patch address.
	 */
	patch_num = (fw->data[MT6628_WMT_PATCH_INFO_OFFSET] >> 4) & 0x0f;
	patch_seq = fw->data[MT6628_WMT_PATCH_INFO_OFFSET] & 0x0f;
	if (!patch_num || !patch_seq || patch_seq > patch_num)
		return -EPROTO;

	memcpy(address, fw->data + MT6628_WMT_PATCH_INFO_OFFSET,
	       sizeof(address));
	address[0] = 0;

	dev_info(&wmt->func->dev,
		 "MT6628 patch %u/%u, address %02x%02x%02x%02x, size %zu\n",
		 patch_seq, patch_num,
		 address[0], address[1], address[2], address[3],
		 fw->size - MT6628_WMT_PATCH_HDR_SIZE);

	/*
	 * WMT_PATCH_ADDRESS_CMD:
	 *   write 0xffffffff to the patch-address register.
	 */
	{
		u8 cmd[] = {
			0x01, 0x08, 0x10, 0x00,
			0x01, 0x01, 0x00, 0x01,
			0xd4, 0x01, 0x09, 0xf0,
			0x00, 0x00, 0x00, 0x00,
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

	/*
	 * WMT_PATCH_P_ADDRESS_CMD carries the address belonging to this
	 * particular patch file.
	 */
	{
		u8 cmd[] = {
			0x01, 0x08, 0x10, 0x00,
			0x01, 0x01, 0x00, 0x01,
			0x48, 0x03, 0x09, 0xf0,
			0x00, 0x00, 0x00, 0x00,
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
		u8 *cmd;

		frag_size = min_t(size_t, MT6628_WMT_PATCH_FRAG_SIZE,
				  fw->size - offset);
		cmd_len = frag_size + 1;

		cmd = kmalloc(frag_size + 5, GFP_KERNEL);
		if (!cmd)
			return -ENOMEM;

		cmd[0] = 0x01;
		cmd[1] = 0x01;
		put_unaligned_le16(cmd_len, cmd + 2);

		if (offset == MT6628_WMT_PATCH_HDR_SIZE)
			cmd[4] = 0x01; /* first */
		else if (offset + frag_size == fw->size)
			cmd[4] = 0x03; /* last */
		else
			cmd[4] = 0x02; /* middle */

		memcpy(cmd + 5, fw->data + offset, frag_size);

		response_len = sizeof(response);
		ret = mt6628_wmt_cmd(wmt, cmd, frag_size + 5,
				     0x01, 1000, response, &response_len);
		kfree(cmd);

		if (ret)
			return ret;

		if (response_len != sizeof(mt6628_wmt_patch_evt) ||
		    memcmp(response, mt6628_wmt_patch_evt,
			   sizeof(mt6628_wmt_patch_evt)))
			return -EPROTO;

		offset += frag_size;
	}

	return 0;
}

static int mt6628_wmt_patch_download(struct mt6628_wmt *wmt,
				     u16 hw_ver, u16 rom_ver)
{
	const char * const *names;
	unsigned int name_count;
	const struct firmware *fw[ARRAY_SIZE(mt6628_e2_patch_names)] = { };
	unsigned int patch_count;
	unsigned int i;
	unsigned int expected_seq = 1;
	int ret;

	/*
	 * The downstream table requires a patch for every supported MT6628
	 * ECO. E1 has its own patch; E2 and later ECOs use the two-part E2
	 * patch set.
	 */
	switch (hw_ver) {
	case 0x8a00:
		names = mt6628_e1_patch_names;
		name_count = ARRAY_SIZE(mt6628_e1_patch_names);
		break;
	case 0x8a10:
	case 0x8b10:
	case 0x8b11:
	case 0x8a11:
		names = mt6628_e2_patch_names;
		name_count = ARRAY_SIZE(mt6628_e2_patch_names);
		break;
	default:
		dev_err(&wmt->func->dev,
			"unsupported MT6628 HW version %#x\n", hw_ver);
		return -ENODEV;
	}

	/*
	 * The Android combo launcher discovers all matching patch files.
	 * Linux has no equivalent userspace launcher, so perform the same
	 * discovery from the known MT6628 firmware names.
	 */
	for (i = 0; i < name_count; i++) {
		ret = request_firmware(&fw[i], names[i], &wmt->func->dev);
		if (ret) {
			dev_err(&wmt->func->dev,
				"failed to load %s: %d\n", names[i], ret);
			goto out_release;
		}

		if (fw[i]->size < MT6628_WMT_PATCH_HDR_SIZE ||
		    fw[i]->data[MT6628_WMT_PATCH_HWVER_OFFSET] !=
			    (rom_ver & 0xff)) {
			dev_err(&wmt->func->dev,
				"invalid %s for ROM %#x\n",
				names[i], rom_ver);
			ret = -EINVAL;
			goto out_release;
		}

		patch_count =
			(fw[i]->data[MT6628_WMT_PATCH_INFO_OFFSET] >> 4) & 0x0f;
		if (!patch_count || patch_count != name_count) {
			dev_err(&wmt->func->dev,
				"invalid patch count %u in %s\n",
				patch_count, names[i]);
			ret = -EPROTO;
			goto out_release;
		}
	}

	/*
	 * The downstream launcher provides patches in arbitrary directory
	 * order, but the kernel downloader consumes them by download
	 * sequence. Validate and order the two known files explicitly.
	 */
	for (expected_seq = 1; expected_seq <= name_count; expected_seq++) {
		unsigned int found = name_count;

		for (i = 0; i < name_count; i++) {
			u8 seq;

			seq = fw[i]->data[MT6628_WMT_PATCH_INFO_OFFSET] & 0x0f;
			if (seq == expected_seq) {
				found = i;
				break;
			}
		}

		if (found == name_count) {
			dev_err(&wmt->func->dev,
				"missing MT6628 patch sequence %u\n",
				expected_seq);
			ret = -EPROTO;
			goto out_release;
		}

		ret = mt6628_wmt_patch_download_one(wmt, fw[found], rom_ver);
		if (ret) {
			dev_err(&wmt->func->dev,
				"MT6628 patch sequence %u failed: %d\n",
				expected_seq, ret);
			goto out_release;
		}

		/*
		 * bq/aquaris-5 resets WMT after every multi-patch fragment
		 * set. This is intentional and must not be collapsed into a
		 * single reset after the whole patch set.
		 */
		ret = mt6628_wmt_reset(wmt);
		if (ret)
			goto out_release;
	}

	ret = 0;

out_release:
	for (i = 0; i < name_count; i++)
		release_firmware(fw[i]);

	return ret;
}

static int mt6628_wmt_reset(struct mt6628_wmt *wmt)
{
	static const u8 cmd[] = {
		0x01, 0x07, 0x01, 0x00,
		0x04,
	};

	return mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x07, 1000,
			      NULL, NULL);
}

static int mt6628_wmt_set_fm_strap(struct mt6628_wmt *wmt, u8 mode)
{
	u8 cmd[] = {
		0x01, 0x05, 0x02, 0x00,
		0x02, mode,
	};

	return mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x05, 1000,
			      NULL, NULL);
}

static int mt6628_wmt_init(struct mt6628_wmt *wmt)
{
	u16 hw_ver;
	u16 rom_ver;
	int ret;

	ret = mt6628_wmt_read_versions(wmt, &hw_ver, &rom_ver);
	if (ret)
		return ret;

	dev_info(&wmt->func->dev,
		 "MT6628 WMT hardware %#x ROM %#x\n",
		 hw_ver, rom_ver);

	/*
	 * MT6628 downstream performs combo patch download before the
	 * normal WMT initialization sequence. E1 uses one patch while E2
	 * and later supported ECOs use the E2 multi-patch set.
	 */
	ret = mt6628_wmt_patch_download(wmt, hw_ver, rom_ver);
	if (ret)
		return ret;

	/*
	 * bq/aquaris-5 performs another reset after the multi-patch loop.
	 * mt6628_wmt_patch_download() already performs the per-patch reset.
	 */
	ret = mt6628_wmt_reset(wmt);
	if (ret)
		return ret;

	ret = mt6628_wmt_coex_init(wmt);
	if (ret)
		return ret;

	ret = mt6628_wmt_set_sdio_driving(wmt);
	if (ret)
		return ret;

	ret = mt6628_wmt_co_clock_init(wmt);
	if (ret)
		return ret;

	ret = mt6628_wmt_merge_if_init(wmt);
	if (ret)
		return ret;

	ret = mt6628_wmt_set_fm_strap(wmt, wmt->fm_strap_mode);
	if (ret)
		return ret;

	return 0;
}

/*
 * MT6628 merge-interface setup used by the MT6589 downstream BSP.
 *
 * These are the exact three entries from wmt_ic_6628.c's
 * merge_pcm_table:
 *   I2S_Slave
 *   DAI_PAD
 *   DAI_EVT
 */
static int mt6628_wmt_merge_if_init(struct mt6628_wmt *wmt)
{
	int ret;

	ret = mt6628_wmt_reg_write(wmt, 0x80050078,
				   0x11010000, 0x07770000);
	if (ret)
		return ret;

	ret = mt6628_wmt_reg_write(wmt, 0x80050074,
				   0x00004444, 0x00007777);
	if (ret)
		return ret;

	return mt6628_wmt_reg_write(wmt, 0x800500a0,
				    0x00000004, 0x00000004);
}

static int mt6628_wmt_coex_init(struct mt6628_wmt *wmt)
{
	u8 cmd[] = {
		0x01, 0x10, 0x02, 0x00,
		0x01, 0x00,
	};
	static const u8 expected[] = {
		0x02, 0x10, 0x01, 0x00, 0x00,
	};
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

	if (response_len != sizeof(expected) ||
	    memcmp(response, expected, sizeof(expected)))
		return -EPROTO;

	return 0;
}

static int mt6628_wmt_co_clock_init(struct mt6628_wmt *wmt)
{
	static const u8 cmd[] = {
		0x01, 0x0a, 0x02, 0x00,
		0x08, 0x03,
	};
	static const u8 expected[] = {
		0x02, 0x0a, 0x01, 0x00, 0x00,
	};
	u8 response[MT6628_WMT_RESPONSE_MAX];
	size_t response_len = sizeof(response);
	int ret;

	if (!wmt->co_clock_enabled)
		return 0;

	ret = mt6628_wmt_cmd(wmt, cmd, sizeof(cmd), 0x0a, 1000,
			     response, &response_len);
	if (ret)
		return ret;

	if (response_len != sizeof(expected) ||
	    memcmp(response, expected, sizeof(expected)))
		return -EPROTO;

	return 0;
}

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

int mt6628_wmt_func_ctrl(struct mt6628_wmt *wmt,
			 enum mt6628_wmt_func func, bool on)
{
	u8 cmd[] = { 0x01, 0x06, 0x02, 0x00, func, on ? 1 : 0 };

	if (!wmt || (func != MT6628_WMT_FUNC_BT &&
			     func != MT6628_WMT_FUNC_FM &&
			     func != MT6628_WMT_FUNC_GPS))
		return -EINVAL;

	return mt6628_wmt_cmd(wmt, cmd, ARRAY_SIZE(cmd), 0x06, 1000,
			      NULL, NULL);
}
EXPORT_SYMBOL_GPL(mt6628_wmt_func_ctrl);

int mt6628_wmt_gps_sync_ctrl(struct mt6628_wmt *wmt, bool on)
{
	u32 value;

	if (!wmt)
		return -EINVAL;

	value = on ? (0x1U << 28) : (0x5U << 28);

	return mt6628_wmt_reg_write(wmt, 0x80050078,
				    value, 0x7U << 28);
}
EXPORT_SYMBOL_GPL(mt6628_wmt_gps_sync_ctrl);

static const struct mfd_cell mt6628_stp_cells[] = {
	{ .name = "mt6628-bt" },
	{ .name = "mt6628-fm" },
	{ .name = "mt6628-gnss" },
};

static int mt6628_stp_probe(struct sdio_func *func,
				const struct sdio_device_id *id)
{
	struct mt6628_wmt *wmt;
	int ret;

	if (func->num != 2)
		return -ENODEV;

	wmt = devm_kzalloc(&func->dev, sizeof(*wmt), GFP_KERNEL);
	if (!wmt)
		return -ENOMEM;

	wmt->func = func;
	mutex_init(&wmt->tx_lock);
	mutex_init(&wmt->rx_lock);
	mutex_init(&wmt->wmt_lock);
	spin_lock_init(&wmt->tx_state_lock);
	init_waitqueue_head(&wmt->tx_waitq);
	init_completion(&wmt->wmt_done);
	wmt->wmt_wait_opcode = 0xff;
	wmt->tx_fifo_free = MT6628_STP_TX_FIFO_SIZE;
	INIT_WORK(&wmt->rx_work, mt6628_stp_rx_work);
	sdio_set_drvdata(func, wmt);

	if (!device_property_read_u8(&func->dev,
				     "mediatek,coex-ant-mode",
				     &wmt->coex_ant_mode))
		wmt->coex_configured = true;

	wmt->co_clock_enabled =
		device_property_read_bool(&func->dev,
					  "mediatek,co-clock");

	if (!device_property_read_u32(&func->dev,
				      "mediatek,sdio-driving-cfg",
				      &wmt->sdio_driving_cfg))
		wmt->sdio_driving_configured = true;

	wmt->fm_strap_mode = 2;
	device_property_read_u8(&func->dev,
				"mediatek,fm-strap-mode",
				&wmt->fm_strap_mode);

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	if (!ret)
		ret = sdio_set_block_size(func, MT6628_STP_BLK_SIZE);
	sdio_release_host(func);
	if (ret)
		goto err_drvdata;

	ret = mt6628_stp_driver_own(wmt);
	if (ret)
		goto err_disable;
	wmt->driver_owned = true;

	/* RX plus TX-completion indications are consumed by this transport. */
	ret = mt6628_stp_write32(wmt, MT6628_STP_CHIER,
				 MT6628_STP_FIRMWARE_INT |
				 MT6628_STP_TX_FIFO_OVERFLOW |
				 MT6628_STP_FW_INT_IND_INDICATOR |
				 MT6628_STP_RX_DONE |
				 MT6628_STP_TX_UNDER_THOLD |
				 MT6628_STP_TX_EMPTY |
				 MT6628_STP_TX_COMPLETE_COUNT);
	if (ret)
		goto err_fw_own;

	sdio_claim_host(func);
	ret = sdio_claim_irq(func, mt6628_stp_irq);
	sdio_release_host(func);
	if (ret)
		goto err_fw_own;
	wmt->irq_claimed = true;

	ret = mt6628_stp_irq_enable(wmt);
	if (ret)
		goto err_irq;

	ret = mt6628_wmt_init(wmt);
	if (ret)
		goto err_irq_disable;

	ret = mfd_add_devices(&func->dev, PLATFORM_DEVID_AUTO,
			      mt6628_stp_cells,
			      ARRAY_SIZE(mt6628_stp_cells), NULL, 0, NULL);
	if (ret)
		goto err_irq_disable;

	dev_info(&func->dev, "MT6628 shared STP transport ready\n");
	return 0;

err_irq_disable:
	mt6628_stp_write8(wmt, MT6628_STP_CHLPCR,
			  MT6628_STP_INT_EN_CLR);
err_irq:
	sdio_claim_host(func);
	sdio_release_irq(func);
	sdio_release_host(func);
	wmt->irq_claimed = false;
err_fw_own:
	if (wmt->driver_owned && !mt6628_stp_fw_own(wmt))
		wmt->driver_owned = false;
err_disable:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
err_drvdata:
	sdio_set_drvdata(func, NULL);
	return ret;
}

static void mt6628_stp_remove(struct sdio_func *func)
{
	struct mt6628_wmt *wmt = sdio_get_drvdata(func);

	if (!wmt)
		return;

	/* Children may need FUNC_OFF, so keep transport/IRQ alive first. */
	mfd_remove_devices(&func->dev);
	WRITE_ONCE(wmt->stopping, true);
	wake_up_all(&wmt->tx_waitq);

	mt6628_stp_write8(wmt, MT6628_STP_CHLPCR,
			  MT6628_STP_INT_EN_CLR);

	if (wmt->irq_claimed) {
		sdio_claim_host(func);
		sdio_release_irq(func);
		sdio_release_host(func);
		wmt->irq_claimed = false;
	}

	cancel_work_sync(&wmt->rx_work);

	if (wmt->driver_owned) {
		if (mt6628_stp_fw_own(wmt))
			dev_warn(&func->dev,
				 "failed to return STP firmware ownership\n");
		else
			wmt->driver_owned = false;
	}

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	sdio_set_drvdata(func, NULL);
}

static const struct sdio_device_id mt6628_stp_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_MEDIATEK,
		      SDIO_DEVICE_ID_MEDIATEK_MT6628) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, mt6628_stp_ids);

static struct sdio_driver mt6628_stp_driver = {
	.name = "mt6628-stp",
	.probe = mt6628_stp_probe,
	.remove = mt6628_stp_remove,
	.id_table = mt6628_stp_ids,
};
module_sdio_driver(mt6628_stp_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 shared WMT/STP SDIO transport");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("mt6628_patch_e1_hdr.bin");
MODULE_FIRMWARE("mt6628_patch_e2_0_hdr.bin");
MODULE_FIRMWARE("mt6628_patch_e2_1_hdr.bin");
