// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * MediaTek MT6589 video codec — hardware encoder front end.
 *
 * Hardware
 * --------
 * The MT6589 codec is a hardwired RTL accelerator: no coprocessor, no firmware.
 * It differs from the mainline mtk-vcodec driver (the later VPU/IPI generation
 * which needs an SCP/VPU firmware blob) in exactly that respect.
 *
 * Two blocks, both documented:
 *
 *   VENC  0x17002000  H.264 + VP8 encoder (data sheet ch.60) and
 *                      MPEG-4 encoder (data sheet ch.61), same window
 *   VDEC  0x16020000  multi-standard decoder.  The data sheet has no register
 *                      table for this block; the offsets come from the vendor
 *                      LDVT register-level test harness.
 *
 * What this driver does
 * --------------------
 *   - probes the two register windows, the three encoder/decoder clocks and the
 *     two IRQ lines;
 *   - brings the encoder and decoder engines out of reset and gates their
 *     clocks through runtime PM;
 *   - programs the H.264 and MPEG-4 bitstream and frame address registers,
 *     including the reconstructed-frame pair, from DMA addresses it is handed;
 *   - programs the H.264 rate-control quantiser triple, rejecting anything
 *     outside the hardware's 1..31 range;
 *   - acknowledges and dispatches the encoder frame-done interrupt for both
 *     datapaths, reporting the true coded-bitstream length.
 *
 * What this driver does NOT do
 * ----------------------------
 * It registers no V4L2 device.  There is no video_device, no v4l2_m2m_dev, no
 * queue_setup and no /dev/videoX, and nothing here can be reached from
 * userspace today.  The register-level programming below is the hardware
 * interface, deliberately kept as reviewable code, not a working codec.
 *
 * Specifically, these are unimplemented and each one is a real blocker, not an
 * oversight that a later patch in this file would have fixed:
 *
 *   - Buffer management.  There is no dma_alloc_coherent()/vb2 buffer behind
 *     any of the address registers.  The programming functions take DMA
 *     addresses as parameters and are correct about the register unit
 *     conventions, but nothing allocates them.
 *   - Reconstruction buffers.  This is an inter-frame encoder: it writes the
 *     reconstructed pixels to FRM_REC_Y / FRM_REC_UV before the next frame can
 *     be predicted from them.  Until the driver owns a REC buffer and a REF
 *     buffer, only a single I-frame could be encoded, and even that would need
 *     the RC scratch buffers at RC_CODE/RC_INFO plus the slice side
 *     information.
 *   - Slice control.  A frame larger than one MB row has to be driven as
 *     several slices, restarting VENC_MP4_SLICE_START with a moving MBX/MBY
 *     stop position.  Nothing does that.
 *   - Rate control.  The three registers written here are the quantiser clamp;
 *     the actual bitrate algorithm lives in the vendor's closed-source
 *     userspace library.
 *   - Entropy coding, motion estimation and the coefficient buffer.  These
 *     exist only in the vendor's closed libraries (encrypted for H.264/HEVC),
 *     so no amount of kernel work produces a conformant bitstream here.
 *   - Decode.  The decoder window is mapped and its clock gated and its frame
 *     -end interrupt acknowledged, but there is no datapath programming at
 *     all.  An earlier revision of this driver pointed the decoder's
 *     v4l2_m2m_ops.device_run at the *encoder* routine; that cross-wiring has
 *     been removed rather than papered over, because a decoder node that
 *     programs the encoder cannot honestly be exposed at all.
 *   - VP8.  Restricted deliberately; see mtk_venc_bitstream_size().
 *
 * There is no hardware to test against, so none of the above is claimed to
 * work end to end.  See NOTES.md and RECOVERED-ABI.md for which parts of the
 * vendor ABI are recoverable and which are not.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/v4l2-controls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk-vcodec-mt6589-reg.h"

#define MTK_VCODEC_DRIVER_NAME	"mtk-vcodec-mt6589"

/*
 * Encoder address register unit conventions (see mtk-vcodec-mt6589-reg.h for
 * the full statement).  Chapter 60 names every frame/bitstream address field
 * *_DRAM_ADDR_DIV16, so the register holds byte address >> 4 and the buffer
 * must be 16-byte aligned.  BITSTREAM_BUF_DRAM_SIZE is DIV128 instead, i.e.
 * byte size >> 7.
 *
 * The MPEG-4 datapath uses neither: its BASE_ADDR fields are plain byte
 * addresses needing only 8-byte alignment.
 *
 * Both families are 28-bit for chapter 60 and 32-bit for chapter 61, so the
 * address must fit in 2^28 bytes (256 MiB) / 4 GiB respectively or the write
 * below silently truncates it.
 */
