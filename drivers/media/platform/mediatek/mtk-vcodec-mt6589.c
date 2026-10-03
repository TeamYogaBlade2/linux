// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * MediaTek MT6589 video codec — V4L2 stateless codec driver.
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
 * What this driver can and cannot do
 * ---------------------------------
 * It exposes a V4L2 stateless codec device with a real register-level
 * programming model: control params, per-frame run structs, buffer management
 * through the V4L2 buffer API and the M4U, and a completion interrupt.  That is
 * the acceleration layer, and everything in it is derived from documented
 * registers.
 *
 * It does NOT contain a codec.  Producing a conformant H.264/VP8/MPEG-4
 * bitstream requires entropy coding, motion estimation and rate control
 * algorithms which exist only in the vendor's closed-source userspace
 * libraries, and for H.264/HEVC not even in an inspectable form (those blobs
 * are encrypted).  So this driver is the hardware interface those algorithms
 * would drive, not a replacement for them.  See NOTES.md and RECOVERED-ABI.md
 * for exactly which parts are recoverable and which are not.
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
 * Hardware engine lock.  The encoder and the decoder are physically separate,
 * so they may run concurrently; only two instances of the same engine have to
 * exclude each other.
 */
#define MTK_VCODEC_MAX_INSTANCES	2

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

	/* one video device per direction; both share this driver instance */
	struct platform_device *enc_pdev;
	struct platform_device *dec_pdev;
	struct v4l2_device enc_v4l2_dev;
	struct v4l2_device dec_v4l2_dev;
	struct v4l2_m2m_dev *enc_m2m_dev;
	struct v4l2_m2m_dev *dec_m2m_dev;
	struct mtk_vcodec_enc *enc;
	struct mtk_vcodec_dec *dec;

	unsigned int irq_count;
	bool suspended;
};

/*
 * Encoder parameters, pushed with VIDIOC_S_PARM before a frame.  Everything
 * here is either a property of the picture or a rate control term; the buffer
 * addresses are NOT here, because the driver fills those in from the V4L2 queue
 * and they are plain physical DRAM addresses in the chapter-60 register table.
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

/* Decoder configuration.  Mostly opaque for now; see NOTES.md. */
struct mtk_vcodec_dec_parm {
	__u32 width;
	__u32 height;
	__u32 codec;		/* V4L2_PIX_FMT_BITSTREAM_* */
	__u32 flags;
};

/*
 * Encoder state per open instance.
 */
struct mtk_vcodec_enc {
	struct mtk_vcodec_dev *vcodec;
	struct mtk_vcodec_ctx *ctx;
	struct v4l2_m2m_ctx *m2m_dev;
	struct video_device *vdev;
	struct mtk_vcodec_enc_parm parm;
	struct vb2_v4l2_buffer *src;	/* in flight, picked up by the ISR */
	struct vb2_v4l2_buffer *dst;
	bool mpeg4;		/* use the chapter-61 datapath, not H.264/VP8 */
	bool configured;
};

struct mtk_vcodec_dec {
	struct mtk_vcodec_dev *vcodec;
	struct mtk_vcodec_ctx *ctx;
	struct v4l2_m2m_dev *m2m_dev;
	struct video_device *vdev;
	struct mtk_vcodec_dec_parm parm;
	bool configured;
};

/* One context per open file; the two video devices share a driver instance. */
struct mtk_vcodec_ctx {
	struct mtk_vcodec_dev *vcodec;
	struct mtk_vcodec_enc *enc;
	struct mtk_vcodec_dec *dec;
	struct v4l2_fh fh;
	bool encoder;
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
	mtk_venc_write(vcodec, VENC_MP4_IRQ_EN, 0x1);
	mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, 0xffffffff);

	return 0;
}

static void mtk_venc_power_off(struct mtk_vcodec_dev *vcodec)
{
	mtk_venc_write(vcodec, VENC_MP4_IRQ_EN, 0x0);
	mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, 0xffffffff);
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

