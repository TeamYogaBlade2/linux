/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * MediaTek MT6589 H.264/VP8 video encoder -- hardware front end.
 *
 * This is the VENC half of what used to be one driver covering both directions of
 * the MT6589 video codec.  The split is not cosmetic: VENC and VDEC are separate
 * hardware blocks with separate register windows (data sheet chapter 60 "H.264/VP8
 * Video Encoder" and chapter 61 "MPEG-4 Video Encoder" at 0x17002000, versus
 * chapter 59 "Video Decoder" at 0x16020000), separate GIC lines (MT_VENC_IRQ_ID =
 * SPI 136 against MT_VDEC_IRQ_ID = SPI 140), separate clock controllers (vencsys
 * CLK_VENC_VEN against vdecsys CLK_VDEC0_VDE / CLK_VDEC1_SMI) and separate power
 * domains (VEN against VDE).  The decoder half now lives in mtk-vdec-mt6589.c.
 *
 * The practical reason the split had to happen is recorded in
 * drivers/pmdomain/mediatek/mt6589-power-domains.md: genpd_dev_pm_attach() attaches
 * a domain only when the "power-domains" property has exactly one phandle
 * (drivers/pmdomain/core.c, the count != 1 test).  One node naming both VEN and VDE
 * therefore got NO domain at all, and not even a deferred probe -- probe succeeded
 * against a dark domain.  One node, one domain, one driver.
 *
 * Hardware
 * --------
 * The MT6589 codec is a hardwired RTL accelerator: no coprocessor, no firmware.  It
 * differs from the mainline mtk-vcodec driver (the later VPU/IPI generation which
 * needs an SCP/VPU firmware blob) in exactly that respect.
 *
 * What this driver does
 * --------------------
 *   - probes the register window, the VENC clock and the encoder IRQ line;
 *   - brings the encoder engine out of reset and gates its clock through runtime PM;
 *   - programs the H.264 and MPEG-4 bitstream and frame address registers,
 *     including the reconstructed-frame pair, from DMA addresses it is handed;
 *   - programs the H.264 rate control into the fields the registers really have,
 *     rejecting quantisers outside the hardware's documented 0..51 range;
 *   - acknowledges and dispatches the encoder frame-done interrupt for both
 *     datapaths, reporting the true coded-bitstream length;
 *   - exposes the H.264 datapath as a V4L2 memory-to-memory codec node, owning the
 *     bitstream, reference, reconstruction and rate control scratch buffers that
 *     the address registers need.
 *
 * The V4L2 layer
 * -------------
 * The register programming below is reachable: this driver exposes a v4l2_m2m_dev
 * with an OUTPUT queue (raw NV12 in) and a CAPTURE queue (H.264 bitstream out), so
 * userspace gets a /dev/videoX.  The buffer ownership the "NOT do" list used to
 * claim was missing now exists -- see struct mtk_venc_ctx in mtk-venc-mt6589.h for
 * the reference, reconstruction and rate control scratch planes, and
 * mtk_venc_device_run() for how a job is set up and retired.
 *
 * What this driver does NOT do
 * ----------------------------
 * It does not produce a conformant H.264 bitstream, and no amount of further kernel
 * work will make it do so.  The specific gaps, each a real blocker rather than an
 * oversight:
 *   - Entropy coding, motion estimation and the coefficient buffer.  These exist
 *     only in the vendor's closed userspace libraries (encrypted for H.264/HEVC),
 *     which are stripped ARM blobs: there is no source to port, only the register
 *     map.  What this driver produces is whatever the RTL emits given the addresses
 *     and quantiser it programs.  Userspace must treat the output as opaque.
 *   - The bitrate algorithm.  What is programmed is the quantiser triple, the target
 *     bit rate, the CBR/initial-QP mode bits, the rate control fps and the P/B
 *     frame QP adjust limiters -- all real registers.  The algorithm that acts on
 *     them, and the contents of the RC scratch memory it loads and saves its state
 *     through, are the vendor's.  The scratch buffers ARE allocated, so the two
 *     RC DRAM address registers point at memory the driver owns instead of at
 *     DRAM address integer * 16; that makes the registers valid, not the rate
 *     control correct.
 *   - Slice control.  A frame larger than one MB row has to be driven as several
 *     slices, restarting VENC_MP4_SLICE_START with a moving MBX/MBY stop position.
 *     Nothing does that.
 *   - SPS/PPS.  They are separate one-shot bits of VENC_CODEC_CTRL and need a
 *     parameter-set buffer to point at, which the driver does not have, so no
 *     parameter set is ever emitted.
 *   - MPEG-4 encoding.  The datapath's address programming is written
 *     (mtk_venc_set_mp4_frame_addr()) and its interrupt is handled, but it needs
 *     three distinct chroma planes plus a per-MB side information buffer at
 *     VENC_MP4_SIDE_ADDR that this driver does not allocate, so
 *     mtk_venc_submit_frame() rejects it with -ENODEV.  Only H.264 is exposed.
 *   - VP8.  Restricted deliberately; see mtk_venc_bitstream_size().
 *
 * Encode timeout
 * --------------
 * mtk_venc_device_run() takes a runtime-PM reference for a frame, before it
 * programs any register, and the completion path drops it when the frame ends.
 * Before that reference there was nothing to drop it for: the resume callback runs
 * mtk_venc_reset(), so the image-type and rate-control programming that used to
 * happen first was discarded before the frame was ever started.
 * Every hardware way of ending a frame is handled: the H.264 frame-done interrupt,
 * the MPEG-4 frame-done interrupt, and a bitstream-buffer overflow on either
 * datapath.  What used to be missing is the case where the encoder never signals
 * completion at all -- a wedged engine or a lost interrupt -- which left
 * frame_pending set and the reference held forever, so every later submit returned
 * -EBUSY with venc_clk pinned on.
 * That is now covered rather than merely recorded: mtk_venc_timeout() fires after
 * MTK_VENC_TIMEOUT_MS and retires the job exactly as the interrupt handler would,
 * dropping the runtime-PM reference and completing both vb2 buffers with
 * V4L2_BUF_FLAG_ERROR.  It does not try to make the hardware usable again; there is
 * no documented way to un-wedge a halted encoder, and saying so is better than
 * pretending.
 *
 * Streamoff
 * ---------
 * A STREAMOFF with a frame in flight is the fourth way a job ends, and it is not a
 * hardware event: v4l2_m2m_cancel_job() calls mtk_venc_job_abort() and then
 * blocks until the job is finished, so that function has to retire it rather than
 * leave it running.  It does so through the same completion path the watchdog uses,
 * which is what keeps the runtime-PM reference exactly-once whichever of the two
 * gets there first.  The frame already in the encoder is left to finish or hang --
 * there is no documented way to halt it -- but the buffers go back to userspace
 * immediately, with V4L2_BUF_FLAG_ERROR.
 *
 * Buffer ownership
 * ----------------
 * One convention, followed by every path in this file.  device_run() PEEKS the source
 * and the destination with v4l2_m2m_next_src_buf()/v4l2_m2m_next_dst_buf() and removes
 * neither; v4l2_m2m_buf_done_and_job_finish() removes both and returns them, and
 * reaches v4l2_m2m_job_finish() to clear TRANS_RUNNING.  This is the hantro driver's
 * convention, and the reason to prefer it here is that this driver's completion is
 * asynchronous -- it runs from a workqueue, from the watchdog, or from job_abort() --
 * and so finds its buffers by peeking the ready list again instead of by carrying
 * pointers from the submit.
 *
 * Mixing the two conventions is what the two bugs here used to be: removing the source
 * in device_run() left the completion helper with a NULL source, so it warned, skipped
 * _v4l2_m2m_job_finish(), returned neither buffer and left TRANS_RUNNING set, hanging
 * STREAMOFF and close() in v4l2_m2m_cancel_job()'s wait_event().  Conversely, calling
 * v4l2_m2m_buf_done() on a merely peeked buffer WARNs on vb->state !=
 * VB2_BUF_STATE_ACTIVE and does nothing at all.
 *
 * Two rules follow, and both paths below depend on them.  Every buffer must be retired
 * exactly once, and job_finish must be reached exactly once, on every path: normal
 * completion, an error after the buffers were taken, and a ready list that turns out to
 * be empty (where there is nothing to return but the job still has to be finished, so
 * v4l2_m2m_job_finish() is called on its own).  Buffers that no job ever consumed are
 * returned by stop_streaming(), which the framework reaches after cancel_job().
 *
 * There is no hardware to test against, so none of the above is claimed to
 * work end to end.
 *
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

#include "mtk-venc-mt6589.h"
#include "mtk-vcodec-mt6589-reg.h"

#define MTK_VENC_DRIVER_NAME	"mtk-venc-mt6589"

struct mtk_venc_dev;

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

/*
 * Frame type and reconstruction flush: VENC_ENCODER_INFO_3 at +0x00C.
 *
 * Declared here rather than in mtk-vcodec-mt6589-reg.h, which is not this change's
 * file, and with the same sourcing discipline.  Chapter 60's summary table gives
 * "17002010 VIDEO_ENCODER_INFO_3 32 0x00100000" (draft/ds/venc.txt:3095-3100) and
 * its bit-field section defines:
 *
 *   15      GEN_REC_FRM   "Triggers de-blocking filter to dump reconstructed frame
 *                         for reference" (draft/ds/venc.txt:3192-3193)
 *   9       FRANE_CODING_REORDER, 8 COMVSEARCH_EN, 7:6 VENC_PDIR,
 *   5:4     B_FRM_IDX, 3 IDR_PIC_ID, 2 IS_IDR_FRM
 *   1:0     VENC_IMG_TYPE "Encoded frame type" -- 0: P-frame, 1: B-frame,
 *                         2: I-frame (draft/ds/venc.txt:3296-3302)
 *
 * Both fields are necessary rather than cosmetic.  Without VENC_IMG_TYPE the only
 * defined state is a P-frame, so every frame would be predicted against the
 * reference buffer -- and on the very first frame there is no reference, so no
 * stream could ever start.  Without GEN_REC_FRM the de-blocking filter never dumps
 * the reconstructed frame, so the buffer the next frame predicts from would be
 * whatever was in it before.
 *
 * The reset value 0x00100000 sets only bit 20 (IME_REFINE_MODE), so every field
 * driven here resets to 0 -- a P-frame with no reconstruction flush.  The driver
 * therefore programs both explicitly rather than relying on reset state.
 */
#define VENC_ENCODER_INFO_3			0x00c
#define VENC_IMG_TYPE_P_FRAME			0x0
#define VENC_IMG_TYPE_I_FRAME			2
#define VENC_IMG_TYPE_MASK			GENMASK(1, 0)
#define VENC_GEN_REC_FRM				BIT(15)

/*
 * Bounded completion watchdog, in milliseconds.
 *
 * How long to wait for a frame before declaring the engine wedged.  The slowest
 * thing this encoder is specified to do is multi-frame processing at 2592x1592@5fps
 * (draft/ds/venc.txt:1777-1782), i.e. 200ms per frame at a larger size than the
 * 720p30 single-frame rate.  One second is five times that, generous enough not to
 * fire on a merely slow encode while still bounding the damage from a genuinely hung
 * one.
 *
 * It is a bound, not a prediction: the point is that the job and the runtime-PM
 * reference are always released eventually, not that this value predicts when.
 */
#define MTK_VENC_TIMEOUT_MS			1000

/*
 * Per-probe encoder state.  This is now the whole of the old merged driver's
 * private state: the decoder half never shared any of it except the platform
 * device, because VENC and VDEC are separate blocks that probe separately.
 */
struct mtk_venc_dev {
	void __iomem *regs;

	struct clk *venc_clk;

	/*
	 * The node's own mutex, used as vfd->lock so format negotiation and
	 * open/teardown are serialised on this driver.
	 */
	struct mutex lock;

	/* Guards the power transition; this is what runtime PM takes. */
	struct mutex enc_lock;

	/*
	 * Open encoder file handles.  Incremented by mtk_venc_open() and
	 * decremented by mtk_venc_release(), so the "enc_users == 0" tests in the
	 * suspend/resume paths are no longer unconditionally true.
	 *
	 * It does NOT hold off suspend for a frame in flight: what keeps the encoder
	 * clocks on across a frame is the runtime-PM reference the submit path takes
	 * and whichever of the interrupt handler or mtk_venc_timeout() drops it, which
	 * is independent of this counter.  What this counter does is stop a system
	 * suspend from tearing the registers down underneath an open but idle stream.
	 */
	atomic_t enc_users;

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
	 * The device this driver instance belongs to.  Assigned in probe() before
	 * the runtime-PM and IRQ paths can run.
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

	unsigned long venc_irq_count;
	unsigned long mp4_irq_count;
	bool suspended;

	/*
	 * The V4L2 mem-to-mem node, with its own v4l2_device, m2m_dev and
	 * video_device.  The decoder has its own node in its own driver; the two are
	 * separate character devices with separate open() lifecycles and separate
	 * format negotiation.
	 */
	struct video_device		vfd;
	struct v4l2_m2m_dev		*m2m_dev;
	struct v4l2_device		v4l2_dev;

	/*
	 * Whether the frame that just retired failed, recorded separately from bs_bytes
	 * rather than inferred from it.  An overflow and the watchdog both retire with a
	 * length of zero, but so does a genuinely empty frame, and those are three
	 * different things.
	 */
	bool				bs_error;

	/*
	 * Bounded completion watchdog.
	 *
	 * The frame-done interrupt, the MPEG-4 frame-done interrupt and the
	 * bitstream-overflow interrupt all retire a frame.  What none of them can do is
	 * retire a frame for an engine that never signals at all.  One delayed_work is
	 * shared by all instances, armed by device_run() when it publishes a job and
	 * disarmed by whichever of the interrupt path or the timeout retires that job
	 * first.
	 *
	 * One work item, not one per context, because the encoder accepts exactly one
	 * frame at a time: mtk_venc_submit_frame() returns -EBUSY for a second concurrent
	 * submit, so there is never more than one job to time out.  It carries the context
	 * pointer of the job it is timing out, so the timeout completes the right
	 * instance's buffers.
	 */
	struct delayed_work		venc_timeout_work;
	struct mtk_venc_ctx		*timeout_ctx;

