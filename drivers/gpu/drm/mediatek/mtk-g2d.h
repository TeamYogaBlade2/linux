/* SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause */
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
 * rejected with -EINVAL leaves the engine untouched.
 *
 * @x, @y are pixel origins into the surface(s), 0..2048 inclusive.  The
 * engine has no independent source and destination origins, so mtk_g2d_blt()
 * applies one pair to both surfaces and therefore only expresses a
 * same-coordinate copy.
 */
int mtk_g2d_blt(struct mtk_g2d *g2d,
		dma_addr_t src, u32 src_pitch, enum g2d_format src_fmt,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height);

/*
 * @color is the raw constant written to the destination format, not a
 * pre-converted DRM-style value.
 */
int mtk_g2d_fill(struct mtk_g2d *g2d,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height, u32 color);

#endif /* _MTK_G2D_H_ */