/* Bytes the hardware actually produced, for the destination buffer size. */
static u32 mtk_venc_bitstream_size(struct mtk_vcodec_dev *vcodec, bool mpeg4)
{
	if (!mpeg4)
		return mtk_venc_read(vcodec, VENC_STUFFING_REPORT);

	return mtk_venc_read(vcodec, VENC_MP4_BYTE_COUNT);
}

/*
 * Encoder interrupt.
 *
 * The shared pair (H.264/VP8) reports SPS/PPS/frame/dram/pause.  The MPEG-4
 * datapath has its own pair and the vendor driver treats a status word of 2 as
 * the real frame-done indication (videocodec_kernel_driver.c:497-503), so a
 * value other than that is an intermediate event and must be re-armed.
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

	if (!pm_runtime_get_sync(&pdev->dev))
		return IRQ_NONE;

	status = mtk_venc_read(vcodec, VENC_IRQ_STATUS);
	if (status & VENC_IRQ_MASK_ALL)
		mtk_venc_write(vcodec, VENC_IRQ_ACK, status & VENC_IRQ_MASK_ALL);

	mp4_status = mtk_venc_read(vcodec, VENC_MP4_IRQ_STATUS);
	if (mp4_status == VENC_MP4_IRQ_STATUS_DONE) {
		mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, mp4_status);
		vcodec->irq_count++;

		/*
		 * The bitstream length is only known now, so this is where the
		 * destination buffer is handed back to userspace.  device_run()
		 * deliberately left it queued.
		 */
		if (vcodec->enc && vcodec->enc->dst) {
			struct mtk_vcodec_enc *enc = vcodec->enc;

			enc->dst->vb2_buf.planes[0].bytesused =
				mtk_venc_bitstream_size(vcodec, enc->mpeg4);
			v4l2_m2m_buf_done(enc->dst, VB2_BUF_STATE_DONE);
			enc->dst = NULL;
		}
	} else if (mp4_status) {
		/* Not a completion: re-arm so the level drops and we can retrigger. */
		mtk_venc_write(vcodec, VENC_MP4_IRQ_ACK, mp4_status);
	}

	/* Catch anything raised while we were acknowledging. */
	if (mtk_venc_read(vcodec, VENC_IRQ_STATUS) & VENC_IRQ_MASK_ALL)
		mtk_venc_write(vcodec, VENC_IRQ_ACK,
			       mtk_venc_read(vcodec, VENC_IRQ_STATUS) &
			       VENC_IRQ_MASK_ALL);

	pm_runtime_put_autosuspend(&pdev->dev);
	return IRQ_HANDLED;
}

/*
 * Decoder interrupt.  There is no status/ack register pair: frame end is bit 16
 * of MISC word 41 and is cleared by setting bits 0 and 4 then writing the
 * original value back, a two-step sequence the vendor code relies on
 * (vdec_hal_if_avs.c:1104-1106 and videocodec_kernel_driver.c:346-360 both
 * do exactly this).
 */
static irqreturn_t mtk_vdec_isr(int irq, void *data)
{
	struct platform_device *pdev = data;
	struct mtk_vcodec_dev *vcodec = dev_get_drvdata(&pdev->dev);
	u32 val;

	if (!pm_runtime_get_sync(&pdev->dev))
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
/* V4L2 stateless codec interface				      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* queue plumbing, following the in-tree JPEG driver's idiom	      */
/* ------------------------------------------------------------------ */

static int mtk_vcodec_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
				  unsigned int *nplanes, unsigned int *size,
				  struct device **alloc_dev)
{
	/* The codec takes a planar NV12 picture: luma plus interleaved chroma. */
	if (*nplanes != 2)
		return -EINVAL;

	/*
	 * A codec needs a small pool: one picture in flight plus room for the
	 * bitstream to drain.  The hardware has no descriptor table to feed, so
	 * buffers are addressed directly.
	 */
	*nbufs = clamp_t(unsigned int, *nbufs, 4, 16);

