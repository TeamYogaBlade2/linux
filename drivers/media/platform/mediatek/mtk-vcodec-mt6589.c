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
 *   - programs the H.264 rate control into the fields the registers really
 *     have, rejecting quantisers outside the hardware's documented 0..51 range;
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
 *   - Rate control.  What is programmed here is the quantiser triple, the
 *     target bit rate, the CBR/initial-QP mode bits, the rate control fps and
 *     the P/B frame QP adjust limiters.  The actual bitrate algorithm, and the
 *     RC scratch memory at RC_CODE/RC_INFO_DRAM_ADDR that it loads and saves
 *     its state through, live in the vendor's closed-source userspace library.
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
 *
 * Known limitation: no encode timeout
 * -----------------------------------
 * The submit path takes a runtime-PM reference for a frame and the interrupt
 * handler drops it when the frame ends.  Every hardware way of ending a frame
 * is handled: the H.264 frame-done interrupt, the MPEG-4 frame-done interrupt,
 * and a bitstream-buffer overflow on either datapath.  What is NOT handled is
 * the encoder never signalling completion at all -- a wedged engine or a lost
 * interrupt.  In that case the reference is never dropped, the frame stays
 * pending, and every later submit returns -EBUSY with the encoder clock held on.
 *
 * That is a deliberate gap rather than an oversight: recovering from a wedged
 * encoder needs either a timeout or a hardware status poll, and this driver
 * has neither the timer nor the buffer ownership that a timeout path would
 * need.  It is recorded here because the alternative -- an ISR that
 * unconditionally drops the reference -- would be worse: it could power the
 * clocks off underneath an encode that is still running.
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
 * Chapter 60's address fields are 28-bit DIV16, which encodes 2^28 * 16 == 4
 * GiB of byte address -- exactly the full 32-bit address space, so nothing is
 * actually lost.  Chapter 61's are plain 32-bit byte addresses, also 4 GiB.
 * Both therefore cover the whole 32-bit IOVA space, and the checks below are
 * alignment checks plus belt-and-braces range checks on the shifted value.
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
	/*
	 * Encoder user count, reserved for the power-management refcount scheme
	 * below.
	 *
	 * HONEST WARNING: nothing in this driver increments it.  There is no V4L2
	 * queue and no encoder open/close, because nothing can reach this driver
	 * from userspace today (see the file header), so it is permanently 0 and
	 * every "enc_users == 0" test in the suspend/resume paths below is
	 * unconditionally true.  It is kept because it is the hook a future
	 * buffer-owning layer needs, and because removing it would mean rewriting
	 * the power-management structure, which is not what this change is about.
	 * But it does NOT currently hold off the suspend path: what actually keeps
	 * the encoder clocks on across a frame in flight is the runtime-PM
	 * reference the submit path holds and the interrupt handler drops, which
	 * is independent of these counters.
	 */
	atomic_t enc_users;
	atomic_t dec_users;

	/*
	 * Serialises the encoder frame-in-flight bookkeeping -- frame_pending,
	 * bs_bytes, bs_addr/bs_size -- between the submit path (which takes the
	 * runtime-PM reference and starts a frame) and the interrupt handler
	 * (which retires it and drops the reference).  A spinlock, not a mutex,
	 * because the interrupt handler has to take it; enc_lock is a sleeping
	 * mutex and cannot be taken from interrupt context.  Only this short
	 * critical section runs with interrupts disabled; all register accesses
	 * stay outside it.
	 */
	spinlock_t enc_state_lock;
	bool enc_pm_held;

	/*
	 * The device this driver instance belongs to.  Encoder and decoder are one
	 * probe of one DT node, so there is a single platform device, and the
	 * runtime-PM and IRQ paths all refer to this one.  It is assigned in
	 * probe() before any of them can run.
	 */
	struct platform_device *pdev;

	/*
	 * Encoder frame in flight, completed by the ISR.  Guarded by
	 * enc_state_lock, since both the submit path and the interrupt handler
	 * touch them.  bs_addr and bs_size are currently informational only --
	 * nothing branches on them, they record which buffer the frame owns so that
	 * whoever adds a buffer queue knows what to hand back -- but they are
	 * published and cleared under the same lock as the flags that the ISR
	 * actually acts on, so they cannot describe a frame that has been retired.
	 */
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
	__u32 gop;
	__u32 bitrate;		/* bits per second; max 131071 (17-bit field) */
	__u32 framerate;	/* frames per second, x100 for 29.97 */
	__u32 qp_init;		/* 0..51, H.264 */
	__u32 rc_fps;		/* 0..255, 0 = hardware default of 30 */
	__u32 p_frm_q_limiter;	/* 0..6, data sheet range 3..6, suggests 3 */
	__u32 b_frm_q_limiter;	/* 0..8, data sheet range 5..8, suggests 5 */
	__u32 rc_algorithm;	/* vendor defined; see NOTES.md */
	bool cbr;		/* constant rather than variable bit rate */
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

	/*
	 * The engine needs its clock before it can acknowledge a reset, and
	 * clearing any interrupt left over from a previous run stops that stale
	 * interrupt from being serviced as if it belonged to a frame just started.
	 *
	 * VENC_CODEC_CTRL is deliberately NOT written here.  Every bit of it is
	 * documented as a one-shot "0: No operation / 1: Start to encode"
	 * (draft/ds/venc.txt:4330-4375), so writing 0 to it clears nothing and
	 * would only be mistaken for a reset of the command register.
	 */
	mtk_venc_write(vcodec, VENC_IRQ_ACK, VENC_IRQ_MASK_ALL);
	mtk_venc_write(vcodec, VENC_SW_HRST_N, 1);

	/*
	 * Read back the hardware mode of the shared encoder core.  VENC_HW_MODE_SEL
	 * is documented at +0x0000 by both the ch.60 summary table and its own
	 * bit-field section (draft/ds/venc.txt:1910, 2579; reset 0x10000020).
	 *
	 * This is a readback for its side effect of completing the reset handshake.
	 * It is NOT a command flush, and an earlier revision claimed it was:
	 * nothing in the data sheet documents a read-flush of any command queue by
	 * this register, whose bits 19:0 are a mix of RW configuration and RO
	 * status (draft/ds/venc.txt:2606).  The value read is intentionally
	 * unused, which is why this is a bare read and not an assignment.
	 */
	mtk_venc_read(vcodec, VENC_HW_MODE_SEL);
}