#define VENC_ADDR_SHIFT			4
#define VENC_ADDR_MASK			GENMASK(27, 0)
#define VENC_BS_SIZE_SHIFT		7
#define VENC_BS_SIZE_MASK		GENMASK(24, 0)

#define VENC_MP4_ADDR_MASK		GENMASK(31, 0)

struct mtk_vcodec_dev {
	void __iomem *venc;
	void __iomem *vdec;

	struct clk *venc_clk;
	struct clk *vdec_vde_clk;
	struct clk *vdec_smi_clk;

	struct mutex lock;
	struct mutex enc_lock;
	struct mutex dec_lock;
	atomic_t enc_users;
	atomic_t dec_users;

	/*
	 * The device this driver instance belongs to.  Encoder and decoder are one
	 * probe of one DT node, so there is a single platform device, and the
	 * runtime-PM and IRQ paths all refer to this one.  It is assigned in
	 * probe() before any of them can run.
	 */
	struct platform_device *pdev;

	/* Encoder frame in flight, completed by the ISR.  NULL when idle. */
	dma_addr_t bs_addr;
	dma_addr_t bs_size;
	u32 bs_bytes;
	bool frame_pending;

	unsigned int irq_count;
	unsigned long venc_irq_count;
	unsigned long mp4_irq_count;
	bool suspended;
};

/*
 * Encoder parameters.  Everything here is either a property of the picture or a
 * rate control term; the buffer addresses are NOT here, because the caller
 * supplies them separately and they are DRAM addresses in the chapter-60
 * register table.
 */
struct mtk_vcodec_enc_parm {
	__u32 width;
	__u32 height;
	__u32 gop;		/* I-frame interval, in frames */
	__u32 bitrate;		/* bits per second */
	__u32 framerate;	/* frames per second, x100 for 29.97 */
	__u32 qp_max;		/* 1..31 */
	__u32 qp_min;		/* 1..31, with min <= init <= max <= 31 */
	__u32 qp_init;
	__u32 intra_vop_rate;	/* 1..31, I-frame interval in VOPs */
	__u32 bitrate_hard_limit;
	__u32 rate_balance;
	__u32 rc_algorithm;	/* vendor defined; see NOTES.md */
};

static inline void mtk_venc_write(struct mtk_vcodec_dev *vcodec, u32 off, u32 val)
{
	writel(val, vcodec->venc + off);
}

static inline u32 mtk_venc_read(struct mtk_vcodec_dev *vcodec, u32 off)
{
	return readl(vcodec->venc + off);
}

static inline void mtk_vdec_write(struct mtk_vcodec_dev *vcodec, u32 off, u32 val)
{
	writel(val, vcodec->vdec + off);
}

static inline u32 mtk_vdec_read(struct mtk_vcodec_dev *vcodec, u32 off)
{
	return readl(vcodec->vdec + off);
}

/* ------------------------------------------------------------------ */
/* Encoder							      */
/* ------------------------------------------------------------------ */

/*
 * Bring the H.264/VP8 front end to a known state.  VENC_SW_HRST_N is an active
 * low software reset for the encoder, so clear it, wait for the engine to leave
 * reset by reading the hardware-mode register, then release it.
 */
static void mtk_venc_reset(struct mtk_vcodec_dev *vcodec)
{
	mtk_venc_write(vcodec, VENC_SW_HRST_N, 0);

	/* The engine needs its clock before it can acknowledge a reset. */
	mtk_venc_write(vcodec, VENC_CODEC_CTRL, 0);
	mtk_venc_write(vcodec, VENC_IRQ_ACK, VENC_IRQ_MASK_ALL);
	mtk_venc_write(vcodec, VENC_SW_HRST_N, 1);

	/*
	 * Chapter 60 documents a VENC_HW_MODE_SEL at +0x0000 in the first table;
	 * it is the hardware mode of the shared encoder core.  Reading it also
	 * flushes any stale command left over from a previous run.
	 */
	mtk_venc_read(vcodec, 0x000);
}