	/*
	 * Deferred completion.  The interrupt handler cannot return vb2 buffers to the
	 * framework itself -- v4l2_m2m_buf_done_and_job_finish() takes a spinlock and can
	 * schedule the next job, neither legal in hard IRQ context -- so it hands the job
	 * here.
	 */
	struct work_struct		venc_complete_work;

	/* Counters so a wedged or overflowing encoder is visible rather than silent. */
	unsigned long			timeout_count;
	unsigned long			overflow_count;

	/*
	 * Whether v4l2_m2m_register_media_controller() succeeded.  Tracked rather
	 * than assumed because it is a no-op returning 0 when there is no media
	 * controller, and its teardown must match its setup exactly -- unregistering a
	 * graph that was never built would corrupt the media device.
	 */
	bool				mc_registered;
};

/*
 * Encoder parameters.  Everything here is either a property of the picture or a
 * rate control term; the buffer addresses are NOT here, because the caller
 * supplies them separately and they are DRAM addresses in the chapter-60
 * register table.
 */
struct mtk_venc_parm {
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

static inline void mtk_venc_write(struct mtk_venc_dev *venc, u32 off, u32 val)
{
	writel(val, venc->regs + off);
}

static inline u32 mtk_venc_read(struct mtk_venc_dev *venc, u32 off)
{
	return readl(venc->regs + off);
}
/* ------------------------------------------------------------------ */
/* Encoder							      */
/* ------------------------------------------------------------------ */

/*
 * Bring the H.264/VP8 front end to a known state.  VENC_SW_HRST_N is an active
 * low software reset for the encoder, so clear it, wait for the engine to leave
 * reset by reading the hardware-mode register, then release it.
 */
static void mtk_venc_reset(struct mtk_venc_dev *venc)
{
	mtk_venc_write(venc, VENC_SW_HRST_N, 0);

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
	mtk_venc_write(venc, VENC_IRQ_ACK, VENC_IRQ_MASK_ALL);
	mtk_venc_write(venc, VENC_SW_HRST_N, 1);

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
	mtk_venc_read(venc, VENC_HW_MODE_SEL);
}

static int mtk_venc_power_on(struct mtk_venc_dev *venc)
{
	int ret = clk_prepare_enable(venc->venc_clk);

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
	mtk_venc_write(venc, VENC_CE, 0x1);

	/*
	 * Enable both interrupt sources.  The shared pair at +0x05c/+0x060 is
	 * acknowledged implicitly by the hardware when IRQs are disabled, while
	 * the MPEG-4 pair at +0x678/+0x67c is level held and must be masked off
	 * explicitly or the interrupt line never drops.
	 */
	mtk_venc_write(venc, VENC_IRQ_ACK, VENC_IRQ_MASK_ALL);
	mtk_venc_write(venc, VENC_MP4_IRQ_EN,
		       VENC_MP4_IRQ_EN_DONE | VENC_MP4_IRQ_EN_FULL);
	mtk_venc_write(venc, VENC_MP4_IRQ_ACK,
		       VENC_MP4_IRQ_ACK_DONE | VENC_MP4_IRQ_ACK_FULL);

	return 0;
}

static void mtk_venc_power_off(struct mtk_venc_dev *venc)
{
	mtk_venc_write(venc, VENC_MP4_IRQ_EN, 0x0);
	mtk_venc_write(venc, VENC_MP4_IRQ_ACK,
		       VENC_MP4_IRQ_ACK_DONE | VENC_MP4_IRQ_ACK_FULL);
	clk_disable_unprepare(venc->venc_clk);
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
static u32 mtk_venc_bitstream_size(struct mtk_venc_dev *venc, bool mpeg4)
{
	if (!mpeg4)
		return mtk_venc_read(venc, VENC_PIC_BITSTREAM_BYTE_CNT) &
		       GENMASK(23, 0);

	return mtk_venc_read(venc, VENC_MP4_BYTE_COUNT) & GENMASK(23, 0);
}

/*
 * Complete one encoder job.
 *
 * Called from exactly two places, and that is the whole point of factoring it out:
 * the frame-done/overflow path in mtk_venc_isr() (via mtk_venc_complete_work) and
 * the watchdog in mtk_venc_timeout().  Whichever arrives first retires the job; the
 * other finds ctx->pending already clear and returns without touching anything.  That
 * is what makes the runtime-PM reference exactly-once: it is taken once per started
 * frame and dropped once by whichever of these two paths wins, and the flag saying so
 * is set and cleared under the same spinlock.
 *
 * @from_timeout distinguishes them for one reason: the watchdog cancellation below.
 * cancel_delayed_work_sync() waits for the work item to finish, so calling it from
 * inside the work item itself is a synchronous wait on one's own completion -- a
 * self-deadlock, not a slow path.  The watchdog therefore passes true and is left to
 * run to the end of this function; the interrupt path passes false and cancels.
 *
 * @state is VB2_BUF_STATE_DONE for a good frame and VB2_BUF_STATE_ERROR for one the
 * hardware could not produce (bitstream overflow, or the watchdog firing).  ERROR
 * reaches userspace as V4L2_BUF_FLAG_ERROR on DQBUF, which is the only way a caller
 * can tell a zero-length frame from a failed one.
 *
 * Process context only.  v4l2_m2m_buf_done_and_job_finish() takes
 * m2m_dev->job_spinlock and can schedule the next job, neither legal in hard IRQ
 * context, which is why mtk_venc_isr() hands off to a work item rather than calling
 * this directly.
 */
static void mtk_venc_complete_job(struct mtk_venc_dev *venc,
				  struct mtk_venc_ctx *ctx, u32 bs_bytes,
				  enum vb2_buffer_state state, bool from_timeout)
{
	struct vb2_v4l2_buffer *dst;
	unsigned long flags;
	bool retire_pm = false;

	/*
	 * Claim the job.  Anything that finds it already claimed -- a spurious
	 * interrupt, the watchdog firing microseconds after a perfectly good frame
	 * done, or a second overflow bit in the same interrupt -- returns without
	 * touching the buffers or the runtime-PM usage count.
	 */
	spin_lock_irqsave(&venc->enc_state_lock, flags);
	if (!ctx->pending) {
		spin_unlock_irqrestore(&venc->enc_state_lock, flags);
		return;
	}
	ctx->pending = false;
	venc->frame_pending = false;
	venc->bs_addr = 0;
	venc->bs_size = 0;
	if (venc->enc_pm_held) {
		venc->enc_pm_held = false;
		retire_pm = true;
	}
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	/*
	 * Stop the watchdog before the job is released -- unless this call IS the
	 * watchdog.  cancel_delayed_work_sync() would then be waiting for this very
	 * work item to reach its own return, so it would never return, and every frame
	 * that timed out would hang the encoder instead of failing it.
	 *
	 * The watchdog needs no cancelling from inside itself: a delayed_work does not
	 * re-arm itself, so it cannot fire again until something re-arms it, and the only
	 * re-arm site (mtk_venc_timeout_arm()) is reached only after a frame has been
	 * started -- which cannot happen until this function has handed the buffers back.
	 *
	 * cancel_delayed_work_sync() rather than the _noflush variant because the timeout
	 * work takes enc_state_lock and calls this very function: it must be certain the
	 * watchdog is not running underneath us as we hand buffers back, or the two
	 * could race over the same vb2 buffer.  Safe to call unconditionally -- if this
	 * is the interrupt winning the race, the work is either not armed or is about to
	 * find pending clear and return.
	 */
	if (!from_timeout)
		cancel_delayed_work_sync(&venc->venc_timeout_work);

	/*
	 * Publish the coded length before the buffer is returned.  bs_bytes is the count
	 * the hardware itself reported, so it is the true bytesused -- but only for a
	 * good frame.  A failed frame retires with a length of zero: an overflow would
	 * otherwise publish the bytes emitted before the encoder gave up, which is a
	 * truncated, unparseable frame, and reporting that as bytesused is worse than
	 * reporting nothing.
	 */
	dst = v4l2_m2m_next_dst_buf(ctx->m2m);
	if (dst) {
		dst->planes[0].bytesused =
			state == VB2_BUF_STATE_DONE ? (size_t)bs_bytes : 0;
		if (state == VB2_BUF_STATE_ERROR)
			dst->flags |= V4L2_BUF_FLAG_ERROR;

		/*
		 * next_dst_buf() is a PEEK, not a remove, and that is what
		 * v4l2_m2m_buf_done_and_job_finish() needs to find: device_run() left both
		 * buffers on their ready lists, and it removes both itself -- the source
		 * unconditionally, the destination under its !is_held test -- before
		 * WARN_ON(!src_buf || !dst_buf).
		 *
		 * The reason device_run() leaves them there is this function.  It is reached
		 * asynchronously -- from the deferred interrupt work, from the watchdog, or
		 * from job_abort() on STREAMOFF -- and it recovers the buffers by peeking the
		 * ready lists again rather than by carrying pointers from the submit.  So the
		 * source must still be on its ready list here, and removing it in device_run()
		 * (which an earlier revision did) left this helper with a NULL source: it
		 * warned, skipped _v4l2_m2m_job_finish() entirely, returned neither buffer,
		 * and never cleared TRANS_RUNNING, which left job_abort() blocked forever in
		 * v4l2_m2m_cancel_job()'s wait_event() and so hung STREAMOFF and close().
		 */
		v4l2_m2m_buf_done_and_job_finish(venc->m2m_dev, ctx->m2m,
						 state);
	} else {
		/*
		 * No capture buffer to return.  The buffers are already gone -- this
		 * context's ready list was emptied under us, which STREAMOFF does before
		 * vb2 reclaims them -- so there is nothing left to hand back, but the job
		 * still has to be finished.  Skipping it would leave TRANS_RUNNING set, and
		 * v4l2_m2m_cancel_job()'s wait_event() would never be woken: the same hang as
		 * above, reached from the other direction.
		 *
		 * v4l2_m2m_job_finish() rather than buf_done_and_job_finish(), because there is
		 * nothing for the latter to return and it would only WARN before doing the
		 * same job_finish.
		 */
		v4l2_m2m_job_finish(venc->m2m_dev, ctx->m2m);
	}

	/*
	 * Drop the reference the submit path took.  This happens on every path, including
	 * the one where the ready list was empty and no buffer could be found to return,
	 * so a job can never release the hardware with the usage count still held.
	 */
	if (retire_pm)
		pm_runtime_put_autosuspend(&venc->pdev->dev);
}

/*
 * Deferred completion, run from the workqueue rather than in the ISR.
 *
 * The ISR decides that a frame ended; this does what has to happen afterwards.  It
 * exists only because v4l2_m2m_buf_done_and_job_finish() takes a spinlock and can
 * schedule the next job, and neither is legal in hard IRQ context.
 *
 * schedule_work() from an ISR is safe -- the workqueue handles reentrancy -- and
 * the work re-reads venc->timeout_ctx rather than carrying a captured context, so
 * two interrupts firing back to back run the work once and the claim inside it makes
 * any second run a no-op.
 */
static void mtk_venc_complete_work(struct work_struct *w)
{
	struct mtk_venc_dev *venc =
		container_of(w, struct mtk_venc_dev, venc_complete_work);
	struct mtk_venc_ctx *ctx;
	u32 bs_bytes;
	unsigned long flags;
	bool failed;

	spin_lock_irqsave(&venc->enc_state_lock, flags);
	ctx = venc->timeout_ctx;
	bs_bytes = venc->bs_bytes;
	/*
	 * Whether the frame failed is a recorded fact, not something inferred from the
	 * length.  Inferring it from "bs_bytes == 0" would report a genuinely empty frame
	 * as an error, which is a different thing and is not what happened.
	 */
	failed = venc->bs_error;
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	if (!ctx)
		return;

	mtk_venc_complete_job(venc, ctx, bs_bytes,
			       failed ? VB2_BUF_STATE_ERROR :
					VB2_BUF_STATE_DONE,
			       false);
}

/*
 * Bounded completion watchdog.
 *
 * Armed by device_run() once a frame has been started, and fired only if the engine
 * has not signalled completion within MTK_VENC_TIMEOUT_MS.
 *
 * What this fixes is a genuine liveness bug rather than an optimisation.  Without it,
 * an encoder that never raised its frame-done interrupt left frame_pending set and
 * the runtime-PM reference held, so every later submit returned -EBUSY and venc_clk
 * stayed on until reboot, with nothing in dmesg to say why.  That gap is recorded in
 * the file header.
 *
 * It does not try to make the hardware usable again -- there is no documented way to
 * un-wedge a halted encoder, and pretending otherwise would be worse than saying so.
 * It bounds the damage instead: the job is completed as failed, the usage count is
 * dropped, and the next STREAMON gets a fresh attempt.
 *
 * Runs in process context from the workqueue, which is what lets it call the same
 * mtk_venc_complete_job() the interrupt path uses.  The claim is the same one, so a
 * frame that completes normally just before the watchdog fires is not reported twice.
 */
static void mtk_venc_timeout(struct work_struct *w)
{
	struct mtk_venc_dev *venc =
		container_of(to_delayed_work(w), struct mtk_venc_dev,
			     venc_timeout_work);
	struct mtk_venc_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&venc->enc_state_lock, flags);
	ctx = venc->timeout_ctx;
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	if (!ctx)
		return;

	venc->timeout_count++;
	dev_warn_ratelimited(&venc->pdev->dev,
			     "encoder did not signal completion within %u ms; failing the frame and releasing the device\n",
			     MTK_VENC_TIMEOUT_MS);

	mtk_venc_complete_job(venc, ctx, 0, VB2_BUF_STATE_ERROR, true);
}

/*
 * Arm the bounded completion watchdog for the job that has just been started.
 */
static void mtk_venc_timeout_arm(struct mtk_venc_dev *venc,
				 struct mtk_venc_ctx *ctx)
{
	schedule_delayed_work(&venc->venc_timeout_work,
			      msecs_to_jiffies(MTK_VENC_TIMEOUT_MS));
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
	struct mtk_venc_dev *venc = dev_get_drvdata(&pdev->dev);
	struct mtk_venc_ctx *ctx;
	unsigned long flags;
	u32 status, mp4_status, bs_bytes = 0;
	bool frm_done = false, mp4_done = false, overflow = false;
	bool frame_failed = false;

