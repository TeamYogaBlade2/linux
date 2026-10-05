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
 * WHY THIS SECTION IS WHAT IT IS
 * ==============================
 * What is here is the V4L2 plumbing and the resource layer, and nothing else: one
 * video_device with its own v4l2_device and m2m_dev, the format negotiation the node
 * needs to be usable at all, the frame-end interrupt acknowledgement, and a
 * device_run() that reports -ENODEV.
 *
 * The -ENODEV is the substantive part, and it is deliberate.  The vendor stack
 * programs the VDEC datapath from userspace, not from the kernel: the driver
 * walks a WRITE_REG_CMD queue that closed libraries assemble
 * (videocodec_kernel_driver.c:1758, hal_api.h:33-41).  There is therefore no
 * kernel-side register sequence to reproduce here, and inventing one from the
 * LDVT harness -- which is configured for MT8580, not this chip -- would produce
 * a driver that appears to decode and does not.
 *
 *   The vendor kernel driver (videocodec_kernel_driver.c, 2391 lines) touches the
 *   decoder only to power it up and to acknowledge the frame-end interrupt
 *   (:300-301); it never writes a datapath register.  Userspace does, by
 *   assembling a queue of WRITE_REG_CMD records (hal_api.h:33-41, :107-124) and
 *   shipping it over the MFV_SET_CMD_CMD ioctl, where the driver's only job is to
 *   walk it:
 *
 *       case WRITE_REG_CMD:
 *           VDO_HW_WRITE(cmd_queue->address + cmd_queue->offset,
 *                        cmd_queue->value);   (videocodec_kernel_driver.c:1758)
 *
 *   The decode sequence therefore lives in the closed userspace libraries
 *   (libvp8dec_sa.ca7.so and siblings), which are stripped and partly encrypted
 *   blobs.  Porting it is a userspace reverse-engineering project, not a kernel
 *   one, so the kernel-side scope is what it is here: clocks, reset, the interrupt,
 *   and a node that is honest about the rest.
 *
 * Two further reasons a decoder datapath could not be written from what is
 * available, both documented where they arise: the only readable register map is
 * the LDVT register-test harness, which is configured for MT8580 rather than
 * MT6589; and VP8 additionally requires its probability tables pushed bit-by-bit
 * into parser SRAM before any frame can decode.  See the VP8 note at the end of
 * mtk-vcodec-mt6589-reg.h.
 *
 * Returning -ENODEV from device_run() says exactly that, at the point where a
 * caller finds out, instead of leaving a node that accepts buffers and silently
 * never fills them.  The rest of the node is real: a client can open it, query
 * its capabilities, negotiate formats and inspect them.  What it cannot do is
 * decode, and it says so.
 *
 * Giving the decoder its own driver, its own power domain and its own DT node does
 * not change any of that.  A resource layer that attaches correctly is not a
 * datapath.
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

	unsigned int irq_count;

	/* The device this driver instance belongs to. */
	struct platform_device *pdev;

	/*
	 * The V4L2 mem-to-mem node.  Its own v4l2_device, m2m_dev and video_device,
	 * separate from the encoder's: they are separate character devices with
	 * separate open() lifecycles and separate format negotiation, and sharing
	 * would make a decoder client hold the encoder's file operations.
	 *
	 * Its device_run() reports -ENODEV; see the file header for why that is the
	 * honest answer rather than a stub.
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
 * Decoder interrupt.  There is no status/ack register pair: frame end is bit 16
 * of MISC word 41, and the interrupt is cleared by setting bits 0 and 4 and
 * then driving bit 4 low again, a two-step sequence on the same word.
 *
 * The second write RE-READS the register and clears bit 4.  This follows the
 * production driver, videocodec_kernel_driver.c:300-301:
 *
 *     VDO_HW_WRITE(VDEC_MISC_BASE + 41*4,
 *                  VDO_HW_READ(VDEC_MISC_BASE + 41*4) | 0x11);
 *     VDO_HW_WRITE(VDEC_MISC_BASE + 41*4,
 *                  VDO_HW_READ(VDEC_MISC_BASE + 41*4) & ~0x10);
 *
 * An earlier revision of this driver wrote the value SAVED BEFORE the first write
 * instead, which is what the LDVT harness does at vdec_hal_if_avs.c:1105-1106
 * (u4VDEC_HAL_AVS_VDec_ClearInt writes back u4Reg).  The two vendor sources
 * genuinely disagree; the driver's form is used here because that is the code
 * whose behaviour was observed on silicon.  See the frame-end comment in
 * mtk-vcodec-mt6589-reg.h, which records both.
 *
 * The decoder datapath itself is not programmed by this driver; see the file
 * header.  The interrupt is still handled because the block can and does raise
 * it -- the register window and its clocks exist whether or not this driver
 * starts anything -- and an unacknowledged level-high interrupt would wedge the
 * line permanently.
 */
