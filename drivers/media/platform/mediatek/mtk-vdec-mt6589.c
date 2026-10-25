// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * MediaTek MT6589 multi-standard video decoder -- resource layer and V4L2 node.
 *
 * This is the VDEC half of what used to be one driver covering both directions of
 * the MT6589 video codec.  VDEC is a separate hardware block from VENC: data sheet
 * chapter 59 "Video Decoder" against chapter 60 "H.264/VP8 Video Encoder", base
 * 0x16020000 against 0x17002000, MT_VDEC_IRQ_ID = GIC SPI 140 against MT_VENC_IRQ_ID
 * = SPI 136, the vdecsys clock controller against vencsys, and the VDE power domain
 * against VEN.  The encoder half lives in mtk-venc-mt6589.c.
 *
 * WHAT THIS DRIVER DOES NOW
 * =========================
 * One V4L2 mem-to-mem node, and a real H.264 datapath behind it: a frame handed
 * to device_run() is parsed, loaded into the bitstream FIFO, and started on the
 * VDEC, and the frame-end interrupt retires it with the picture in the CAPTURE
 * buffer.
 *
 * The datapath is transcribed from the vendor LDVT harness
 * (mediatek/platform/mt6589/kernel/drivers/ldvt/vdec/), which is the only
 * register documentation that exists for this block: data sheet chapter 59 has
 * no register section at all.  The harness is *compiled* as MT8580
 * (include/vdec_info_common.h:125, CONFIG_CHIP_VER_CURR 80) while running on
 * MT6589, so every sequence taken from it records which side of which
 * `#if CONFIG_CHIP_VER_*` it came from, and the MT6589 branch is preferred
 * wherever the harness distinguishes them.  Those citations are in the register
 * map and at each function.
 *
 * WHAT IS NOT DONE, and is refused rather than approximated:
 *
 *   - CABAC.  The slice-header alignment and the context-initialisation stroke
 *     are not implemented, and a CABAC frame decoded without them is garbage.  A
 *     CABAC stream is rejected at submit.
 *   - Multi-slice pictures.  Only first_mb_in_slice == 0 is accepted; a picture
 *     whose second slice was silently dropped would be correct at the top and
 *     stale at the bottom.
 *   - Field/MBAFF coding.  frame_mbs_only_flag == 0 is refused.
 *   - Weighted prediction.  The harness only raises a handshake for the weight
 *     table and never writes a coefficient, so there is nothing to program.
 *   - A full H.264 sliding-window DPB.  The reference list is "every previously
 *     decoded picture, most recent first", which is right for num_ref_frames == 0
 *     and for IPPPPP and wrong for multi-reference reordering.  See
 *     mtk_vdec_h264_ref_list().
 *
 * NOT VERIFIED ON HARDWARE.  What has been verified is the parser, on the host,
 * against real x264 and ffmpeg output - which is how the missing
 * dec_ref_pic_marking step below was found, and it was rejecting valid streams.
 * The register sequences have not been run on silicon; the report that
 * accompanied this work lists what to check first when they are.
 *
 * The vendor's own kernel driver (videocodec_kernel_driver.c:1758) is not a model
 * for this: it walks a WRITE_REG_CMD queue that closed userspace libraries
 * assemble, so the decode sequence on a stock ROM lives in userspace, not in the
 * kernel.  This driver is the mainline equivalent of what those libraries do.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/v4l2-controls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "mtk-vdec-mt6589.h"
#include "mtk-vdec-mt6589-dp.h"
#include "mtk-vcodec-mt6589-reg.h"

#define MTK_VDEC_DRIVER_NAME	"mtk-vdec-mt6589"

struct mtk_vdec_dev {
	void __iomem *regs;

	struct clk *vdec_vde_clk;
	struct clk *vdec_smi_clk;

	/*
	 * The decoder's own lock, distinct from vdec_lock below which guards the
	 * power transition and is what runtime PM takes.  This one serialises format
	 * negotiation and open/teardown on the node; it is wired in as vfd->lock.
	 */
	struct mutex node_lock;

	/* Guards the power transition; this is what runtime PM takes. */
	struct mutex vdec_lock;

	/*
	 * Open decoder file handles.  Incremented by mtk_vdec_open() and decremented
	 * by mtk_vdec_release(), so the "dec_users == 0" tests in the runtime PM
	 * paths are not vacuously true.  It does not hold off suspend for a frame
	 * in flight -- no frame is ever in flight, because the decoder has no
	 * datapath -- it stops a system suspend from tearing down an open but idle node.
	 */
	atomic_t dec_users;

	bool suspended;

	/*
	 * Set by mtk_vdec_remove() before it forces the runtime-PM transition.
	 * Removal must power the block down even if a client still has the node
	 * open, because the DRAM it DMAs into is about to be freed.
	 */
	bool removing;

	unsigned int irq_count;

	/* The device this driver instance belongs to. */
	struct platform_device *pdev;

	/*
	 * The V4L2 mem-to-mem node.  Its own v4l2_device, m2m_dev and video_device,
	 * separate from the encoder's: they are separate character devices with
	 * separate open() lifecycles and separate format negotiation, and sharing
	 * would make a decoder client hold the encoder's file operations.
	 *
	 * Its device_run() starts one H.264 frame; see the file header for what is
	 * refused and why.
	 */
	struct video_device		vfd;
	struct v4l2_m2m_dev		*m2m_dev;
	struct v4l2_device		v4l2_dev;

	/*
	 * Whether v4l2_m2m_register_media_controller() succeeded.  Tracked because it
	 * is a no-op returning 0 when there is no media controller, and unregistering
	 * a graph that was never built would corrupt the media device.
	 */
	bool				mc_registered;

	/*
	 * The datapath's DRAM: bitstream FIFO, prediction scratch, reference
	 * pictures, MV and wrapper work areas.  Allocated on the first frame and
	 * held until remove(), because it is not per-open: only one frame can be in
	 * flight on the block, and job_ready() enforces that by reporting busy
	 * until the previous frame retires.
	 */
	struct mtk_vdec_dp		dp;

	/*
	 * Serialises frame-in-flight bookkeeping between the submit path, the
	 * frame-end interrupt and the watchdog.  A spinlock rather than a mutex
	 * because the interrupt handler takes it; the same shape the encoder half
	 * of this driver uses, and for the same reason.
	 */
	spinlock_t			state_lock;

	/* The context whose frame is decoding, or NULL.  Under state_lock. */
	struct mtk_vdec_ctx		*active;

	/*
	 * The deferred completion and its bounded watchdog.
	 *
	 * The interrupt handler cannot return vb2 buffers to the framework itself:
	 * v4l2_m2m_buf_done_and_job_finish() takes a spinlock and can schedule the
	 * next job, neither of which is legal in hard IRQ context.  So the ISR
	 * records that the frame ended and hands the work here.
	 *
	 * The watchdog exists because the frame-end interrupt is the only thing that
	 * can retire a frame, and this driver's entire knowledge of it comes from a
	 * vendor harness: an engine that never signals would otherwise leave the job
	 * and the runtime-PM reference stuck forever, and STREAMOFF would hang in
	 * v4l2_m2m_cancel_job()'s wait_event().
	 */
	struct work_struct		complete_work;
	struct delayed_work		timeout_work;

	/* Whether the interrupt handler saw the frame end.  Under state_lock. */
	bool				frame_done;
	/* Whether the runtime-PM reference for the active frame is held. */
	bool				dp_pm_held;

	/* Set when the hardware reported a decode error for the frame. */
	bool				frame_error;
	u32				frame_err_reg;

	unsigned long			timeout_count;

	/* Decoded and written picture count, for the reference list. */
	u32				pic_count;
};

static inline void mtk_vdec_write(struct mtk_vdec_dev *vdec, u32 off, u32 val)
{
	writel(val, vdec->regs + off);
}

static inline u32 mtk_vdec_read(struct mtk_vdec_dev *vdec, u32 off)
{
	return readl(vdec->regs + off);
}