	status = mtk_venc_read(venc, VENC_IRQ_STATUS);
	if (status & VENC_IRQ_MASK_ALL) {
		venc->venc_irq_count++;
		mtk_venc_write(venc, VENC_IRQ_ACK, status & VENC_IRQ_MASK_ALL);

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
	mp4_status = mtk_venc_read(venc, VENC_MP4_IRQ_STATUS);
	if (mp4_status & VENC_MP4_IRQ_STATUS_FULL) {
		venc->mp4_irq_count++;
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
		mtk_venc_write(venc, VENC_MP4_IRQ_ACK, VENC_MP4_IRQ_ACK_FULL);
	}

	if (mp4_status & VENC_MP4_IRQ_STATUS_FRAME) {
		venc->mp4_irq_count++;
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
		mtk_venc_write(venc, VENC_MP4_IRQ_ACK, VENC_MP4_IRQ_ACK_DONE);
	} else if (mp4_status & VENC_MP4_IRQ_STATUS_SLICE) {
		/*
		 * A slice boundary is an intermediate event: ack it so the level
		 * drops, but do not treat the frame as finished.
		 */
		mtk_venc_write(venc, VENC_MP4_IRQ_ACK, VENC_MP4_IRQ_ACK_DONE);
	}

	/* Catch anything raised while we were acknowledging. */
	status = mtk_venc_read(venc, VENC_IRQ_STATUS);
	if (status & VENC_IRQ_MASK_ALL)
		mtk_venc_write(venc, VENC_IRQ_ACK, status & VENC_IRQ_MASK_ALL);

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
	* The bookkeeping that used to live here -- clearing frame_pending, enc_pm_held
	* and the bs_* fields -- is now mtk_venc_complete_job()'s, so that the watchdog
	* and the interrupt share one claim on the job and the runtime-PM reference can
	* be released exactly once.  All this function does now is decide *whether* a frame
	* ended and hand that to the completion path.
	*
	* The fourth retiring event -- the hardware never signalling completion at all, a
	* wedged encoder or a lost interrupt -- is mtk_venc_timeout(), so the reference can
	* no longer be stranded either.  That is the gap the file header used to record as
	* deliberate.
	*/
	frame_failed = overflow && !frm_done && !mp4_done;

	if (frm_done)
		bs_bytes = mtk_venc_bitstream_size(venc, false);
	else if (mp4_done)
		bs_bytes = mtk_venc_bitstream_size(venc, true);
	else if (frame_failed)
		bs_bytes = 0;

	if (frm_done || mp4_done || frame_failed) {
		if (frame_failed)
			venc->overflow_count++;

		/*
		 * Publish the outcome, then defer to process context.
		 *
		 * The result is recorded here, under the lock the completion path reads it
		 * under, because only the ISR knows the coded length: it is a register read
		 * taken outside the lock (this driver does not hold a spinlock across
		 * register accesses) and has to be handed over.
		 *
		 * v4l2_m2m_buf_done_and_job_finish() takes m2m_dev->job_spinlock and can
		 * schedule the next job from inside, neither of which may happen in hard
		 * IRQ context, so the buffer bookkeeping cannot be done here.
		 *
		 * The claim itself -- the test-and-clear of ctx->pending -- is deliberately
		 * NOT done here.  Leaving it in the completion path means the interrupt and
		 * the watchdog contend for exactly one claim and the loser returns without
		 * touching anything, which is the whole reason there is a claim at all.
		 */
		spin_lock_irqsave(&venc->enc_state_lock, flags);
		venc->bs_bytes = bs_bytes;
		venc->bs_error = frame_failed;
		ctx = venc->timeout_ctx;
		spin_unlock_irqrestore(&venc->enc_state_lock, flags);

		if (ctx && ctx->pending)
			schedule_work(&venc->venc_complete_work);
	}

	return IRQ_HANDLED;
}


/* ------------------------------------------------------------------ */
/* Encoder programming, from the register map			      */
/* ------------------------------------------------------------------ */

/**
 * mtk_venc_set_frame_addr - program the H.264/VP8 frame and bitstream buffers.
 * @venc: driver instance
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
 * Reached from mtk_venc_device_run() via mtk_venc_submit_frame(); see that
 * function for how the source, bitstream, reference and reconstruction planes are
 * resolved from the vb2 buffers and the driver-owned REF/REC planes.
 */
static int mtk_venc_set_frame_addr(struct mtk_venc_dev *venc,
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

	mtk_venc_write(venc, VENC_BITSTREAM_BUF_ADDR,
		       lower_32_bits(bs_addr >> VENC_ADDR_SHIFT));
	mtk_venc_write(venc, VENC_BITSTREAM_BUF_SIZE,
		       lower_32_bits(bs_size >> VENC_BS_SIZE_SHIFT));

	mtk_venc_write(venc, VENC_FRM_CUR_Y_ADDR,
		       lower_32_bits(src_y >> VENC_ADDR_SHIFT));
	mtk_venc_write(venc, VENC_FRM_CUR_UV_ADDR,
		       lower_32_bits(src_uv >> VENC_ADDR_SHIFT));
	mtk_venc_write(venc, VENC_FRM_REF_Y_ADDR,
		       lower_32_bits(ref_y >> VENC_ADDR_SHIFT));
	mtk_venc_write(venc, VENC_FRM_REF_UV_ADDR,
		       lower_32_bits(ref_uv >> VENC_ADDR_SHIFT));
	mtk_venc_write(venc, VENC_FRM_REC_Y_ADDR,
		       lower_32_bits(rec_y >> VENC_ADDR_SHIFT));
	mtk_venc_write(venc, VENC_FRM_REC_UV_ADDR,
		       lower_32_bits(rec_uv >> VENC_ADDR_SHIFT));

	return 0;
}

/**
 * mtk_venc_set_mp4_frame_addr - program the MPEG-4 datapath buffers.
 * @venc: driver instance
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
 * Still no caller: the MPEG-4 datapath is not exposed as a format, because
 * mtk_venc_submit_frame() rejects it with -ENODEV until the per-MB side information
 * buffer at VENC_MP4_SIDE_ADDR is owned.  See the file header.
 */
__maybe_unused
static int mtk_venc_set_mp4_frame_addr(struct mtk_venc_dev *venc,
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

	mtk_venc_write(venc, VENC_MP4_SRCADR_Y, lower_32_bits(src_y));
	mtk_venc_write(venc, VENC_MP4_SRCADR_CB, lower_32_bits(src_cb));
	mtk_venc_write(venc, VENC_MP4_SRCADR_CR, lower_32_bits(src_cr));
	mtk_venc_write(venc, VENC_MP4_BITADR, lower_32_bits(bs_addr));
	mtk_venc_write(venc, VENC_MP4_RECADR_Y, lower_32_bits(rec_y));
	mtk_venc_write(venc, VENC_MP4_RECADR_CB, lower_32_bits(rec_cb));
	mtk_venc_write(venc, VENC_MP4_RECADR_CR, lower_32_bits(rec_cr));
	mtk_venc_write(venc, VENC_MP4_REFADR_Y, lower_32_bits(ref_y));
	mtk_venc_write(venc, VENC_MP4_REFADR_CB, lower_32_bits(ref_cb));
	mtk_venc_write(venc, VENC_MP4_REFADR_CR, lower_32_bits(ref_cr));

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
static int mtk_venc_set_rate_control(struct mtk_venc_dev *venc,
				     const struct mtk_venc_parm *p)
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
	enc_info_0 = mtk_venc_read(venc, VENC_ENCODER_INFO_0);
	enc_info_0 &= ~VENC_QP_I_FRM_MASK;
	enc_info_0 |= (p->qp_init << VENC_QP_I_FRM_SHIFT) & VENC_QP_I_FRM_MASK;

	enc_info_1 = mtk_venc_read(venc, VENC_ENCODER_INFO_1);
	enc_info_1 &= ~VENC_QP_P_FRM_MASK;
	enc_info_1 |= (p->qp_init << VENC_QP_P_FRM_SHIFT) & VENC_QP_P_FRM_MASK;

	mtk_venc_write(venc, VENC_RATECONTROL_INFO_0, rc_info_0);
	mtk_venc_write(venc, VENC_RATECONTROL_INFO_1, rc_info_1);
	mtk_venc_write(venc, VENC_ENCODER_INFO_0, enc_info_0);
	mtk_venc_write(venc, VENC_ENCODER_INFO_1, enc_info_1);

	return 0;
}

/**
 * mtk_venc_set_rc_scratch_addr - validate the rate control scratch buffers.
 * @venc: driver instance
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
 * Reached from mtk_venc_device_run(), which points the two registers at the
 * rate control scratch buffers the driver allocates.  See that function for why
 * having them allocated matters even though the algorithm behind them is closed.
 */
static int mtk_venc_set_rc_scratch_addr(struct mtk_venc_dev *venc,
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

	mtk_venc_write(venc, VENC_RC_CODE_DRAM_ADDR,
		       lower_32_bits(rc_code_addr >> VENC_ADDR_SHIFT));
	mtk_venc_write(venc, VENC_RC_INFO_DRAM_ADDR,
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
static void mtk_venc_start_frame(struct mtk_venc_dev *venc, bool mpeg4)
{
	if (mpeg4) {
		/* One-shot trigger, self-clearing: a preceding write of 0 is a no-op. */
		mtk_venc_write(venc, VENC_MP4_FRAME_START, 0x1);
		return;
	}

	mtk_venc_write(venc, VENC_CODEC_CTRL, VENC_CODEC_CTRL_ENC_FRM);
}

/*
 * Submit one frame's worth of programming.
 *
 * The caller owns the DMA buffers and must not release or reuse them until the
 * frame-done interrupt has arrived: the encoder is still reading the source
 * picture when this returns.  venc->frame_pending records that and the ISR
 * clears it, and venc->bs_bytes holds the coded length afterwards.
 *
 * Note what frame_pending is and is not.  It is bookkeeping that keeps the
 * driver from double-submitting and pairs the runtime-PM reference with its
 * frame -- it is NOT a wait mechanism.  There is no wait_event, no poll and no
 * completion callback, so a caller cannot currently block on it; a caller that
 * wanted to would have to add the notifier or queue machinery, which is exactly
 * the V4L2 layer the file header says does not exist yet.
 *
 * Runtime PM.  The reference for an encode is NOT taken here any more: the caller
 * takes it before programming anything (see mtk_venc_device_run()), because
 * pm_runtime_resume_and_get() can run the resume callback, which resets the very
 * registers this function writes, and programming them first meant the reset threw
 * the writes away.  This function therefore assumes the caller already holds one and
 * returns the encoder to exactly the state it found if it fails to start anything --
 * mtk_venc_device_run() owns the matching put.
 *
 * Return: 0 on success, -EINVAL for a zero or misaligned buffer address or a
 *	   bad quantiser setting, -EBUSY if a frame is already in flight,
 *	   -ENODEV for the MPEG-4 datapath.
 */
static int mtk_venc_submit_frame(struct mtk_venc_dev *venc, bool mpeg4,
				 const struct mtk_venc_parm *parm,
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
	 * One frame in flight at a time.  This is what keeps the runtime-PM reference
	 * the caller took exact: rejecting a concurrent submit means there is never
	 * more than one outstanding frame_pending, so never more than one
	 * outstanding reference for the one completion that will retire it.
	 *
	 * Checked under enc_state_lock, because the interrupt handler that clears
	 * frame_pending runs under enc_state_lock; checking under enc_lock alone
	 * would leave the flag genuinely racy against the ISR.
	 */
	spin_lock_irqsave(&venc->enc_state_lock, flags);
	if (venc->frame_pending) {
		spin_unlock_irqrestore(&venc->enc_state_lock, flags);
		ret = -EBUSY;
		goto out_unlock_nolock;
	}
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	mutex_lock(&venc->enc_lock);

	/*
	 * Re-check under the lock that serialises the register programming: the
	 * previous frame may have completed between the two checks.
	 */
	spin_lock_irqsave(&venc->enc_state_lock, flags);
	if (venc->frame_pending) {
		spin_unlock_irqrestore(&venc->enc_state_lock, flags);
		ret = -EBUSY;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	ret = mtk_venc_set_frame_addr(venc, bs_addr, bs_size,
				      src_y, src_uv, ref_y, ref_uv,
				      rec_y, rec_uv);
	if (!ret)
		ret = mtk_venc_set_rate_control(venc, parm);

	/*
	 * If the programming failed, nothing was started, so no interrupt will arrive
	 * to release the reference the caller took.  enc_pm_held is deliberately not set
	 * on this path, because nothing is outstanding: the caller sees a non-zero
	 * return and drops the reference itself.
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
	 * in flight, retire nothing, and the runtime-PM reference the caller took would
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
	spin_lock_irqsave(&venc->enc_state_lock, flags);
	venc->bs_addr = bs_addr;
	venc->bs_size = bs_size;
	venc->bs_bytes = 0;
	venc->frame_pending = true;
	venc->enc_pm_held = true;
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	mtk_venc_start_frame(venc, false);
	ret = 0;

out_unlock:
	mutex_unlock(&venc->enc_lock);
	return ret;

out_unlock_nolock:
	return ret;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */

/*
 * The driver-private context for a file handle.
 *
 * file2m2m() reaches the m2m_dev, but the negotiated formats, the buffer queues
 * and the driver-owned frame buffers all hang off the per-instance context, so the
 * two are tied together in one accessor rather than every ioctl carrying both.
 *
 * file->private_data is the struct v4l2_fh, because that is what the V4L2 core
 * and v4l2_m2m_fop_poll()/v4l2_m2m_fop_mmap() expect it to be -- an earlier
 * revision pointed it straight at this context, which those two dereference as a
 * v4l2_fh and read a m2m context out of.  The v4l2_fh is the first member of the
 * context, so recovering one from the other is an offset, not a lookup.
 */
#define file2ctx(f)							\
	container_of(file_to_v4l2_fh(f), struct mtk_venc_ctx, fh)

/*
 * This tree has no file2m2m(); the m2m_dev pointer lives in the driver instance,
 * so reach it through the same video_device the framework does.
 */
#define file2m2m(f) video_drvdata(f)

/* ------------------------------------------------------------------ */
/* Controls							      */
/* ------------------------------------------------------------------ */

/*
 * Encoder controls.
 *
 * Bitrate and QP are the two the hardware genuinely has registers for:
 * RC_TARGET_BIT_RATE is 17 bits wide and the QP fields take 0..51 for H.264
 * (draft/ds/venc.txt:4072-4087 and :2802-2804).  FPS, CBR and GOP round out what
 * mtk_venc_parm carries.
 *
 * The ranges below are the register field widths and documented ranges, not round
 * numbers chosen for convenience, so a value the hardware cannot hold is refused at
 * S_EXT_CTRLS rather than truncated into something else at submit time.
 */
enum {
	MTK_VENC_CTRL_FIRST = V4L2_CID_USER_BASE,
	MTK_VENC_CTRL_BITRATE,
	MTK_VENC_CTRL_QP,
	MTK_VENC_CTRL_FPS,
	MTK_VENC_CTRL_CBR,
	MTK_VENC_CTRL_GOP,
	MTK_VENC_CTRL_LAST,
};

/* RC_TARGET_BIT_RATE is bits 16:0 (draft/ds/venc.txt:4078-4087). */
#define MTK_VENC_CTRL_BITRATE_MIN	1
#define MTK_VENC_CTRL_BITRATE_MAX	131071
#define MTK_VENC_CTRL_BITRATE_DEF	131071
/* The H.264 QP range is [0, 51], stated for all three QP fields (draft/ds/venc.txt:2803). */
#define MTK_VENC_CTRL_QP_MIN		0
#define MTK_VENC_CTRL_QP_MAX		51
#define MTK_VENC_CTRL_QP_DEF		26
/* RC_FPS is bits 23:16 of RATECONTROL_INFO_1, so 8 bits wide. */
#define MTK_VENC_CTRL_FPS_MIN		1
#define MTK_VENC_CTRL_FPS_MAX		255
#define MTK_VENC_CTRL_FPS_DEF		30
#define MTK_VENC_CTRL_CBR_MIN		0
#define MTK_VENC_CTRL_CBR_MAX		1
#define MTK_VENC_CTRL_CBR_DEF		0
#define MTK_VENC_CTRL_GOP_MIN		1
#define MTK_VENC_CTRL_GOP_MAX		255
#define MTK_VENC_CTRL_GOP_DEF		1

/*
 * Create the encoder's controls on this instance's handler.
 *
 * v4l2_ctrl_new_std() in this tree takes (hdl, ops, id, min, max, step, def) with no
 * explicit type argument; the type is inferred from the min/max/def triple, so a
 * non-zero default with min 0 gives a boolean and a range with min >= 1 gives an
 * integer.  That inference is exactly what CBR_MIN == 0 relies on.
 *
 * Return: 0 on success, or a negative errno with the handler left usable (the caller
 *	   frees it).
 */
static int mtk_venc_ctrl_init(struct mtk_venc_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->ctrl_hdl;

	v4l2_ctrl_handler_init(hdl, MTK_VENC_CTRL_LAST - MTK_VENC_CTRL_FIRST);
	hdl->lock = &ctx->dev->lock;
	if (hdl->error)
		return hdl->error;

	ctx->ctrl_bitrate = v4l2_ctrl_new_std(hdl, NULL,
					     MTK_VENC_CTRL_BITRATE,
					     MTK_VENC_CTRL_BITRATE_MIN,
					     MTK_VENC_CTRL_BITRATE_MAX, 1,
					     MTK_VENC_CTRL_BITRATE_DEF);
	ctx->ctrl_qp = v4l2_ctrl_new_std(hdl, NULL, MTK_VENC_CTRL_QP,
					MTK_VENC_CTRL_QP_MIN,
					MTK_VENC_CTRL_QP_MAX, 1,
					MTK_VENC_CTRL_QP_DEF);
	ctx->ctrl_fps = v4l2_ctrl_new_std(hdl, NULL, MTK_VENC_CTRL_FPS,
					MTK_VENC_CTRL_FPS_MIN,
					MTK_VENC_CTRL_FPS_MAX, 1,
					MTK_VENC_CTRL_FPS_DEF);
	ctx->ctrl_cbr = v4l2_ctrl_new_std(hdl, NULL, MTK_VENC_CTRL_CBR,
					MTK_VENC_CTRL_CBR_MIN,
					MTK_VENC_CTRL_CBR_MAX, 1,
					MTK_VENC_CTRL_CBR_DEF);
	ctx->ctrl_gop = v4l2_ctrl_new_std(hdl, NULL, MTK_VENC_CTRL_GOP,
					MTK_VENC_CTRL_GOP_MIN,
					MTK_VENC_CTRL_GOP_MAX, 1,
					MTK_VENC_CTRL_GOP_DEF);

	/*
	 * A NULL here means v4l2_ctrl_new_std() rejected the definition, which is a bug
	 * in the list above rather than anything a user can cause.  The per-frame submit
	 * path dereferences these without checking, so it has to be caught here.
	 */
	if (!ctx->ctrl_bitrate || !ctx->ctrl_qp || !ctx->ctrl_fps ||
	    !ctx->ctrl_cbr || !ctx->ctrl_gop)
		return -ENODEV;

	return 0;
}


/* ------------------------------------------------------------------ */
/* Formats and controls						      */
/* ------------------------------------------------------------------ */

/*
 * Plane geometry for a raw NV12 picture.
 *
 * Both dimensions are rounded up to a whole macroblock before the planes are
 * divided.  The encoder is macroblock based (VENC_MP4_MBX_LMT is "Source Buffer
 * Width in Number of Macroblocks", draft/ds/mp4.txt:6461), so a picture whose plane
 * size is not a whole number of 16x16 macroblocks describes an address range the
 * datapath would read past the end of.
 *
 * @y_size is the LUMA plane and nothing else, and that is deliberate: it is the
 * offset the chroma address is computed from (base + y_size, because the source is
 * one semi-planar buffer), so redefining it as a total would move every chroma
 * address in the driver.  Chroma is half the luma plane in each direction, which
 * is what YUV420 means, so a buffer holding this picture is y_size * 3 / 2 bytes;
 * mtk_venc_frame_alloc_size() is where that arithmetic lives.
 */
static void mtk_venc_enc_plane_size(unsigned int w, unsigned int h,
				    unsigned long *y_size)
{
	w = round_up(w, MTK_VENC_MB_SIZE);
	h = round_up(h, MTK_VENC_MB_SIZE);

	*y_size = (unsigned long)w * h;
}

static void mtk_venc_src_setup(struct v4l2_format *f)
{
	unsigned long y_size;

	mtk_venc_enc_plane_size(f->fmt.pix.width, f->fmt.pix.height, &y_size);

	f->fmt.pix.bytesperline = f->fmt.pix.width;
	f->fmt.pix.sizeimage = y_size + y_size / 2;
}

static void mtk_venc_dst_setup(struct v4l2_format *f)
{
	/*
	 * Round up to the 128-byte granularity VENC_BITSTREAM_BUF_SIZE requires:
	 * mtk_venc_set_frame_addr() rejects a size that is not a multiple of 128, so
	 * accepting one here would only turn an S_FMT into a submit-time failure.
	 * Rounding here means the buffer the user sized is the buffer the hardware is
	 * told about.
	 */
	f->fmt.pix.sizeimage = round_up(f->fmt.pix.sizeimage,
					MTK_VENC_BS_SIZE_ALIGN);
}

/*
 * Is @f a format this driver can accept on the OUTPUT queue?
 *
 * Exactly one: NV12.  Chapter 60 states the supported input layouts as "YUV420 two
 * plane scan-line (NV12/NV21), YUV420 three plane scan-line (YV12/I420) or MTK block
 * formats" (draft/ds/venc.txt:1790-1791), and this datapath programs a single
 * CUR_UV address, so a two-plane semi-planar buffer is the only one of those that
 * maps onto one pointer without inventing a deinterleave step the hardware does not
 * have.
 */
static bool mtk_venc_src_fmt_ok(const struct v4l2_format *f)
{
	return f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	       f->fmt.pix.pixelformat == V4L2_PIX_FMT_NV12;
}

/*
 * Validate a picture size against what the hardware can do.
 *
 * Zero is rejected outright, since a zero-size frame would make every derived plane
 * size zero and every address check trivially pass.  Odd width or height is rejected
 * because YUV420 chroma is half the luma plane in each direction and a truncated
 * chroma plane is not a layout the datapath can address.  The ceiling is
 * MTK_VENC_MAX_WIDTH x MTK_VENC_MAX_HEIGHT, from Level 4.1 at 720p30 in Table 60-1
 * (draft/ds/venc.txt:1761-1770) rounded up to a whole macroblock; see
 * mtk-venc-mt6589.h.
 */
static int mtk_venc_check_size(unsigned int w, unsigned int h)
{
	if (!w || !h)
		return -EINVAL;

	if (w % 2 || h % 2)
		return -EINVAL;

	if (w > MTK_VENC_MAX_WIDTH || h > MTK_VENC_MAX_HEIGHT)
		return -EINVAL;

	return 0;
}

/*
 * ENUMFMT.
 *
 * There is exactly one format per queue, so this is "index 0 is it, anything else
 * does not exist".  That is honest rather than sparse: this driver genuinely cannot
 * be asked for anything else, and offering a second index the hardware cannot do
 * would be worse than not offering it.
 */
static int mtk_venc_enum_fmt_cap(struct file *file, void *priv,
				       struct v4l2_fmtdesc *d)
{
	if (d->index)
		return -EINVAL;

	d->flags = V4L2_FMT_FLAG_COMPRESSED;
	strscpy(d->description, "H.264 bitstream", sizeof(d->description));
	d->pixelformat = V4L2_PIX_FMT_H264;
	return 0;
}

static int mtk_venc_enum_fmt_out(struct file *file, void *priv,
				       struct v4l2_fmtdesc *d)
{
	if (d->index)
		return -EINVAL;

	d->flags = 0;
	strscpy(d->description, "NV12", sizeof(d->description));
	d->pixelformat = V4L2_PIX_FMT_NV12;
	return 0;
}

static int mtk_venc_g_fmt_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_venc_ctx *ctx = file2ctx(file);

	*f = ctx->dst_fmt;

	return 0;
}

static int mtk_venc_g_fmt_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_venc_ctx *ctx = file2ctx(file);

	*f = ctx->src_fmt;

	return 0;
}

/*
 * Round a requested format into something the encoder can run, or refuse it.
 *
 * A caller asking for an unsupported pixel format gets -EINVAL rather than a silent
 * substitution, because there is only one source format here and quietly handing
 * back a different one would mean the user goes on to hand the driver a layout it
 * never agreed to.
 */
static int mtk_venc_try_fmt_cap(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	/*
	 * The coded bitstream is not a picture: there is no width or height for the
	 * driver to echo back, because the coded size is not known until the frame is
	 * done.  sizeimage is the buffer size, and it is the only field with meaning
	 * here.
	 */
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (f->fmt.pix.sizeimage < MTK_VENC_BS_SIZE_MIN ||
	    f->fmt.pix.sizeimage > MTK_VENC_BS_SIZE_MAX)
		return -EINVAL;

	f->fmt.pix.width = 0;
	f->fmt.pix.height = 0;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	mtk_venc_dst_setup(f);

	return 0;
}

static int mtk_venc_try_fmt_out(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	int ret;

	if (!mtk_venc_src_fmt_ok(f))
		return -EINVAL;

	ret = mtk_venc_check_size(f->fmt.pix.width, f->fmt.pix.height);
	if (ret)
		return ret;

	f->fmt.pix.field = V4L2_FIELD_NONE;
	mtk_venc_src_setup(f);

	return 0;
}

static int mtk_venc_s_fmt_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_venc_ctx *ctx = file2ctx(file);
	int ret;

	ret = mtk_venc_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	ctx->dst_fmt = *f;

	return 0;
}


/*
 * Buffer ownership.
 *
 * The reference and reconstruction buffers are why this encoder is inter-frame: the
 * datapath reads the current frame, predicts against FRM_REF_* and DMAs the
 * reconstructed pixels out to FRM_REC_* (see the VENC_FRM_REF_* / VENC_FRM_REC_*
 * definitions in mtk-venc-mt6589-reg.h), so all of them have to be real DRAM of
 * the right size before any frame can be encoded.
 *
 * They are driver-owned rather than user-supplied because they are not part of any
 * standard's bitstream: asking userspace for them would invent an ABI nobody else
 * implements.  Coherent rather than streaming DMA because the hardware both reads
 * and writes them, and a cache flush between the encode and the next frame's read of
 * the REC buffer would be a correctness hazard for no size gain.
 *
 * dma_alloc_coherent() gives the 16-byte alignment the DIV16 address fields require
 * as a side effect of its page-aligned allocation.
 *
 * Return: 0 on success or a negative errno, having freed whatever it did allocate so
 * a partial failure leaves nothing behind.
 */
static void mtk_venc_free_frame_buffers(struct mtk_venc_ctx *ctx);

/*
 * Bytes one frame buffer actually occupies.
 *
 * NV12 is luma followed by half as much chroma again, and the datapath is handed
 * the chroma address as base + luma size, so an allocation of exactly the luma
 * size puts the whole chroma plane past the end of it -- the hardware would DMA
 * half a picture into memory the driver does not own.  frame_size itself stays
 * the LUMA size precisely because it is that offset; only the allocation is
 * scaled.  See mtk_venc_enc_plane_size().
 */
static inline unsigned long mtk_venc_frame_alloc_size(unsigned long y_size)
{
	return y_size + y_size / 2;
}

static int mtk_venc_alloc_frame_buffers(struct mtk_venc_ctx *ctx)
{
	struct device *dev = &ctx->dev->pdev->dev;
	unsigned long alloc_size;
	unsigned int i;
	int ret = 0;

	mtk_venc_enc_plane_size(ctx->src_fmt.fmt.pix.width,
				ctx->src_fmt.fmt.pix.height,
				&ctx->frame_size);
	if (!ctx->frame_size)
		return -EINVAL;

	alloc_size = mtk_venc_frame_alloc_size(ctx->frame_size);

	/*
	 * Two buffers, ping-ponged: this frame reads one and writes the other, and the
	 * next frame reverses it.  An earlier revision allocated four -- a ref[] and a
	 * rec[] pair -- and handed frame N the pair with the same index, which meant the
	 * next frame predicted from ref[] the encoder had never written to.  See the
	 * comment on MTK_VENC_FRAME_BUFFER.
	 */
	for (i = 0; i < MTK_VENC_FRAME_BUFFER; i++) {
		ctx->frame_vaddr[i] = dma_alloc_coherent(dev, alloc_size,
							 &ctx->frame_addr[i],
							 GFP_KERNEL);
		if (!ctx->frame_vaddr[i]) {
			ret = -ENOMEM;
			goto err_free;
		}
	}

	/*
	 * The rate control loads and saves its state through these two scratch
	 * buffers, and the data sheet sizes them by reference to code this driver does
	 * not have: "The required DRAM size is equal to (the length of ROM code + 16)"
	 * for RC_CODE_DRAM_ADDR_DIV16 (draft/ds/venc.txt:5072-5074), and the identical
	 * sentence for RC_INFO_DRAM_ADDR_DIV16 (draft/ds/venc.txt:5132-5136).  There
	 * is no size here to compute from anything available, so
	 * MTK_VENC_RC_CACHE_SIZE is a generous fixed allocation.
	 *
	 * Allocating it at all is the point.  These two registers are DIV16 DRAM
	 * ADDRESS registers, so before this existed the only thing that could be
	 * written into them was a small integer, which pointed the rate control's
	 * scratch pointer at DRAM address integer * 16 -- wherever the integrator
	 * happened to have mapped something low.  Now they point at memory the driver
	 * owns.
	 *
	 * HONEST CAVEAT: what the hardware does with the CONTENTS of this memory is the
	 * vendor's closed rate control algorithm, not anything this driver implements.
	 * Allocating it makes the registers valid; it does not make the bitrate control
	 * correct, and nothing in this driver makes the resulting bitstream conformant.
	 */
	ctx->rc_code_vaddr = dma_alloc_coherent(dev, MTK_VENC_RC_CACHE_SIZE,
						&ctx->rc_code_addr, GFP_KERNEL);
	if (!ctx->rc_code_vaddr) {
		ret = -ENOMEM;
		goto err_free;
	}

	ctx->rc_info_vaddr = dma_alloc_coherent(dev, MTK_VENC_RC_CACHE_SIZE,
						&ctx->rc_info_addr, GFP_KERNEL);
	if (!ctx->rc_info_vaddr) {
		ret = -ENOMEM;
		goto err_free;
	}

	return 0;

err_free:
	mtk_venc_free_frame_buffers(ctx);
	return ret;
}

/*
 * Free everything mtk_venc_alloc_frame_buffers() allocated.
 *
 * Safe on a partially allocated context: the context is zeroed on open, so every
 * address is either NULL or a real allocation, and dma_free_coherent() on a NULL
 * address is a no-op.  That is what lets the failure path above call this without
 * knowing how far the allocation got.
 */
static void mtk_venc_free_frame_buffers(struct mtk_venc_ctx *ctx)
{
	struct device *dev = &ctx->dev->pdev->dev;
	unsigned long alloc_size = mtk_venc_frame_alloc_size(ctx->frame_size);
	unsigned int i;

	for (i = 0; i < MTK_VENC_FRAME_BUFFER; i++) {
		dma_free_coherent(dev, alloc_size, ctx->frame_vaddr[i],
				  ctx->frame_addr[i]);
		ctx->frame_vaddr[i] = NULL;
		ctx->frame_addr[i] = 0;
	}

	dma_free_coherent(dev, MTK_VENC_RC_CACHE_SIZE, ctx->rc_code_vaddr,
			  ctx->rc_code_addr);
	ctx->rc_code_vaddr = NULL;
	ctx->rc_code_addr = 0;

	dma_free_coherent(dev, MTK_VENC_RC_CACHE_SIZE, ctx->rc_info_vaddr,
			  ctx->rc_info_addr);
	ctx->rc_info_vaddr = NULL;
	ctx->rc_info_addr = 0;
}

/*
 * s_fmt on the OUTPUT queue.
 *
 * Changing the picture geometry changes the size of the driver-owned frame buffers,
 * so this reallocates them.  The rate control scratch does not depend on the picture
 * and is left alone.
 *
 * It refuses to do any of that while either queue holds buffers.  The reallocation
 * frees the reference/reconstruction planes the hardware is DMA-ing through and
 * hands back different ones, so a buffer already queued is holding a vb2 buffer
 * whose geometry no longer describes the picture -- and if a frame is in flight, the
 * plane it is writing into may be unmapped while the engine is still using it.
 * queue_setup() sizes the queues from src_fmt, so a resize has to start from a state
 * where nothing is allocated against the old geometry: REQBUFS(0) or STREAMOFF
 * first.
 */
static int mtk_venc_s_fmt_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_venc_ctx *ctx = file2ctx(file);
	int ret;

	ret = mtk_venc_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	if (vb2_get_num_buffers(&ctx->m2m->out_q_ctx.q) ||
	    vb2_get_num_buffers(&ctx->m2m->cap_q_ctx.q))
		return -EBUSY;

	/*
	 * The previous format is not restored on failure.  Leaving a context whose
	 * buffers do not match its declared geometry is exactly the state where a later
	 * submit reads past the end of an allocation; zeroing the geometry instead makes
	 * the next S_FMT see an unconfigured context, which queue_setup() refuses.
	 */
	mtk_venc_free_frame_buffers(ctx);
	ctx->src_fmt = *f;
	ctx->frame_count = 0;

	ret = mtk_venc_alloc_frame_buffers(ctx);
	if (ret) {
		memset(&ctx->src_fmt, 0, sizeof(ctx->src_fmt));
		return ret;
	}

	return 0;
}

/*
 * queue_setup - how many buffers of what size each queue needs, and how big each
 * plane is.
 *
 * The OUTPUT queue holds raw NV12 frames from userspace, so its geometry comes
 * straight from the negotiated source format.  The CAPTURE queue holds coded
 * bitstreams, so its size is the negotiated sizeimage: the buffer the encoder is
 * allowed to fill, not a prediction of what it will fill it with.
 *
 * One plane on both sides.  The source is a single NV12 plane holding luma
 * followed by chroma, which the datapath splits into its two address registers,
 * and the coded bitstream is one contiguous byte range.
 *
 * Counts are clamped rather than refused.  Raising a too-small count to the
 * minimum is the V4L2 convention for REQBUFS and is what lets a caller ask for
 * one buffer and get a pipeline it can actually drive; two is the smallest that
 * does not stall on every frame.
 */
static int mtk_venc_queue_setup(struct vb2_queue *vq,
				      unsigned int *num_buffers,
				      unsigned int *num_planes,
				      unsigned int sizes[],
				      struct device *alloc_devs[])
{
	struct mtk_venc_ctx *ctx = vq->drv_priv;
	struct device *dev = &ctx->dev->pdev->dev;
	unsigned int min = 2;

