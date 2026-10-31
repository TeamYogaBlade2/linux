// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * MT6589 VDEC: allocation and cache maintenance for the H.264 datapath DRAM.
 *
 * See mtk-vdec-mt6589-dp.h for what each region is for.  This file is the
 * arithmetic and the coherency; the register programming is in
 * mtk-vdec-mt6589.c.
 *
 * COHERENCY.  The MT6589 VDEC is not coherent with the CPU.  The vendor stack
 * gets away with it because it runs on an ARM11/A7 user process whose cache
 * operations it controls through libvcodec; here every buffer that crosses the
 * boundary is explicitly synced:
 *
 *   bitstream FIFO        written by the CPU, read by the parser  -> to_device
 *   reference pictures    written by the MC, read by the MC       -> to_cpu before
 *                                                                    the CPU reuses
 *                                                                    one as a source
 *   prediction scratch    read+written by the parser             -> no CPU access
 *   MV store              read+written by the MC                 -> no CPU access
 *
 * The last two are never touched by the CPU, so they need no maintenance at all
 * - they only have to be allocated, zeroed once, and mapped.
 *
 * The one that matters is the FIFO: forgetting its to_device sync gives a parser
 * that reads stale DRAM and decodes a frame that looks like a bitstream error,
 * with no register anywhere reporting a cause.
 */

#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "mtk-vdec-mt6589-dp.h"
#include "mtk-vcodec-mt6589-reg.h"

/*
 * One picture's worth of DPB: luma plane, chroma plane at 1.5x, then the
 * motion-vector store at 7/4 of the luma size.  The 7/4 total is the harness's
 * own accounting (`u4DramPicArea = u4DramPicSize * 7 >> 2`,
 * verify/vdec_verify_vparser_h264.c:1553) and the layout follows it: luma, then
 * chroma, then MV.
 */
static size_t mtk_vdec_pic_bytes(void)
{
	size_t luma = (size_t)MTK_VDEC_PIC_W * MTK_VDEC_PIC_H;

	/* 7/4 of luma, rounded up to a page so the next picture stays aligned. */
	return round_up((luma * 7) / 4, PAGE_SIZE);
}

/* 512-byte aligned reference planes: the harness's own ASSERTs on the PP/VP8
 * plane addresses are (& 0x1FF) == 0 (vdec_hal_if_vp8.c:423-426).
 */
#define MTK_VDEC_PLANE_ALIGN	0x200

int mtk_vdec_dp_alloc(struct device *dev, struct mtk_vdec_dp *dp)
{
	size_t pic_sz, off;
	int ret;

	if (dp->allocated)
		return 0;

	pic_sz = mtk_vdec_pic_bytes();

	/*
	 * Lay the regions out in one run.  Order is chosen so the two largest
	 * (the reference pictures) sit at an offset that is already 512-byte
	 * aligned given the regions before them, which keeps the per-picture
	 * stride a multiple of the alignment requirement.
	 */
	dp->fifo_off = 0;
	off = VDEC_VFIFO_SIZE;
	off = round_up(off, MTK_VDEC_PLANE_ALIGN);

	dp->pred_off = off;
	off += VDEC_AVC_PRED_SIZE;
	off = round_up(off, MTK_VDEC_PLANE_ALIGN);

	dp->ref_off = off;
	dp->ref_pic_sz = pic_sz;
	off += pic_sz * MTK_VDEC_REF_PICS;
	off = round_up(off, MTK_VDEC_PLANE_ALIGN);

	dp->mv_off = off;
	/* One MV store per reference picture, same 7/4 - 3/2 = 1/4 ratio. */
	off += pic_sz * MTK_VDEC_REF_PICS / 7;
	off = round_up(off, MTK_VDEC_PLANE_ALIGN);

	dp->vld_wrap_off = off;
	off += VDEC_VLD_WRAPPER_SIZE;
	off = round_up(off, MTK_VDEC_PLANE_ALIGN);

	dp->pp_wrap_off = off;
	off += VDEC_PP_WRAPPER_SIZE;
	off = round_up(off, PAGE_SIZE);

	dp->mem.size = off;
	dp->mem.owner = dev;

	dp->mem.base = dma_alloc_coherent(dev, dp->mem.size, &dp->mem.dma,
					  GFP_KERNEL | __GFP_ZERO);
	if (!dp->mem.base) {
		ret = -ENOMEM;
		goto err;
	}

	dp->allocated = true;

	dev_dbg(dev, "vdec dp: %zu bytes at %pad (fifo %zu pred %zu ref %zu mv %zu)\n",
		dp->mem.size, &dp->mem.dma, dp->fifo_off, dp->pred_off,
		dp->ref_off, dp->mv_off);
	return 0;

err:
	dp->allocated = false;
	return ret;
}