static int mtk_vdec_power_on(struct mtk_vdec_dev *vdec)
{
	int ret;

	ret = clk_prepare_enable(vdec->vdec_vde_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(vdec->vdec_smi_clk);
	if (ret) {
		clk_disable_unprepare(vdec->vdec_vde_clk);
		return ret;
	}

	return 0;
}

static void mtk_vdec_power_off(struct mtk_vdec_dev *vdec)
{
	clk_disable_unprepare(vdec->vdec_smi_clk);
	clk_disable_unprepare(vdec->vdec_vde_clk);
}

/*
 * Decoder interrupt.
 *
 * There is no status/ack register pair: frame end is bit 16 of MISC word 41, and
 * the interrupt is cleared by setting bits 0 and 4 and then driving bit 4 low
 * again, a two-step sequence on the same word.
 *
 * The second write RE-READS the register and clears bit 4.  This follows the
 * production driver, videocodec_kernel_driver.c:300-301:
 *
 *     VDO_HW_WRITE(VDEC_MISC_BASE + 41*4,
 *                  VDO_HW_READ(VDEC_MISC_BASE + 41*4) | 0x11);
 *     VDO_HW_WRITE(VDEC_MISC_BASE + 41*4,
 *                  VDO_HW_READ(VDEC_MISC_BASE + 41*4) & ~0x10);
 *
 * The LDVT harness instead writes back the value SAVED BEFORE the first write
 * (vdec_hal_if_avs.c:1105-1106).  The two vendor sources genuinely disagree; the
 * production driver's form is used here because that is the code whose behaviour
 * was observed on real silicon.  mtk-vcodec-mt6589-reg.h records both.
 *
 * The acknowledge is unconditional on the way in - it has to be, or a level-high
 * line stays asserted - but the *completion* is only claimed if the frame-end bit
 * was actually set and a frame is in flight.
 *
 * What this handler does not do is retire the frame.  v4l2_m2m_buf_done_and_job_finish()
 * takes a spinlock and can schedule the next job, neither legal here, so the work
 * is handed to mtk_vdec_complete_work().  What it does do is record the error
 * state, because the error registers are only meaningful now: they accumulate
 * during the frame and say nothing after the next one starts.
 */
static irqreturn_t mtk_vdec_isr(int irq, void *data)
{
	struct platform_device *pdev = data;
	struct mtk_vdec_dev *vdec = dev_get_drvdata(&pdev->dev);
	unsigned long flags;
	u32 val, err;

	val = mtk_vdec_read(vdec, VDEC_MISC_FRAME_END);

	/*
	 * No runtime-PM reference is taken here.  The submit path took one and
	 * mtk_vdec_complete() is what drops it, from process context; the access
	 * below is safe anyway because an interrupt from the block means its
	 * clock is already running.
	 */
	if (!(val & VDEC_FRAME_END_BIT))
		return IRQ_NONE;

	mtk_vdec_write(vdec, VDEC_MISC_FRAME_END,
		       val | VDEC_FRAME_END_SET | VDEC_FRAME_END_ACK);
	mtk_vdec_write(vdec, VDEC_MISC_FRAME_END,
		       mtk_vdec_read(vdec, VDEC_MISC_FRAME_END) &
		       ~VDEC_FRAME_END_ACK_CLR);

	vdec->irq_count++;

	/*
	 * A non-zero error accumulator means the parser rejected part of the
	 * bitstream.  The frame still completed, so its buffers are still
	 * written - but what was written is not trustworthy, and reporting it as
	 * DONE is how a decode bug becomes an "image looks weird" bug report.
	 */
	err = mtk_vdec_read(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_ERR_ACCUM);

	spin_lock_irqsave(&vdec->state_lock, flags);
	if (vdec->active && !vdec->frame_done) {
		vdec->frame_done = true;
		if (err) {
			vdec->frame_error = true;
			vdec->frame_err_reg = err;
		}
		schedule_work(&vdec->complete_work);
	}
	spin_unlock_irqrestore(&vdec->state_lock, flags);

	if (err)
		dev_warn_ratelimited(&pdev->dev,
				     "AVC parser reported errors 0x%08x\n", err);

	return IRQ_HANDLED;
}

/* The decoder's accessor, for the same v4l2_fh reason as file2ctx() above. */
#define file2dectx(f)							\
	container_of(file_to_v4l2_fh(f), struct mtk_vdec_ctx, fh)

/*
 * This tree has no file2m2m(); the m2m_dev pointer lives in the driver instance,
 * so reach it through the same video_device the framework does.
 */
#define file2m2m(f) video_drvdata(f)

/*
 * Decoder formats.  A decoder's format roles are the encoder's inverted: the
 * OUTPUT queue carries the compressed bitstream that goes IN, and the CAPTURE
 * queue carries the raw picture that comes OUT.
 *
 * One codec is declared, H.264, because one codec has a datapath.  The hardware
 * block is multi-standard -- the vendor harness carries per-codec tables for
 * MPEG-1/2, MPEG-4, H.263, H.264, WMV, VC-1, RM, VP6, VP8 and AVS -- but the
 * others need a bitstream front end this driver does not have.  In particular VP8
 * is NOT decodable here and is no longer advertised: the block's VP8 path needs
 * probability tables pushed bit-by-bit into parser SRAM before a frame can start,
 * and the retail device decodes VP8 in software (libvp8dec_sa.ca7.so calls
 * vpx_codec_decode()), so there is nothing to gain by pretending.  See the VP8
 * note at the end of mtk-vcodec-mt6589-reg.h.
 *
 * H.264 is declared at V4L2_PIX_FMT_H264, which is what a client must negotiate
 * on the OUTPUT queue for device_run() to accept the stream.
 */

/*
 * Bitstream buffer bounds for the decoder's compressed input.
 *
 * These are bounds on what a client may hand the node, not on any hardware
 * register: the decoder's bitstream FIFO (V_FIFO_SZ, vdec_drv_fileio.h:23) is
 * an internal DRAM buffer the datapath owns, and this driver allocates none of
 * it.  The window below is simply the range of elementary-stream buffer sizes
 * that makes sense for the formats the block takes, used to validate negotiation
 * so a client cannot negotiate a zero-sized or unbounded compressed buffer.
 */
#define MTK_VDEC_BS_SIZE_MIN		(4 * 1024)
#define MTK_VDEC_BS_SIZE_MAX		(4 * 1024 * 1024)
#define MTK_VDEC_BS_SIZE_DEFAULT	(256 * 1024)

/*
 * Plane geometry for a raw NV12 picture, identical in shape to the encoder's
 * mtk_vdec_plane_size(): both dimensions rounded up to a whole macroblock
 * before the planes are divided, because the datapath is macroblock based.
 */
static void mtk_vdec_plane_size(unsigned int w, unsigned int h,
				unsigned long *y_size)
{
	w = round_up(w, MTK_VDEC_MB_SIZE);
	h = round_up(h, MTK_VDEC_MB_SIZE);

	*y_size = (unsigned long)w * h;
}

static void mtk_vdec_src_setup(struct v4l2_format *f)
{
	unsigned long y_size;

	mtk_vdec_plane_size(f->fmt.pix.width, f->fmt.pix.height, &y_size);

	f->fmt.pix.bytesperline = f->fmt.pix.width;
	f->fmt.pix.sizeimage = y_size + y_size / 2;
}

static int mtk_vdec_check_size(unsigned int w, unsigned int h)
{
	if (!w || !h)
		return -EINVAL;

	if (w % 2 || h % 2)
		return -EINVAL;

	if (w > MTK_VDEC_MAX_WIDTH || h > MTK_VDEC_MAX_HEIGHT)
		return -EINVAL;

	return 0;
}

/*
 * ENUMFMT.  Exactly one format per queue, as with the encoder: index 0 is it and
 * anything else does not exist.  Offering a second index the hardware cannot do
 * would be worse than not offering it.
 */
static int mtk_vdec_enum_fmt_cap(struct file *file, void *priv,
				       struct v4l2_fmtdesc *d)
{
	if (d->index)
		return -EINVAL;

	d->flags = 0;
	strscpy(d->description, "NV12", sizeof(d->description));
	d->pixelformat = V4L2_PIX_FMT_NV12;
	return 0;
}

static int mtk_vdec_enum_fmt_out(struct file *file, void *priv,
				       struct v4l2_fmtdesc *d)
{
	if (d->index)
		return -EINVAL;

	d->flags = V4L2_FMT_FLAG_COMPRESSED;
	strscpy(d->description, "H.264 bitstream", sizeof(d->description));
	d->pixelformat = V4L2_PIX_FMT_H264;
	return 0;
}

static int mtk_vdec_g_fmt_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_vdec_ctx *ctx = file2dectx(file);

	*f = ctx->dst_fmt;

	return 0;
}

static int mtk_vdec_g_fmt_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_vdec_ctx *ctx = file2dectx(file);

	*f = ctx->src_fmt;

	return 0;
}

/*
 * TRYFMT for the raw picture out (CAPTURE queue).
 *
 * This is the decoder's counterpart to the encoder's NV12 source, and it has
 * real geometry: width and height are the picture being decoded, validated by
 * mtk_vdec_check_size() against the block's ceiling and rounded up to whole
 * macroblocks by mtk_vdec_src_setup() before the planes are divided.
 */
static int mtk_vdec_try_fmt_cap(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	int ret;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	if (f->fmt.pix.pixelformat != V4L2_PIX_FMT_NV12)
		return -EINVAL;

	ret = mtk_vdec_check_size(f->fmt.pix.width, f->fmt.pix.height);
	if (ret)
		return ret;

	f->fmt.pix.field = V4L2_FIELD_NONE;
	mtk_vdec_src_setup(f);

	return 0;
}

/*
 * TRYFMT for the compressed input (OUTPUT queue).
 *
 * The bitstream is not a picture, so width and height are zeroed rather than
 * echoed: the coded size is not known until the frame is decoded.  sizeimage is
 * the buffer size and the only field with meaning here, bounded so a client
 * cannot negotiate a zero-sized or unbounded compressed buffer.
 */

static int mtk_vdec_try_fmt_out(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	if (f->fmt.pix.sizeimage < MTK_VDEC_BS_SIZE_MIN ||
	    f->fmt.pix.sizeimage > MTK_VDEC_BS_SIZE_MAX)
		return -EINVAL;

	f->fmt.pix.width = 0;
	f->fmt.pix.height = 0;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	f->fmt.pix.field = V4L2_FIELD_NONE;

	return 0;
}

static int mtk_vdec_s_fmt_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_vdec_ctx *ctx = file2dectx(file);
	int ret;

	ret = mtk_vdec_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	ctx->dst_fmt = *f;

	return 0;
}

static int mtk_vdec_s_fmt_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct mtk_vdec_ctx *ctx = file2dectx(file);
	int ret;

	ret = mtk_vdec_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	ctx->src_fmt = *f;

	return 0;
}

static int mtk_vdec_queue_setup(struct vb2_queue *vq,
				      unsigned int *num_buffers,
				      unsigned int *num_planes,
				      unsigned int sizes[],
				      struct device *alloc_devs[])
{
	struct mtk_vdec_ctx *ctx = vq->drv_priv;
	struct device *dev = &ctx->dev->pdev->dev;
	unsigned int min = 2;

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
	 * later.  open() seeds both formats with usable defaults, so this only fires if
	 * a client has explicitly zeroed one.
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
 * Program the AVC parser with a parsed SPS.
 *
 * Field packing transcribed from the vendor harness
 * (hal/vdec_hal_if_h264.c:1176-1197); see the per-field documentation in
 * mtk-vcodec-mt6589-reg.h.  The width/height register is the one field whose
 * location differs between MT8580 and earlier silicon, and MT6589 takes the
 * older AVC_VLD form.
 */
static void mtk_vdec_h264_set_sps(struct mtk_vdec_dev *vdec,
				  const struct mtk_h264_sps *sps)
{
	u32 v;

	v = FIELD_PREP(VDEC_AVC_SPS_CHROMA_FORMAT, sps->chroma_format_idc);
	v |= FIELD_PREP(VDEC_AVC_SPS_LOG2_MAX_FRAME_NUM,
			sps->log2_max_frame_num_minus4);
	v |= FIELD_PREP(VDEC_AVC_SPS_POC_TYPE, sps->pic_order_cnt_type);
	v |= FIELD_PREP(VDEC_AVC_SPS_LOG2_MAX_POC_LSB,
			sps->log2_max_pic_order_cnt_lsb_minus4);
	v |= sps->delta_pic_order_always_zero_flag ?
		VDEC_AVC_SPS_DELTA_POC_ALWAYS_ZERO : 0;
	v |= FIELD_PREP(VDEC_AVC_SPS_NUM_REF_FRAMES, sps->num_ref_frames);
	v |= sps->frame_mbs_only_flag ? VDEC_AVC_SPS_FRAME_MBS_ONLY : 0;
	v |= sps->mb_adaptive_frame_field_flag ?
		VDEC_AVC_SPS_MB_ADAPTIVE_FRAME_FIELD : 0;
	v |= sps->direct_8x8_inference_flag ?
		VDEC_AVC_SPS_DIRECT_8X8_INFERENCE : 0;
	/*
	 * The harness sets bit 21 unconditionally.  Its meaning is not
	 * documented in any source available here, so it is reproduced rather
	 * than explained - if decode misbehaves, this is a candidate.
	 */
	v |= VDEC_AVC_SPS_RESERVED_21;

	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_SPS, v);

	v = FIELD_PREP(VDEC_AVC_PIC_HEIGHT_IN_MBS,
		       sps->pic_height_in_map_units_minus1);
	v |= FIELD_PREP(VDEC_AVC_PIC_WIDTH_IN_MBS,
			sps->pic_width_in_mbs_minus1);
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_PIC_SIZE, v);
}