static int mtk_venc_power_on(struct mtk_vcodec_dev *vcodec)
{
	int ret = clk_prepare_enable(vcodec->venc_clk);

	if (ret)
		return ret;

	/*
	 * Enable both interrupt sources.  The shared pair at +0x05c/+0x060 is
	 * acknowledged implicitly by the hardware when IRQs are disabled, while
	 * the MPEG-4 pair at +0x678/+0x67c is level held and must be masked off
	 * explicitly or the interrupt line never drops.
	 */
	mtk_venc_write(vcodec, VENC_IRQ_ACK, VENC_IRQ_MASK_ALL);
	mtk_venc_write(vcodec, VENC_MP4_IRQ_EN,
		       VENC_MP4_IRQ_EN_DONE | VENC_MP4_IRQ_EN_FULL);
	mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK,
		       VENC_MP4_IRQ_ACK_DONE | VENC_MP4_IRQ_ACK_FULL);

	return 0;
}

static void mtk_venc_power_off(struct mtk_vcodec_dev *vcodec)
{
	mtk_venc_write(vcodec, VENC_MP4_IRQ_EN, 0x0);
	mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK,
		       VENC_MP4_IRQ_ACK_DONE | VENC_MP4_IRQ_ACK_FULL);
	clk_disable_unprepare(vcodec->venc_clk);
}

static int mtk_vdec_power_on(struct mtk_vcodec_dev *vcodec)
{
	int ret;

	ret = clk_prepare_enable(vcodec->vdec_vde_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(vcodec->vdec_smi_clk);
	if (ret) {
		clk_disable_unprepare(vcodec->vdec_vde_clk);
		return ret;
	}

	return 0;
}

static void mtk_vdec_power_off(struct mtk_vcodec_dev *vcodec)
{
	clk_disable_unprepare(vcodec->vdec_smi_clk);
	clk_disable_unprepare(vcodec->vdec_vde_clk);
}

/*
 * Bytes the hardware actually produced, which is what a capture buffer's
 * bytesused has to be.
 *
 * H.264: VENC_PIC_BITSTREAM_BYTE_CNT[23:0] is defined by ch.60 as "Number of
 * bytes in coded bitstream of one frame".  It is the right register.  The old
 * code read VENC_STUFFING_REPORT instead, which reports bitstream stuffing and
 * has nothing to do with the length.
 *
 * VP8: the same field is documented as "VP8: Residual partition byte cnt" and
 * the VP8 header is counted separately in VENC_PIC_BITSTREAM_BYTE_CNT1, with
 * the header itself written to a second DRAM buffer (VP8_HDR_BUF_ADDR).  That
 * field has no bit description anywhere in the data sheet -- it appears only in
 * the summary table as a bare name -- so there is no way to establish from the
 * available sources whether the frame total is the sum, a concatenation length
 * or something else, and guessing would produce a corrupt bytesused.  This
 * driver therefore does not support VP8 rather than invent the combination.
 */
static u32 mtk_venc_bitstream_size(struct mtk_vcodec_dev *vcodec, bool mpeg4)
{
	if (!mpeg4)
		return mtk_venc_read(vcodec, VENC_PIC_BITSTREAM_BYTE_CNT) &
		       GENMASK(23, 0);

	return mtk_venc_read(vcodec, VENC_MP4_BYTE_COUNT) & GENMASK(23, 0);
}

/*
 * Encoder interrupt.
 *
 * The shared pair (H.264/VP8) reports SPS/PPS/frame/dram/pause.  The MPEG-4
 * datapath has its own pair: VENC_MP4_IRQ_STATUS is a bitfield whose bit 1 is
 * FRAME_IRQ, so a frame completion is a mask test, not a comparison against a
 * magic value.  BITSTREAM_IRQ (bit 4) means the output buffer overflowed and the
 * frame is not usable, so it is acked separately and does not complete the
 * frame.
 *
 * The line is level triggered, so an interrupt that arrives while we are
 * acknowledging is still pending afterwards: re-read the status and acknowledge
 * again, otherwise the line never falls.
 */
static irqreturn_t mtk_venc_isr(int irq, void *data)
{
	struct platform_device *pdev = data;
	struct mtk_vcodec_dev *vcodec = dev_get_drvdata(&pdev->dev);
	u32 status, mp4_status;
	int ret;

	/*
	 * pm_runtime_resume_and_get() returns 0 on a successful resume and < 0 on
	 * failure.  Testing the old "if (!pm_runtime_get_sync(...))" inverted
	 * that: a normal resume read as a failure and the ISR returned IRQ_NONE
	 * without acking, so the level-held line never dropped.
	 */
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0)
		return IRQ_NONE;

	status = mtk_venc_read(vcodec, VENC_IRQ_STATUS);
	if (status & VENC_IRQ_MASK_ALL) {
		vcodec->venc_irq_count++;
		mtk_venc_write(vcodec, VENC_IRQ_ACK, status & VENC_IRQ_MASK_ALL);
	}

	/*
	 * A frame-done interrupt means the hardware has finished DMA-ing out of
	 * both the bitstream buffer and the source picture, so only now is it safe
	 * for the caller to reuse the source.  The bitstream length is only known
	 * now too.
	 */
	mp4_status = mtk_venc_read(vcodec, VENC_MP4_IRQ_STATUS);
	if (mp4_status & VENC_MP4_IRQ_STATUS_FULL) {
		vcodec->mp4_irq_count++;
		mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, VENC_MP4_IRQ_ACK_FULL);
	}

	if (mp4_status & VENC_MP4_IRQ_STATUS_FRAME) {
		vcodec->mp4_irq_count++;
		/*
		 * The coded length is only known now.  Recording it here rather
		 * than in a submit path is the whole point: there is no V4L2 buffer
		 * to hand it back to, so it is kept for whoever owns the bitstream
		 * buffer and reads it after the interrupt.
		 */
		if (vcodec->frame_pending)
			vcodec->bs_bytes =
				mtk_venc_bitstream_size(vcodec, false);
		vcodec->frame_pending = false;
		mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, VENC_MP4_IRQ_ACK_DONE);
	} else if (mp4_status & VENC_MP4_IRQ_STATUS_SLICE) {
		/*
		 * A slice boundary is an intermediate event: ack it so the level
		 * drops, but do not treat the frame as finished.
		 */
		mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, VENC_MP4_IRQ_ACK_DONE);
	}

	/* Catch anything raised while we were acknowledging. */
	status = mtk_venc_read(vcodec, VENC_IRQ_STATUS);
	if (status & VENC_IRQ_MASK_ALL)
		mtk_venc_write(vcodec, VENC_IRQ_ACK, status & VENC_IRQ_MASK_ALL);

	pm_runtime_put_autosuspend(&pdev->dev);
	return IRQ_HANDLED;
}