	/*
	 * A queue type this context has no format for is a programming error, not a
	 * user error: REQBUFS arrives through v4l2_m2m_reqbufs(), which has already
	 * validated the type.  So it gets a WARN_ON and a refusal rather than a
	 * plausible-looking default.
	 */
	if (vq->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	    vq->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		WARN_ON(1);
		return -EINVAL;
	}

	/*
	 * No negotiated format means no geometry, and a queue sized against a zero
	 * format would allocate zero bytes for every buffer.  Refuse rather than accept
	 * a queue that cannot work: this is reachable by a REQBUFS with no preceding
	 * S_FMT, and it is far easier to diagnose here than as an allocation failure
	 * later.
	 */
	if (vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	    !ctx->src_fmt.fmt.pix.sizeimage)
		return -EINVAL;

	if (vq->type == V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    !ctx->dst_fmt.fmt.pix.sizeimage)
		return -EINVAL;

	*num_planes = 1;
	sizes[0] = vq->type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
		ctx->src_fmt.fmt.pix.sizeimage : ctx->dst_fmt.fmt.pix.sizeimage;

	if (*num_buffers < min)
		*num_buffers = min;

	alloc_devs[0] = dev;

	return 0;
}

/*
 * Buffer ops.
 *
 * Nothing here computes anything: the buffer userspace hands over IS the source
 * picture, and the encoder reads it from exactly where userspace put it.  What the
 * ops do is refuse states the hardware cannot honour.
 */

/*
 * A stateless one-shot submit is refused outright.
 *
 * V4L2_BUF_FLAG_LAST_SRC is a UAPI flag this checkout's videodev2.h does not carry
 * (it has V4L2_BUF_FLAG_LAST but not the _SRC variant), so it is defined here under
 * an #ifndef: against a kernel whose UAPI does define it, the definition is skipped
 * and the check below is a real test of the flag userspace actually sets.
 *
 * The encoder here is inter-frame -- it predicts from a reference frame a previous
 * job reconstructed -- so a stateless request has no such history.  Encoding it
 * would predict against whatever bytes happen to be in the reference buffer: a
 * corrupt stream, or on the very first frame an I-frame asked to be something else.
 * Refusing is the only honest answer, and it is why this driver does not advertise
 * stateless support.
 */
#ifndef V4L2_BUF_FLAG_LAST_SRC
#define V4L2_BUF_FLAG_LAST_SRC		0x00400000
#endif

static int mtk_venc_verify_src(struct vb2_buffer *vb, const void *pb)
{
	const struct v4l2_buffer *buf = pb;

	if (buf->index < 0 || buf->index >= vb->num_planes)
		return -EINVAL;

	if (buf->flags & V4L2_BUF_FLAG_LAST_SRC)
		return -EINVAL;

	return 0;
}

static int mtk_venc_verify_dst(struct vb2_buffer *vb, const void *pb)
{
	const struct v4l2_buffer *buf = pb;

	if (buf->index < 0 || buf->index >= vb->num_planes)
		return -EINVAL;

	return 0;
}

/*
 * Dispatch to the right queue's rules.
 *
 * verify_planes_array is the one buffer op that is type-dependent: it runs on the
 * capture queue as well as the output one, and the stateless check belongs only to a
 * source buffer.  Keying on the queue type rather than installing two different
 * structures means neither queue can be given the other's rules by mistake.
 */
static int mtk_venc_verify_planes(struct vb2_buffer *vb, const void *pb)
{
	if (vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return mtk_venc_verify_src(vb, pb);

	return mtk_venc_verify_dst(vb, pb);
}

static void mtk_venc_init_buf(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = container_of(vb, struct vb2_v4l2_buffer,
						    vb2_buf);

	vbuf->sequence = 0;
	vbuf->flags = 0;
	vbuf->planes[0].bytesused = 0;
}

static void mtk_venc_fill_user_buffer(struct vb2_buffer *vb, void *pb)
{
	const struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;

	src = container_of(vb, struct vb2_v4l2_buffer, vb2_buf);
	dst = container_of(pb, struct vb2_v4l2_buffer, vb2_buf);

	dst->sequence = src->sequence;
	dst->flags = src->flags;
	dst->planes[0].bytesused = src->planes[0].bytesused;
	dst->timecode = src->timecode;
}

static const struct vb2_buf_ops mtk_venc_buf_ops = {
	.verify_planes_array	= mtk_venc_verify_planes,
	.init_buffer		= mtk_venc_init_buf,
	.fill_user_buffer	= mtk_venc_fill_user_buffer,
};

/*
 * start_streaming - STREAMON reached the queue.
 *
 * Nothing is programmed in the hardware here, and that is a fact worth stating
 * rather than leaving implicit: a frame is started by device_run() and finished by
 * the frame-done interrupt, so streaming on means exactly "the framework may now
 * call device_run".  A reader looking for where STREAMON touches VENC_CODEC_CTRL
 * would otherwise assume it had been missed.
 *
 * What it does do is reset the frame counter, so a fresh stream starts a fresh GOP.
 * Without that the keyframe decision would continue from wherever the previous
 * STREAMON left off, and a client that seeks or restarts would get a P-frame first
 * with no reference behind it.
 */
static int mtk_venc_start_streaming(struct vb2_queue *q,
					  unsigned int count)
{
	struct mtk_venc_ctx *ctx = q->drv_priv;

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
		ctx->frame_count = 0;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Job execution							      */
/* ------------------------------------------------------------------ */

/*
 * Is this the first frame of a GOP?
 *
 * The GOP control is the user's keyframe interval and this turns it into something
 * the hardware can act on: VENC_IMG_TYPE selects I-frame or P-frame and nothing
 * else, so the driver has to decide where the I-frames go.
 *
 * With a GOP of 1 -- the default -- every frame is an I-frame.  That is the
 * conservative default: an I-frame needs no reference history, so a stream is
 * decodable from its first frame, and it is what a caller who has not thought about
 * inter prediction should get.  With a GOP of N, every Nth frame is an I-frame and
 * the rest are P-frames.
 */
static bool mtk_venc_is_keyframe(struct mtk_venc_ctx *ctx)
{
	unsigned int gop = v4l2_ctrl_g_ctrl(ctx->ctrl_gop);

	if (gop <= 1)
		return true;

	return (ctx->frame_count % gop) == 0;
}

/*
 * Assemble the mtk_venc_parm the register programming takes, from the
 * per-instance control values.
 *
 * Every value that has a documented register range is validated by
 * mtk_venc_set_rate_control() itself, which knows the field widths; the control
 * ranges below make sure a value that cannot be held is refused at S_EXT_CTRLS
 * rather than truncated at submit time.
 */
static void mtk_venc_get_parm(struct mtk_venc_ctx *ctx,
				    struct mtk_venc_parm *p)
{
	memset(p, 0, sizeof(*p));

	p->width = ctx->src_fmt.fmt.pix.width;
	p->height = ctx->src_fmt.fmt.pix.height;
	p->bitrate = v4l2_ctrl_g_ctrl(ctx->ctrl_bitrate);
	p->qp_init = v4l2_ctrl_g_ctrl(ctx->ctrl_qp);
	p->rc_fps = v4l2_ctrl_g_ctrl(ctx->ctrl_fps);
	p->cbr = v4l2_ctrl_g_ctrl(ctx->ctrl_cbr);
	p->gop = v4l2_ctrl_g_ctrl(ctx->ctrl_gop);
}

/*
 * job_ready - is there a complete job to run?
 *
 * The check that matters is that a destination buffer is actually queued.  Without
 * it, V4L2_BUF_FLAG_LAST on a capture buffer -- how userspace says "this was the
 * last frame" -- could never be honoured, because the encoder would start a job with
 * nowhere to put the bitstream and then have nowhere to hand it back.
 *
 * Both queues must have something ready.  The framework requires this before it
 * calls device_run(), but stating it here makes the guarantee local: this driver will
 * never start a frame whose bitstream has nowhere to go or whose source it cannot
 * read.
 */
static int mtk_venc_job_ready(void *priv)
{
	struct mtk_venc_ctx *ctx = priv;

	if (!v4l2_m2m_num_src_bufs_ready(ctx->m2m))
		return 0;

	if (!v4l2_m2m_num_dst_bufs_ready(ctx->m2m))
		return 0;

	return 1;
}

/*
 * device_run - encode one frame.
 *
 * This is the function that makes mtk_venc_submit_frame(),
 * mtk_venc_set_frame_addr(), mtk_venc_set_rate_control() and
 * mtk_venc_set_rc_scratch_addr() reachable: each was correct register code with no
 * caller, and this is the caller.  None of them was modified to get here.
 *
 * The ordering is the correctness argument and it is not arbitrary:
 *
 *   1. PEEK both buffers, without removing either.  v4l2_m2m_next_src_buf() and
 *      v4l2_m2m_next_dst_buf() only take the head of the ready list and leave it there;
 *      the removal is left to the completion path, because
 *      v4l2_m2m_buf_done_and_job_finish() removes both buffers itself and would find
 *      nothing if this function had taken them off first.  That is not a detail: this
 *      driver's completion runs later and from somewhere else (an ISR-deferred workqueue,
 *      the watchdog, or job_abort()), and it recovers the buffers by peeking the ready
 *      list again rather than by carrying pointers here -- so leaving them on the list is
 *      what makes the buffers findable at all.  This is the convention the hantro
 *      driver uses (hantro_drv.c: hantro_job_finish_no_pm()); the bdisp-style drivers
 *      that remove in device_run() have no such deferral, because they complete the job
 *      synchronously and can hold the pointers in their context.
 *   2. Take the runtime-PM reference, BEFORE any register programming.  This is the
 *      ordering that an earlier revision got wrong, and it matters because
 *      pm_runtime_resume_and_get() may run the resume callback, which enables clocks
 *      and then calls mtk_venc_reset().  Programming VENC_ENCODER_INFO_3 and the RC
 *      scratch addresses before that reference was taken meant every one of those
 *      writes landed in registers the resume callback then reset from 0x00100000,
 *      so the frame was started with a default image type and the rate control
 *      pointed at DRAM address zero.  mtk_venc_submit_frame() therefore no longer
 *      takes a reference of its own; exactly one is taken per frame here.
 *   3. Publish ctx->pending BEFORE starting the frame.  Starting first and publishing
 *      afterwards is a lost-wakeup race: the encoder can raise ENC_FRM_INT and the
 *      completion path can run before we get here, find no job, retire nothing, and
 *      leave the runtime-PM reference taken in step 2 held forever.
 *   4. Arm the watchdog, so a frame that never completes cannot strand the job.
 *
 * Reference and reconstruction
 * ---------------------------
 * The datapath predicts against FRM_REF_* and writes the reconstruction to
 * FRM_REC_*, and the next frame has to predict from what this one reconstructed.
 * That is a genuine ping-pong over the two driver-owned frame buffers:
 *
 *   frame N uses frame_addr[idx] as its reference and frame_addr[!idx] as its REC,
 *   then ctx->buf_idx ^= 1 makes frame N+1's reference frame N's REC.
 *
 * so no copy is needed and none is done.  With a single buffer the REC would be both
 * the one the hardware writes and the one it predicts from, and the prediction would
 * read the frame the encoder is mid-way through writing.
 *
 * The array used to be two: a ref[] and a rec[] pair, both indexed by buf_idx.  That
 * is not a ping-pong, and the comment above used to claim it was -- frame N+1 was
 * handed ref[!idx], a buffer nothing ever writes, so every P-frame after the first
 * predicted against whatever happened to be in it.
 *
 * Each buffer is NV12 and holds luma followed by half as much chroma again, so it is
 * allocated at frame_size * 3 / 2 while frame_size itself stays the luma plane size,
 * which is the offset both chroma addresses below are computed from.
 *
 * The first frame of a GOP is an I-frame and is programmed as one
 * (VENC_IMG_TYPE = 2), so it does not read the reference at all; that is what makes
 * the reference buffer being uninitialised at the start of a stream harmless rather
 * than a source of garbage in the first frames.
 *
 * This makes the reference chain correct.  It does NOT make the bitstream
 * conformant: see the file header on what still lives only in the vendor's closed
 * userspace libraries.
 */
static void mtk_venc_device_run(void *priv)
{
	struct mtk_venc_ctx *ctx = priv;
	struct mtk_venc_dev *venc = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct mtk_venc_parm parm;
	unsigned long flags;
	dma_addr_t bs_addr, src_y, src_uv, ref_y, ref_uv, rec_y, rec_uv, bs_size;
	unsigned int idx;
	bool keyframe, pm_taken = false;
	int ret;

	src_buf = v4l2_m2m_next_src_buf(ctx->m2m);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->m2m);
	if (!src_buf || !dst_buf)
		goto err_put;

	/*
	 * The NV12 source is one plane holding luma followed by chroma, and the datapath
	 * has a separate address register for each, so the chroma address is the base
	 * plus the luma plane size.  mtk_venc_enc_plane_size() rounds the geometry up to
	 * whole macroblocks before dividing, so this offset is the one the hardware will
	 * walk to.
	 */
	src_y = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	src_uv = src_y + ctx->frame_size;

	bs_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);

	/*
	 * The ping-pong.  frame_addr[buf_idx] is this frame's REFERENCE and
	 * frame_addr[!buf_idx] is its RECONSTRUCTION, so the flip at the end of this
	 * function makes the next frame's reference the one just written.  Chroma sits
	 * half a luma plane in, which is why the buffers are allocated at 3/2 of
	 * frame_size while frame_size remains the luma plane size.
	 */
	idx = ctx->buf_idx % MTK_VENC_FRAME_BUFFER;
	ref_y = ctx->frame_addr[idx];
	ref_uv = ref_y + ctx->frame_size;
	rec_y = ctx->frame_addr[!idx];
	rec_uv = rec_y + ctx->frame_size;

	mtk_venc_get_parm(ctx, &parm);

	/*
	 * The bitstream buffer size the hardware is told about is the queue's negotiated
	 * sizeimage, not the length of whatever the user happened to allocate.  The
	 * encoder writes up to sizeimage bytes and raises BS_DRAM_FULL_INT past it, so
	 * telling it the real bound is what makes that interrupt mean "overflow" rather
	 * than "silently wrote past the buffer".  VENC_BITSTREAM_BUF_SIZE is DIV128,
	 * which mtk_venc_dst_setup() has already rounded this to.
	 */
	bs_size = ctx->dst_fmt.fmt.pix.sizeimage;

	keyframe = mtk_venc_is_keyframe(ctx);

	/*
	 * Publish the job before anything is programmed that could make it complete.
	 * ctx->pending is the claim mtk_venc_complete_job() takes, and it is what stops
	 * the watchdog or the interrupt from retiring a frame that has not been started.
	 *
	 * venc->enc_pm_held is deliberately NOT set here even though the reference is
	 * about to be taken: it means "a reference is outstanding for a frame that was
	 * started", and nothing has started yet.  Setting it early would let a stray
	 * interrupt -- the resume path clears and acknowledges every pending interrupt,
	 * so one can arrive at any point -- retire a job that never ran and drop a
	 * reference nobody took.
	 */
	spin_lock_irqsave(&venc->enc_state_lock, flags);
	ctx->pending = true;
	venc->bs_error = false;
	venc->bs_bytes = 0;
	venc->timeout_ctx = ctx;
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	/*
	 * Take the runtime-PM reference for this frame, and take it HERE -- before the
	 * first register write below, not inside mtk_venc_submit_frame().
	 *
	 * pm_runtime_resume_and_get() runs mtk_venc_runtime_resume() when the count
	 * goes from zero, which enables the clocks and calls mtk_venc_reset().  Reset
	 * restores VENC_ENCODER_INFO_3 to 0x00100000 and clears everything else, so the
	 * INFO_3 write and the two RC scratch address writes that used to precede this
	 * call were simply discarded before the frame was ever started: every frame was
	 * an I-frame with GEN_REC_FRM unset and the rate control pointed at DRAM address
	 * zero.  The reference is now taken exactly once per frame, here.
	 *
	 * It is also taken before ctx->pending is checked against frame_pending below,
	 * but ctx->pending was just published under enc_state_lock, which is the same
	 * lock the interrupt handler and the watchdog take, so the two cannot interleave:
	 * either this frame's own claim is already visible to them, or they are still
	 * working on the previous frame.
	 *
	 * If the resume fails nothing was programmed and the claim is taken straight
	 * back below; the PM core has already undone its own increment when the resume
	 * callback fails, so there is nothing to put here.
	 */
	ret = pm_runtime_resume_and_get(&venc->pdev->dev);
	if (ret)
		goto err_job;

	pm_taken = true;

	/*
	 * Tell the engine what kind of frame this is, and ask it to commit the
	 * reconstruction when it is done.
	 *
	 * GEN_REC_FRM is what makes the REC buffer worth having: it triggers the
	 * de-blocking filter to dump the reconstructed frame for reference
	 * (draft/ds/venc.txt:3192-3193).  Without it the next frame would predict from
	 * an unfiltered -- and in practice never-written -- REC buffer.
	 *
	 * The whole word is written rather than read-modify-written because every field
	 * in it this driver does not drive (the colocated-MV option, the B-frame index,
	 * the IDR id, the prediction direction) resets to 0, so leaving them explicitly
	 * 0 is the documented reset state anyway.  VENC_ENCODER_INFO_3 resets to
	 * 0x00100000 (draft/ds/venc.txt:3095-3100), i.e. only bit 20 set, so no
	 * programming of this register's own bits survives a reset.
	 */
	mtk_venc_write(venc, VENC_ENCODER_INFO_3,
			VENC_GEN_REC_FRM |
			(keyframe ? VENC_IMG_TYPE_I_FRAME :
				     VENC_IMG_TYPE_P_FRAME));

	/*
	 * Point the rate control's scratch state at memory this driver owns.  This is the
	 * call that makes mtk_venc_set_rc_scratch_addr() reachable; before it, the two RC
	 * DRAM address registers held whatever small integer was written into them and the
	 * hardware loaded its state from wherever integer * 16 pointed.
	 */
	ret = mtk_venc_set_rc_scratch_addr(venc, ctx->rc_code_addr,
					    ctx->rc_info_addr);
	if (ret)
		goto err_job;

	ret = mtk_venc_submit_frame(venc, false, &parm, bs_addr, bs_size,
				    src_y, src_uv, ref_y, ref_uv,
				    rec_y, rec_uv);
	if (ret)
		goto err_job;

	/*
	 * The frame is running.  Arm the watchdog now that there is something to time
	 * out, and advance the ping-pong so the next frame's reference is this frame's
	 * reconstruction.
	 */
	mtk_venc_timeout_arm(venc, ctx);
	ctx->buf_idx ^= 1;
	ctx->frame_count++;

	return;

err_job:
	/*
	 * Nothing was started, so no interrupt will arrive and the watchdog has nothing
	 * to time out.  Take the claim back -- otherwise it would later retire a frame
	 * that never ran -- and hand the buffers to userspace as failed, so a rejected
	 * job does not silently swallow a buffer the caller is waiting to reuse.
	 *
	 * The reference this function took has to go back too.  Nothing is outstanding,
	 * so it is dropped directly rather than through the enc_pm_held dance in
	 * mtk_venc_complete_job(): there is no job for that path to retire and no
	 * interrupt is coming to do it later.  A failed resume is the one case where
	 * there was never a reference to give back -- the PM core has already undone its
	 * own increment when the resume callback fails -- hence the flag.
	 */
	spin_lock_irqsave(&venc->enc_state_lock, flags);
	ctx->pending = false;
	venc->timeout_ctx = NULL;
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	if (pm_taken)
		pm_runtime_put_autosuspend(&venc->pdev->dev);

	dst_buf->flags |= V4L2_BUF_FLAG_ERROR;
	dst_buf->planes[0].bytesused = 0;
	v4l2_m2m_buf_done_and_job_finish(venc->m2m_dev, ctx->m2m,
					 VB2_BUF_STATE_ERROR);
	return;

err_put:
	/*
	 * Reached only when the ready lists were empty, which job_ready() is supposed to
	 * have prevented.  Whichever buffer does exist is still on its ready list and still
	 * ACTIVE in vb2, because nothing below this point removed it, so the same helper
	 * the completion path uses is the correct one: it removes what it finds and calls
	 * v4l2_m2m_buf_done() on each, which is a no-op with a WARN_ON(vb->state !=
	 * VB2_BUF_STATE_ACTIVE) for a buffer that is NOT active.
	 *
	 * The previous revision called v4l2_m2m_buf_done() directly on a buffer it had
	 * only peeked, on the strength of the source having been removed -- but the
	 * destination was never removed by anyone, so that one warned and returned without
	 * doing anything, and the buffer was neither returned to userspace nor marked.
	 *
	 * This must be the helper rather than a bare v4l2_m2m_buf_done() pair, because the
	 * job is still marked TRANS_RUNNING: device_run() returns to the framework without
	 * a job_finish, so a STREAMOFF arriving now would block forever in
	 * v4l2_m2m_cancel_job()'s wait_event().  buf_done_and_job_finish() reaches
	 * _v4l2_m2m_job_finish() and clears it.
	 *
	 * Both lists empty is the same hang by another route -- there is nothing to return,
	 * but the job still has to be finished -- so that case takes plain
	 * v4l2_m2m_job_finish().  buf_done_and_job_finish() would only WARN before skipping
	 * _v4l2_m2m_job_finish() and leaving TRANS_RUNNING set.
	 */
	if (src_buf || dst_buf)
		v4l2_m2m_buf_done_and_job_finish(venc->m2m_dev, ctx->m2m,
						 VB2_BUF_STATE_ERROR);
	else
		v4l2_m2m_job_finish(venc->m2m_dev, ctx->m2m);
}

/*
 * job_abort - STREAMOFF arrived with a frame in flight.
 *
 * v4l2_m2m_cancel_job() calls this and then waits on m2m_ctx->finished for
 * TRANS_RUNNING to clear, so whatever this returns is a hard promise that no
 * further buffer bookkeeping for the current frame will happen on this context.
 *
 * An earlier revision did nothing here at all, which broke that promise: the frame
 * kept running, its completion arrived seconds later -- or at the next watchdog --
 * and completed the job on a context whose STREAMOFF had already returned, with
 * queues that had since been drained or a file that had been closed.  That is the
 * use-after-free the file header warns about, reached by the most ordinary
 * STREAMOFF there is.
 *
 * So the in-flight job is retired here, exactly as the watchdog would have: the
 * hardware cannot be halted without corrupting its state (there is no documented way,
 * and pretending otherwise is worse than saying so), but the job itself does not
 * have to wait for it.  mtk_venc_complete_job() takes the ctx->pending claim, so if
 * the engine does finish and the interrupt path completes the same job, the loser
 * finds the claim clear and returns without touching anything -- and vice versa.
 *
 * ctx->pending is therefore NOT cleared directly here: clearing it would let a late
 * completion find no job and strand the runtime-PM reference, which is the exact
 * bug the claim exists to prevent.  Taking the claim through the same function the
 * interrupt uses is what keeps the acquire and release paired.
 */
static void mtk_venc_job_abort(void *priv)
{
	struct mtk_venc_ctx *ctx = priv;
	struct mtk_venc_dev *venc = ctx->dev;

	if (!ctx->pending)
		return;

	dev_warn_ratelimited(&venc->pdev->dev,
			     "STREAMOFF with a frame in flight; failing it rather than waiting for the encoder\n");

	/*
	 * The buffers go back as failed, the reference is dropped, and the watchdog is
	 * cancelled -- all by mtk_venc_complete_job().  The frame in the hardware is
	 * left to finish or hang; what matters here is that the framework's wait on
	 * m2m_ctx->finished cannot outlive this context.
	 */
	mtk_venc_complete_job(venc, ctx, 0, VB2_BUF_STATE_ERROR, false);
}


/*
 * stop_streaming - STREAMOFF.
 *
 * Return whatever is still queued on this queue's ready list.
 *
 * A frame may still be in flight, but by the time this runs the framework has already
 * called v4l2_m2m_cancel_job(), so job_abort() has retired it and its buffers are gone;
 * there is nothing of this frame's left to touch.  What IS left is every buffer the
 * client queued but that was never picked up by a job -- and those are still owned by
 * the driver, because vb2 marks a buffer ACTIVE when buf_queue() hands it over and only
 * clears that when v4l2_m2m_buf_done() is called for it.
 *
 * So doing nothing here is not a neutral choice.  vb2 checks after stop_streaming
 * returns and WARN_ONs on a non-zero owned_by_drv_count, printing "driver bug:
 * stop_streaming operation is leaving buffer N in active state" for each one it then
 * has to reclaim itself (videobuf2-core.c, __vb2_queue_cancel()).  That is a kernel
 * warning on the most ordinary STREAMOFF there is -- qbuf some buffers, never stream on,
 * stream off again -- and it is this driver being told it leaked buffers it still owns.
 *
 * Removal, not a peek, and one v4l2_m2m_buf_done() per removed buffer: the count only
 * falls when a buffer leaves the ready list, so this loop terminates.  Each buffer is
 * returned as ERROR rather than as it was queued, since STREAMOFF has ended the stream
 * and there is no frame behind them.  hantro_return_bufs() does exactly this.
 */
static void mtk_venc_stop_streaming(struct vb2_queue *q)
{
	struct mtk_venc_ctx *ctx = q->drv_priv;

	for (;;) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->m2m);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->m2m);