static void mtk_vdec_h264_set_pps(struct mtk_vdec_dev *vdec,
				  const struct mtk_h264_pps *pps)
{
	u32 v;

	v = pps->entropy_coding_mode_flag ? VDEC_AVC_PPS_ENTROPY_CODING : 0;
	v |= pps->pic_order_present_flag ? VDEC_AVC_PPS_PIC_ORDER_PRESENT : 0;
	v |= pps->weighted_pred_flag ? VDEC_AVC_PPS_WEIGHTED_PRED : 0;
	v |= FIELD_PREP(VDEC_AVC_PPS_WEIGHTED_BIPRED_IDC,
			pps->weighted_bipred_idc);
	v |= FIELD_PREP(VDEC_AVC_PPS_PIC_INIT_QP,
			(pps->pic_init_qp_minus26 & 0x3f));
	v |= FIELD_PREP(VDEC_AVC_PPS_CHROMA_QP_OFFSET,
			(pps->chroma_qp_index_offset & 0x1f));
	v |= pps->deblocking_filter_control_present_flag ?
		VDEC_AVC_PPS_DEBLOCKING_CONTROL : 0;
	v |= pps->constrained_intra_pred_flag ?
		VDEC_AVC_PPS_CONSTRAINED_INTRA : 0;
	v |= pps->transform_8x8_mode_flag ? VDEC_AVC_PPS_TRANSFORM_8X8 : 0;
	v |= FIELD_PREP(VDEC_AVC_PPS_SECOND_CHROMA_QP_OFFSET,
			(pps->second_chroma_qp_index_offset & 0x1f));

	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_PPS_1, v);

	/*
	 * Word 2 carries the scaling-list presence bits, which this driver
	 * never sets: a stream that needs a user scaling list is rejected during
	 * parsing rather than given a flag pointing at a table it does not have.
	 */
	v = FIELD_PREP(VDEC_AVC_PPS_NUM_REF_L0, pps->num_ref_idx_l0_active_minus1);
	v |= FIELD_PREP(VDEC_AVC_PPS_NUM_REF_L1, pps->num_ref_idx_l1_active_minus1);
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_PPS_2, v);
}

static void mtk_vdec_h264_set_slice(struct mtk_vdec_dev *vdec,
				    const struct mtk_h264_slice *slice)
{
	u32 v;

	v = FIELD_PREP(VDEC_AVC_SHDR_FIRST_MB, slice->first_mb_in_slice);
	v |= FIELD_PREP(VDEC_AVC_SHDR_SLICE_TYPE, slice->slice_type);
	v |= slice->field_pic_flag ? VDEC_AVC_SHDR_FIELD_PIC : 0;
	v |= slice->bottom_field_flag ? VDEC_AVC_SHDR_BOTTOM_FIELD : 0;
	v |= slice->direct_spatial_mv_pred_flag ?
		VDEC_AVC_SHDR_DIRECT_SPATIAL_MV : 0;
	v |= FIELD_PREP(VDEC_AVC_SHDR_NUM_REF_L0,
			slice->num_ref_idx_l0_active_minus1);
	v |= FIELD_PREP(VDEC_AVC_SHDR_NUM_REF_L1,
			slice->num_ref_idx_l1_active_minus1);

	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_SHDR_1, v);
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_CUR_PIC_NUM,
		       slice->frame_num);
}

/*
 * Walk the OUTPUT buffer and load every parameter set it carries, then parse
 * the first slice header against them.
 *
 * Returns 0 when the context holds a usable SPS/PPS and a parsed slice, or a
 * negative errno describing what the stream is missing.  Nothing is programmed
 * unless this succeeds, so a partial parse cannot leave half a picture in the
 * registers.
 */
static int mtk_vdec_h264_load_stream(struct mtk_vdec_ctx *ctx,
				     const u8 *data, size_t len)
{
	struct mtk_h264_slice slice;
	size_t pos = 0, nal_start = 0, nal_len = 0;
	bool have_slice = false;
	int ret;

	while (mtk_h264_find_nal(data, len, &pos, &nal_start, &nal_len) == 0) {
		const u8 *nal;
		u8 type;

		if (!nal_len || nal_start >= len)
			continue;

		nal = data + nal_start;
		type = nal[0] & 0x1f;

		switch (type) {
		case MTK_H264_NAL_SPS: {
			struct mtk_h264_sps sps;

			ret = mtk_h264_parse_sps(nal + 1, nal_len - 1, &sps);
			if (ret)
				return ret;
			ctx->ps.sps[sps.id] = sps;
			break;
		}
		case MTK_H264_NAL_PPS: {
			struct mtk_h264_pps pps;

			ret = mtk_h264_parse_pps(nal + 1, nal_len - 1, &pps);
			if (ret)
				return ret;
			ctx->ps.pps[pps.id] = pps;
			break;
		}
		case MTK_H264_NAL_SLICE:
		case MTK_H264_NAL_IDR:
			if (have_slice)
				break;
			ret = mtk_h264_parse_slice(nal + 1, nal_len - 1, type,
						   (nal[0] >> 5) & 0x3,
						   &ctx->ps, &slice);
			if (ret)
				return ret;
			have_slice = true;
			break;
		default:
			break;
		}
	}

	if (!have_slice)
		return -ENODATA;

	ctx->slice = slice;
	return 0;
}

/*
 * Program the picture geometry taken from the SPS the slice refers to.
 * Separate from the parameter-set writes so the caller can resolve the SPS
 * through the slice's PPS.
 */
static int mtk_vdec_h264_program(struct mtk_vdec_ctx *ctx)
{
	struct mtk_vdec_dev *vdec = ctx->dev;
	const struct mtk_h264_pps *pps = &ctx->ps.pps[ctx->slice.pps_id];
	const struct mtk_h264_sps *sps = &ctx->ps.sps[pps->sps_id];

	if (!pps->valid || !sps->valid || !ctx->slice.valid)
		return -ENOENT;

	mtk_vdec_h264_set_sps(vdec, sps);
	mtk_vdec_h264_set_pps(vdec, pps);
	mtk_vdec_h264_set_slice(vdec, &ctx->slice);

	dev_dbg(&vdec->pdev->dev,
		"h264: %ux%u mb=%ux%u refs=%u poc=%u slice_type=%u first_mb=%u\n",
		sps->width, sps->height,
		sps->pic_width_in_mbs_minus1 + 1,
		sps->pic_height_in_map_units_minus1 + 1,
		sps->num_ref_frames, sps->pic_order_cnt_type,
		ctx->slice.slice_type, ctx->slice.first_mb_in_slice);

	return 0;
}

/*
 * Bring the AVC parser out of reset.
 *
 * Sequence from the MT6589 branch of the vendor harness's H.264 bring-up
 * (vdec_hw_h264.c:423-500, the CONFIG_CHIP_VER_CURR < MT8560 arm, which is the
 * branch this silicon takes).  In order:
 *
 *   1. AVC_VLD_CTRL: enable, the "search start code" counter mode, and the
 *      inverse-check flags that are on for this configuration.
 *   2. MC_OPBUF = 6, which the harness writes unconditionally for H.264 with no
 *      explanation.  6 is reproduced.
 *   3. The prediction/scratch address, or 0 when the caller has none.
 *   4. Reset the AVC VLD sum, a two-write strobe.
 *
 * pred_addr is a raw device address (RW_AVLD_PRED_ADDR takes no shift), and 0 is
 * accepted: the harness writes 0 when it has no scratch buffer
 * (vdec_hw_h264.c:458).  This driver always has one, but the parameter keeps the
 * "no buffer" case expressible rather than silently passing a bogus address.
 */
static int mtk_vdec_h264_bring_up(struct mtk_vdec_dev *vdec, u32 pred_addr)
{
	u32 ctrl;

	ctrl = VDEC_AVC_CTRL_EN
	     | VDEC_AVC_CTRL_RDY_WITH_CNT
	     | VDEC_AVC_CTRL_RDY_CNT_THD
	     | VDEC_AVC_CTRL_MEM_PAUSE_EN
	     | VDEC_AVC_CTRL_SUM6_APPEND_INV
	     | VDEC_AVC_CTRL_READ_FLAG_CHK_INV
	     | VDEC_AVC_CTRL_NOT_CHK_DATA_VALID;

	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_CTRL, ctrl);

	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_OPBUF, VDEC_MC_OPBUF_AVC);

	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_PRED_ADDR,
		       pred_addr);

	/* Two-write strobe: assert, then deassert. */
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_RESET_SUM,
		       VDEC_AVC_RESET_SUM_ON);
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_RESET_SUM,
		       VDEC_AVC_RESET_SUM_OFF);

	return 0;
}

/* Busy-wait helpers.  Values from the harness; see each call site. */
#define MTK_VDEC_FETCH_SPIN		0x1000	/* fgVDecWaitVldFetchOk's bound */
#define MTK_VDEC_VBAR_SPIN		0x10000
#define MTK_VDEC_REF_SPIN		0x10000

/*
 * Priming the barrel shifter, which is how the parser is told where the frame's
 * bitstream begins.
 *
 * Transcribed from fgH264VLDInitBarrelShifter(), hal/vdec_hw_h264.c:245-411,
 * taking the MT6589 branches:
 *
 *   - the `#else //for 6589` arm of the byte-alignment loop (:376), NOT the
 *     MT8580 arm above it.  The two differ in the u4TgtByteAddr/i bookkeeping
 *     around the emulation-prevention check, and the 6589 arm is the simpler of
 *     the two: step 8 bits per byte, and consume an extra byte when
 *     RW_AVLD_RM03R reports that the current 3-byte window is 00 00 03.
 *   - the `< MT8580` arm of the VBAR wait (:311-345), which on this chip's
 *     version constants means `& 0x03000003 == 0x01000001` with
 *     MEM_PAUSE_SUPPORT defined (vdec_hw_common.h:28).
 *   - RW_VLD_ASYNC's write-enable (:274), which is `< MT8580` only.  VLD_ASYNC
 *     is a *shared* VLD register, not an AVC_VLD one, so it is written at
 *     VDEC_VLD_BASE + offset like the harness does.
 *
 * The retry loop is the harness's too, and it is not decoration: priming fails
 * when the FIFO's read pointer is not yet backed by valid data, and the harness
 * retries the whole thing up to five times rather than giving up.
 *
 * Returns 0 on success, -ETIMEDOUT if the fetch never completes.  A timeout here
 * is the honest answer to "the parser cannot see the bitstream", and is much
 * better than starting a decode that will produce nothing.
 */