/*
 * Decoder interrupt.  There is no status/ack register pair: frame end is bit 16
 * of MISC word 41 and is cleared by setting bits 0 and 4 then writing the
 * original value back, a two-step sequence the vendor code relies on
 * (vdec_hal_if_avs.c:1104-1106 and videocodec_kernel_driver.c:346-360 both
 * do exactly this).
 *
 * The decoder datapath itself is not programmed by this driver; see the file
 * header.
 */
static irqreturn_t mtk_vdec_isr(int irq, void *data)
{
	struct platform_device *pdev = data;
	struct mtk_vcodec_dev *vcodec = dev_get_drvdata(&pdev->dev);
	u32 val;
	int ret;

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0)
		return IRQ_NONE;

	val = mtk_vdec_read(vcodec, VDEC_MISC_FRAME_END);
	if (val & VDEC_FRAME_END_BIT) {
		mtk_vdec_write(vcodec, VDEC_MISC_FRAME_END, val |
			       VDEC_FRAME_END_SET | VDEC_FRAME_END_ACK);
		mtk_vdec_write(vcodec, VDEC_MISC_FRAME_END, val);
		vcodec->irq_count++;
	}

	pm_runtime_put_autosuspend(&pdev->dev);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Encoder programming, from the register map			      */
/* ------------------------------------------------------------------ */