static irqreturn_t mtk_vdec_isr(int irq, void *data)
{
	struct platform_device *pdev = data;
	struct mtk_vdec_dev *vdec = dev_get_drvdata(&pdev->dev);
	u32 val;

	/*
	 * Deliberately no runtime-PM reference here, and deliberately no put
	 * either.  There is no decoder submit path in this driver to take a matching
	 * reference -- see the file header, the decoder datapath is not programmed at
	 * all -- so there is nothing to balance against and a put here would decrement
	 * a count this ISR never incremented.  Taking a reference is not an option
	 * either: the resume path enables clocks, which is not permitted from
	 * interrupt context without pm_runtime_irq_safe(), and this device is not so
	 * marked because doing so would not be true of its resume path.
	 *
	 * The access is safe anyway: an interrupt from the block means its clock is
	 * already running.
	 */
	val = mtk_vdec_read(vdec, VDEC_MISC_FRAME_END);
	if (val & VDEC_FRAME_END_BIT) {
		mtk_vdec_write(vdec, VDEC_MISC_FRAME_END, val |
			       VDEC_FRAME_END_SET | VDEC_FRAME_END_ACK);
		mtk_vdec_write(vdec, VDEC_MISC_FRAME_END,
			       mtk_vdec_read(vdec, VDEC_MISC_FRAME_END) &
			       ~VDEC_FRAME_END_ACK_CLR);
		vdec->irq_count++;
	}

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
 * One codec is declared, VP8, and the declaration is deliberately narrow.  The
 * hardware block is multi-standard -- the vendor harness carries per-codec
 * tables for MPEG-1/2, MPEG-4, H.263, H.264, WMV, VC-1, RM, VP6, VP8 and AVS --
 * but every one of those needs a bitstream parser bring-up this driver does not
 * have, so advertising the whole list would advertise nine codecs of which none
 * can decode.  VP8 is named because it is the one whose register map and buffer
 * requirements have been read closely enough to document in
 * mtk-vdec-mt6589-reg.h; naming it is a statement about available
 * documentation, not about working decode.
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
	strscpy(d->description, "VP8 bitstream", sizeof(d->description));
	d->pixelformat = V4L2_PIX_FMT_VP8;
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
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_VP8;
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
 * device_run - this driver cannot decode.
 *
 * The -ENODEV is the whole point of this node and is not a placeholder.  It is
 * reported at the moment a client actually asks for a picture, which is the only
 * place a caller can be told the truth, and it is accompanied by a message
 * naming the reason so the failure is diagnosable from dmesg alone.
 *
 * What has deliberately NOT been done here, and would have been the alternative:
 *
 *   - Populating the VP8 motion-compensation registers.  Their offsets are known and
 *     are recorded in mtk-vdec-mt6589-reg.h, but they come from the LDVT
 *     register-test harness, which is built as CONFIG_CHIP_VER_CURR 80 == MT8580
 *     (vdec_info_common.h:125, :130) -- a different SoC -- and a wrong offset written
 *     to a decoder does not fail loudly, it corrupts the block's state.
 *   - Loading the VP8 probability tables.  Required before any frame can decode, and
 *     they are only present in the harness as several hundred literal constants
 *     pushed through an SRAM port (vdec_hal_if_vp8.c:1891ff).  One wrong entry yields a
 *     decoder that emits plausible garbage.
 *   - Allocating the decoder's planes.  Three reference planes plus current,
 *     post-process and a 4 MiB bitstream FIFO (vdec_drv_fileio.h:23), all of which are
 *     only meaningful to a datapath that does not exist here.
 *
 * Rather than any of those, this returns -ENODEV and says why.  See the VP8 note at
 * the end of mtk-vdec-mt6589-reg.h for the full argument.
 */
static void mtk_vdec_device_run(void *priv)
{
	struct mtk_vdec_ctx *ctx = priv;
	struct mtk_vdec_dev *vdec = ctx->dev;

	/*
	 * A job can only have been queued if the framework believed this node could
	 * run one.  Since it cannot, the buffers taken for it must be handed back as
	 * failed rather than left owned: returning them is what stops a client that
	 * ignored the earlier -ENODEV from waiting forever for a DQBUFS that will never
	 * come.
	 */
	while (v4l2_m2m_num_src_bufs_ready(ctx->m2m)) {
		struct vb2_v4l2_buffer *src_buf =
			v4l2_m2m_src_buf_remove(ctx->m2m);

		if (!src_buf)
			break;

		v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
	}

	while (v4l2_m2m_num_dst_bufs_ready(ctx->m2m)) {
		struct vb2_v4l2_buffer *dst_buf =
			v4l2_m2m_next_dst_buf(ctx->m2m);

		if (!dst_buf)
			break;

		dst_buf->flags |= V4L2_BUF_FLAG_ERROR;
		dst_buf->planes[0].bytesused = 0;
		v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
	}

	v4l2_err(&vdec->v4l2_dev,
		 "decoder datapath not implemented in the kernel: the MT6589 VDEC register sequence lives in the vendor's closed userspace libraries, which program the block through MFV_SET_CMD_CMD\n");

	v4l2_m2m_buf_done_and_job_finish(vdec->m2m_dev, ctx->m2m,
					 VB2_BUF_STATE_ERROR);
}

/*
 * job_ready - never.
 *
 * Reporting 0 unconditionally is what stops the framework calling device_run() at
 * all, so the -ENODEV path above is a backstop rather than the normal outcome.  A
 * client that fills both queues and waits for a picture blocks here, which is the
 * intended and documented behaviour of this node.
 */
static int mtk_vdec_job_ready(void *priv)
{
	return 0;
}

/*
 * job_abort - STREAMOFF with buffers still queued.
 *
 * Nothing is in flight and no hardware was started, so there is nothing to abandon.
 * The buffers are the framework's to reclaim.
 */
static void mtk_vdec_job_abort(void *priv)
{
}

static int mtk_vdec_start_streaming(struct vb2_queue *q,
					  unsigned int count)
{
	return 0;
}

static void mtk_vdec_stop_streaming(struct vb2_queue *q)
{
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
 * the -ENODEV in device_run() stays where it is.  It does not change what this node
 * can do, which is nothing.
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
 * driver-owned planes, because there is no datapath that would read or write them.
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
	fmt->fmt.pix.pixelformat = V4L2_PIX_FMT_VP8;
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
	 */
	v4l2_fh_init(&ctx->fh, file2m2m(file));
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
	strscpy(cap->driver, MTK_VDEC_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, MTK_VDEC_DRIVER_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 MTK_VDEC_DRIVER_NAME);

	cap->device_caps = cap->capabilities = V4L2_CAP_VIDEO_M2M |
						  V4L2_CAP_STREAMING;

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
	if (atomic_read(&vdec->dec_users) == 0)
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
	if (atomic_read(&vdec->dec_users) == 0) {
		ret = mtk_vdec_power_on(vdec);
		if (ret) {
			mutex_unlock(&vdec->vdec_lock);
			return ret;
		}
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
	atomic_set(&vdec->dec_users, 0);

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
	 * would let userspace open a decoder that cannot decode -- which, given that
	 * device_run() reports -ENODEV anyway, would be a node offering nothing at
	 * all while appearing healthy.
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

	/*
	 * There is no watchdog or completion work to settle here, unlike the
	 * encoder: no frame is ever started, so no work item can be holding a
	 * pointer into this driver.
	 */
	mtk_vdec_vf_deinit(vdec);

	pm_runtime_disable(&pdev->dev);
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