static int mtk_vdec_h264_prime_barrel_shifter(struct mtk_vdec_dev *vdec,
					      dma_addr_t fifo_sa,
					      dma_addr_t rd_ptr,
					      dma_addr_t wr_ptr)
{
	void __iomem *avc = vdec->regs + VDEC_AVC_VLD_BASE;
	void __iomem *vld = vdec->regs + VDEC_VLD_BASE;
	unsigned int attempt, spin;
	u32 v;

	for (attempt = 0; attempt < 5; attempt++) {
		/* WO_VLD_WPTR bit 0: clear the process state. */
		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_WPTR,
			       readl(vld + VDEC_VLD_WPTR) |
			       VDEC_VLD_WPTR_CLEAR);

		/* Twice, exactly as the harness does (:268-269). */
		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_RPTR,
			       (u32)rd_ptr);
		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_RPTR,
			       (u32)rd_ptr);

		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_WPTR,
			       (u32)wr_ptr);
		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_ASYNC,
			       readl(vld + VDEC_VLD_ASYNC) |
			       VDEC_VLD_ASYNC_WR_ENABLE);

		/*
		 * The FIFO's own bounds, in >> 6 units.  The harness sets these
		 * once at bring-up (vVDecSetVLDVFIFO, vdec_hw_common.c:553);
		 * repeating them here costs two writes and makes the priming
		 * self-contained, which matters because the retry loop re-primes
		 * a block whose FIFO may have been reset.
		 */
		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_VSTART,
			       (u32)(fifo_sa >> 6));
		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_VEND,
			       (u32)((fifo_sa + VDEC_VFIFO_SIZE) >> 6));

		mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_RESET_SUM,
			       VDEC_AVC_RESET_SUM_ON);

		/* Start the fetch and wait for it, with the harness's bound. */
		mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_PROC,
			       VDEC_VLD_PROC_INIFET);
		for (spin = 0; spin < MTK_VDEC_FETCH_SPIN; spin++) {
			if (readl(vld + VDEC_VLD_FETCHOK) & VDEC_VLD_FETCH_OK)
				break;
			cpu_relax();
		}
		if (spin == MTK_VDEC_FETCH_SPIN)
			continue;	/* retry the whole handshake */
		break;
	}

	if (attempt == 5) {
		dev_err(&vdec->pdev->dev,
			"barrel shifter never reported a completed fetch\n");
		return -ETIMEDOUT;
	}

	mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_PROC,
		       VDEC_VLD_PROC_INIBR);

	/*
	 * HW workaround, quoted as such by the harness (:305-306): the sum reset
	 * cannot be released until the barrel shifter has finished initialising,
	 * so this waits for VBAR before dropping RESET_SUM.
	 */
	for (spin = 0; spin < MTK_VDEC_VBAR_SPIN; spin++) {
		v = readl(vld + VDEC_VLD_VBAR);
		if ((v & 0x03000003) == 0x01000001)
			break;
		cpu_relax();
	}
	if (spin == MTK_VDEC_VBAR_SPIN)
		dev_warn(&vdec->pdev->dev,
			 "barrel shifter VBAR never matched (0x%08x)\n",
			 readl(vld + VDEC_VLD_VBAR));

	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_RESET_SUM,
		       VDEC_AVC_RESET_SUM_OFF);

	/*
	 * Byte-align: step the shifter 8 bits at a time over however many bytes
	 * the read pointer is past the 16-byte boundary it starts on.
	 *
	 * The emulation-prevention adjustment is the harness's: whenever
	 * RW_AVLD_RM03R says the current window is 00 00 03 (:381), step one more
	 * byte, so the shifter lands after the escape rather than inside it.
	 * Without it every following bit is shifted by 8 for the rest of the
	 * frame.
	 */
	v = (u32)rd_ptr & 0xf;
	while (v) {
		v--;
		if ((readl(avc + VDEC_AVC_VLD_RM03R) &
		     VDEC_AVC_RM03R_FIND_03) && v)
			v--;

		/* Step 8 bits: AVC_VLD's shifter window is RO_AVLD_BARL + 8*4. */
		(void)readl(avc + 8 * 4);
	}

	dev_dbg(&vdec->pdev->dev,
		"barrel shifter primed: rd=%pad wr=%pad window=0x%08x\n",
		&rd_ptr, &wr_ptr, readl(avc));
	return 0;
}

/*
 * Build the reference picture list.
 *
 * MT6589 has no DPB register: the driver fills MC_P_LIST0 (the plane address of
 * each reference) and AVC_VLD_REORD_P0_RPL (its PicNum) itself, having first reset
 * the list and declared how many frame numbers the stream uses.
 *
 * Transcribed from vVDEC_HAL_H264_InitPRefList() and
 * vVDEC_HAL_H264_SetPRefPicListReg(), hal/vdec_hal_if_h264.c:662-826.
 *
 * What is deliberately NOT reproduced, and why:
 *
 *   - The full sliding-window DPB of H.264 8.2.5.  The harness implements it in
 *     its *verify* layer (verify/vdec_verify_vparser_h264.c), not in the HAL, and
 *     the HAL only consumes the resulting list.  This driver keeps the simplest
 *     correct rule instead: every previously decoded picture is a reference, most
 *     recent first.  That is exactly right for a stream with num_ref_frames == 0
 *     (no references at all, which is what baseline x264 emits) and for
 *     IPPPPP with one reference; it is wrong for streams that use several
 *     references with non-trivial reordering.  Saying so is better than pretending
 *     otherwise - see the report and NOTEs.md.
 *   - B lists, field pictures and long-term references.  Not reachable through
 *     this driver's parser: it accepts frame pictures only, and it has no
 *     long-term bookkeeping.
 *
 * cur_pic_num is the current picture's PicNum, which for a frame picture is just
 * its frame_num (vdec_verify_vparser_h264.c:396).
 */
static void mtk_vdec_h264_ref_list(struct mtk_vdec_dev *vdec,
				   struct mtk_vdec_ctx *ctx,
				   const struct mtk_h264_sps *sps,
				   u32 cur_pic_num)
{
	void __iomem *avc = vdec->regs + VDEC_AVC_VLD_BASE;
	u32 max_frame_num = 1u << (sps->log2_max_frame_num_minus4 + 4);
	unsigned int i, nrefs;
	u32 spin;

	/* No B lists: this driver only decodes frame pictures from one list. */
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_AMC_P_LIST0_FLD, 0);

	/* Reset the list, then wait for the hardware to acknowledge. */
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_RESET_PIC_NUM,
		       VDEC_AVC_RESET_PIC_NUM);
	for (spin = 0; spin < MTK_VDEC_REF_SPIN; spin++) {
		if (!(readl(avc + VDEC_AVC_VLD_RESET_PIC_NUM) & 0x1))
			break;
		cpu_relax();
	}

	/* MAX_PIC_NUM takes the frame count doubled for non-frame pictures. */
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_MAX_PIC_NUM,
		       max_frame_num);
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_CUR_PIC_NUM,
		       cur_pic_num);

	/*
	 * Fill the list.  Slot 0 is the most recent picture.  A slot's two halves
	 * are:
	 *
	 *   MC_P_LIST0[i]        the reference plane's luma address
	 *   AVC_VLD_REORD_P0_RPL the reference's PicNum, with bit 19 selecting
	 *                        long-term (0 here: no long-term support)
	 *
	 * Both are written even when there is no reference, so a stale entry from a
	 * previous frame at the same geometry cannot be used by mistake.  With no
	 * reference the address is the current picture's own plane and the count is
	 * zero, which is what an IDR frame needs: nothing in it refers to another
	 * picture.
	 */
	nrefs = (vdec->pic_count < MTK_VDEC_REF_PICS) ? vdec->pic_count
						      : MTK_VDEC_REF_PICS;

	for (i = 0; i < VDEC_MC_P_LIST0_COUNT; i++) {
		u32 picnum = 0;
		dma_addr_t addr;

		if (i < nrefs) {
			/* Most recent first: pic_count-1, -2, ... */
			unsigned int idx = (vdec->pic_count - 1 - i) %
					   MTK_VDEC_REF_PICS;

			addr = mtk_vdec_dp_ref_dma(&vdec->dp, idx);
			/*
			 * Each stored picture's PicNum is its own frame_num.  The
			 * driver does not keep a per-slot frame_num table, so the
			 * numbers are derived from the current one by counting
			 * backwards, which is correct only for the simple case
			 * this driver supports - see the note above.
			 */
			picnum = (cur_pic_num + max_frame_num - (i + 1) % max_frame_num) %
				 max_frame_num;
		} else {
			addr = mtk_vdec_dp_ref_dma(&vdec->dp,
						   vdec->pic_count %
						   MTK_VDEC_REF_PICS);
		}

		mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_P_LIST0 + i * 4,
			       (u32)addr);
		/* Bit 19 clear: short-term reference. */
		mtk_vdec_write(vdec,
			       VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_REORD_P0_RPL + i * 4,
			       picnum & 0x7ffff);
	}

	dev_dbg(&vdec->pdev->dev,
		"ref list: cur_pic_num=%u max_frame_num=%u refs=%u\n",
		cur_pic_num, max_frame_num, nrefs);
}

/*
 * Program the MC/PP planes for the picture about to be decoded and write the
 * output into the CAPTURE buffer.
 *
 * Sources, one per register:
 *
 *   MC_UMV_PIC_WIDTH/HEIGHT  pixels, full frame height even for a field
 *                            (vdec_hal_if_h264.c:1408-1410, u4PicW/u4PicH from
 *                            vdec_verify_vparser_h264.c:325-326)
 *   MC_PP_ENABLE             1 enables post-processing
 *   MC_PP_Y_ADDR             destination luma >> 9      (vdec_hal_if_h264.c:1450)
 *   MC_PP_C_ADDR             destination chroma >> 8    (vdec_hal_if_h264.c:1451)
 *   MC_PP_WB_BY_POST         1: write the picture back   (:1452)
 *   MC_PP_DBLK_MODE          DBLK_Y | DBLK_C             (:1453)
 *   MC_PP_X/Y_RANGE          MB extent minus one         (:1454-1455)
 *   MC_PP_MB_WIDTH           MB width                    (:1456)
 *   MC_ADDRSWAP              ADDRSWAP_OFF                (:1394)
 *
 * The two >> shifts are the harness's and are not interchangeable: 512 for luma
 * and 256 for chroma.  Using one for the other aims the writeback at the wrong
 * plane, which reads back as a picture that is half correct and half the previous
 * frame.
 *
 * The in-loop deblocking filter is always enabled.  Turning it off is a
 * bitstream decision (disable_deblocking_filter_idc == 1) that the harness has
 * commented out at :1440-1443; the driver follows the harness and lets the
 * hardware apply the slice's own parameters, which it reads from the bitstream.
 */