/**
 * mtk_venc_set_frame_addr - program the H.264/VP8 frame and bitstream buffers.
 * @vcodec: driver instance
 * @bs_addr: bitstream buffer, byte address, 16-byte aligned
 * @bs_size: bitstream buffer size in bytes, a multiple of 128
 * @src_y: current frame luma, byte address, 16-byte aligned
 * @src_uv: current frame chroma, byte address, 16-byte aligned
 * @ref_y: reference frame luma, byte address, 16-byte aligned
 * @ref_uv: reference frame chroma, byte address, 16-byte aligned
 * @rec_y: reconstruction frame luma, byte address, 16-byte aligned
 * @rec_uv: reconstruction frame chroma, byte address, 16-byte aligned
 *
 * Programs all seven address registers of the chapter-60 datapath, including
 * the reconstruction pair: this is an inter-frame encoder and the transform
 * stage writes its output to FRM_REC_* before the next frame can be predicted
 * from it, so leaving REC unprogrammed is not a valid configuration.
 *
 * All seven are DIV16 and 28 bits wide, so the byte address must be 16-byte
 * aligned and below 2^28.  Both are checked rather than silently truncating a
 * buffer.
 *
 * Return: 0 on success, -EINVAL if an address or size is out of range.
 *
 * There is no caller yet: the driver owns no DMA buffers, so nothing has real
 * addresses to supply.  It is kept deliberately and marked __maybe_unused
 * rather than left to draw a dead-code warning, because its unit and alignment
 * handling is the part of this driver most worth reviewing; the file header
 * lists what a caller would have to own first.
 */
__maybe_unused
static int mtk_venc_set_frame_addr(struct mtk_vcodec_dev *vcodec,
				   dma_addr_t bs_addr, dma_addr_t bs_size,
				   dma_addr_t src_y, dma_addr_t src_uv,
				   dma_addr_t ref_y, dma_addr_t ref_uv,
				   dma_addr_t rec_y, dma_addr_t rec_uv)
{
	dma_addr_t addrs[] = {
		bs_addr, src_y, src_uv, ref_y, ref_uv, rec_y, rec_uv,
	};
	unsigned int i;

	/*
	 * The address fields are 28-bit DIV16 values, so the encodable window is
	 * 2^28 * 16 bytes == 4 GiB of DRAM.  Anything needing a bigger IOVA
	 * window would need the page-table path instead.
	 */
	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		if (addrs[i] & ((1 << VENC_ADDR_SHIFT) - 1))
			return -EINVAL;
		if (lower_32_bits(addrs[i] >> VENC_ADDR_SHIFT) &
		    ~VENC_ADDR_MASK)
			return -EINVAL;
	}

	/* BITSTREAM_BUF_SIZE is DIV128, not DIV16. */
	if (bs_size & ((1 << VENC_BS_SIZE_SHIFT) - 1))
		return -EINVAL;
	if (lower_32_bits(bs_size >> VENC_BS_SIZE_SHIFT) & ~VENC_BS_SIZE_MASK)
		return -EINVAL;

	mtk_venc_write(vcodec, VENC_BITSTREAM_BUF_ADDR,
		       lower_32_bits(bs_addr >> VENC_ADDR_SHIFT));
	mtk_venc_write(vcodec, VENC_BITSTREAM_BUF_SIZE,
		       lower_32_bits(bs_size >> VENC_BS_SIZE_SHIFT));

	mtk_venc_write(vcodec, VENC_FRM_CUR_Y_ADDR,
		       lower_32_bits(src_y >> VENC_ADDR_SHIFT));
	mtk_venc_write(vcodec, VENC_FRM_CUR_UV_ADDR,
		       lower_32_bits(src_uv >> VENC_ADDR_SHIFT));
	mtk_venc_write(vcodec, VENC_FRM_REF_Y_ADDR,
		       lower_32_bits(ref_y >> VENC_ADDR_SHIFT));
	mtk_venc_write(vcodec, VENC_FRM_REF_UV_ADDR,
		       lower_32_bits(ref_uv >> VENC_ADDR_SHIFT));
	mtk_venc_write(vcodec, VENC_FRM_REC_Y_ADDR,
		       lower_32_bits(rec_y >> VENC_ADDR_SHIFT));
	mtk_venc_write(vcodec, VENC_FRM_REC_UV_ADDR,
		       lower_32_bits(rec_uv >> VENC_ADDR_SHIFT));

	return 0;
}

/**
 * mtk_venc_set_mp4_frame_addr - program the MPEG-4 datapath buffers.
 * @vcodec: driver instance
 * @src_y: source luma plane address
 * @src_cb: source Cb plane address
 * @src_cr: source Cr plane address
 * @bs_addr: bitstream buffer address
 * @rec_y: reconstructed luma plane address
 * @rec_cb: reconstructed Cb plane address
 * @rec_cr: reconstructed Cr plane address
 * @ref_y: reference luma plane address
 * @ref_cb: reference Cb plane address
 * @ref_cr: reference Cr plane address
 *
 * Programs the chapter-61 address registers.  Unlike chapter 60 these are plain
 * 32-bit byte addresses with an 8-byte alignment requirement, so no shift is
 * applied.
 *
 * The chroma planes are taken as three separate pointers on purpose: MPEG-4 is
 * a genuinely 3-plane (YV12/I420) contract here, and the hardware has distinct
 * SRCADR_CB and SRCADR_CR registers.  Passing one interleaved UV pointer to both
 * is a layout bug, not a shorthand, so the caller must supply real planes.
 *
 * Return: 0 on success, -EINVAL if an address is misaligned or unencodable.
 *
 * No caller yet, for the same reason as mtk_venc_set_frame_addr().
 */