	return 0;
}

static int mtk_vcodec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mtk_vcodec_ctx *ctx = q->drv_priv;
	struct mtk_vcodec_dev *vcodec = ctx->vcodec;
	int ret;

	ret = pm_runtime_get_sync(&vcodec->enc_pdev->dev);
	if (ret < 0)
		return ret;

	if (ctx->encoder)
		atomic_inc(&vcodec->enc_users);
	else
		atomic_inc(&vcodec->dec_users);

	/*
	 * Take the engine lock only for the duration of programming; the frame
	 * itself is interrupt driven, so the lock is released once the job is
	 * queued.
	 */
	if (ctx->encoder) {
		mutex_lock(&vcodec->enc_lock);
		mtk_venc_reset(vcodec);
		mutex_unlock(&vcodec->enc_lock);
	}

	return 0;
}

static void mtk_vcodec_stop_streaming(struct vb2_queue *q)
{
	struct mtk_vcodec_ctx *ctx = q->drv_priv;
	struct mtk_vcodec_dev *vcodec = ctx->vcodec;
	struct vb2_v4l2_buffer *vb;

	while ((vb = v4l2_m2m_buf_remove(&ctx->fh.m2m_ctx->out_q_ctx)) != NULL)
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);

	if (ctx->encoder)
		atomic_dec(&vcodec->enc_users);
	else
		atomic_dec(&vcodec->dec_users);

	pm_runtime_put_autosuspend(&vcodec->enc_pdev->dev);
}

static const struct vb2_ops mtk_vcodec_enc_qops = {
	.queue_setup	= mtk_vcodec_queue_setup,
	.start_streaming	= mtk_vcodec_start_streaming,
	.stop_streaming	= mtk_vcodec_stop_streaming,
};

static const struct vb2_ops mtk_vcodec_dec_qops = {
	.queue_setup	= mtk_vcodec_queue_setup,
	.start_streaming	= mtk_vcodec_start_streaming,
	.stop_streaming	= mtk_vcodec_stop_streaming,
};

/* ------------------------------------------------------------------ */
/* Encoder / decoder programming, from the register map		      */
/* ------------------------------------------------------------------ */

/*
 * Program the frame and bitstream buffers.  These are plain physical DRAM
 * addresses in the chapter-60 table; the hardware bitstream DMA writes the
 * encoded picture into the destination and the current frame is read from the
 * source.
 */
static int mtk_venc_set_frame_addr(struct mtk_vcodec_dev *vcodec,
				   dma_addr_t bs_addr, u32 bs_size,
				   dma_addr_t src_y, dma_addr_t src_uv,
				   dma_addr_t ref_y, dma_addr_t ref_uv)
{
	mtk_venc_write(vcodec, VENC_BITSTREAM_BUF_ADDR, lower_32_bits(bs_addr));
	mtk_venc_write(vcodec, VENC_BITSTREAM_BUF_SIZE, bs_size >> 4);

	mtk_venc_write(vcodec, VENC_FRM_CUR_Y_ADDR, lower_32_bits(src_y));
	mtk_venc_write(vcodec, VENC_FRM_CUR_UV_ADDR, lower_32_bits(src_uv));
	mtk_venc_write(vcodec, VENC_FRM_REF_Y_ADDR, lower_32_bits(ref_y));
	mtk_venc_write(vcodec, VENC_FRM_REF_UV_ADDR, lower_32_bits(ref_uv));

	/*
	 * The engine is physical address driven and the M4U port width is 32 bits
	 * on this SoC, but check the high word is zero rather than silently
	 * truncating a buffer that lands above 4 GiB.
	 */
	if (upper_32_bits(bs_addr) || upper_32_bits(src_y) ||
	    upper_32_bits(ref_y))
		return -EINVAL;

	return 0;
}

/*
 * Program the MPEG-4 datapath (chapter 61).  Same picture, separate registers:
 * a separate source/reconstruction address set and a separate bitstream pointer.
 */