static void mtk_vdec_h264_set_planes(struct mtk_vdec_dev *vdec,
				     const struct mtk_h264_sps *sps,
				     dma_addr_t dst_y, dma_addr_t dst_c)
{
	unsigned int mbx = sps->pic_width_in_mbs_minus1 + 1;
	unsigned int mby = sps->pic_height_in_map_units_minus1 + 1;

	/* Non-frame-coded streams get a doubled height; this driver refuses
	 * those, so the simple form is the one that applies.
	 */
	if (!sps->frame_mbs_only_flag)
		mby *= 2;

	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_UMV_PIC_WIDTH, mbx * 16);
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_UMV_PIC_HEIGHT, mby * 16);

	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_ENABLE, 1);
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_Y_ADDR,
		       (u32)(dst_y >> 9));
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_C_ADDR,
		       (u32)(dst_c >> 8));
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_WB_BY_POST, 1);
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_DBLK_MODE,
		       VDEC_MC_PP_DBLK_Y | VDEC_MC_PP_DBLK_C);
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_X_RANGE, mbx - 1);
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_Y_RANGE, mby - 1);
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_MB_WIDTH, mbx);

	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_ADDRSWAP,
		       VDEC_MC_ADDRSWAP_OFF);

	/*
	 * The two wrapper work areas.  DecStart programs both unconditionally
	 * (:1657-1660) and their contents are not documented anywhere available
	 * here; they are allocated and pointed at because the harness always does.
	 * If this driver decodes to garbage, leaving them at zero is one of the
	 * first alternatives to test.
	 */
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_VLD_WRAPPER,
		       (u32)mtk_vdec_dp_vld_wrap_dma(&vdec->dp));
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_PP_WRAPPER,
		       (u32)mtk_vdec_dp_pp_wrap_dma(&vdec->dp));

	/* Where the MC writes motion vectors for this picture (>> 4). */
	mtk_vdec_write(vdec, VDEC_AVC_MV_BASE + VDEC_AVC_MV_WR_ADDR,
		       (u32)(mtk_vdec_dp_mv_dma(&vdec->dp) >> 4) +
		       vdec->pic_count % MTK_VDEC_REF_PICS);
}

/*
 * Write the slice-header word 2.
 *
 * Packing from hal/vdec_hal_if_h264.c:1524-1541:
 *
 *   [6:0]   slice_qp_delta, 7 bits signed
 *   [8:7]   disable_deblocking_filter_idc, 2 bits
 *   [12:9]  slice_alpha_c0_offset_div2, 4 bits
 *   [16:13] slice_beta_offset_div2, 4 bits
 *   [18:17] nal_ref_idc, 2 bits
 *   [19]    IdrPicFlag
 *   [21:20] cabac_init_idc, 2 bits
 *
 * The AVC_VLD block decodes the slice data itself, but the deblocking and CABAC
 * parameters come from the header, which it is handed - so all of them have to be
 * in this word.
 */
static void mtk_vdec_h264_set_slice2(struct mtk_vdec_dev *vdec,
				     const struct mtk_h264_slice *slice)
{
	u32 v = (u32)slice->slice_qp_delta & 0x7f;

	v |= FIELD_PREP(GENMASK(8, 7), slice->disable_deblocking_filter_idc);
	v |= FIELD_PREP(GENMASK(12, 9),
			(u32)slice->slice_alpha_c0_offset_div2 & 0xf);
	v |= FIELD_PREP(GENMASK(16, 13),
			(u32)slice->slice_beta_offset_div2 & 0xf);
	v |= FIELD_PREP(GENMASK(18, 17), slice->nal_ref_idc & 0x3);
	v |= (u32)(slice->nal_type == MTK_H264_NAL_IDR) << 19;
	v |= FIELD_PREP(GENMASK(21, 20), slice->cabac_init_idc & 0x3);

	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_SHDR_2, v);

	/*
	 * CABAC only: the harness strokes INIT_CTX_SRAM once the header is
	 * written (:1543-1546).  This driver refuses CABAC at submit, so the
	 * write cannot fire today - it is here so that removing that refusal
	 * does not silently leave the context initialisation out.
	 */
	if (slice->cabac_init_idc) {
		mtk_vdec_write(vdec,
			       VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_INIT_CTX_SRAM, 1);
	}
}

/* Longest a frame may take before the watchdog declares the engine wedged. */
#define MTK_VDEC_TIMEOUT_MS	1000

/*
 * Start the pipeline for one frame.
 *
 * Order matters and comes from i4VDEC_HAL_H264_DecStart()
 * (vdec_hal_if_h264.c:1311-1765), with the pieces that are specific to
 * MT8580 removed and the pieces that are MT6589's kept:
 *
 *   1. MC thresholds and picture geometry (:1398-1410).
 *   2. AVC error mask (:1421), which decides which bitstream defects are fatal
 *      and which are concealed.
 *   3. Wrapper switch off for 4:2:0 (:1435) - the `MC_WRAPPER_SWITCH` write is
 *      under `< MT8560`, so it applies here.
 *   4. PP planes and the writeback target (:1445-1465).
 *   5. NBM address-swap mode (:1571), also `>= MT8550`, so it applies.
 *   6. MC bug threshold and the SHDR2 word (:1518, :1541).
 *   7. The stall wait (:1548) before the trigger.
 *   8. AVC_VLD_PROC = 1 (:1763), the trigger.
 *
 * Deliberately skipped, each for a stated reason:
 *
 *   - RW_AVLD_RM03R's "turn off find start code" write (:1489).  The harness
 *     clears BIT(5) (or BIT(9) without MEM_PAUSE_SUPPORT) of a register the
 *     driver has not otherwise established; the same register's BIT(11) is the
 *     00 00 03 detector the barrel-shifter priming reads, so clearing bits in it
 *     blind could disable the very check the priming depends on.
 *   - MC_MODE_CTL (:1478).  FPGA-board tuning, no production value known.
 *   - The DV bank's latency-buffer bypass for narrow pictures (:1580).  VP8/AVS
 *     territory, and the register's meaning is not established for H.264.
 */
static int mtk_vdec_h264_start(struct mtk_vdec_dev *vdec,
			       struct mtk_vdec_ctx *ctx,
			       const struct mtk_h264_sps *sps,
			       const struct mtk_h264_pps *pps,
			       dma_addr_t dst_y, dma_addr_t dst_c)
{
	void __iomem *avc = vdec->regs + VDEC_AVC_VLD_BASE;
	u32 spin, cur_pic_num;
	unsigned int mbx = sps->pic_width_in_mbs_minus1 + 1;
	unsigned int mby = sps->pic_height_in_map_units_minus1 + 1;

	if (!sps->frame_mbs_only_flag) {
		/*
		 * Field-coded streams need the doubled height, MBAFF and the
		 * field reference list.  None of that is implemented, and
		 * starting one anyway would decode a picture into the wrong
		 * geometry rather than failing.
		 */
		return -EOPNOTSUPP;
	}

	/* 1. Thresholds and geometry. */
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_MC_BUSY_THD,
		       0x8000);
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_TIMEOUT_THD,
		       0xffffffff);

	/* 2. Error mask. */
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_ERR_MASK,
		       VDEC_AVC_ERR_MASK_VAL);

	/* 3. Mono wrapper switch: 0 for 4:2:0. */
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_WRAPPER_SWITCH,
		       sps->chroma_format_idc == 0 ? 1 : 0);

	/* 4. Planes and writeback. */
	mtk_vdec_h264_set_planes(vdec, sps, dst_y, dst_c);

	/* 5. Address swap. */
	mtk_vdec_write(vdec, VDEC_MC_BASE + VDEC_MC_NBM_CTRL,
		       VDEC_MC_ADDRSWAP_OFF);

	/* 6. Reference list, then SHDR2. */
	cur_pic_num = ctx->slice.frame_num;
	mtk_vdec_h264_ref_list(vdec, ctx, sps, cur_pic_num);
	mtk_vdec_h264_set_slice2(vdec, &ctx->slice);

	/* 7. Wait for the parser to be unstalled before triggering it. */
	for (spin = 0; spin < MTK_VDEC_VBAR_SPIN; spin++) {
		if (!(readl(avc + VDEC_AVC_VLD_STATUS) & VDEC_AVC_STATUS_STALL))
			break;
		cpu_relax();
	}
	if (spin == MTK_VDEC_VBAR_SPIN) {
		dev_err(&vdec->pdev->dev,
			"AVC parser stalled before the frame could start\n");
		return -EBUSY;
	}

	/* 8. Trigger. */
	mtk_vdec_write(vdec, VDEC_AVC_VLD_BASE + VDEC_AVC_VLD_PROC, 0x1);

	dev_dbg(&vdec->pdev->dev, "frame started: %ux%u mb=%ux%u dst=%pad\n",
		mbx * 16, mby * 16, mbx, mby, &dst_y);
	return 0;
}

/*
 * Retire one frame.
 *
 * The single place a frame's buffers are returned and its runtime-PM reference
 * dropped.  Reached from three directions -- the deferred interrupt work, the
 * watchdog, and job_abort() on STREAMOFF -- and the claim on ->active under
 * state_lock is what makes it happen exactly once: whichever caller finds
 * ->active NULL returns without touching the buffers.
 *
 * @state is VB2_BUF_STATE_DONE for a frame the hardware completed without
 * reporting a decode error, and ERROR otherwise.  ERROR reaches userspace as
 * V4L2_BUF_FLAG_ERROR on DQBUF, which is how a client tells a failed frame from
 * a black one - the CAPTURE buffer would otherwise look like a successfully
 * decoded picture that happens to be garbage.
 *
 * Process context only, and not for style reasons: v4l2_m2m_buf_done_and_job_finish()
 * takes m2m_dev->job_spinlock and can schedule the next job.  That is why the ISR
 * hands off to a work item instead of calling this.
 */
static void mtk_vdec_complete(struct mtk_vdec_dev *vdec,
			      struct mtk_vdec_ctx *ctx,
			      enum vb2_buffer_state state,
			      bool from_timeout)
{
	unsigned long flags;

	spin_lock_irqsave(&vdec->state_lock, flags);
	if (vdec->active != ctx) {
		spin_unlock_irqrestore(&vdec->state_lock, flags);
		return;
	}
	vdec->active = NULL;
	ctx->pending = false;
	vdec->frame_done = false;

	if (!from_timeout) {
		/*
		 * Cancel the watchdog unless this call IS the watchdog:
		 * cancel_delayed_work_sync() from inside the work item would wait
		 * on its own completion and never return.
		 */
		spin_unlock_irqrestore(&vdec->state_lock, flags);
		cancel_delayed_work_sync(&vdec->timeout_work);
		spin_lock_irqsave(&vdec->state_lock, flags);
	}
	spin_unlock_irqrestore(&vdec->state_lock, flags);

	if (state == VB2_BUF_STATE_DONE) {
		/*
		 * The picture is in the CAPTURE buffer, so this is the one point
		 * where the reference store's contents have to be made visible to
		 * the CPU - because the *next* frame may be decoded from this
		 * buffer's plane as a reference, and the cache invalidation that
		 * has to happen before the hardware reads it is the one below.
		 */
		mtk_vdec_dp_sync_to_cpu(&vdec->dp,
					vdec->dp.ref_off,
					vdec->dp.ref_pic_sz * MTK_VDEC_REF_PICS);
		vdec->pic_count++;
	}

	v4l2_m2m_buf_done_and_job_finish(vdec->m2m_dev, ctx->m2m, state);

	if (vdec->dp_pm_held) {
		vdec->dp_pm_held = false;
		pm_runtime_put_autosuspend(&vdec->pdev->dev);
	}
}

