// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * Private declarations for the MediaTek MT6589 video codec driver.
 *
 * Buffer geometry and per-instance state for the V4L2 mem-to-mem encoder node.
 * The register map is deliberately NOT here: it lives in mtk-vcodec-mt6589-reg.h
 * with its per-field sourcing, and this header is only the Linux plumbing around
 * it.
 */

#ifndef _MTK_VCODEC_MT6589_H_
#define _MTK_VCODEC_MT6589_H_

#include <linux/dma-mapping.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>

struct mtk_vcodec_dev;

/*
 * Picture geometry the encoder is driven at.
 *
 * The datapath is macroblock based.  VENC_MP4_MBX_LMT.MB_XLIMIT is documented as
 * "It should be bigger than 0 and smaller than 255.  If this register is set to
 * 128, the source picture buffer width will be 2,048" (draft/ds/mp4.txt:6503-
 * 6507), and VENC_MP4_MBY_LMT.MB_YLIMIT says the same for height
 * (draft/ds/mp4.txt:6553-6557), so one macroblock row is at most 254 MBs, i.e.
 * 4064 pixels.
 *
 * Chapter 60 states a much lower capability for the H.264 encoder proper: Table
 * 60-1 "Main features" gives H.264 Profile High, Level 4.1, Speed 720p@30fps
 * (draft/ds/venc.txt:1761-1770), and multi-frame processing tops out at
 * 2592x1592@5fps (draft/ds/venc.txt:1777-1782).  The sibling VDEC block in the same
 * window is documented as "full-HD 30fps" (draft/ds/venc.txt:1423), so 1920x1088
 * is what this silicon is specified for.  1088 rather than 1080 because 1080 is
 * not a whole number of 16-pixel macroblock rows.
 */
#define MTK_VENC_MAX_WIDTH			1920
#define MTK_VENC_MAX_HEIGHT			1088

/* One macroblock: the alignment every frame address register implies. */
#define MTK_VENC_MB_SIZE			16

/*
 * Bitstream buffer bounds.
 *
 * VENC_BITSTREAM_BUF_SIZE is a DIV128 field 25 bits wide, so any size that is not
 * a multiple of 128 is rejected by mtk_venc_set_frame_addr() and the largest
 * describable buffer is 2^25 * 128 == 4 GiB.  The default is deliberately
 * conservative: a 720p30 frame at a sane quality needs a few hundred kilobytes,
 * and over-allocating makes the overflow path (BS_DRAM_FULL_INT) fire later
 * rather than sooner, which hides a bad rate control rather than surfacing it.
 */
#define MTK_VENC_BS_SIZE_MIN			(128 * 1024)
#define MTK_VENC_BS_SIZE_MAX			(4 * 1024 * 1024)
#define MTK_VENC_BS_SIZE_DEFAULT		MTK_VENC_BS_SIZE_MIN
#define MTK_VENC_BS_SIZE_ALIGN			128

/*
 * Rate control scratch buffer size.
 *
 * VENC_RC_CODE_DRAM_ADDR and VENC_RC_INFO_DRAM_ADDR are DIV16 28-bit DRAM
 * ADDRESS registers, and the data sheet sizes the memory they point at in terms
 * of code this driver does not have: "The required DRAM size is equal to (the
 * length of ROM code + 16)" for both (draft/ds/venc.txt:5072-5074 for the code
 * buffer, :5132-5136 for the info buffer).  That length is a property of the
 * closed rate control image, so this is a generous fixed allocation rather than a
 * computed one.
 */
#define MTK_VENC_RC_CACHE_SIZE			(64 * 1024)

/*
 * Reference/reconstruction buffer pairs.
 *
 * Two, because a genuine ping-pong needs two: frame N predicts from REF[N & 1] and
 * writes REC[N & 1]; frame N+1's reference IS frame N's reconstruction.  With one
 * pair the encoder would be reading the very buffer it is writing.
 */
#define MTK_VCODEC_REF_BUFFER			2

/*
 * Per-instance encoder state.
 *
 * One of these exists per open file descriptor, not per hardware block: the
 * reference and reconstruction planes are picture-sized and belong to the stream
 * being encoded, so two independent encodes must not fight over them.
 */
struct mtk_vcodec_enc_ctx {
	struct mtk_vcodec_dev			*dev;

	/* The m2m context the framework allocated for this open. */
	struct v4l2_m2m_ctx			*m2m;

	/* Raw NV12 frames in (OUTPUT queue), encoded H.264 out (CAPTURE queue). */
	struct v4l2_format			src_fmt;
	struct v4l2_format			dst_fmt;

	/*
	 * Driver-owned reference and reconstruction planes.
	 *
	 * Coherent DMA rather than vb2 buffers: they are never queued, never dequeued
	 * and never mapped into userspace, so a vb2 buffer lifecycle around each would
	 * buy nothing.  Coherent rather than streaming because the hardware both reads
	 * and writes them and a cache flush between the encode and the next frame's
	 * read of the REC buffer would be a correctness hazard for no size gain.
	 *
	 * dma_alloc_coherent() gives 16-byte alignment as a side effect of its
	 * page-aligned allocation, which is what the DIV16 address fields require.
	 */
	dma_addr_t				ref_addr[MTK_VCODEC_REF_BUFFER];
	void					*ref_vaddr[MTK_VCODEC_REF_BUFFER];
	dma_addr_t				rec_addr[MTK_VCODEC_REF_BUFFER];
	void					*rec_vaddr[MTK_VCODEC_REF_BUFFER];

	/* Luma plane size, rounded up to whole macroblocks. */
	unsigned long				frame_size;

	/* Rate control scratch: loaded and saved by the hardware, never by us. */
	dma_addr_t				rc_code_addr;
	dma_addr_t				rc_info_addr;
	void					*rc_code_vaddr;
	void					*rc_info_vaddr;

	/* Which (REF, REC) pair the next frame uses; alternates per frame. */
	unsigned int				buf_idx;

	/* Frame counter, for the GOP keyframe decision. */
	unsigned long				frame_count;

	/*
	 * Per-job state, touched by device_run() under the driver's enc_state_lock
	 * and by whichever of the interrupt path or the watchdog retires the job.
	 *
	 * pending is the per-instance claim: it is set only once a frame has actually
	 * been started, and cleared by whichever completion path wins.  That is what
	 * makes the runtime-PM reference exactly-once -- a spurious interrupt or a late
	 * watchdog finds it clear and returns without touching the buffers or the usage
	 * count.
	 */
	bool					pending;

	struct v4l2_ctrl_handler		ctrl_hdl;

	/*
	 * Cached control pointers, resolved once at open.
	 *
	 * This tree's v4l2-ctrls.h has no v4l2_ctrl_get_std(): a value is read with
	 * v4l2_ctrl_g_ctrl() on a &struct v4l2_ctrl, which must be looked up first.
	 * Resolving once here keeps the per-frame submit path free of a handler-wide
	 * search.  A NULL pointer means open() failed to create that control, which it
	 * treats as fatal, so these are never dereferenced unguarded.
	 */
	struct v4l2_ctrl			*ctrl_bitrate;
	struct v4l2_ctrl			*ctrl_qp;
	struct v4l2_ctrl			*ctrl_fps;
	struct v4l2_ctrl			*ctrl_cbr;
	struct v4l2_ctrl			*ctrl_gop;
};

#endif /* _MTK_VCODEC_MT6589_H_ */