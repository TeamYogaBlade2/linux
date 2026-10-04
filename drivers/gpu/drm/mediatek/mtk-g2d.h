/* SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause) */
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef _MTK_G2D_H_
#define _MTK_G2D_H_

#include <linux/dma-mapping.h>
#include <linux/types.h>

struct mtk_g2d;

/* CLRFMT encodings, from the data sheet's SRC_CON/DST_CON description. */
enum g2d_format {
	g2d_clrfmt_rgb565 = 0,
	g2d_clrfmt_pargb8888,
	g2d_clrfmt_argb8888,
	g2d_clrfmt_rgb888,
	g2d_clrfmt_xrgb8888,
};

struct g2d_format_info {
	u32 clrfmt;
	u32 bytes_per_pixel;
	/* Required start-address alignment: 2, 4 or 1 for no constraint. */
	u32 address_align;
};

/*
 * Pitch is in bytes, not pixels, and width/height are in pixels.
 *
 * Both calls are fully synchronous: they return only once the engine has
 * gone idle, and they report -ETIMEDOUT if it did not (after attempting a
 * warm reset, so a subsequent call starts from a known state).  Every
 * argument is validated before any register is programmed, so a request
 * rejected with -EINVAL leaves the engine untouched.  A NULL @g2d is
 * -EINVAL rather than a crash.
 *
 * Origins.  This block has no origin register of any kind, and no per-surface
 * size register: G2D_W2M_SIZE is the only geometry register, and it sizes the
 * one scan window that both the source and the destination port move through.
 *
 *   - The source and destination ORIGINS are independent.  They are not
 *     independent in hardware terms - they are two plain base addresses,
 *     G2D_SRC_ADDR and G2D_W2M_ADDR, with the origin folded into them in
 *     software - but the practical consequence is that mtk_g2d_blt_rect()
 *     can move a rectangle from one position to a different position.
 *   - The source and destination SIZES are not independent.  Both ports
 *     always move width x height pixels, because only one size register
 *     exists.  A copy that needs different source and destination sizes is
 *     not expressible at all.
 *
 * Note also that G2D_DST_* is the destination *read* port, used for
 * read-modify-write blending; the only writable address is G2D_W2M_ADDR.  So
 * G2D_DST_ADDR cannot be used as a second write target or as a substitute for
 * a destination origin.
 */

/**
 * mtk_g2d_blt - copy a rectangle at one origin shared by both surfaces.
 * @x, @y: pixel origin, applied to both the source and the destination
 *
 * The shared-origin form of mtk_g2d_blt_rect(): @src_x = @dst_x = @x and
 * @src_y = @dst_y = @y.  It expresses a same-coordinate copy.  Use
 * mtk_g2d_blt_rect() to move a rectangle to a different position.
 */
int mtk_g2d_blt(struct mtk_g2d *g2d,
		dma_addr_t src, u32 src_pitch, enum g2d_format src_fmt,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height);

/**
 * mtk_g2d_blt_rect - copy a rectangle between two independent origins.
 * @src_x, @src_y: origin within the source surface, in pixels
 * @dst_x, @dst_y: origin within the destination surface, in pixels
 * @width, @height: rectangle size, in pixels, 1..2048
 *
 * @width and @height are shared by both surfaces and cannot differ.  Each
 * surface's origin is validated against that surface's own pitch, bytes per
 * pixel and address alignment, and each rectangle must fit within one row of
 * its surface, origin included.
 *
 * A blit of a buffer onto itself is permitted only when the two rectangles do
 * not overlap; an overlapping in-place blit is -EINVAL.  The engine reads and
 * writes row by row and its ordering is not specified, so an overlapping
 * in-place copy has no defined result.
 *
 * Blocks until the engine is idle.
 */
int mtk_g2d_blt_rect(struct mtk_g2d *g2d,
		      dma_addr_t src, u32 src_pitch, enum g2d_format src_fmt,
		      u32 src_x, u32 src_y,
		      dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		      u32 dst_x, u32 dst_y,
		      u32 width, u32 height);

/*
 * @color is the raw constant written to the destination format, not a
 * pre-converted DRM-style value.
 *
 * @x, @y, @width and @height are the origin and size of the filled rectangle,
 * which must fit within one row of the destination, origin included.  Blocks
 * until the engine is idle; -EINVAL for a NULL @g2d.
 */
int mtk_g2d_fill(struct mtk_g2d *g2d,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height, u32 color);

/**
 * mtk_g2d_get - find the G2D device belonging to a DRM device.
 * @dev: a mediatek-drm device
 *
 * The DRM device and the G2D block are separate platform devices under the same
 * MMSYS parent.  Returns the &struct mtk_g2d that @dev may use, with a
 * reference held, or NULL if there is none - in which case an accelerated path
 * must fall back rather than fail.  An MDP DRM device, for instance, has no
 * G2D.
 *
 * The reference is what makes the pointer safe: it holds the platform device
 * alive, so the result cannot dangle if the block is unbound.  It must be
 * released with mtk_g2d_put().
 */
struct mtk_g2d *mtk_g2d_get(struct device *dev);

/**
 * mtk_g2d_put - release a reference taken by mtk_g2d_get().
 *
 * NULL is accepted and ignored, so a caller that treats "no blitter" as an
 * ordinary case needs no branch of its own.
 */
void mtk_g2d_put(struct mtk_g2d *g2d);

/**
 * mtk_g2d_device - the &struct device behind an engine.
 *
 * Exposed so a caller holding a reference can attribute diagnostics to the
 * right device.  The &struct mtk_g2d itself is private to the driver.
 */
struct device *mtk_g2d_device(struct mtk_g2d *g2d);

#endif /* _MTK_G2D_H_ */