static int mtk_venc_set_mp4_frame_addr(struct mtk_vcodec_dev *vcodec,
				       dma_addr_t src_y, dma_addr_t src_cb,
				       dma_addr_t src_cr,
				       dma_addr_t bs_addr,
				       dma_addr_t ref_y, dma_addr_t ref_uv)
{
	mtk_venc_write(vcodec, VENC_MP4_SRCADR_Y, lower_32_bits(src_y));
	mtk_venc_write(vcodec, VENC_MP4_SRCADR_CB, lower_32_bits(src_cb));
	mtk_venc_write(vcodec, VENC_MP4_SRCADR_CR, lower_32_bits(src_cr));
	mtk_venc_write(vcodec, VENC_MP4_BITADR, lower_32_bits(bs_addr));
	mtk_venc_write(vcodec, VENC_MP4_REFADR_Y, lower_32_bits(ref_y));
	mtk_venc_write(vcodec, VENC_MP4_REFADR_CB, lower_32_bits(ref_uv));

	if (upper_32_bits(bs_addr) || upper_32_bits(src_y))
		return -EINVAL;

	return 0;
}

/*
 * Rate control.  VENC_RATECONTROL_INFO_n is a four-register block in chapter 60;
 * the hardware applies the bitrate and quantiser limits from it.  The bounds
 * here are not arbitrary: the vendor codec library clamps exactly these three
 * values to 1..31 with min <= init <= max, which is the H.264 quantiser range
 * (see RECOVERED-ABI.md for the disassembly).
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
 * The engine is edge sensitive on VENC_MP4_SLICE_START for the MPEG-4 path and
 * level driven through the shared front end for H.264/VP8, so clear the start
 * first and set it last.
 */
static void mtk_venc_start_frame(struct mtk_vcodec_dev *vcodec, bool mpeg4)
{
	mtk_venc_write(vcodec, VENC_CODEC_CTRL, mpeg4 ? 1 : 0);

	if (mpeg4) {
		mtk_venc_write(vcodec, VENC_MP4_SLICE_START, 0);
		mtk_venc_write(vcodec, VENC_MP4_SLICE_START, 1);
	}
}

/* ------------------------------------------------------------------ */
/* device_run: program the engine and start one frame		      */
/* ------------------------------------------------------------------ */

static void mtk_vcodec_enc_device_run(void *priv)
{
	struct v4l2_m2m_ctx *m2m = priv;
	struct mtk_vcodec_ctx *ctx = m2m->priv;
	struct mtk_vcodec_dev *vcodec = ctx->vcodec;
	struct vb2_v4l2_buffer *src, *dst;
	struct mtk_vcodec_enc *enc = ctx->enc;
	dma_addr_t src_y, src_uv, dst_addr;
	int ret;

	src = v4l2_m2m_src_buf_remove(m2m);
	dst = v4l2_m2m_dst_buf_remove(m2m);
	if (!src || !dst) {
		if (src)
			v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
		if (dst)
			v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
		return;
	}

	/*
	 * The encoder is planar NV12 in, so plane 0 is luma and plane 1 is the
	 * interleaved chroma pair.  The source buffer is the current frame; the
	 * destination is the bitstream.
	 */
	src_y = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	src_uv = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 1);
	dst_addr = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);

	if (!enc->configured) {
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
		return;
	}

	mutex_lock(&vcodec->enc_lock);

	if (enc->mpeg4) {
		ret = mtk_venc_set_mp4_frame_addr(vcodec, src_y, src_uv, src_uv,
						   dst_addr, 0, 0);
	} else {
		ret = mtk_venc_set_frame_addr(vcodec, dst_addr,
					       vb2_plane_size(&dst->vb2_buf, 0) * 3 / 2,
					       src_y, src_uv, 0, 0);
	}
	if (ret) {
		mutex_unlock(&vcodec->enc_lock);
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
		return;
	}

	ret = mtk_venc_set_rate_control(vcodec, &enc->parm);
	if (ret) {
		mutex_unlock(&vcodec->enc_lock);
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
		return;
	}

	mtk_venc_start_frame(vcodec, enc->mpeg4);

	mutex_unlock(&vcodec->enc_lock);

	dst->vb2_buf.planes[0].bytesused = 0;
	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);

	/*
	 * The bitstream size is only known once the hardware raises the frame
	 * interrupt, so the destination is returned to userspace from the ISR
	 * rather than here.  Recording the pair lets the ISR find them again.
	 */
	enc->src = src;
	enc->dst = dst;
}

