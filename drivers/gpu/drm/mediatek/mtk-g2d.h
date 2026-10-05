/* SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause) */
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef _MTK_G2D_H_
#define _MTK_G2D_H_

#include <linux/dma-mapping.h>
#include <linux/types.h>

struct mtk_g2d;

/**
 * struct mtk_g2d_drm_private - per-G2D-node state the ioctl layer needs.
 * @g2d: the engine this node programs
 *
 * It lives here, and holds nothing but the engine pointer, because
 * mtk-g2d.c must stay free of DRM dependencies: it owns the hardware and the
 * programming sequence, while mtk-g2d-uapi.c owns the DRM device and the
 * ioctls.  The node's &struct drm_device is not referenced at all - no field
 * of it is needed by either file - so the two can be built without DRM
 * headers seeing each other.
 *
 * This is deliberately not a second route to the engine from elsewhere in the
 * tree.  The old mtk_g2d_get() walked the device tree looking for a sibling
 * G2D on behalf of the display DRM device, which existed only because the
 * display device was reaching a block it does not own.  G2D now owns a DRM
 * device of its own, so the engine is reached only from the node that was
 * created with it, and nothing outside mtk-g2d-uapi.c has a pointer to it.
 */
struct mtk_g2d_drm_private {
	struct mtk_g2d *g2d;
};

/**
 * mtk_g2d_register_drm - register G2D's own DRM render node.
 * @dev: the G2D platform device
 * @g2d: the engine, already probed and idle
 *
 * Allocates a &drm_device with no parent - G2D is not a component of
 * anything, and giving it a parent would imply a device tree topology it does
 * not have - and registers it as DRIVER_GEM | DRIVER_RENDER with the G2D
 * ioctls and PRIME.  The result is a /dev/dri/renderD* node and nothing else:
 * no KMS, no primary node, no master.
 *
 * Returns an opaque handle to pass to mtk_g2d_unregister_drm(), or an error
 * pointer.  The handle is void * rather than &drm_device so that this header,
 * and with it mtk-g2d.c, can stay free of DRM types; mtk-g2d-uapi.c is the
 * only object that interprets it.
 *
 * Implemented in mtk-g2d-uapi.c, not here: that is the only one of the two
 * objects that includes DRM headers.
 */
void *mtk_g2d_register_drm(struct device *dev, struct mtk_g2d *g2d);

/**
 * mtk_g2d_unregister_drm - take the render node back down.
 * @drm: handle from mtk_g2d_register_drm(), or NULL
 *
 * Unplugs the device and drops the driver reference.  Must run while the
 * engine's clocks are still on, and must be the last thing that can start a
 * G2D operation.
 *
 * The unplug is what makes that true.  drm_dev_unregister() only unpublishes
 * the minor - it does not stop an ioctl on a file that is already open -
 * whereas drm_dev_unplug() sets ->unplugged and then synchronises the read
 * section the ioctls run inside, so on return no operation is executing and
 * none can start.  The caller may therefore release the engine's clocks as
 * soon as this returns.
 *
 * The reference dropped here is the driver's, and it is the last one only once
 * every open file has closed.  That is what keeps the private data the ioctls
 * dereference alive until the last of them has finished with it.
 *
 * A NULL handle is ignored, which is what makes this safe to call
 * unconditionally from mtk_g2d_remove() when probe may have stopped before
 * registration.
 */
void mtk_g2d_unregister_drm(void *drm);

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
 * gone idle, and they report -ETIMEDOUT if it did not.  On that path the
 * driver warm-resets the engine and re-polls several times; if it is still
 * busy after the last of them it is *wedged* - shut out for good - because the
 * hardware may then still be writing the address last programmed and no
 * register write documented here can stop it.  A wedged engine refuses every
 * subsequent call with -ETIMEDOUT without programming a register or waiting on
 * the hardware, and the buffers of the operation that wedged it are not
 * released, so the memory they name is not reused underneath the engine.
 * Every argument is validated before any register is programmed, so a request
 * rejected with -EINVAL leaves the engine untouched.  A NULL @g2d is -EINVAL
 * rather than a crash.
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
 * A blit of a buffer onto itself is -EINVAL, even when the two rectangles are
 * far apart.  The engine reads a row and writes a row with no ordering this
 * driver can specify, and whether the rectangles overlap at all cannot be
 * answered from the arguments: with differing pitches and origins, row n of
 * the source and row n of the destination sit at different strides, so the
 * rows whose byte ranges intersect are not the rows whose y ranges do.
 * Requiring two surfaces is the safe rule; offering in-place blits would need
 * a real per-row address walk to be correct.
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

/* The &struct device behind an engine, so a caller holding a reference can
  * attribute diagnostics to the right device.
  */
struct device *mtk_g2d_device(struct mtk_g2d *g2d);

#endif /* _MTK_G2D_H_ */
