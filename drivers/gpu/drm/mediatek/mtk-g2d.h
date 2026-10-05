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

/**
 * mtk_g2d_max_pitch - largest pitch the engine can express, in bytes.
 *
 * The pitch registers hold 14 bits and 0x2000 is the usable maximum.  Exposed so
 * that a caller which has to check a pitch *before* handing it over - a
 * userspace ABI, say - reads the same number the engine will apply.
 */
u32 mtk_g2d_max_pitch(void);

/**
 * mtk_g2d_max_width - largest scan window width, in pixels.
 *
 * G2D_W2M_SIZE holds WIDTH as a 12-bit field documented as 1..2048.
 */
u32 mtk_g2d_max_width(void);

/**
 * mtk_g2d_max_height - largest scan window height, in pixels.
 *
 * G2D_W2M_SIZE holds HEIGHT as a 12-bit field documented as 1..2048.
 */
u32 mtk_g2d_max_height(void);

/**
 * mtk_g2d_addr_align - required start-address alignment of one format.
 * @format: an &enum g2d_format
 * @align: alignment in bytes, returned to the caller
 *
 * The data sheet requires 2-byte alignment for RGB565 and 4-byte for the 8888
 * formats; RGB888 may start at any address, which is reported as 1.
 *
 * Returns 0 and writes @align, or -EINVAL if @format is not a format the engine
 * can encode - in which case @align is not written.
 */
int mtk_g2d_addr_align(u32 format, u32 *align);

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
 * Origins and sizes.  This block has no origin register and no per-surface
 * size register:
 *
 *   - The source and destination ORIGINS are independent.  They are not
 *     independent in hardware terms - they are two plain base addresses,
 *     G2D_SRC_ADDR and G2D_W2M_ADDR, with the origin folded into them in
 *     software - but the practical consequence is that mtk_g2d_blt_rect()
 *     can move a rectangle from one position to a different position.
 *   - The source and destination SIZES are not independent.  G2D_W2M_SIZE is
 *     the only geometry register and it sizes the one scan window both ports
 *     move through, so both always move width x height pixels.  A copy that
 *     needs different source and destination sizes is not expressible at all.
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
 * The reference holds the platform device alive, so the result cannot dangle if
 * the block is unbound.  Release it with mtk_g2d_put().
 */
struct mtk_g2d *mtk_g2d_get(struct device *dev);

/**
 * mtk_g2d_put - release a reference taken by mtk_g2d_get().
 *
 * NULL is accepted and ignored, so a caller that treats "no blitter" as an
 * ordinary case needs no branch of its own.
 */
void mtk_g2d_put(struct mtk_g2d *g2d);

/* The &struct device behind an engine, so a caller holding a reference can
 * attribute diagnostics to the right device.
 */
struct device *mtk_g2d_device(struct mtk_g2d *g2d);

#endif /* _MTK_G2D_H_ */