		if (!vbuf)
			break;

		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

/*
 * buf_queue - a buffer was queued, hand it to the framework's ready lists.
 *
 * This is not optional plumbing.  v4l2_m2m_buf_queue() is what moves a buffer from
 * vb2's queued state onto the m2m ready list, and the only thing that reads those
 * lists is the framework's own scheduling: v4l2_m2m_num_src_bufs_ready() and
 * v4l2_m2m_num_dst_bufs_ready() -- which this driver's job_ready() calls -- and
 * v4l2_m2m_next_src_buf()/v4l2_m2m_next_dst_buf().  Without it both counts are
 * permanently zero, job_ready() never returns 1, device_run() is never reached,
 * and every buffer a client queues is accepted and then never run.
 *
 * It also has to exist for the queue to be created at all: vb2_core_queue_init()
 * WARN_ON()s on a vb2_ops without a buf_queue and returns -EINVAL, so an earlier
 * revision that omitted it could not even open the node.
 */
static void mtk_venc_buf_queue(struct vb2_buffer *vb)
{
	struct mtk_venc_ctx *ctx = vb->vb2_queue->drv_priv;
	struct vb2_v4l2_buffer *vbuf =
		container_of(vb, struct vb2_v4l2_buffer, vb2_buf);

	v4l2_m2m_buf_queue(ctx->m2m, vbuf);
}

static const struct vb2_ops mtk_venc_qops = {
	.queue_setup		= mtk_venc_queue_setup,
	.buf_queue		= mtk_venc_buf_queue,
	.start_streaming	= mtk_venc_start_streaming,
	.stop_streaming		= mtk_venc_stop_streaming,
};

/*
 * queue_init - build the two vb2 queues.
 *
 * Called by v4l2_m2m_ctx_init() once per open, before any buffer exists, so this
 * is where the queue types, the memory model and the queue operations are chosen.
 *
 * The buffers are the source picture and the coded bitstream, both of which the
 * hardware DMAs to and from, so vb2_dma_contig_memops is the right allocator: it
 * is what this hardware's IOMMU actually wants and what the DIV16 address
 * registers need.
 *
 * drv_priv points at the context so that queue_setup() and the buffer ops can
 * reach the negotiated format without re-deriving it from the queue.
 */
static const struct vb2_buf_ops mtk_venc_buf_ops;

static int mtk_venc_queue_init(void *priv, struct vb2_queue *src_vq,
				     struct vb2_queue *dst_vq)
{
	struct mtk_venc_ctx *ctx = priv;
	int ret;