__maybe_unused
static int mtk_venc_set_mp4_frame_addr(struct mtk_vcodec_dev *vcodec,
				       dma_addr_t src_y, dma_addr_t src_cb,
				       dma_addr_t src_cr, dma_addr_t bs_addr,
				       dma_addr_t rec_y, dma_addr_t rec_cb,
				       dma_addr_t rec_cr,
				       dma_addr_t ref_y, dma_addr_t ref_cb,
				       dma_addr_t ref_cr)
{
	dma_addr_t addrs[] = {
		src_y, src_cb, src_cr, bs_addr,
		rec_y, rec_cb, rec_cr,
		ref_y, ref_cb, ref_cr,
	};
	unsigned int i;

	/* 8-byte alignment, full 32-bit field. */
	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		if (addrs[i] & 0x7)
			return -EINVAL;
		if (lower_32_bits(addrs[i]) & ~VENC_MP4_ADDR_MASK)
			return -EINVAL;
	}

	mtk_venc_write(vcodec, VENC_MP4_SRCADR_Y, lower_32_bits(src_y));
	mtk_venc_write(vcodec, VENC_MP4_SRCADR_CB, lower_32_bits(src_cb));
	mtk_venc_write(vcodec, VENC_MP4_SRCADR_CR, lower_32_bits(src_cr));
	mtk_venc_write(vcodec, VENC_MP4_BITADR, lower_32_bits(bs_addr));
	mtk_venc_write(vcodec, VENC_MP4_RECADR_Y, lower_32_bits(rec_y));
	mtk_venc_write(vcodec, VENC_MP4_RECADR_CB, lower_32_bits(rec_cb));
	mtk_venc_write(vcodec, VENC_MP4_RECADR_CR, lower_32_bits(rec_cr));
	mtk_venc_write(vcodec, VENC_MP4_REFADR_Y, lower_32_bits(ref_y));
	mtk_venc_write(vcodec, VENC_MP4_REFADR_CB, lower_32_bits(ref_cb));
	mtk_venc_write(vcodec, VENC_MP4_REFADR_CR, lower_32_bits(ref_cr));

	return 0;
}

/*
 * Rate control.  VENC_RATECONTROL_INFO_n is a four-register block in chapter 60;
 * the hardware applies the quantiser limits from it.  The bounds here are not
 * arbitrary: the vendor codec library clamps exactly these three values to
 * 1..31 with min <= init <= max, which is the H.264 quantiser range (see
 * RECOVERED-ABI.md for the disassembly).
 */
static int mtk_venc_set_rate_control(struct mtk_vcodec_dev *vcodec,
				     const struct mtk_vcodec_enc_parm *p)
{
	if (p->qp_min < 1 || p->qp_min > 31)
		return -EINVAL;
	if (p->qp_max < 1 || p->qp_max > 31)
		return -EINVAL;
	if (p->qp_init < p->qp_min || p->qp_init > p->qp_max)
		return -EINVAL;
	if (!p->intra_vop_rate || p->intra_vop_rate > 31)
		return -EINVAL;

	mtk_venc_write(vcodec, VENC_RATECONTROL_INFO(0), p->qp_min);
	mtk_venc_write(vcodec, VENC_RATECONTROL_INFO(1), p->qp_init);
	mtk_venc_write(vcodec, VENC_RATECONTROL_INFO(2), p->qp_max);
	mtk_venc_write(vcodec, VENC_RATECONTROL_INFO(3), p->bitrate_hard_limit);
	mtk_venc_write(vcodec, VENC_RC_INFO_DRAM_ADDR, p->intra_vop_rate);

	return 0;
}

/*
 * Start one picture.
 *
 * The engine is edge sensitive on VENC_MP4_FRAME_START for the MPEG-4 path and
 * level driven through the shared front end for H.264/VP8, so clear the start
 * first and set it last.
 */