/* Deferred completion: the ISR decided a frame ended, this retires it. */
static void mtk_vdec_complete_work(struct work_struct *w)
{
	struct mtk_vdec_dev *vdec =
		container_of(w, struct mtk_vdec_dev, complete_work);
	struct mtk_vdec_ctx *ctx;
	enum vb2_buffer_state state;
	unsigned long flags;

	spin_lock_irqsave(&vdec->state_lock, flags);
	ctx = vdec->active;
	state = vdec->frame_error ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;
	spin_unlock_irqrestore(&vdec->state_lock, flags);

	if (ctx)
		mtk_vdec_complete(vdec, ctx, state, false);
}

static void mtk_vdec_timeout(struct work_struct *w)
{
	struct mtk_vdec_dev *vdec =
		container_of(to_delayed_work(w), struct mtk_vdec_dev,
			     timeout_work);
	struct mtk_vdec_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&vdec->state_lock, flags);
	ctx = vdec->active;
	spin_unlock_irqrestore(&vdec->state_lock, flags);

	if (!ctx)
		return;

	vdec->timeout_count++;
	v4l2_err(&vdec->v4l2_dev,
		 "decoder did not signal frame end within %u ms; failing the frame\n",
		 MTK_VDEC_TIMEOUT_MS);
	mtk_vdec_complete(vdec, ctx, VB2_BUF_STATE_ERROR, true);
}

/*
 * device_run - start one H.264 frame.
 *
 * The job is only started, not finished: the buffers stay on their ready lists
 * and are returned by mtk_vdec_complete() when the frame-end interrupt arrives.
 * That is why every failure path below still ends in a v4l2_m2m_buf_done_and_job_finish()
 * with ERROR - leaving TRANS_RUNNING set would hang STREAMOFF in
 * v4l2_m2m_cancel_job()'s wait_event().
 */
static void mtk_vdec_device_run(void *priv)
{
	struct mtk_vdec_ctx *ctx = priv;
	struct mtk_vdec_dev *vdec = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	const struct mtk_h264_pps *pps;
	const struct mtk_h264_sps *sps;
	const u8 *data;
	size_t data_off, data_len;
	unsigned int data_bits;
	dma_addr_t rd_ptr, wr_ptr, dst_y, dst_c;
	unsigned long flags;
	size_t len;
	bool pm_held = false;
	int ret;

	src_buf = v4l2_m2m_next_src_buf(ctx->m2m);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->m2m);
	if (!src_buf || !dst_buf) {
		v4l2_err(&vdec->v4l2_dev, "device_run without a buffer pair\n");
		goto fail;
	}

	/*
	 * Only H.264 has a datapath.  Checking here as well as at S_FMT is not
	 * redundant: the format can be changed between S_FMT and QBUF, and a VP8
	 * buffer parsed as H.264 finds no start code and fails - which is safe but
	 * reports the wrong reason, so the reason is stated directly.
	 */
	if (ctx->src_fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_H264) {
		v4l2_err(&vdec->v4l2_dev,
			 "only H.264 is decoded by this driver (got '%4s')\n",
			 (const char *)&ctx->src_fmt.fmt.pix.pixelformat);
		goto fail;
	}

	data = vb2_plane_vaddr(&src_buf->vb2_buf, 0);
	len = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	if (!data || !len) {
		v4l2_err(&vdec->v4l2_dev, "empty bitstream buffer\n");
		goto fail;
	}

	ret = mtk_vdec_h264_load_stream(ctx, data, len);
	if (ret) {
		/*
		 * A buffer carrying only parameter sets is not an error: clients
		 * commonly send SPS/PPS separately from the first slice.  Anything
		 * else is reported, because a silently dropped frame is how a
		 * parse bug becomes an inexplicable stall.
		 */
		if (ret != -ENODATA)
			v4l2_err(&vdec->v4l2_dev,
				 "bitstream parse failed: %d\n", ret);
		goto fail;
	}

	pps = &ctx->ps.pps[ctx->slice.pps_id];
	sps = &ctx->ps.sps[pps->sps_id];

	if (!pps->valid || !sps->valid || !ctx->slice.valid) {
		v4l2_err(&vdec->v4l2_dev, "no usable parameter sets\n");
		goto fail;
	}

	/*
	 * CABAC is refused rather than attempted.  The slice-header alignment
	 * and the context initialisation are not implemented (see
	 * mtk_vdec_h264_set_slice2()), and a CABAC frame decoded without them
	 * produces garbage that looks like a hardware fault rather than like a
	 * missing feature.  Baseline streams do not use CABAC.
	 */
	if (pps->entropy_coding_mode_flag) {
		v4l2_err(&vdec->v4l2_dev,
			 "CABAC streams are not supported yet; use a CAVLC stream\n");
		goto fail;
	}

	/*
	 * Only one slice per picture.  A multi-slice picture needs the second
	 * slice's registers programmed against the same reference list and the
	 * same output plane, which this driver does not do; decoding only the
	 * first slice would produce a picture with a correct top and a stale
	 * remainder, so the driver refuses instead.
	 */
	if (ctx->slice.first_mb_in_slice) {
		v4l2_err(&vdec->v4l2_dev,
			 "multi-slice pictures are not supported yet (first_mb=%u)\n",
			 ctx->slice.first_mb_in_slice);
		goto fail;
	}

	/*
	 * Locate slice_data.  The hardware decodes it off the FIFO, so the driver
	 * has to hand it a byte range and a bit count.
	 */
	ret = mtk_h264_slice_data_bits(data, len, ctx->slice.nal_type,
					    &ctx->ps, &ctx->slice,
					    &data_off, &data_len, &data_bits);
	if (ret) {
		v4l2_err(&vdec->v4l2_dev,
			 "cannot find slice data (len=%zu): %d\n", len, ret);
		goto fail;
	}

	/* Where the hardware writes the picture: the CAPTURE buffer, NV12. */
	dst_y = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	dst_c = dst_y + (dma_addr_t)round_up(sps->width, MTK_VDEC_MB_SIZE) *
			 round_up(sps->height, MTK_VDEC_MB_SIZE);

	if (vb2_plane_size(&dst_buf->vb2_buf, 0) <
	    (unsigned long)round_up(sps->width, MTK_VDEC_MB_SIZE) *
			    round_up(sps->height, MTK_VDEC_MB_SIZE) * 3 / 2) {
		v4l2_err(&vdec->v4l2_dev,
			 "capture buffer too small for a %ux%u picture\n",
			 sps->width, sps->height);
		goto fail;
	}

	if (mtk_vdec_dp_alloc(&vdec->pdev->dev, &vdec->dp)) {
		v4l2_err(&vdec->v4l2_dev,
			 "cannot allocate the decoder's DRAM\n");
		goto fail;
	}

	ret = mtk_vdec_dp_fifo_push(&vdec->dp, data + data_off, data_len,
				    &rd_ptr, &wr_ptr);
	if (ret) {
		v4l2_err(&vdec->v4l2_dev,
			 "slice does not fit in the bitstream FIFO: %d\n", ret);
		goto fail;
	}

	/*
	 * Take the runtime-PM reference BEFORE touching any register.
	 *
	 * The completion path drops a reference with pm_runtime_put_autosuspend()
	 * when dp_pm_held is set, and until now nothing ever took one: the
	 * suspend/resume refcount was decremented without ever being incremented.
	 * That is an underflow in the PM core's counter, and its effects are
	 * exactly the kind of unrelated-looking corruption that shows up as a
	 * garbled stack - the core's bookkeeping is a plain int.
	 *
	 * It also has a direct hardware meaning: writing a register while the
	 * block's clocks are gated is undefined behaviour on this SoC.
	 */
	ret = pm_runtime_resume_and_get(&vdec->pdev->dev);
	if (ret) {
		v4l2_err(&vdec->v4l2_dev,
			 "cannot power the decoder: %d\n", ret);
		goto fail;
	}
	pm_held = true;

	mtk_vdec_h264_bring_up(vdec, (u32)mtk_vdec_dp_pred_dma(&vdec->dp));

	ret = mtk_vdec_h264_prime_barrel_shifter(vdec,
						 mtk_vdec_dp_fifo_dma(&vdec->dp),
						 rd_ptr, wr_ptr);
	if (ret)
		goto fail;

	ret = mtk_vdec_h264_program(ctx);
	if (ret) {
		v4l2_err(&vdec->v4l2_dev,
			 "cannot program the AVC parser: %d\n", ret);
		goto fail;
	}

	/*
	 * Publish the job before starting the hardware, so an interrupt that
	 * arrives immediately cannot find nothing to retire.
	 */
	spin_lock_irqsave(&vdec->state_lock, flags);
	ctx->pending = true;
	vdec->active = ctx;
	vdec->frame_done = false;
	vdec->frame_error = false;
	vdec->dp_pm_held = true;
	spin_unlock_irqrestore(&vdec->state_lock, flags);
	/*
	 * The reference now belongs to the completion path: it drops it when
	 * dp_pm_held is set, so this frame must not drop it again.
	 */
	pm_held = false;

	ret = mtk_vdec_h264_start(vdec, ctx, sps, pps, dst_y, dst_c);
	if (ret) {
		spin_lock_irqsave(&vdec->state_lock, flags);
		vdec->active = NULL;
		ctx->pending = false;
		vdec->dp_pm_held = false;
		spin_unlock_irqrestore(&vdec->state_lock, flags);
		v4l2_err(&vdec->v4l2_dev, "cannot start the pipeline: %d\n", ret);
		/*
		 * The job was never published, so nothing will retire it and
		 * nothing will drop the runtime-PM reference taken above.
		 */
		pm_runtime_put_autosuspend(&vdec->pdev->dev);
		goto fail;
	}

	schedule_delayed_work(&vdec->timeout_work,
			      msecs_to_jiffies(MTK_VDEC_TIMEOUT_MS));

	dev_dbg(&vdec->pdev->dev,
		"submitted: %ux%u slice_data %zu bytes + %u bits\n",
		sps->width, sps->height, data_len, data_bits);
	return;