	/*
	 * vb2_queue_init() requires each queue to carry a lock and does not
	 * supply one, so without this every open() fails with -EINVAL before a
	 * single format can be negotiated.  One mutex serves both queues, which
	 * is what serialises a queue operation against the other side of a
	 * frame.
	 */
	mutex_init(&ctx->vb_queue_lock);

	src_vq->lock		= &ctx->vb_queue_lock;
	dst_vq->lock		= &ctx->vb_queue_lock;

	src_vq->type		= V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes	= VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv	= ctx;
	src_vq->buf_struct_size	= sizeof(struct v4l2_m2m_buffer);
	src_vq->ops		= &mtk_venc_qops;
	src_vq->buf_ops		= &mtk_venc_buf_ops;
	src_vq->mem_ops		= &vb2_dma_contig_memops;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type		= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes	= VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv	= ctx;
	dst_vq->buf_struct_size	= sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops		= &mtk_venc_qops;
	dst_vq->buf_ops		= &mtk_venc_buf_ops;
	dst_vq->mem_ops		= &vb2_dma_contig_memops;

	return vb2_queue_init(dst_vq);
}

/*
 * open - a userspace client starts using the encoder.
 *
 * The per-instance context owns the reference, reconstruction and rate control
 * scratch allocations, so it is created here and torn down in release().  It is
 * per file descriptor rather than per device because those buffers are sized from
 * the negotiated picture geometry, and two clients encoding different resolutions
 * must not fight over them.
 */
static int mtk_venc_open(struct file *file)
{
	struct mtk_venc_dev *venc = file2m2m(file);
	struct mtk_venc_ctx *ctx;
	struct v4l2_format *fmt;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = venc;

	/*
	 * Defaults, so a client that never sets a format still gets something
	 * coherent.  The source format deliberately starts with no geometry: there is
	 * no sensible default picture size, and queue_setup() refuses a queue sized
	 * against a zero format, so the failure lands where it is diagnosable rather
	 * than as an allocation failure later.
	 */
	fmt = &ctx->src_fmt;
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;

	fmt = &ctx->dst_fmt;
	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	fmt->fmt.pix.field = V4L2_FIELD_NONE;
	fmt->fmt.pix.sizeimage = MTK_VENC_BS_SIZE_DEFAULT;

	/*
	 * Controls are created before the queues exist, so a client that sets the
	 * bitrate or QP with EXT_CTRLS does so against a fully populated handler.
	 */
	ret = mtk_venc_ctrl_init(ctx);
	if (ret)
		goto err_free;

	v4l2_fh_init(&ctx->fh, file2m2m(file));
	ctx->fh.m2m_ctx = ctx->m2m = v4l2_m2m_ctx_init(venc->m2m_dev, ctx,
							&mtk_venc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		ctx->fh.m2m_ctx = NULL;
		ctx->m2m = NULL;
		v4l2_fh_exit(&ctx->fh);
		goto err_free;
	}

	/*
	 * Last, because it is what publishes file->private_data.  v4l2_m2m_fop_poll()
	 * and v4l2_m2m_fop_mmap() both reach the m2m context through that pointer read
	 * as a struct v4l2_fh, and video_ioctl2() does the same for every ioctl below,
	 * so an earlier revision that set file->private_data to the bare context made
	 * all three dereference the wrong type.
	 */
	v4l2_fh_add(&ctx->fh, file);

	/*
	 * enc_users keeps a system suspend from tearing the encoder's registers down
	 * underneath an open but idle stream.  It is decremented in release().
	 */
	atomic_inc(&venc->enc_users);

	return 0;

err_free:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	kfree(ctx);

	return ret;
}

/*
 * release - the client is done with the encoder.
 *
 * The queues are drained first, because that is what releases any buffer the
 * hardware is still holding a DMA handle on.
 */
static int mtk_venc_release(struct file *file)
{
	struct mtk_venc_ctx *ctx = file2ctx(file);
	struct mtk_venc_dev *venc = ctx->dev;
	unsigned long flags;
	bool owns_job;

	/*
	 * Settle any job this instance still owns before ANY of its memory goes away.
	 *
	 * Both work items are cancelled, and cancel_delayed_work_sync() makes this a
	 * guarantee rather than a hope: it returns only once the watchdog is not
	 * running.  The watchdog is what retires a job the hardware has stopped
	 * completing, so after these two calls no completion path can still be holding
	 * a pointer to this context.
	 *
	 * Order matters against the frees below.  If the frame were freed first, a
	 * watchdog firing in the same window would hand a job's buffers back to the
	 * framework after the REF/REC planes the encoder was DMA-ing through had been
	 * unmapped -- a use-after-free visible only when an encode wedges exactly as
	 * the client closes the node.
	 *
	 * The cancels are CONDITIONAL on this context owning the job, which matters
	 * because both work items are per-device, not per-open.  The encoder takes one
	 * frame at a time and the m2m framework runs one context at a time, so
	 * timeout_ctx names at most one running job -- but with two handles open, one
	 * of them idle, an unconditional cancel here would disarm the IDLE handle's
	 * counterpart's watchdog and leave a genuinely wedged frame with no way to be
	 * timed out.  That is the exact liveness bug the watchdog exists to fix, so it
	 * is not acceptable to reintroduce it in the close path.
	 */
	spin_lock_irqsave(&venc->enc_state_lock, flags);
	owns_job = venc->timeout_ctx == ctx;
	if (owns_job)
		venc->timeout_ctx = NULL;
	spin_unlock_irqrestore(&venc->enc_state_lock, flags);

	if (owns_job) {
		cancel_delayed_work_sync(&venc->venc_timeout_work);
		cancel_work_sync(&venc->venc_complete_work);
	}

	v4l2_m2m_ctx_release(ctx->m2m);

	/*
	 * Unpublish the file handle before the context it lives in goes away.
	 * v4l2_fh_del() takes this handle off the video_device's fh_list and clears
	 * file->private_data, which is now a pointer into the kfree()d ctx below, and
	 * v4l2_fh_exit() closes the event subscriptions it may have opened.  Both have
	 * to happen while the v4l2_fh is still valid.
	 */
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);