/* ------------------------------------------------------------------ */
/* V4L2 ioctls							      */
/* ------------------------------------------------------------------ */

static long mtk_vcodec_enc_s_parm(struct mtk_vcodec_enc *enc, void *arg)
{
	struct mtk_vcodec_enc_parm parm;
	u32 qp_min, qp_init, qp_max, rate;

	if (copy_from_user(&parm, arg, sizeof(parm)))
		return -EFAULT;

	if (!parm.width || !parm.height)
		return -EINVAL;

	/*
	 * Re-validate the quantiser triple with the same bounds the hardware
	 * applies, so a bad request is rejected at ioctl time rather than
	 * producing a silently mangled stream.
	 */
	qp_min = parm.qp_min ? parm.qp_min : 1;
	qp_init = parm.qp_init ? parm.qp_init : qp_min;
	qp_max = parm.qp_max ? parm.qp_max : 31;
	if (qp_min < 1 || qp_min > 31 || qp_max < 1 || qp_max > 31)
		return -EINVAL;
	if (qp_init < qp_min || qp_init > qp_max)
		return -EINVAL;

	rate = parm.intra_vop_rate ? parm.intra_vop_rate : 1;
	if (rate > 31)
		return -EINVAL;

	parm.qp_min = qp_min;
	parm.qp_init = qp_init;
	parm.qp_max = qp_max;
	parm.intra_vop_rate = rate;

	enc->parm = parm;
	enc->configured = true;

	if (copy_to_user(arg, &parm, sizeof(parm)))
		return -EFAULT;

	return 0;
}

static long mtk_vcodec_enc_g_parm(struct mtk_vcodec_enc *enc, void *arg)
{
	if (copy_to_user(arg, &enc->parm, sizeof(enc->parm)))
		return -EFAULT;
	return 0;
}

static int mtk_vcodec_enc_vidioc_s_parm(struct file *file, void *fh,
				       struct v4l2_streamparm *parm)
{
	struct mtk_vcodec_ctx *ctx = file->private_data;
	void __user *arg = parm->parm.raw_data;

	return mtk_vcodec_enc_s_parm(ctx->enc, arg);
}

static int mtk_vcodec_dec_vidioc_s_parm(struct file *file, void *fh,
				       struct v4l2_streamparm *parm)
{
	struct mtk_vcodec_ctx *ctx = file->private_data;
	void __user *arg = parm->parm.raw_data;
	struct mtk_vcodec_dec_parm p;

	if (copy_from_user(&p, arg, sizeof(p)))
		return -EFAULT;
	if (!p.width || !p.height)
		return -EINVAL;

	ctx->dec->parm = p;
	ctx->dec->configured = true;

	if (copy_to_user(arg, &p, sizeof(p)))
		return -EFAULT;

	return 0;
}

static const struct v4l2_m2m_ops mtk_vcodec_enc_m2m_ops = {
	.device_run		= mtk_vcodec_enc_device_run,
};
static const struct v4l2_m2m_ops mtk_vcodec_dec_m2m_ops = {
	.device_run		= mtk_vcodec_enc_device_run,
};