fail:
	/*
	 * Everything from the pm_runtime_resume_and_get() above to the
	 * publication of the job can fail with the reference still held and
	 * dp_pm_held unset, so it has to be dropped here or the device never
	 * suspends again and every later get() leaks too.
	 */
	if (pm_held)
		pm_runtime_put_autosuspend(&vdec->pdev->dev);

	v4l2_m2m_buf_done_and_job_finish(vdec->m2m_dev, ctx->m2m,
					 VB2_BUF_STATE_ERROR);
}

/*
 * job_ready - is there a frame that can be started, and is the block free?
 *
 * Two conditions, and both matter:
 *
 *   - a buffer pair on the ready lists, which is the framework's own question;
 *   - no frame already in flight.  The hardware has one parser, one MC and one
 *     DPB, and this driver allocates a single set of them, so a second
 *     concurrent job would overwrite the first's reference store and its plane
 *     addresses while the first was still being decoded.
 *
 * Returning 0 while a frame is in flight is what makes the m2m core defer the
 * next job until mtk_vdec_complete() clears ->active and calls
 * v4l2_m2m_buf_done_and_job_finish(), which retries the queue.
 */
static int mtk_vdec_job_ready(void *priv)
{
	struct mtk_vdec_ctx *ctx = priv;
	struct mtk_vdec_dev *vdec = ctx->dev;
	unsigned long flags;
	bool busy;

	if (!v4l2_m2m_num_src_bufs_ready(ctx->m2m) ||
	    !v4l2_m2m_num_dst_bufs_ready(ctx->m2m))
		return 0;

	spin_lock_irqsave(&vdec->state_lock, flags);
	busy = vdec->active != NULL;
	spin_unlock_irqrestore(&vdec->state_lock, flags);

	return busy ? 0 : 1;
}

/*
 * job_abort - STREAMOFF while a frame is in flight.
 *
 * The block is still running, so it cannot simply be forgotten: the parser would
 * carry on writing into a CAPTURE buffer that vb2 is about to give back to
 * userspace.  The engine is disabled first, then the job is retired as failed.
 */
static void mtk_vdec_job_abort(void *priv)
{
	struct mtk_vdec_ctx *ctx = priv;
	struct mtk_vdec_dev *vdec = ctx->dev;
	unsigned long flags;
	bool active;

	spin_lock_irqsave(&vdec->state_lock, flags);
	active = vdec->active == ctx;
	spin_unlock_irqrestore(&vdec->state_lock, flags);

	if (!active)
		return;

	/*
	 * Stop the parser before the buffers go away.  PROC's abort bit is the
	 * only quiesce the harness offers (VLD_ABRT, vdec_hw_common.h:239 - a
	 * shared VLD register, so it is at VDEC_VLD_BASE + offset).
	 */
	mtk_vdec_write(vdec, VDEC_VLD_BASE + VDEC_VLD_PROC, VDEC_VLD_PROC_ABRT);

	mtk_vdec_complete(vdec, ctx, VB2_BUF_STATE_ERROR, false);
}

static int mtk_vdec_start_streaming(struct vb2_queue *q,
					  unsigned int count)
{
	return 0;
}

/*
 * stop_streaming - STREAMOFF.
 *
 * Structurally the encoder's, and for the same reason.  Buffers a client queued but that
 * no job ever consumed are still owned by the driver -- vb2 marks them ACTIVE in
 * buf_queue() and only clears that on a buf_done() -- so returning them here is what
 * keeps vb2's post-stop_streaming owned_by_drv_count check satisfied.  Without this,
 * the most ordinary qbuf-then-streamoff sequence WARNs with "driver bug:
 * stop_streaming operation is leaving buffer N in active state".
 *
 * There is never a job to worry about: job_ready() returns 0 unconditionally, so
 * device_run() above cannot have run, and the framework calls v4l2_m2m_cancel_job()
 * before this anyway.
 */
static void mtk_vdec_stop_streaming(struct vb2_queue *q)
{
	struct mtk_vdec_ctx *ctx = q->drv_priv;

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
 * Structurally the encoder's, and for the same two reasons.  vb2_core_queue_init()
 * refuses to build a queue whose vb2_ops has no buf_queue, so without this the
 * decoder node cannot be opened at all; and v4l2_m2m_buf_queue() is what populates
 * the ready lists job_ready() counts.
 *
 * The decoder's job_ready() returns 0 unconditionally, so those lists are read but
 * never acted on: this makes the node openable and its QBUF path functional, and
 * device_run() stays as the refusal it is.  It does not change what this node can
 * do, which is nothing.
 */
static void mtk_vdec_buf_queue(struct vb2_buffer *vb)
{
	struct mtk_vdec_ctx *ctx = vb->vb2_queue->drv_priv;
	struct vb2_v4l2_buffer *vbuf =
		container_of(vb, struct vb2_v4l2_buffer, vb2_buf);

	v4l2_m2m_buf_queue(ctx->m2m, vbuf);
}

static const struct vb2_ops mtk_vdec_qops = {
	.queue_setup		= mtk_vdec_queue_setup,
	.buf_queue		= mtk_vdec_buf_queue,
	.start_streaming	= mtk_vdec_start_streaming,
	.stop_streaming		= mtk_vdec_stop_streaming,
};

static const struct vb2_buf_ops mtk_vdec_buf_ops;

static int mtk_vdec_queue_init(void *priv, struct vb2_queue *src_vq,
				     struct vb2_queue *dst_vq)
{
	struct mtk_vdec_ctx *ctx = priv;
	int ret;

	mutex_init(&ctx->vb_queue_lock);

	src_vq->lock		= &ctx->vb_queue_lock;
	dst_vq->lock		= &ctx->vb_queue_lock;

	src_vq->type		= V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes	= VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv	= ctx;
	src_vq->buf_struct_size	= sizeof(struct v4l2_m2m_buffer);
	src_vq->ops		= &mtk_vdec_qops;
	src_vq->buf_ops		= &mtk_vdec_buf_ops;
	src_vq->mem_ops		= &vb2_dma_contig_memops;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type		= V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes	= VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv	= ctx;
	dst_vq->buf_struct_size	= sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops		= &mtk_vdec_qops;
	dst_vq->buf_ops		= &mtk_vdec_buf_ops;
	dst_vq->mem_ops		= &vb2_dma_contig_memops;

	return vb2_queue_init(dst_vq);
}

/*
 * open - a userspace client starts using the decoder.
 *
 * The per-instance context is created here and torn down in release().  It holds
 * the negotiated formats and nothing else: unlike the encoder there are no
 * driver-owned planes: they belong to the datapath and are allocated by
 * mtk_vdec_dp_alloc(), not per open.
 */
static int mtk_vdec_open(struct file *file)
{
	struct mtk_vdec_dev *vdec = file2m2m(file);
	struct mtk_vdec_ctx *ctx;
	struct v4l2_format *fmt;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = vdec;

	/*
	 * Defaults so a client that never negotiates gets something coherent.  The
	 * capture format deliberately starts with no geometry: queue_setup() refuses a
	 * queue sized against a zero format, so the failure lands where it is
	 * diagnosable rather than as an allocation failure later.
	 */
	fmt = &ctx->src_fmt;
	fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
	fmt->fmt.pix.sizeimage = MTK_VDEC_BS_SIZE_DEFAULT;

	fmt = &ctx->dst_fmt;
	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	/*
	 * 1920x1088 rather than 1920x1080: the decoder is macroblock based, so a
	 * height that is not a whole number of 16-pixel macroblock rows describes a
	 * plane size the datapath would walk past the end of.  This is the block's
	 * documented full-HD working point (draft/ds/venc.txt:1423) at the macroblock
	 * granularity this driver uses everywhere else.
	 */
	fmt->fmt.pix.width = MTK_VDEC_MAX_WIDTH;
	fmt->fmt.pix.height = MTK_VDEC_MAX_HEIGHT;
	mtk_vdec_src_setup(fmt);

	/*
	 * Same shape as the encoder's open(), and for the same reason: file->private_data
	 * has to be a v4l2_fh, because v4l2_m2m_fop_poll() and v4l2_m2m_fop_mmap() read a
	 * m2m context out of it and video_ioctl2() does the same for every ioctl.
	 *
	 * v4l2_fh_init() takes the struct video_device, not the driver's own
	 * device struct.  This used to pass file2m2m(file), which is
	 * video_drvdata() - i.e. the mtk_vdec_dev - so fh->vdev pointed at
	 * the wrong object entirely.  v4l2_fh_init() then dereferenced
	 * vdev->ctrl_handler and set bits in vdev->flags and
	 * vdev->valid_ioctls before v4l2_fh_add() read vdev->prio and took
	 * vdev->fh_lock, all at offsets belonging to unrelated fields.  The
	 * result was the reported v4l2_fh_add() failure at open() time.
	 *
	 * video_devdata() is the accessor that returns the video_device the
	 * file was opened through, which is what the fh has to be linked
	 * into.
	 */
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->fh.m2m_ctx = ctx->m2m = v4l2_m2m_ctx_init(vdec->m2m_dev, ctx,
							&mtk_vdec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		int ret = PTR_ERR(ctx->fh.m2m_ctx);

		ctx->fh.m2m_ctx = NULL;
		ctx->m2m = NULL;
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);

		return ret;
	}

	v4l2_fh_add(&ctx->fh, file);

	/*
	 * dec_users keeps a system suspend from tearing down the decoder's registers
	 * underneath an open but idle node.  Decremented in release().
	 */
	atomic_inc(&vdec->dec_users);

	return 0;
}

static int mtk_vdec_release(struct file *file)
{
	struct mtk_vdec_ctx *ctx = file2dectx(file);
	struct mtk_vdec_dev *vdec = ctx->dev;

	/*
	 * No watchdog and no completion work, unlike the encoder: nothing is ever
	 * started, so no path can still be holding a pointer to this context once the
	 * queues are drained.
	 */
	v4l2_m2m_ctx_release(ctx->m2m);

	/* Unpublish the handle while the context it is embedded in is still alive. */
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);

	kfree(ctx);

	atomic_dec(&vdec->dec_users);

	return 0;
}