	mtk_venc_free_frame_buffers(ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	kfree(ctx);

	atomic_dec(&venc->enc_users);

	return 0;
}

static const struct v4l2_file_operations mtk_venc_fops = {
	.owner		= THIS_MODULE,
	.open		= mtk_venc_open,
	.release	= mtk_venc_release,
	.poll		= v4l2_m2m_fop_poll,
	/*
	 * Without this the node had no way to receive an ioctl at all: the V4L2 core
	 * refuses to open a video_device whose fops has no unlocked_ioctl.  video_ioctl2
	 * is also what resolves file_to_v4l2_fh() and dispatches to mtk_venc_ioctl_ops.
	 */
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static int mtk_venc_querycap(struct file *file, void *priv,
				   struct v4l2_capability *cap)
{
	/*
	 * V4L2_CAP_VIDEO_M2M is the mem-to-mem capability.  This driver's UAPI copy has
	 * no separate V4L2_CAP_VIDEO_CODEC bit, so a codec node is expressed as a
	 * mem-to-mem node whose capture side is a compressed format;
	 * V4L2_CAP_STREAMING is what advertises VIDIOC_STREAMON to userspace.
	 */
	strscpy(cap->driver, MTK_VENC_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, MTK_VENC_DRIVER_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 MTK_VENC_DRIVER_NAME);

	/* Nothing here needs 32-bit interface emulation, so they are the same set. */
	cap->device_caps = cap->capabilities = V4L2_CAP_VIDEO_M2M |
						  V4L2_CAP_STREAMING;

	return 0;
}

static const struct v4l2_ioctl_ops mtk_venc_ioctl_ops = {
	.vidioc_querycap		= mtk_venc_querycap,

	.vidioc_enum_fmt_vid_cap	= mtk_venc_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out	= mtk_venc_enum_fmt_out,
	.vidioc_g_fmt_vid_cap	= mtk_venc_g_fmt_cap,
	.vidioc_g_fmt_vid_out	= mtk_venc_g_fmt_out,
	.vidioc_try_fmt_vid_cap	= mtk_venc_try_fmt_cap,
	.vidioc_try_fmt_vid_out	= mtk_venc_try_fmt_out,
	.vidioc_s_fmt_vid_cap	= mtk_venc_s_fmt_cap,
	.vidioc_s_fmt_vid_out	= mtk_venc_s_fmt_out,

	/*
	 * Buffer lifecycle is multiplexed by the framework: those ioctls arrive without
	 * a queue type and it dispatches them to the right one.
	 */
	.vidioc_reqbufs		= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf	= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf		= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf		= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf	= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs	= v4l2_m2m_ioctl_create_bufs,
	.vidioc_remove_bufs	= v4l2_m2m_ioctl_remove_bufs,
	.vidioc_expbuf		= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon	= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff	= v4l2_m2m_ioctl_streamoff,
};

static const struct v4l2_m2m_ops mtk_venc_m2m_ops = {
	.device_run	= mtk_venc_device_run,
	.job_ready	= mtk_venc_job_ready,
	.job_abort	= mtk_venc_job_abort,
};

/*
 * The video_device template.
 *
 * A static instance is copied into the driver's own storage in vf_init() rather
 * than allocated, because every field in it is a constant: this tree's
 * struct video_device has no per-instance allocation need beyond the struct
 * itself, and video_set_drvdata()/vfd->lock are what bind it to this probe.
 *
 * .vfl_dir = VFL_DIR_M2M is what makes this a mem-to-mem node rather than a
 * video capture or output one, and .minor = -1 asks for an automatic minor.
 */
static struct video_device mtk_venc_videodev = {
	.name		= MTK_VENC_DRIVER_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &mtk_venc_fops,
	.ioctl_ops	= &mtk_venc_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.device_caps	= V4L2_CAP_STREAMING,
};

/*
 * Bring up the encoder's video_device.
 *
 * Every step that registers something is undone explicitly on the failure paths
 * below, in reverse order, because the v4l2_device is embedded in this driver's
 * own private state rather than devm-managed: if video_register_device() fails,
 * the m2m context has to be released and the v4l2_device unregistered, or the
 * node is left half-built.
 */
static int mtk_venc_vf_init(struct mtk_venc_dev *venc)
{
	struct v4l2_device *v4l2_dev = &venc->v4l2_dev;
	struct video_device *vfd;
	int ret;

	/*
	 * v4l2_device_register() first: it is what makes the v4l2_device usable and it
	 * is what later teardown has to undo.  It also takes dev->driver_data, so
	 * platform_set_drvdata() must have run first (probe() does that before calling
	 * this).
	 */
	ret = v4l2_device_register(&venc->pdev->dev, v4l2_dev);
	if (ret)
		return ret;

	venc->vfd = mtk_venc_videodev;
	vfd = &venc->vfd;

	vfd->lock = &venc->lock;
	vfd->v4l2_dev = v4l2_dev;
	vfd->device_caps |= V4L2_CAP_VIDEO_M2M;
	video_set_drvdata(vfd, venc);

	venc->m2m_dev = v4l2_m2m_init(&mtk_venc_m2m_ops);
	if (IS_ERR(venc->m2m_dev)) {
		ret = PTR_ERR(venc->m2m_dev);
		venc->m2m_dev = NULL;
		goto err_unregister_dev;
	}

	/*
	 * Register the node with the video core FIRST, then attach it to the media
	 * graph.  The order is not interchangeable: v4l2_m2m_register_media_controller()
	 * needs vfd->minor to name the /dev/videoX interface it creates, and minor
	 * numbers are only allocated by video_register_device().
	 */
	ret = video_register_device(vfd, VFL_TYPE_VIDEO, 0);
	if (ret)
		goto err_release_m2m;

	/*
	 * Register the media controller.  v4l2_m2m_register_media_controller() builds
	 * the three m2m entities (source, processing, sink), creates their pads and
	 * links them together; it is a no-op returning 0 when there is no media
	 * controller, so the node still works without one.
	 *
	 * A failure here is not fatal to the node: the video device is registered and
	 * usable through the streaming ioctls regardless, it just has no graph
	 * representation.  It is reported rather than allowed to fail probe over
	 * something userspace can still reach.
	 */
	ret = v4l2_m2m_register_media_controller(venc->m2m_dev, vfd,
						MEDIA_ENT_F_PROC_VIDEO_ENCODER);
	if (ret) {
		v4l2_warn(v4l2_dev,
			  "failed to register m2m media controller: %d\n", ret);
		venc->mc_registered = false;
	} else {
		venc->mc_registered = true;
	}

	v4l2_info(v4l2_dev, "encoder registered as /dev/video%d\n", vfd->num);

	return 0;

err_release_m2m:
	v4l2_m2m_release(venc->m2m_dev);
	venc->m2m_dev = NULL;
err_unregister_dev:
	v4l2_device_unregister(v4l2_dev);

	return ret;
}

/*
 * Undo mtk_venc_vf_init().
 *
 * Order matters: the node is unregistered first so no new open can race the
 * teardown, then the media controller (whose entities and links it owns), then
 * the m2m context, and only then the v4l2_device itself.
 */
static void mtk_venc_vf_deinit(struct mtk_venc_dev *venc)
{
	if (venc->m2m_dev) {
		if (venc->mc_registered)
			v4l2_m2m_unregister_media_controller(venc->m2m_dev);
		venc->mc_registered = false;
		video_unregister_device(&venc->vfd);
	}

	v4l2_m2m_release(venc->m2m_dev);
	venc->m2m_dev = NULL;

	v4l2_device_unregister(&venc->v4l2_dev);
}
static int mtk_venc_runtime_suspend(struct device *dev)
{
	struct mtk_venc_dev *venc = dev_get_drvdata(dev);

	mutex_lock(&venc->enc_lock);
	/*
	 * Power down only when nothing is open.  With a handle open the
	 * encoder is in use and its clock has to stay on: the registers are
	 * read and written throughout a frame, including from the interrupt
	 * handler.  The test is inverted from what it was - `== 0` meant both
	 * the suspend and the resume below skipped power_on() while any handle
	 * was open, so the block ran with venc_clk off for the whole life of
	 * the node.
	 */
	if (atomic_read(&venc->enc_users) == 0)
		mtk_venc_power_off(venc);
	mutex_unlock(&venc->enc_lock);

	venc->suspended = true;
	return 0;
}

static int mtk_venc_runtime_resume(struct device *dev)
{
	struct mtk_venc_dev *venc = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&venc->enc_lock);
	/*
	 * Always power on here.  This used to be conditional on there being no
	 * open handle, which inverted the sense of the thing: with a handle
	 * open the encoder was in use, and skipping power_on() left it running
	 * with venc_clk off, so every register write in device_run and every
	 * read in the interrupt handler touched a dark block.
	 *
	 * clk_prepare_enable() is reference counted, so doing this on every
	 * resume is safe, and the matching power_off() in suspend only happens
	 * when nothing is open - so the count cannot leak.
	 */
	ret = mtk_venc_power_on(venc);
	if (ret) {
		mutex_unlock(&venc->enc_lock);
		return ret;
	}
	mtk_venc_reset(venc);
	mutex_unlock(&venc->enc_lock);

	venc->suspended = false;
	return 0;
}

static int mtk_venc_probe(struct platform_device *pdev)
{
	struct mtk_venc_dev *venc;
	int ret;

	venc = devm_kzalloc(&pdev->dev, sizeof(*venc), GFP_KERNEL);
	if (!venc)
		return -ENOMEM;

	platform_set_drvdata(pdev, venc);

	/*
	 * The single platform device of this driver instance.  Assigned before
	 * anything else so that the runtime-PM and IRQ paths below, which all
	 * refer to venc->pdev or to &pdev->dev, always have a valid owner.
	 */
	venc->pdev = pdev;

	mutex_init(&venc->lock);
	mutex_init(&venc->enc_lock);
	spin_lock_init(&venc->enc_state_lock);

	/*
	 * The two work items are initialised here rather than lazily: the watchdog is
	 * armed per job by device_run() and the completion work is scheduled from the
	 * encoder ISR, both of which can happen before any streaming begins.
	 */
	INIT_DELAYED_WORK(&venc->venc_timeout_work, mtk_venc_timeout);
	INIT_WORK(&venc->venc_complete_work, mtk_venc_complete_work);
	venc->enc_pm_held = false;
	atomic_set(&venc->enc_users, 0);

	/*
	 * One register window, one domain, one clock, one IRQ -- which is what the
	 * split was for.  The old merged node declared two of each and therefore
	 * attached neither power domain (see the file header).
	 *
	 * clock-names is required in the binding: the encoder clock comes from the
	 * vencsys provider, and provider-local ids collide across providers, so
	 * index-based lookup cannot tell them apart.
	 */
	venc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(venc->regs))
		return PTR_ERR(venc->regs);