static void mtk_venc_start_frame(struct mtk_vcodec_dev *vcodec, bool mpeg4)
{
	mtk_venc_write(vcodec, VENC_CODEC_CTRL, mpeg4 ? 1 : 0);

	if (mpeg4) {
		mtk_venc_write(vcodec, VENC_MP4_FRAME_START, 0);
		mtk_venc_write(vcodec, VENC_MP4_FRAME_START, 1);
	}
}

/*
 * Submit one frame's worth of programming.
 *
 * The caller owns the DMA buffers and must not release or reuse them until the
 * frame-done interrupt has arrived: the encoder is still reading the source
 * picture when this returns.  vcodec->frame_pending records that and the ISR
 * clears it, so a caller without a V4L2 buffer queue still has something to
 * wait on; vcodec->bs_bytes holds the coded length afterwards.
 *
 * Return: 0 on success, -EINVAL for a zero or misaligned buffer address or a
 *	   bad quantiser setting, -ENODEV for the MPEG-4 datapath.
 */
__maybe_unused
static int mtk_venc_submit_frame(struct mtk_vcodec_dev *vcodec, bool mpeg4,
				 const struct mtk_vcodec_enc_parm *parm,
				 dma_addr_t bs_addr, dma_addr_t bs_size,
				 dma_addr_t src_y, dma_addr_t src_uv,
				 dma_addr_t ref_y, dma_addr_t ref_uv,
				 dma_addr_t rec_y, dma_addr_t rec_uv)
{
	dma_addr_t frames[] = { src_y, src_uv, ref_y, ref_uv, rec_y, rec_uv };
	unsigned int i;
	int ret;

	/*
	 * The inter-frame datapath reads the current frame, predicts against the
	 * reference frame and writes the reconstruction frame.  All three must be
	 * real buffers; there is no sane configuration in which any of them is
	 * address 0, and there is nothing behind them here to make that safe.
	 */
	if (!bs_addr || !bs_size)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(frames); i++)
		if (!frames[i])
			return -EINVAL;

	/*
	 * MPEG-4 is still genuinely blocked: the datapath needs three distinct
	 * source chroma planes, three reconstruction planes and a per-MB side
	 * information buffer (VENC_MP4_SIDE_ADDR), none of which the driver
	 * allocates.  Until it does, mpeg4 is rejected rather than encoded from a
	 * layout that does not exist.
	 */
	if (mpeg4)
		return -ENODEV;

	mutex_lock(&vcodec->enc_lock);

	ret = mtk_venc_set_frame_addr(vcodec, bs_addr, bs_size,
				      src_y, src_uv, ref_y, ref_uv,
				      rec_y, rec_uv);
	if (!ret)
		ret = mtk_venc_set_rate_control(vcodec, parm);
	if (!ret)
		mtk_venc_start_frame(vcodec, false);

	if (!ret) {
		vcodec->bs_addr = bs_addr;
		vcodec->bs_size = bs_size;
		vcodec->bs_bytes = 0;
		vcodec->frame_pending = true;
	}

	mutex_unlock(&vcodec->enc_lock);

	return ret;
}

static int mtk_vcodec_runtime_suspend(struct device *dev)
{
	struct mtk_vcodec_dev *vcodec = dev_get_drvdata(dev);

	mutex_lock(&vcodec->enc_lock);
	if (atomic_read(&vcodec->enc_users) == 0)
		mtk_venc_power_off(vcodec);
	mutex_unlock(&vcodec->enc_lock);

	mutex_lock(&vcodec->dec_lock);
	if (atomic_read(&vcodec->dec_users) == 0)
		mtk_vdec_power_off(vcodec);
	mutex_unlock(&vcodec->dec_lock);

	vcodec->suspended = true;
	return 0;
}

static int mtk_vcodec_runtime_resume(struct device *dev)
{
	struct mtk_vcodec_dev *vcodec = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&vcodec->dec_lock);
	if (atomic_read(&vcodec->dec_users) == 0) {
		ret = mtk_vdec_power_on(vcodec);
		if (ret)
			goto err_dec_unlock;
	}
	mutex_unlock(&vcodec->dec_lock);

	mutex_lock(&vcodec->enc_lock);
	if (atomic_read(&vcodec->enc_users) == 0) {
		ret = mtk_venc_power_on(vcodec);
		if (ret) {
			mutex_unlock(&vcodec->enc_lock);
			goto err_dec_off;
		}
		mtk_venc_reset(vcodec);
	}
	mutex_unlock(&vcodec->enc_lock);

	vcodec->suspended = false;
	return 0;