void mtk_vdec_dp_free(struct device *dev, struct mtk_vdec_dp *dp)
{
	if (!dp->allocated)
		return;

	dma_free_coherent(dev, dp->mem.size, dp->mem.base, dp->mem.dma);
	dp->mem.base = NULL;
	dp->mem.dma = 0;
	dp->allocated = false;
}

/*
 * Cache maintenance.
 *
 * dma_alloc_coherent() already returns non-cacheable memory on most ARM
 * configurations, in which case these are no-ops - but they are not guaranteed
 * to be, a coherent pool can be backed by cacheable memory on some platforms,
 * and the cost of the call is nothing next to being wrong.  They are written in
 * terms of the DMA API rather than of arm's cache primitives so that the
 * direction is explicit at every call site.
 */
void mtk_vdec_dp_sync_to_dev(struct mtk_vdec_dp *dp, size_t off, size_t len)
{
	if (!dp->allocated || !len)
		return;

	dma_sync_single_for_device(dp->mem.owner, dp->mem.dma + off, len,
				   DMA_TO_DEVICE);
}

void mtk_vdec_dp_sync_to_cpu(struct mtk_vdec_dp *dp, size_t off, size_t len)
{
	if (!dp->allocated || !len)
		return;

	dma_sync_single_for_cpu(dp->mem.owner, dp->mem.dma + off, len,
				DMA_FROM_DEVICE);
}

dma_addr_t mtk_vdec_dp_fifo_dma(struct mtk_vdec_dp *dp)
{
	return dp->mem.dma + dp->fifo_off;
}

dma_addr_t mtk_vdec_dp_pred_dma(struct mtk_vdec_dp *dp)
{
	return dp->mem.dma + dp->pred_off;
}

dma_addr_t mtk_vdec_dp_ref_dma(struct mtk_vdec_dp *dp, unsigned int pic)
{
	if (pic >= MTK_VDEC_REF_PICS)
		pic = 0;

	return dp->mem.dma + dp->ref_off + pic * dp->ref_pic_sz;
}

dma_addr_t mtk_vdec_dp_mv_dma(struct mtk_vdec_dp *dp)
{
	return dp->mem.dma + dp->mv_off;
}

dma_addr_t mtk_vdec_dp_vld_wrap_dma(struct mtk_vdec_dp *dp)
{
	return dp->mem.dma + dp->vld_wrap_off;
}

dma_addr_t mtk_vdec_dp_pp_wrap_dma(struct mtk_vdec_dp *dp)
{
	return dp->mem.dma + dp->pp_wrap_off;
}

/*
 * Push one frame's slice data into the bitstream FIFO.
 *
 * The FIFO is a ring and the driver owns only the write side.  The hardware's
 * read pointer is set to the frame's first byte and its write bound to the frame's
 * end, so the parser is told "the data is between these two addresses" rather
 * than being left to discover the end of the ring.  That is exactly how the
 * harness does it (verify/vdec_verify_vparser_h264.c:2216-2227: u4VLDRdPtr =
 * fifo + frame start, u4VLDWrPtr = fifo + V_FIFO_SZ), including its choice of the
 * FIFO end as the write pointer for the non-ring configuration.
 *
 * Returns the read/write pointers in the raw-address units RW_VLD_RPTR and
 * WO_VLD_WPTR take.  (Not the >> 6 of VSTART/VEND - that conversion belongs to
 * the caller, because it writes those two registers itself.)
 */
int mtk_vdec_dp_fifo_push(struct mtk_vdec_dp *dp, const u8 *src, size_t len,
			  dma_addr_t *rd_ptr, dma_addr_t *wr_ptr)
{
	dma_addr_t fifo = mtk_vdec_dp_fifo_dma(dp);
	size_t wr = dp->fifo_wr;

	if (!dp->allocated)
		return -ENODEV;
	if (!src || !len)
		return -EINVAL;
	if (len > VDEC_VFIFO_SIZE)
		return -ENOSPC;

	/* Wrap rather than split: a frame must be contiguous for the parser. */
	if (wr + len > VDEC_VFIFO_SIZE)
		wr = 0;

	memcpy((u8 *)dp->mem.base + dp->fifo_off + wr, src, len);
	mtk_vdec_dp_sync_to_dev(dp, dp->fifo_off + wr, len);

	*rd_ptr = fifo + wr;
	*wr_ptr = fifo + VDEC_VFIFO_SIZE;

	dp->fifo_wr = round_up(wr + len, 16);
	if (dp->fifo_wr + len > VDEC_VFIFO_SIZE)
		dp->fifo_wr = 0;

	return 0;
}