	venc->venc_clk = devm_clk_get(&pdev->dev, "venc");
	if (IS_ERR(venc->venc_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(venc->venc_clk),
				     "failed to get venc clock\n");

	ret = devm_request_irq(&pdev->dev, platform_get_irq(pdev, 0),
			       mtk_venc_isr, IRQF_TRIGGER_LOW,
			       dev_name(&pdev->dev), pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request venc irq\n");

	/*
	 * Register the V4L2 node last, once every resource it depends on is in place:
	 * a /dev/videoX node that exists while its clocks or IRQ are missing would let
	 * userspace open an encoder that cannot encode.
	 */
	ret = mtk_venc_vf_init(venc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register encoder video device\n");

	pm_runtime_enable(&pdev->dev);

	return 0;
}

/*
 * remove - the platform device is going away.
 *
 * The node is torn down before the runtime-PM domain is: an open file handle holds
 * a runtime-PM reference, and letting the video device go first is what stops a new
 * open from arriving after the resources under it have been released.
 */
static void mtk_venc_remove(struct platform_device *pdev)
{
	struct mtk_venc_dev *venc = dev_get_drvdata(&pdev->dev);

	/*
	 * Settle any watchdog work BEFORE the node goes away, so the completion path
	 * cannot run against a half-torn-down m2m_dev.
	 */
	cancel_delayed_work_sync(&venc->venc_timeout_work);
	cancel_work_sync(&venc->venc_complete_work);

	mtk_venc_vf_deinit(venc);

	pm_runtime_disable(&pdev->dev);
}

static const struct dev_pm_ops mtk_venc_pm_ops = {
	.runtime_suspend = mtk_venc_runtime_suspend,
	.runtime_resume = mtk_venc_runtime_resume,
	.runtime_idle = pm_runtime_idle,
};

static const struct of_device_id mtk_venc_of_match[] = {
	{ .compatible = "mediatek,mt6589-venc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_venc_of_match);

static struct platform_driver mtk_venc_driver = {
	.probe = mtk_venc_probe,
	.remove = mtk_venc_remove,
	.driver = {
		.name = MTK_VENC_DRIVER_NAME,
		.pm = &mtk_venc_pm_ops,
		.of_match_table = mtk_venc_of_match,
	},
};

module_platform_driver(mtk_venc_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6589 H.264/VP8 video encoder front end");
MODULE_LICENSE("GPL");
