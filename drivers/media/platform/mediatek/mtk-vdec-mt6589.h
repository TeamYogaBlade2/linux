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

struct mtk_vdec_dev;

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
 * Deliberately small, and that is the point rather than an omission.  Unlike the
 * encoder, this driver owns no decoder buffers at all: the reference, current,
 * post-process and 4 MiB bitstream FIFO planes all have to exist before a frame can
 * be decoded (see the VP8 note in mtk-vcodec-mt6589-reg.h), but they belong to the
 * datapath, and there is no datapath here to point them at.  Allocating several
 * picture-sized planes per open that nothing ever reads or writes would be tens of
 * megabytes of untouched memory presented as if it were working state.
 *
 * What IS per-instance is the negotiated format, because that is what the node
 * exposes to userspace and what makes the node usable for negotiation at all.
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
};

#endif /* _MTK_VDEC_MT6589_H_ */
