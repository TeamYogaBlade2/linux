// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * MT6589 VDEC: the per-instance DRAM the H.264 datapath needs.
 *
 * Everything in here exists because the hardware cannot decode a frame without
 * it, and none of it is visible to V4L2: the bitstream FIFO, the parser's
 * prediction scratch, the reference picture store and the two motion-vector /
 * wrapper work areas.  The CAPTURE queue's buffers are the only DRAM this driver
 * does not own.
 *
 * Why a separate file: mtk-vdec-mt6589.c is the V4L2 node and the resource
 * layer, and the sizes below are derived from the vendor harness rather than
 * from anything V4L2 knows about.  Keeping the allocation policy next to the
 * derivation means an offset in a register map and the buffer it points at can
 * be read together.
 */

#ifndef _MTK_VDEC_MT6589_DP_H_
#define _MTK_VDEC_MT6589_DP_H_

#include <linux/dma-mapping.h>
#include <linux/types.h>

struct device;

/*
 * Reference picture store: how many pictures the DPB holds.
 *
 * MT6589 has no DPB register - the driver builds the reference list by hand
 * into AVC_VLD_REORD_P0_RPL and MC_P_LIST0 - so this is the driver's own
 * capacity limit.  It is the *smaller* of two constraints:
 *
 *   17   the harness's ceiling on ucMaxFBufNum (verify/vdec_verify_vparser_h264.c:1545)
 *   32   the number of reference-list slots the register map provides
 *        (AVC_VLD_REORD_P0_RPL is 32 words, mtk-vcodec-mt6589-reg.h)
 *
 * 17 is what applies, and it exceeds H.264's own maximum of 16 references plus
 * the picture being decoded, so no conforming stream is refused by it.
 */
#define MTK_VDEC_DPB_PICS		17

/*
 * Sizes the harness fixes, quoted where the register map cites them.
 *
 * MTK_VDEC_PIC_PLANE_SZ is the full-HD luma plane.  The vendor harness sizes its
 * decode buffers at PIC_Y_SZ = 1920*1088 (verify/vdec_verify_mm_map.h:289) and
 * the motion-vector store at 7/4 of that per picture - i.e. one picture's total
 * allocation is luma + chroma (1.5x) + MV (0.25x) (vdec_verify_vparser_h264.c:553).
 * The driver keeps those as one contiguous run per picture so that the picture is
 * physically contiguous, which is what an offset-based plane address requires.
 */
#define MTK_VDEC_PIC_W			1920
#define MTK_VDEC_PIC_H			1088

/*
 * What is allocated per open, in one dma_alloc_coherent() so the whole datapath
 * shares one coherency domain and one set of cache operations.
 *
 * The MT6589 VDEC is NOT coherent with the CPU, which is why every buffer that
 * the hardware reads gets dma_sync_single_for_device() after the CPU fills it
 * and every buffer it writes gets ..._for_cpu() before the CPU reads it.  Doing
 * this via one allocation rather than one per plane is a deliberate trade: the
 * arithmetic below is exact, and a single region cannot be partially mapped.
 */
struct mtk_vdec_dp_mem {
	void		*base;		/* CPU view */
	dma_addr_t	dma;		/* device view, the base of every offset below */
	size_t		size;
	struct device	*owner;		/* for the cache maintenance in .c */
};

/*
 * Geometry of the reference picture store, in bytes from dp_mem.dma.
 *
 * Two pictures rather than 17: a driver that has not been tested on silicon
 * should not reserve 17 * 3.1 MiB = 53 MiB per open on a tablet that shipped
 * with 1 GiB.  Two is enough for the case this datapath is built to get right -
 * an IDR followed by P frames - and it is the *stream* that limits it further:
 * a stream declaring num_ref_frames = 0 has no references at all, which is what
 * x264 emits at baseline, and mtk_vdec_dp_refs_needed() reports that so the
 * caller can say so in dmesg rather than guess.
 */
#define MTK_VDEC_REF_PICS		2

/**
 * struct mtk_vdec_dp - the datapath's DRAM, owned by the device.
 *
 * Allocated lazily on the first frame, freed at remove(), and shared by every
 * open because there can only ever be one frame in flight: job_ready() reports
 * the device busy until the previous frame is retired, so two contexts cannot be
 * decoding at once.  That is a property of this V4L2 node, not of the hardware,
 * and it is what makes one DPB legal.
 */
struct mtk_vdec_dp {
	struct mtk_vdec_dp_mem	mem;
	bool			allocated;

	/* Offsets from mem.dma, all computed once at allocation time. */
	size_t			fifo_off;	/* bitstream FIFO */
	size_t			pred_off;	/* parser prediction scratch */
	size_t			ref_off;	/* first reference picture */
	size_t			ref_pic_sz;	/* stride between pictures */
	size_t			mv_off;		/* motion-vector store */
	size_t			vld_wrap_off;	/* VLD wrapper work area */
	size_t			pp_wrap_off;	/* PP wrapper work area */

	/*
	 * The bitstream FIFO's write offset, in bytes from fifo_off.  This side of
	 * the ring is the driver's; the hardware owns the read side.  It lives
	 * here rather than in the per-open context because the FIFO is shared by
	 * every open - only one frame can be in flight - so a per-context copy
	 * would disagree with itself as soon as two clients took turns.
	 */
	u32			fifo_wr;
};

int mtk_vdec_dp_alloc(struct device *dev, struct mtk_vdec_dp *dp);
void mtk_vdec_dp_free(struct device *dev, struct mtk_vdec_dp *dp);

/* Cache maintenance, in the direction the hardware needs it. */
void mtk_vdec_dp_sync_to_dev(struct mtk_vdec_dp *dp, size_t off, size_t len);
void mtk_vdec_dp_sync_to_cpu(struct mtk_vdec_dp *dp, size_t off, size_t len);

/* Device addresses for the register writes, each already in the unit it needs. */
dma_addr_t mtk_vdec_dp_fifo_dma(struct mtk_vdec_dp *dp);
dma_addr_t mtk_vdec_dp_pred_dma(struct mtk_vdec_dp *dp);
dma_addr_t mtk_vdec_dp_ref_dma(struct mtk_vdec_dp *dp, unsigned int pic);
dma_addr_t mtk_vdec_dp_mv_dma(struct mtk_vdec_dp *dp);
dma_addr_t mtk_vdec_dp_vld_wrap_dma(struct mtk_vdec_dp *dp);
dma_addr_t mtk_vdec_dp_pp_wrap_dma(struct mtk_vdec_dp *dp);

/*
 * Copy one frame's slice NAL into the bitstream FIFO and return the read/write
 * pointers the barrel-shifter init needs, in the units those registers take.
 *
 * Returns 0, or -ENOSPC when the frame does not fit in the FIFO.
 */
int mtk_vdec_dp_fifo_push(struct mtk_vdec_dp *dp, const u8 *src, size_t len,
			  dma_addr_t *rd_ptr, dma_addr_t *wr_ptr);

#endif /* _MTK_VDEC_MT6589_DP_H_ */