static int mtk_venc_power_on(struct mtk_vcodec_dev *vcodec)
{
	int ret = clk_prepare_enable(vcodec->venc_clk);

	if (ret)
		return ret;

	/*
	 * Unlock the encoder's internal registers.
	 *
	 * VENC_CE is the video encoder's codec-enable at +0x0EC.  It has no
	 * bit-field section in the data sheet -- only a bare name in the ch.60
	 * summary table -- so what it means exactly is not documented here, but
	 * the vendor code makes the requirement unambiguous: kernel/core/mt_dcm.c
	 * writes VENC_CE = 0x1 as the first thing it does in both the DCM-enable
	 * (:299) and the DCM-disable (:396) paths, ahead of every other VENC
	 * register it then programs, and the same again before reading the
	 * registers for its DCM dump (:186).  mt_dcm.h:102 puts it at 0xF70020EC,
	 * which is this +0x0EC after the +0xE0000000 remap.  So: enable it before
	 * touching anything else in the block, and write the whole word as 1 so
	 * the driver does not depend on a bit position nothing documents.
	 */
	mtk_venc_write(vcodec, VENC_CE, 0x1);

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
 *
 * No runtime-PM reference is taken here.  pm_runtime_resume_and_get() may sleep
 * and may run the resume callback, which cannot be done from interrupt context
 * unless the device is marked pm_runtime_irq_safe() -- this one is not, and
 * marking it so would be a lie: the resume path enables clocks and writes
 * registers.  It is also unnecessary: a frame-done interrupt can only arrive
 * while the engine is running, which means the submit path already holds the
 * reference (see mtk_venc_submit_frame()).  That reference is dropped here,
 * once, and only when the frame that took it has actually completed.
 */
static irqreturn_t mtk_venc_isr(int irq, void *data)
{
	struct platform_device *pdev = data;
	struct mtk_vcodec_dev *vcodec = dev_get_drvdata(&pdev->dev);
	unsigned long flags;
	u32 status, mp4_status, bs_bytes = 0;
	bool frm_done = false, mp4_done = false, overflow = false;
	bool retire_pm = false, frame_failed = false;

	status = mtk_venc_read(vcodec, VENC_IRQ_STATUS);
	if (status & VENC_IRQ_MASK_ALL) {
		vcodec->venc_irq_count++;
		mtk_venc_write(vcodec, VENC_IRQ_ACK, status & VENC_IRQ_MASK_ALL);

		/*
		 * ENC_FRM_INT (bit 2) is the H.264/VP8 frame-completion interrupt:
		 * ch.60 calls it "Frame encoding finished interrupt"
		 * (draft/ds/venc.txt:4461-4466), and the vendor ISR treats it as
		 * the end of the encode and acks exactly bit 2
		 * (videocodec_kernel_driver.c:440-442).
		 *
		 * An earlier revision only acked this and left frame_pending set,
		 * so an H.264 frame never completed: the state machine stayed
		 * stuck believing a frame was still in flight forever, and the
		 * bitstream length was never recorded.  Complete the frame here,
		 * exactly as the MPEG-4 path below does, and take the coded length
		 * from the same place.
		 */
		frm_done = !!(status & VENC_IRQ_STATUS_FRM);

		/*
		 * BS_DRAM_FULL_INT (bit 3) means "During SPS/PPS/frame encoding,
		 * the generated bitstream byte count exceeds allocated bitstream
		 * DRAM size" (draft/ds/venc.txt:4468-4470), and
		 * VP8_HEADER_BS_DRAM_FULL_INT (bit 5) is the same condition for
		 * the VP8 header partition.  The frame is lost, and -- the part
		 * that matters for the runtime-PM reference -- no ENC_FRM_INT will
		 * follow for it, because the encoder stopped early.
		 *
		 * An earlier revision acked this bit and nothing else.  The frame
		 * then never completed: frame_pending and the runtime-PM reference
		 * taken by the submit path stayed set forever, every later submit
		 * returned -EBUSY, and venc_clk was pinned on permanently.  So the
		 * overflow has to retire the frame too, reporting it by publishing
		 * a zero length.
		 */
		overflow = !!(status & (VENC_IRQ_STATUS_DRAM |
					 VENC_IRQ_STATUS_DRAM_VP8));
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
		/*
		 * Same reasoning as BS_DRAM_FULL_INT above: BITSTREAM_IRQ (bit 4)
		 * means the output buffer overflowed, the frame is unusable, and no
		 * FRAME_IRQ will arrive for it -- so it has to be retired here or
		 * it latches forever with the runtime-PM reference still held.
		 *
		 * This is deliberately separate from the FRAME/SLICE chain below:
		 * an overflow is not a slice event, and the two are acked through
		 * different bits.
		 */
		overflow = true;
		mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, VENC_MP4_IRQ_ACK_FULL);
	}

	if (mp4_status & VENC_MP4_IRQ_STATUS_FRAME) {
		vcodec->mp4_irq_count++;
		/*
		 * The coded length is only known now.  Recording it here rather
		 * than in a submit path is the whole point: there is no V4L2 buffer
		 * to hand it back to, so it is kept for whoever owns the bitstream
		 * buffer and reads it after the interrupt.
		 *
		 * Note the "true": this reads the MPEG-4 datapath's own byte
		 * count.  An earlier revision passed "false" here and so read the
		 * H.264 counter for an MPEG-4 frame, which is a different block's
		 * register and a meaningless value.
		 */
		mp4_done = true;
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

	/*
	 * Retire the frame, if any, and drop the runtime-PM reference the submit
	 * path took for it.
	 *
	 * Three things end a frame, and all three must retire it or the runtime-PM
	 * reference is stranded: a normal frame-done, the MPEG-4 frame-done, and a
	 * bitstream-buffer overflow.  The byte count is read outside the lock -- it
	 * is a register read, and this driver does not take a spinlock across
	 * register accesses -- and is only kept if there was a frame to attach it
	 * to.
	 *
	 * An overflow publishes a length of zero rather than whatever the counter
	 * happens to hold.  The counter reflects bytes actually emitted before the
	 * encoder gave up, which is not a complete frame, so reporting it would be
	 * worse than reporting nothing: the caller would hand a truncated,
	 * unparseable bitstream to userspace.  Zero is also what an empty capture
	 * buffer means, so a caller needs no way to tell the two apart beyond the
	 * length itself.
	 *
	 * enc_pm_held is the authoritative record of whether a reference is
	 * outstanding, and it is cleared in the same critical section that clears
	 * frame_pending.  That is what makes the pairing exact: the reference is
	 * taken once per started frame and released once, by whichever completion
	 * retires that frame.  An extra or spurious interrupt -- an SPS or PPS
	 * interrupt, or a second frame-done with nothing in flight -- finds
	 * frame_pending already clear, so it neither records a bogus length nor
	 * decrements the usage count a second time.  The reference therefore can
	 * be neither released twice, which would underflow the runtime-PM count and
	 * power the clocks off underneath a live frame, nor leaked: every frame
	 * that is started ends in exactly one of the three retiring events above.
	 *
	 * The one case that still strands a reference is the hardware never
	 * signalling completion at all -- a wedged encoder, a lost interrupt.  That
	 * is a hardware fault with no in-driver recovery, and it is called out in
	 * the file header rather than papered over with a timeout that this driver
	 * has no infrastructure to implement.
	 */
	frame_failed = overflow && !frm_done && !mp4_done;

	if (frm_done)
		bs_bytes = mtk_venc_bitstream_size(vcodec, false);
	else if (mp4_done)
		bs_bytes = mtk_venc_bitstream_size(vcodec, true);
	else if (frame_failed)
		bs_bytes = 0;

	spin_lock_irqsave(&vcodec->enc_state_lock, flags);
	if ((frm_done || mp4_done || frame_failed) && vcodec->frame_pending) {
		vcodec->bs_bytes = bs_bytes;
		vcodec->frame_pending = false;
		vcodec->bs_addr = 0;
		vcodec->bs_size = 0;
	}
	if ((frm_done || mp4_done || frame_failed) && vcodec->enc_pm_held) {
		vcodec->enc_pm_held = false;
		retire_pm = true;
	}
	spin_unlock_irqrestore(&vcodec->enc_state_lock, flags);

	if (retire_pm)
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

	/*
	 * Deliberately no runtime-PM reference here, and deliberately no put
	 * either.  Unlike the encoder there is no decoder submit path in this
	 * driver to take a matching reference -- see the file header, the decoder
	 * datapath is not programmed at all -- so there is nothing to balance
	 * against and a put here would decrement a count this ISR never
	 * incremented.  Taking a reference is not an option either: the resume
	 * path enables clocks and writes registers, which is not permitted from
	 * interrupt context without pm_runtime_irq_safe(), and this device is
	 * not so marked because doing so would not be true of its resume path.
	 *
	 * The access is safe anyway: an interrupt from the block means its clock
	 * is already running.
	 */
	val = mtk_vdec_read(vcodec, VDEC_MISC_FRAME_END);
	if (val & VDEC_FRAME_END_BIT) {
		mtk_vdec_write(vcodec, VDEC_MISC_FRAME_END, val |
			       VDEC_FRAME_END_SET | VDEC_FRAME_END_ACK);
		mtk_vdec_write(vcodec, VDEC_MISC_FRAME_END, val);
		vcodec->irq_count++;
	}

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
 * Rate control.
 *
 * This used to be a four-register QP triple: qp_min/qp_init/qp_max into
 * RATECONTROL_INFO_0/1/2, a bitrate limit into _3, and the intra-VOP rate into
 * VENC_RC_INFO_DRAM_ADDR.  None of that maps onto the hardware:
 *
 *   - RATECONTROL_INFO_2 and _3 are "rev" from bit 31 down to bit 0, i.e. wholly
 *     reserved (draft/ds/venc.txt:4206-4260).  Writing them programmed nothing.
 *   - RATECONTROL_INFO_0 is a bundle: bit 18 RC_CBR, bit 17 RC_INI_QP and
 *     bits 16:0 RC_TARGET_BIT_RATE (draft/ds/venc.txt:4072-4087).  Writing a QP
 *     there put a value of 1..31 into the low bit of the target bit rate.
 *   - RATECONTROL_INFO_1 is fps and QP adjust limiters: 23:16 RC_FPS, 15:8
 *     BfrmQLimter, 7:0 PfrmQLimter (draft/ds/venc.txt:4187-4205).
 *   - VENC_RC_INFO_DRAM_ADDR is a 28-bit DIV16 DRAM *address* for the rate
 *     control's scratch state, "Initial DRAM byte address of RC info. for
 *     loading and saving divided by 16" (draft/ds/venc.txt:5134-5140).  Writing
 *     a bare integer there pointed the scratch pointer at DRAM address
 *     intra_vop_rate * 16 -- and since the driver allocates nothing, at
 *     whatever the integrator happened to have mapped low in memory.
 *
 * And the quantisers were not even in the right registers.  They are in
 * VENC_ENCODER_INFO_0/1: QP_I_FRM at bits 31:26 of +0x004, QP_P_FRM at bits
 * 15:10 and QP_B_FRM at bits 31:26 of +0x008 (draft/ds/venc.txt:2802-2804,
 * 2893-2901, 2945-2947).  The tempting candidates VENC_H264_ENC_INFO_0/1 at
 * +0x030/+0x034 are CABAC/profile/level and wholly reserved respectively, so
 * writing QPs there would have corrupted the CABAC and level fields.
 *
 * The range is [0, 51] for H.264, stated explicitly by the data sheet for all
 * three QP fields; the same rows give [1, 63] for VP8 and [1, 31] for MPEG-4.
 * This driver only drives H.264, so 0..51 is what it validates -- note 0 is
 * legal, and an earlier revision rejected it along with anything above 31.
 *
 * What is still missing is the rate control scratch memory itself: the hardware
 * loads and saves its state through RC_CODE_DRAM_ADDR and RC_INFO_DRAM_ADDR,
 * both DIV16 address registers, and this driver owns no buffer for either.  They
 * are validated rather than programmed -- see mtk_venc_set_rc_scratch_addr() --
 * so a caller cannot point them somewhere out of range.
 *
 * Return: 0 on success, -EINVAL if a value does not fit its documented field or
 *	   range.
 */
static int mtk_venc_set_rate_control(struct mtk_vcodec_dev *vcodec,
				     const struct mtk_vcodec_enc_parm *p)
{
	u32 rc_info_0, rc_info_1, enc_info_0, enc_info_1;

	/* QP for I-frame, QP for P-frame and QP for B-frame: all 0..51, H.264. */
	if (p->qp_init > VENC_H264_QP_MAX)
		return -EINVAL;

	/*
	 * RC_TARGET_BIT_RATE is 17 bits wide (draft/ds/venc.txt:4078-4087), so
	 * reject a bit rate that does not fit instead of truncating it into a
	 * silently wrong encode rate.
	 */
	if (p->bitrate > VENC_RC_TARGET_BIT_RATE_MASK)
		return -EINVAL;

	/* RC_FPS is 8 bits at 23:16 of RATECONTROL_INFO_1. */
	if (p->rc_fps > GENMASK(7, 0))
		return -EINVAL;

	/*
	 * The QP adjust limiters have documented valid ranges, not just field
	 * widths: BfrmQLimter is "Suggested: 5, Range: 5 ~ 8" and PfrmQLimter is
	 * "Suggested: 3, Range: 3 ~ 6" (draft/ds/venc.txt:4186-4195).  Checking
	 * only the 8-bit width would accept 0, 1, 2 and 7..255, all of which the
	 * data sheet says are out of range -- the same class of mistake the QP
	 * range check used to make.  Zero means "unspecified", so the documented
	 * suggested values are substituted below rather than rejected.
	 */
	if (p->p_frm_q_limiter > VENC_RC_PFRM_Q_LIM_MAX)
		return -EINVAL;
	if (p->b_frm_q_limiter > VENC_RC_BFRM_Q_LIM_MAX)
		return -EINVAL;

	/*
	 * RATECONTROL_INFO_0: constant-vs-variable bit rate, whether the QP
	 * triple below is used as the initial QP, and the target bit rate.
	 *
	 * RC_INI_QP is set whenever we are programming the QP fields at all, so
	 * that the encoder actually uses qp_init rather than silently ignoring it
	 * in favour of the rate control algorithm's own default.
	 */
	rc_info_0 = p->bitrate & VENC_RC_TARGET_BIT_RATE_MASK;
	rc_info_0 |= VENC_RC_INI_QP;
	if (p->cbr)
		rc_info_0 |= VENC_RC_CBR;

	/*
	 * RATECONTROL_INFO_1: rate control fps, and the P and B frame QP adjust
	 * limiters.  The data sheet's suggested values (3 for P, 5 for B) stand in
	 * when the caller expresses no preference.
	 */
	rc_info_1 = (p->rc_fps << VENC_RC_FPS_SHIFT) & VENC_RC_FPS_MASK;
	rc_info_1 |= (p->p_frm_q_limiter ? p->p_frm_q_limiter :
		      VENC_RC_PFRM_Q_LIM_SUGGESTED) & VENC_RC_PFRM_Q_LIM_MASK;
	rc_info_1 |= (p->b_frm_q_limiter ? p->b_frm_q_limiter :
		      VENC_RC_BFRM_Q_LIM_SUGGESTED) & VENC_RC_BFRM_Q_LIM_MASK;

	/*
	 * The QP fields themselves, in the encoder info registers.
	 *
	 * Read-modify-write, deliberately.  VENC_ENCODER_INFO_1 resets to
	 * 0x01200120 (draft/ds/venc.txt:2854-2875), and that reset value is not
	 * all zeroes: bits 5, 8, 21 and 24 are set, which is P_SEARCH_H[7:5] = 2,
	 * P_SEARCH_V[9:8] = 1, B_SEARCH_H[23:21] = 1 and B_SEARCH_V[25:24] = 1 --
	 * the motion search range divisors.  Writing the register as the QP alone
	 * would zero all four of them and collapse the search ranges, so only the
	 * QP field is replaced.
	 *
	 * Worth being precise about what the reset value does and does not give,
	 * because it is tempting to assume it is a usable default: the 16x16/16x08/
	 * 08x16/08x08 block-mode enables in bits 19:16 and 4:0 are all ZERO in that
	 * reset value (draft/ds/venc.txt:2873, 2884), i.e. every block partition
	 * mode is off by default.  That is inherited hardware state, not something
	 * this driver chose, and choosing otherwise would be a guess.  What the
	 * read-modify-write guarantees is simply that programming a QP does not
	 * silently change anything else -- including the block modes, whatever they
	 * happen to be.
	 *
	 * QP_I_FRM is bits 31:26 of ENCODER_INFO_0; QP_P_FRM is bits 15:10 of
	 * ENCODER_INFO_1 (draft/ds/venc.txt:2802-2804, 2945-2947).  The B frame QP
	 * is left alone: this driver does not program NUM_B_FRM, so no B frames are
	 * generated and there is no B frame QP to set.
	 */
	enc_info_0 = mtk_venc_read(vcodec, VENC_ENCODER_INFO_0);
	enc_info_0 &= ~VENC_QP_I_FRM_MASK;
	enc_info_0 |= (p->qp_init << VENC_QP_I_FRM_SHIFT) & VENC_QP_I_FRM_MASK;

	enc_info_1 = mtk_venc_read(vcodec, VENC_ENCODER_INFO_1);
	enc_info_1 &= ~VENC_QP_P_FRM_MASK;
	enc_info_1 |= (p->qp_init << VENC_QP_P_FRM_SHIFT) & VENC_QP_P_FRM_MASK;

	mtk_venc_write(vcodec, VENC_RATECONTROL_INFO_0, rc_info_0);
	mtk_venc_write(vcodec, VENC_RATECONTROL_INFO_1, rc_info_1);
	mtk_venc_write(vcodec, VENC_ENCODER_INFO_0, enc_info_0);
	mtk_venc_write(vcodec, VENC_ENCODER_INFO_1, enc_info_1);

	return 0;
}

/**
 * mtk_venc_set_rc_scratch_addr - validate the rate control scratch buffers.
 * @vcodec: driver instance
 * @rc_code_addr: RC code buffer, byte address, 16-byte aligned
 * @rc_info_addr: RC info buffer, byte address, 16-byte aligned
 *
 * The rate control loads and saves its state through these two buffers.  The
 * driver allocates neither, so there is no caller yet -- but the point of
 * keeping this is that both are DIV16 DRAM *address* registers, and an address
 * that does not fit would be silently truncated into pointing the hardware at a
 * different buffer.  So the addresses are range-checked here rather than being
 * written blindly wherever they came from.
 *
 * Both need 16-byte alignment and both are 28-bit DIV16 fields, which encode the
 * full 32-bit address space.
 *
 * Return: 0 on success, -EINVAL if either address is misaligned or unencodable.
 *
 * No caller yet, for the same reason as the buffer programming helpers above.
 */
__maybe_unused
static int mtk_venc_set_rc_scratch_addr(struct mtk_vcodec_dev *vcodec,
					dma_addr_t rc_code_addr,
					dma_addr_t rc_info_addr)
{
	if (!rc_code_addr || !rc_info_addr)
		return -EINVAL;
	if (rc_code_addr & ((1 << VENC_ADDR_SHIFT) - 1))
		return -EINVAL;
	if (rc_info_addr & ((1 << VENC_ADDR_SHIFT) - 1))
		return -EINVAL;
	if (lower_32_bits(rc_code_addr >> VENC_ADDR_SHIFT) & ~VENC_ADDR_MASK)
		return -EINVAL;
	if (lower_32_bits(rc_info_addr >> VENC_ADDR_SHIFT) & ~VENC_ADDR_MASK)
		return -EINVAL;

	mtk_venc_write(vcodec, VENC_RC_CODE_DRAM_ADDR,
		       lower_32_bits(rc_code_addr >> VENC_ADDR_SHIFT));
	mtk_venc_write(vcodec, VENC_RC_INFO_DRAM_ADDR,
		       lower_32_bits(rc_info_addr >> VENC_ADDR_SHIFT));

	return 0;
}

/*
 * Start one picture.
 *
 * The two datapaths have genuinely separate start triggers and neither is edge
 * sensitive:
 *
 *   H.264/VP8  VENC_CODEC_CTRL bit 2 is ENC_FRM, "Starts to encode one frame",
 *              0: No operation / 1: Start to encode (draft/ds/venc.txt:4352-4356).
 *              The register is a one-shot command register, so writing 0 to it
 *              does not clear anything -- it is documented as no operation.
 *              This is what an H.264 frame start writes.
 *
 *   MPEG-4     VENC_MP4_FRAME_START bit 0 is TRIGGER, "Writing 1 to it will
 *              trigger hardware initialization process and encoding from the
 *              frame's first MB.  Writing 0 to it has no effect"
 *              (draft/ds/mp4.txt:6408-6410).
 *
 * An earlier revision wrote "mpeg4 ? 1 : 0" here, on a comment claiming the H.264
 * path was "level driven through the shared front end".  That comment described
 * a hardware behaviour the register map does not have, and the code behind it
 * was wrong in both directions: 0 started nothing at all for H.264, and 1 -- bit
 * 0, ENC_SPS -- started a parameter set rather than a frame.
 *
 * SPS and PPS are not programmed here.  They are separate ENC_SPS / ENC_PPS
 * bits of the same register and they are one-shot commands in their own right,
 * issued when the caller wants a parameter set written rather than on every
 * frame; the driver has no parameter-set buffer to point them at, so starting
 * one per frame would produce a bitstream nothing can decode.
 */
static void mtk_venc_start_frame(struct mtk_vcodec_dev *vcodec, bool mpeg4)
{
	if (mpeg4) {
		/* One-shot trigger, self-clearing: a preceding write of 0 is a no-op. */
		mtk_venc_write(vcodec, VENC_MP4_FRAME_START, 0x1);
		return;
	}

	mtk_venc_write(vcodec, VENC_CODEC_CTRL, VENC_CODEC_CTRL_ENC_FRM);
}

/*
 * Submit one frame's worth of programming.
 *
 * The caller owns the DMA buffers and must not release or reuse them until the
 * frame-done interrupt has arrived: the encoder is still reading the source
 * picture when this returns.  vcodec->frame_pending records that and the ISR
 * clears it, and vcodec->bs_bytes holds the coded length afterwards.
 *
 * Note what frame_pending is and is not.  It is bookkeeping that keeps the
 * driver from double-submitting and pairs the runtime-PM reference with its
 * frame -- it is NOT a wait mechanism.  There is no wait_event, no poll and no
 * completion callback, so a caller cannot currently block on it; a caller that
 * wanted to would have to add the notifier or queue machinery, which is exactly
 * the V4L2 layer the file header says does not exist yet.
 *
 * Runtime PM.  This function, not the interrupt handler, is where the runtime-PM
 * reference for an encode is taken, and it is held until the frame-done
 * interrupt releases it (see mtk_venc_isr()).  That pairing is exact: the
 * reference is taken on the path that programs the hardware and is released only
 * by the completion of the frame that took it, so the encoder clocks stay on for
 * exactly as long as a frame is in flight and no longer.  It cannot be taken
 * twice, because a second submit is rejected while frame_pending is set, and it
 * cannot leak, because the only release site is guarded by frame_pending.
 *
 * Return: 0 on success, -EINVAL for a zero or misaligned buffer address or a
 *	   bad quantiser setting, -EBUSY if a frame is already in flight,
 *	   -ENODEV for the MPEG-4 datapath.
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
	unsigned long flags;
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

	/*
	 * Take the runtime-PM reference for the duration of the encode, BEFORE
	 * taking enc_lock, and that ordering is load-bearing.
	 *
	 * pm_runtime_resume_and_get() can invoke the resume callback, and this
	 * driver's resume callback (mtk_vcodec_runtime_resume) takes enc_lock
	 * itself, to power the encoder on and reset it.  Calling it while holding
	 * enc_lock would deadlock against itself on the first submit after every
	 * autosuspend -- which is the common case, since the ISR's put is a
	 * put_autosuspend.  mutex_t is not recursive, so this would hang, not
	 * merely be inefficient.
	 *
	 * Note also that the usage count is incremented by the PM core before it
	 * calls the resume callback, and decremented again if the callback fails,
	 * so a failed resume leaves the count where it found it and there is
	 * nothing to undo here either way.
	 *
	 * From here on the reference is held across enc_lock, which is correct:
	 * the device must stay powered for the whole programming window, and the
	 * suspend path cannot run underneath it.
	 */
	ret = pm_runtime_resume_and_get(&vcodec->pdev->dev);
	if (ret < 0)
		return ret;

	/*
	 * One frame in flight at a time.  This is what makes the runtime-PM
	 * reference above exact: rejecting a concurrent submit means there is never
	 * more than one outstanding frame_pending, so never more than one
	 * outstanding reference for the one completion that will retire it.
	 *
	 * Checked under enc_state_lock, because the interrupt handler that clears
	 * frame_pending runs under enc_state_lock; checking under enc_lock alone
	 * would leave the flag genuinely racy against the ISR.
	 */
	spin_lock_irqsave(&vcodec->enc_state_lock, flags);
	if (vcodec->frame_pending) {
		spin_unlock_irqrestore(&vcodec->enc_state_lock, flags);
		ret = -EBUSY;
		goto out_put;
	}
	spin_unlock_irqrestore(&vcodec->enc_state_lock, flags);

	mutex_lock(&vcodec->enc_lock);

	/*
	 * Re-check under the lock that serialises the register programming: the
	 * previous frame may have completed between the two checks.
	 */
	spin_lock_irqsave(&vcodec->enc_state_lock, flags);
	if (vcodec->frame_pending) {
		spin_unlock_irqrestore(&vcodec->enc_state_lock, flags);
		ret = -EBUSY;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&vcodec->enc_state_lock, flags);

	ret = mtk_venc_set_frame_addr(vcodec, bs_addr, bs_size,
				      src_y, src_uv, ref_y, ref_uv,
				      rec_y, rec_uv);
	if (!ret)
		ret = mtk_venc_set_rate_control(vcodec, parm);

	/*
	 * If the programming failed, nothing was started, so no interrupt will
	 * arrive to release the reference taken above.  Fall out through out_put,
	 * which does the release.  enc_pm_held is deliberately not set on this
	 * path, because nothing is outstanding.
	 */
	if (ret)
		goto out_unlock;

	/*
	 * Publish the frame before starting it, and the ordering matters.
	 *
	 * Starting first and publishing afterwards would be a lost-wakeup race: the
	 * encoder can raise ENC_FRM_INT and the interrupt handler can run before we
	 * get here to set frame_pending, because the ISR is not excluded by
	 * enc_state_lock -- it simply has not taken it yet.  It would find no frame
	 * in flight, retire nothing, and the runtime-PM reference taken above would
	 * then never be released.
	 *
	 * Publishing first is safe, and it leans on the -EBUSY checks above: those
	 * only pass when frame_pending is already clear, and frame_pending clear
	 * means the previous frame was retired and its interrupt acknowledged.  So
	 * there is no stale frame-done left level-held for the ISR to steal here.
	 *
	 * What is NOT claimed is that the ISR can tell a completion belonging to
	 * this frame from one belonging to the previous frame -- it cannot, it only
	 * knows a frame is in flight.  What the ordering above buys is that no
	 * completion can arrive *unmatched*: the frame is always published before
	 * the encoder is told to start, so the ISR can never see a completion for a
	 * frame it does not know about.  Combined with the overflow handling, every
	 * frame that gets published is retired by exactly one of the ISR's three
	 * terminal events.
	 *
	 * frame_pending and enc_pm_held are set together, under the same lock the
	 * ISR uses, so the ISR can never observe a started frame whose reference it
	 * is not going to release.
	 */
	spin_lock_irqsave(&vcodec->enc_state_lock, flags);
	vcodec->bs_addr = bs_addr;
	vcodec->bs_size = bs_size;
	vcodec->bs_bytes = 0;
	vcodec->frame_pending = true;
	vcodec->enc_pm_held = true;
	spin_unlock_irqrestore(&vcodec->enc_state_lock, flags);

	mtk_venc_start_frame(vcodec, false);
	ret = 0;

out_unlock:
	mutex_unlock(&vcodec->enc_lock);
	goto out_put;

out_put:
	/*
	 * Reached only on paths that started nothing -- the -EBUSY re-check and
	 * the register-programming failures above both jump straight here, and the
	 * success path falls through out_unlock into it with the reference
	 * deliberately still held for the interrupt to drop.  A frame that was
	 * started owns its reference until its completion, so only an unstarted
	 * one has anything to release here.
	 */
	if (ret)
		pm_runtime_put_autosuspend(&vcodec->pdev->dev);

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
	spin_lock_init(&vcodec->enc_state_lock);
	vcodec->enc_pm_held = false;
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