static const struct v4l2_file_operations mtk_vdec_fops = {
	.owner		= THIS_MODULE,
	.open		= mtk_vdec_open,
	.release	= mtk_vdec_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static int mtk_vdec_querycap(struct file *file, void *priv,
				   struct v4l2_capability *cap)
{
	struct video_device *vfd = video_devdata(file);

	strscpy(cap->driver, MTK_VDEC_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, MTK_VDEC_DRIVER_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 MTK_VDEC_DRIVER_NAME);

	/*
	 * Leave device_caps alone - v4l_querycap() has already set it from the
	 * video_device and WARNs if the driver disagrees - and mark the
	 * capability set as per-device by including V4L2_CAP_DEVICE_CAPS.
	 *
	 * That bit is not decoration: v4l_querycap() checks that capabilities is
	 * a superset of device_caps | V4L2_CAP_DEVICE_CAPS, so omitting it made
	 * every VIDIOC_QUERYCAP on this node trip that WARN, and userspace could
	 * not tell that device_caps was the authoritative set.
	 */
	cap->capabilities = vfd->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static const struct v4l2_ioctl_ops mtk_vdec_ioctl_ops = {
	.vidioc_querycap		= mtk_vdec_querycap,

	.vidioc_enum_fmt_vid_cap	= mtk_vdec_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out	= mtk_vdec_enum_fmt_out,
	.vidioc_g_fmt_vid_cap	= mtk_vdec_g_fmt_cap,
	.vidioc_g_fmt_vid_out	= mtk_vdec_g_fmt_out,
	.vidioc_try_fmt_vid_cap	= mtk_vdec_try_fmt_cap,
	.vidioc_try_fmt_vid_out	= mtk_vdec_try_fmt_out,
	.vidioc_s_fmt_vid_cap	= mtk_vdec_s_fmt_cap,
	.vidioc_s_fmt_vid_out	= mtk_vdec_s_fmt_out,

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

static const struct v4l2_m2m_ops mtk_vdec_m2m_ops = {
	.device_run	= mtk_vdec_device_run,
	.job_ready	= mtk_vdec_job_ready,
	.job_abort	= mtk_vdec_job_abort,
};

/*
 * The video_device template, mirroring mtk_vcodec_enc_videodev exactly.
 *
 * A static instance is copied into the driver's own storage in vf_init() rather
 * than allocated, because every field in it is a constant.  .vfl_dir = VFL_DIR_M2M
 * makes it a mem-to-mem node, and .minor = -1 asks for an automatic minor.
 */
static struct video_device mtk_vdec_videodev = {
	.name		= MTK_VDEC_DRIVER_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &mtk_vdec_fops,
	.ioctl_ops	= &mtk_vdec_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.device_caps	= V4L2_CAP_STREAMING,
};

/*
 * Bring up the decoder's video_device.
 *
 * Structurally identical to mtk_vcodec_enc_vf_init(), including its failure
 * teardown in reverse order, because the two nodes own their registration in the
 * same way: the v4l2_device is embedded in this driver's private state rather than
 * devm-managed, so a partial failure has to be unwound by hand or the node is left
 * half-built.
 */
static int mtk_vdec_vf_init(struct mtk_vdec_dev *vdec)
{
	struct v4l2_device *v4l2_dev = &vdec->v4l2_dev;
	struct video_device *vfd;
	int ret;

	ret = v4l2_device_register(&vdec->pdev->dev, v4l2_dev);
	if (ret)
		return ret;

	vdec->vfd = mtk_vdec_videodev;
	vfd = &vdec->vfd;

	vfd->lock = &vdec->node_lock;
	vfd->v4l2_dev = v4l2_dev;
	vfd->device_caps |= V4L2_CAP_VIDEO_M2M;
	video_set_drvdata(vfd, vdec);

	vdec->m2m_dev = v4l2_m2m_init(&mtk_vdec_m2m_ops);
	if (IS_ERR(vdec->m2m_dev)) {
		ret = PTR_ERR(vdec->m2m_dev);
		vdec->m2m_dev = NULL;
		goto err_unregister_dev;
	}

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, 0);
	if (ret)
		goto err_release_m2m;

	/*
	 * A failure here is not fatal to the node: the video device is registered and
	 * usable through the streaming ioctls regardless, it just has no graph
	 * representation.  Reported rather than allowed to fail probe over something
	 * userspace can still reach.  mc_registered tracks it so teardown matches setup.
	 */
	ret = v4l2_m2m_register_media_controller(vdec->m2m_dev, vfd,
						MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_warn(v4l2_dev,
			  "failed to register m2m media controller: %d\n", ret);
		vdec->mc_registered = false;
	} else {
		vdec->mc_registered = true;
	}

	v4l2_info(v4l2_dev, "decoder registered as /dev/video%d\n", vfd->num);

	return 0;

err_release_m2m:
	v4l2_m2m_release(vdec->m2m_dev);
	vdec->m2m_dev = NULL;
err_unregister_dev:
	v4l2_device_unregister(v4l2_dev);

	return ret;
}

/*
 * Undo mtk_vdec_vf_init(), in the same reverse order as the encoder's
 * equivalent: node first so no new open can race the teardown, then the media
 * controller, then the m2m context, then the v4l2_device.
 */
static void mtk_vdec_vf_deinit(struct mtk_vdec_dev *vdec)
{
	if (vdec->m2m_dev) {
		if (vdec->mc_registered)
			v4l2_m2m_unregister_media_controller(vdec->m2m_dev);
		vdec->mc_registered = false;
		video_unregister_device(&vdec->vfd);
	}

	v4l2_m2m_release(vdec->m2m_dev);
	vdec->m2m_dev = NULL;

	v4l2_device_unregister(&vdec->v4l2_dev);
}
static int mtk_vdec_runtime_suspend(struct device *dev)
{
	struct mtk_vdec_dev *vdec = dev_get_drvdata(dev);

	mutex_lock(&vdec->vdec_lock);
	/*
	 * Normally only power down when no client holds the node open: a system
	 * suspend must not tear the registers out from under an idle decoder
	 * that is still owned.  Removal is the exception - the driver is going
	 * away and the DRAM is about to be freed, so the block has to be
	 * stopped regardless of who has it open.
	 */
	if (atomic_read(&vdec->dec_users) == 0 || vdec->removing)
		mtk_vdec_power_off(vdec);
	mutex_unlock(&vdec->vdec_lock);

	vdec->suspended = true;
	return 0;
}

static int mtk_vdec_runtime_resume(struct device *dev)
{
	struct mtk_vdec_dev *vdec = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&vdec->vdec_lock);
	/*
	 * Always power on here, for the same reason as the encoder: gating this
	 * on there being no open handle inverted the sense of it, leaving the
	 * block running with its clock off for the whole life of the node - and
	 * the interrupt handler reads those registers.  clk_prepare_enable() is
	 * reference counted and suspend only powers off when nothing is open, so
	 * the count cannot leak.
	 */
	ret = mtk_vdec_power_on(vdec);
	if (ret) {
		mutex_unlock(&vdec->vdec_lock);
		return ret;
	}
	mutex_unlock(&vdec->vdec_lock);

	vdec->suspended = false;
	return 0;
}

static int mtk_vdec_probe(struct platform_device *pdev)
{
	struct mtk_vdec_dev *vdec;
	int ret;

	vdec = devm_kzalloc(&pdev->dev, sizeof(*vdec), GFP_KERNEL);
	if (!vdec)
		return -ENOMEM;

	platform_set_drvdata(pdev, vdec);

	vdec->pdev = pdev;

	mutex_init(&vdec->node_lock);
	mutex_init(&vdec->vdec_lock);
	spin_lock_init(&vdec->state_lock);
	atomic_set(&vdec->dec_users, 0);

	INIT_WORK(&vdec->complete_work, mtk_vdec_complete_work);
	INIT_DELAYED_WORK(&vdec->timeout_work, mtk_vdec_timeout);

	/*
	 * One register window, one domain, one pair of clocks, one IRQ -- which is
	 * what the split from the merged vcodec node was for.  The old node declared
	 * two domains, and genpd_dev_pm_attach() attaches a domain only when there is
	 * exactly one phandle, so it got no domain and no deferred probe at all.
	 *
	 * clock-names is required in the binding: both decoder clocks come from the
	 * vdecsys provider, and provider-local ids collide across providers, so
	 * index-based lookup cannot tell them apart.
	 */
	vdec->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vdec->regs))
		return PTR_ERR(vdec->regs);

	vdec->vdec_vde_clk = devm_clk_get(&pdev->dev, "vdec-vde");
	if (IS_ERR(vdec->vdec_vde_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(vdec->vdec_vde_clk),
				     "failed to get vdec vde clock\n");

	vdec->vdec_smi_clk = devm_clk_get(&pdev->dev, "vdec-smi");
	if (IS_ERR(vdec->vdec_smi_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(vdec->vdec_smi_clk),
				     "failed to get vdec smi clock\n");

	ret = devm_request_irq(&pdev->dev, platform_get_irq(pdev, 0),
			       mtk_vdec_isr, IRQF_TRIGGER_HIGH,
			       dev_name(&pdev->dev), pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request vdec irq\n");

	/*
	 * Register the V4L2 node last, once every resource it depends on is in
	 * place.  A /dev/videoX that exists while its clocks or IRQ are missing
	 * would let userspace open a decoder that cannot decode while appearing
	 * healthy, and the datapath's first symptom would be a hung job rather than
	 * a probe failure.
	 */
	ret = mtk_vdec_vf_init(vdec);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register decoder video device\n");

	pm_runtime_enable(&pdev->dev);

	return 0;
}

/*
 * remove - the platform device is going away.
 *
 * The node is torn down before the runtime-PM domain is: letting the video device
 * go first is what stops a new open from arriving after the resources under it
 * have been released.
 */
static void mtk_vdec_remove(struct platform_device *pdev)
{
	struct mtk_vdec_dev *vdec = dev_get_drvdata(&pdev->dev);

	mtk_vdec_vf_deinit(vdec);

	/*
	 * Settle the completion machinery before the DRAM goes away, and in this
	 * order: cancel the watchdog and wait for any queued completion first, so
	 * neither can be running while the buffers and the reference store are
	 * freed underneath them.
	 */
	cancel_delayed_work_sync(&vdec->timeout_work);
	cancel_work_sync(&vdec->complete_work);

	/*
	 * Put the device back into suspend before freeing anything it can DMA
	 * to.  pm_runtime_disable() only stops *new* resumes - it does not run
	 * the suspend callback of a device that is currently active, so the
	 * clocks stay on, the hardware stays enabled, and dp_free() hands a
	 * DMA-capable region back to the allocator while the block can still
	 * write to it.  Force the transition first; the power-off in
	 * mtk_vdec_runtime_suspend() is what actually quiesces it.
	 */
	vdec->removing = true;
	pm_runtime_force_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	mtk_vdec_dp_free(&pdev->dev, &vdec->dp);
}

static const struct dev_pm_ops mtk_vdec_pm_ops = {
	.runtime_suspend = mtk_vdec_runtime_suspend,
	.runtime_resume = mtk_vdec_runtime_resume,
	.runtime_idle = pm_runtime_idle,
};

static const struct of_device_id mtk_vdec_of_match[] = {
	{ .compatible = "mediatek,mt6589-vdec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_vdec_of_match);

static struct platform_driver mtk_vdec_driver = {
	.probe = mtk_vdec_probe,
	.remove = mtk_vdec_remove,
	.driver = {
		.name = MTK_VDEC_DRIVER_NAME,
		.pm = &mtk_vdec_pm_ops,
		.of_match_table = mtk_vdec_of_match,
	},
};

module_platform_driver(mtk_vdec_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6589 video decoder front end");
MODULE_LICENSE("GPL");