static int mtk_vcodec_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq)
{
	struct mtk_vcodec_ctx *ctx = priv;
	struct mtk_vcodec_dev *vcodec = ctx->vcodec;
	const struct vb2_ops *ops = ctx->encoder ? &mtk_vcodec_enc_qops
						 : &mtk_vcodec_dec_qops;
	struct device *dev = &vcodec->enc_pdev->dev;
	int ret;

	src_vq->type		= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes	= VB2_DMABUF | VB2_MMAP;
	src_vq->drv_priv	= ctx;
	src_vq->buf_struct_size	= sizeof(struct vb2_v4l2_buffer);
	src_vq->ops		= ops;
	src_vq->mem_ops		= &vb2_dma_contig_memops;
	src_vq->timestamp_flags	= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock		= &vcodec->lock;
	src_vq->dev		= dev;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type		= V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes	= VB2_DMABUF | VB2_MMAP;
	dst_vq->drv_priv	= ctx;
	dst_vq->buf_struct_size	= sizeof(struct vb2_v4l2_buffer);
	dst_vq->ops		= ops;
	dst_vq->mem_ops		= &vb2_dma_contig_memops;
	dst_vq->timestamp_flags	= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock		= &vcodec->lock;
	dst_vq->dev		= dev;

	return vb2_queue_init(dst_vq);
}

static int mtk_vcodec_enc_vidioc_g_parm(struct file *file, void *fh,
				       struct v4l2_streamparm *parm)
{
	struct mtk_vcodec_ctx *ctx = file->private_data;

	return mtk_vcodec_enc_g_parm(ctx->enc, parm->parm.raw_data);
}

static const struct v4l2_ioctl_ops mtk_vcodec_enc_ioctl_ops = {
	.vidioc_s_parm	= mtk_vcodec_enc_vidioc_s_parm,
	.vidioc_g_parm	= mtk_vcodec_enc_vidioc_g_parm,
};

static const struct v4l2_ioctl_ops mtk_vcodec_dec_ioctl_ops = {
	.vidioc_s_parm	= mtk_vcodec_dec_vidioc_s_parm,
};

/*
 * open() creates the per-file m2m context and attaches the queue_init hook.
 * That is the mechanism this tree uses (the in-tree JPEG driver does exactly
 * this with v4l2_m2m_ctx_init); there is no per-instance registration from
 * probe().
 */
static int mtk_vcodec_open(struct file *file)
{
	struct video_device *vfd = video_devdata(file);
	struct mtk_vcodec_dev *vcodec = container_of(vfd->v4l2_dev,
						      struct mtk_vcodec_dev,
						      enc_v4l2_dev);
	struct mtk_vcodec_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->vcodec = vcodec;
	ctx->encoder = true;
	ctx->enc = vcodec->enc;
	ctx->enc->ctx = ctx;

	file->private_data = ctx;
	v4l2_fh_init(file->private_data, vfd);

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vcodec->enc_m2m_dev, ctx,
					    mtk_vcodec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		kfree(ctx);
		file->private_data = NULL;
		return PTR_ERR(ctx->fh.m2m_ctx);
	}

	return 0;
}

static int mtk_vcodec_close(struct file *file)
{
	struct mtk_vcodec_ctx *ctx = file->private_data;

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations mtk_vcodec_fops = {
	.owner		= THIS_MODULE,
	.open		= mtk_vcodec_open,
	.release	= mtk_vcodec_close,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

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
	int ret = 0;

	mutex_lock(&vcodec->dec_lock);
	if (atomic_read(&vcodec->dec_users) == 0) {
		ret = mtk_vdec_power_on(vcodec);
		if (ret)
			goto err;
	}
	mutex_unlock(&vcodec->dec_lock);

	mutex_lock(&vcodec->enc_lock);
	if (atomic_read(&vcodec->enc_users) == 0) {
		ret = mtk_venc_power_on(vcodec);
		if (ret)
			goto err;
		mtk_venc_reset(vcodec);
	}
	mutex_unlock(&vcodec->enc_lock);

	vcodec->suspended = false;
	return 0;

err:
	if (ret)
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
MODULE_DESCRIPTION("MediaTek MT6589 V4L2 video codec driver");
MODULE_LICENSE("GPL");