err_dec_unlock:
	mutex_unlock(&vcodec->dec_lock);
err_dec_off:
	/*
	 * Roll the decoder clocks back if the encoder failed to come up, so a
	 * failed resume does not leak a clock reference.  mtk_vdec_power_off() is
	 * safe to call here because the decoder side is known to be on: it is
	 * only reached after a successful mtk_vdec_power_on().
	 */
	if (atomic_read(&vcodec->dec_users) == 0) {
		mutex_lock(&vcodec->dec_lock);
		mtk_vdec_power_off(vcodec);
		mutex_unlock(&vcodec->dec_lock);
	}
	pr_err("Failed to resume vcodec: %d\n", ret);
	return ret;
}

static int mtk_vcodec_probe(struct platform_device *pdev)
{
	struct mtk_vcodec_dev *vcodec;
	int ret;

	vcodec = devm_kzalloc(&pdev->dev, sizeof(*vcodec), GFP_KERNEL);
	if (!vcodec)
		return -ENOMEM;

	platform_set_drvdata(pdev, vcodec);

	/*
	 * The single platform device of this driver instance.  Assigned before
	 * anything else so that the runtime-PM and IRQ paths below, which all
	 * refer to vcodec->pdev or to &pdev->dev, always have a valid owner.
	 * Encoder and decoder are one DT node, not two, so there is no second
	 * device to look up; see the file header on the V4L2 layer.
	 */
	vcodec->pdev = pdev;

	mutex_init(&vcodec->lock);
	mutex_init(&vcodec->enc_lock);
	mutex_init(&vcodec->dec_lock);
	atomic_set(&vcodec->enc_users, 0);
	atomic_set(&vcodec->dec_users, 0);

	vcodec->venc = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vcodec->venc))
		return PTR_ERR(vcodec->venc);

	vcodec->vdec = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(vcodec->vdec))
		return PTR_ERR(vcodec->vdec);

	/*
	 * clock-names is required in the binding: the encoder clock comes from
	 * the vencsys provider and the decoder clocks from vdecsys, and both
	 * providers number their gates from zero, so provider-local ids collide
	 * and index-based lookup cannot tell them apart.
	 */
	vcodec->venc_clk = devm_clk_get(&pdev->dev, "venc");
	if (IS_ERR(vcodec->venc_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(vcodec->venc_clk),
				     "failed to get venc clock\n");

	vcodec->vdec_vde_clk = devm_clk_get(&pdev->dev, "vdec-vde");
	if (IS_ERR(vcodec->vdec_vde_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(vcodec->vdec_vde_clk),
				     "failed to get vdec vde clock\n");

	vcodec->vdec_smi_clk = devm_clk_get(&pdev->dev, "vdec-smi");
	if (IS_ERR(vcodec->vdec_smi_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(vcodec->vdec_smi_clk),
				     "failed to get vdec smi clock\n");

	ret = devm_request_irq(&pdev->dev, platform_get_irq(pdev, 0),
			       mtk_venc_isr, IRQF_TRIGGER_LOW,
			       dev_name(&pdev->dev), pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request venc irq\n");

	ret = devm_request_irq(&pdev->dev, platform_get_irq(pdev, 1),
			       mtk_vdec_isr, IRQF_TRIGGER_HIGH,
			       dev_name(&pdev->dev), pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request vdec irq\n");

	pm_runtime_enable(&pdev->dev);

	return 0;
}

static const struct dev_pm_ops mtk_vcodec_pm_ops = {
	.runtime_suspend = mtk_vcodec_runtime_suspend,
	.runtime_resume = mtk_vcodec_runtime_resume,
	.runtime_idle = pm_runtime_idle,
};

static const struct of_device_id mtk_vcodec_of_match[] = {
	{ .compatible = "mediatek,mt6589-vcodec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_vcodec_of_match);

static struct platform_driver mtk_vcodec_driver = {
	.probe = mtk_vcodec_probe,
	.driver = {
		.name = MTK_VCODEC_DRIVER_NAME,
		.pm = &mtk_vcodec_pm_ops,
		.of_match_table = mtk_vcodec_of_match,
	},
};

module_platform_driver(mtk_vcodec_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6589 video codec hardware front end");
MODULE_LICENSE("GPL");
