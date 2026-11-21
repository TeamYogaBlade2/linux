// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * Private declarations for the MediaTek MT6589 video DECODER driver.
 *
 * The decoder's half of what used to be mtk-vcodec-mt6589.h.  It is deliberately
 * small: see struct mtk_vdec_ctx below and the note on what this driver does NOT
 * do.  VDEC is a separate hardware block from VENC -- separate register window
 * (0x16020000 against the encoder's 0x17002000), separate GIC line (SPI 140
 * against SPI 136), separate clock controller (vdecsys) and separate power domain
 * (VDE against VEN) -- so it now has its own driver, its own probe and its own
 * device tree node.
 */

#ifndef _MTK_VDEC_MT6589_H_
#define _MTK_VDEC_MT6589_H_

#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/v4l2-mem2mem.h>

#include "mtk-vdec-mt6589-h264.h"

struct mtk_vdec_dev;
struct mtk_vdec_dp;

/*
 * Decoder picture geometry.
 *
 * The decoder's own ceiling is stated only indirectly.  The data sheet's ch.60
 * summary describes the sibling VDEC block as "full-HD 30fps"
 * (draft/ds/venc.txt:1423), which is the same 1920x1088 working point the encoder
 * half of this driver uses, so the same constants apply.  The vendor harness sizes
 * its picture planes as PIC_Y_SZ = 1920*1088 and DEC_PP_Y_SZ = 1920*1088
 * (verify/vdec_verify_mm_map.h:289, :308), which agrees.
 *
 * Decoding is macroblock based, exactly as encoding is, so the same 16-pixel
 * granularity applies to the picture height.
 */
#define MTK_VDEC_MAX_WIDTH		1920
#define MTK_VDEC_MAX_HEIGHT		1088

/* One macroblock, the alignment every decoder plane address implies. */
#define MTK_VDEC_MB_SIZE		16

/*
 * Per-instance decoder state.
 *
 * The picture buffers this driver needs -- reference planes, current, post-process
 * and a 4 MiB bitstream FIFO -- are NOT here.  They belong to the datapath rather
 * than to an open file: only one frame can be in flight on the block, job_ready()
 * enforces that, and the same DRAM serves whichever context is decoding.  They
 * live in struct mtk_vdec_dp on the device.  See mtk-vdec-mt6589-dp.h.
 *
 * What IS per-instance is the negotiated format and the parsed stream state,
 * because two clients can have the node open at once with different streams.
 */
struct mtk_vdec_ctx {
	/* First member, for the same file2dectx() reason as the encoder's fh. */
	struct v4l2_fh			fh;

	struct mtk_vdec_dev		*dev;

	/*
	 * Serialises buffer queueing for both vb2 queues.  vb2 requires each
	 * queue to be given one - vb2_queue_init() has
	 * WARN_ON(!q->lock); return -EINVAL - and neither vb2 nor the m2m core
	 * supplies it.  One mutex serves both queues, so queueing a source is
	 * serialised against queueing the capture buffer for the same frame.
	 */
	struct mutex			vb_queue_lock;

	/* The m2m context the framework allocated for this open. */
	struct v4l2_m2m_ctx		*m2m;

	/*
	 * Compressed bitstream in (OUTPUT queue), raw NV12 out (CAPTURE queue) --
	 * the mirror image of the encoder, which is why the format roles invert.
	 */
	struct v4l2_format		src_fmt;
	struct v4l2_format		dst_fmt;

	/*
	 * Parameter sets parsed out of the stream.  The MT6589 AVC parser takes
	 * already-parsed SPS/PPS/slice fields rather than reading the bitstream
	 * itself, so these have to be kept per context: an SPS arrives once and
	 * every following slice header is parsed against it.
	 */
	struct mtk_h264_ps		ps;

	/* Last slice header parsed, for the register programming step. */
	struct mtk_h264_slice		slice;

	/*
	 * Frame in flight, for the completion path.
	 *
	 * pending is set by device_run() when it starts the hardware and cleared by
	 * whichever of the frame-end interrupt, the watchdog or job_abort() retires
	 * the job first.  It is the exactly-once claim on the buffers and on the
	 * runtime-PM reference: two paths that both find it set cannot both return
	 * the same pair of buffers.
	 *
	 * Guarded by dev->state_lock, which is a spinlock because the interrupt
	 * handler takes it.
	 */
	bool				pending;

	/*
	 * The decoded-and-written picture count and the picture order count of the
	 * last frame, which is what the reference list is built from.  Decoder-side
	 * DPB bookkeeping, not hardware state: MT6589 has no DPB register, the
	 * driver fills the reference list itself.
	 */
	u32				frame_num;
	s32				last_poc;

};

#endif /* _MTK_VDEC_MT6589_H_ */
