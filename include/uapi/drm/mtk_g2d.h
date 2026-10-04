/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) WITH Linux-syscall-note */
/*
 * mtk_g2d.h - userspace interface to the MediaTek G2D 2D blitter
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef _UAPI_MTK_G2D_H_
#define _UAPI_MTK_G2D_H_

#include <linux/types.h>

#include <drm/drm.h>	/* DRM_IOWR(), DRM_COMMAND_BASE, DRM_IOCTL_BASE */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * The G2D block does 2D copies: bitblt between two surfaces, and constant
 * colour fill.  These ioctls expose exactly that - nothing else the hardware
 * can do (flips, alpha blending, dithering, scaling) is reachable yet.
 *
 * Where these ioctls live
 *
 * They are driver ioctls on the mediatek-drm card node, not a separate /dev.
 * G2D is an off-path accelerator for the display pipeline rather than a
 * compositor node of its own, so it belongs to the DRM device that owns the
 * buffers it moves; a second device node would mean a second, parallel way of
 * naming those same buffers for no benefit.
 *
 * Buffer references
 *
 * Every surface is named by a GEM handle, not by a raw DMA-BUF fd and never by
 * a user pointer:
 *
 *   - A user pointer is meaningless here.  G2D is a DMA engine with no MMU in
 *     this path: the display subsystem consumes raw physical addresses, so the
 *     hardware is handed the buffer's DMA address directly and a CPU address
 *     has no hardware meaning at all.
 *   - A DMA-BUF fd is *how a buffer gets here*, not how it is named.  Buffers
 *     created by DRM clients arrive as GEM handles directly; buffers shared
 *     with another driver arrive as GEM handles after DRM_IOCTL_PRIME_FD_TO_HANDLE
 *     has imported the fd.  Using GEM handles for both means one namespace and
 *     no second import path to keep in sync.
 *
 * A GEM handle also pins the buffer for the lifetime of the file: handles are
 * looked up with drm_gem_object_lookup(), which takes a reference, and the
 * caller cannot free the backing pages while a handle exists.  That is what
 * makes it safe for the engine to be programmed with the buffer's address.
 *
 * What the engine can express
 *
 * The block has a single geometry register, G2D_W2M_SIZE, which sizes the one
 * scan window that both the source and the destination port move through.  So:
 *
 *   - The source and destination *origins* are independent (two base-address
 *     registers), so a rectangle can be moved to a different position.
 *   - The source and destination *sizes* are not.  Both ports always move
 *     width x height pixels; a copy needing different source and destination
 *     sizes cannot be expressed and is not offered here.
 *
 * Synchronous execution
 *
 * Every ioctl here is *blocking*: it returns only once the engine has stopped.
 * The engine is polled for completion with a bounded budget (100 ms), so a call
 * cannot hang indefinitely.  There is consequently no fence, no async submit
 * and no queue - a caller that wants to batch work must serialise the calls
 * itself.
 *
 * A 32-bit-on-ARM consideration: an imported buffer must be physically
 * contiguous, because the engine is given one base address and a pitch with
 * no descriptor.  This is enforced by the prime import path
 * (drm_gem_dma_prime_import_sg_table() rejects a non-contiguous scatterlist),
 * so a non-contiguous DMA-BUF fails at PRIME_FD_TO_HANDLE time rather than
 * silently producing a copy from the wrong address.
 */

/**
 * enum mtk_g2d_ioctl_id - ioctl IDs
 *
 * Place new ioctls at the end.  Do not re-order, replace or remove entries:
 * the numbers are part of the ABI.
 *
 * These IDs are not meant to be used directly; use the DRM_IOCTL_MTK_G2D_xxx
 * definitions.
 */
enum mtk_g2d_ioctl_id {
	/** @MTK_G2D_GET_CAP: Query blitter capabilities and limits. */
	MTK_G2D_GET_CAP = 0x00,

	/** @MTK_G2D_BLT: Copy one rectangle between two surfaces. */
	MTK_G2D_BLT,

	/** @MTK_G2D_FILL: Fill one rectangle of a surface with a constant. */
	MTK_G2D_FILL,

	/**
	 * __MTK_G2D_IOCTL_COUNT: number of G2D ioctls, not an ioctl itself.
	 *
	 * A driver registering the table needs this as a compile-time constant,
	 * because &drm_driver.num_ioctls applies sizeof() to an array it cannot
	 * see the definition of.  The driver static_assert()s that its table has
	 * exactly this many entries, so the two cannot drift apart.
	 */
	__MTK_G2D_IOCTL_COUNT,
};

/** Number of G2D ioctls, for &drm_driver.num_ioctls. */
#define MTK_G2D_NR_IOCTLS	__MTK_G2D_IOCTL_COUNT

/**
 * enum mtk_g2d_cap_type - capability types for MTK_G2D_GET_CAP
 *
 * Place new types at the end.  Do not re-order, replace or remove.
 */
enum mtk_g2d_cap_type {
	/** @MTK_G2D_CAP_INFO: Engine identity, formats and hard limits. */
	MTK_G2D_CAP_INFO = 0x00,
};

/**
 * enum mtk_g2d_format - pixel formats the engine's CLRFMT field encodes
 *
 * These are the encodings the hardware has, named as they are stored in
 * memory.  They are *not* DRM fourcc values: a caller converts between the two
 * itself.  bytes_per_pixel and address_align are reported by
 * @MTK_G2D_CAP_INFO so that a caller can lay out its own pitch arithmetic
 * without hardcoding the table.
 *
 * Note that RGB888 is the packed 24bpp format (three bytes per pixel, three
 * bytes per row), which is why it is the one format whose address_align is 1:
 * the data sheet places no alignment constraint on it.
 *
 * Place new values at the end.  Do not re-order, replace or remove - these
 * numbers are what MTK_G2D_BLT and MTK_G2D_FILL take in their format fields.
 */
enum mtk_g2d_format {
	/** @MTK_G2D_RGB565: 16bpp, 2 bytes per pixel, 2-byte aligned. */
	MTK_G2D_RGB565 = 0,

	/** @MTK_G2D_PARGB8888: 32bpp, 4 bytes per pixel, 4-byte aligned. */
	MTK_G2D_PARGB8888,

	/** @MTK_G2D_ARGB8888: 32bpp, 4 bytes per pixel, 4-byte aligned. */
	MTK_G2D_ARGB8888,

	/** @MTK_G2D_RGB888: packed 24bpp, 3 bytes per pixel, any alignment. */
	MTK_G2D_RGB888,

	/** @MTK_G2D_XRGB8888: 32bpp, 4 bytes per pixel, 4-byte aligned. */
	MTK_G2D_XRGB8888,

	__MTK_G2D_FORMAT_COUNT,
};

/**
 * struct mtk_g2d_cap_info - returned by @MTK_G2D_GET_CAP / @MTK_G2D_CAP_INFO
 *
 * @version: MTK_G2D_CAP_INFO_VERSION, the layout revision of this struct
 * @formats: bitmap of supported @enum mtk_g2d_format values, bit N set if
 *	@enum mtk_g2d_format N is usable
 * @max_width: largest scan window width the engine can express, in pixels
 * @max_height: largest scan window height the engine can express, in pixels
 * @max_pitch: largest pitch the engine can express, in bytes
 * @min_width: smallest usable scan window width, in pixels (always 1)
 * @min_height: smallest usable scan window height, in pixels (always 1)
 * @reserved: must be zero
 *
 * The limits are reported rather than left implicit so a client can lay out a
 * buffer and compute its pitch without hardcoding register widths: @max_pitch
 * is 0x2000 because the pitch registers hold 14 bits and 0x2000 is the usable
 * maximum, and @max_width / @max_height are 2048 because G2D_W2M_SIZE holds
 * them as 12-bit fields documented as 1..2048.  Exceeding any of them is
 * -EINVAL, not a silent truncation.
 */
#define MTK_G2D_CAP_INFO_VERSION	1

struct mtk_g2d_cap_info {
	/** @version: layout version of this struct, @MTK_G2D_CAP_INFO_VERSION. */
	__u32 version;

	/** @formats: bitmap of supported @enum mtk_g2d_format values. */
	__u32 formats;

	/** @max_width: maximum scan window width in pixels. */
	__u32 max_width;

	/** @max_height: maximum scan window height in pixels. */
	__u32 max_height;

	/** @max_pitch: maximum pitch in bytes. */
	__u32 max_pitch;

	/** @min_width: minimum scan window width in pixels. */
	__u32 min_width;

	/** @min_height: minimum scan window height in pixels. */
	__u32 min_height;

	/** @reserved: must be zero. */
	__u32 reserved;
};

/**
 * struct mtk_g2d_get_cap - arguments for DRM_IOCTL_MTK_G2D_GET_CAP
 *
 * @size: size of @cap_info, for forward compatibility.  Must be
 *	@MTK_G2D_CAP_INFO_VERSION-sized on input; the driver reports how many
 *	bytes it filled.
 * @cap_info: pointer to a &mtk_g2d_cap_info to fill, or NULL to query only
 *	the size
 *
 * Fails with -EINVAL if @size is smaller than the driver's minimum, and with
 * -E2BIG if @cap_info is not NULL but @size is smaller than the struct the
 * driver would fill - the caller is then expected to retry with a bigger
 * buffer.  @cap_info is zeroed before the fields are written, so a caller
 * that passes a newer @size learns the version it got.
 *
 * The read-only nature of this ioctl means it needs no authentication: any
 * process that can open the card node can ask what the engine can do.
 */
struct mtk_g2d_get_cap {
	/** @size: in: size of @cap_info; out: bytes actually written. */
	__u32 size;

	/** @pad: must be zero. */
	__u32 pad;

	/** @cap_info: pointer to a &mtk_g2d_cap_info, or NULL. */
	__u64 cap_info;
};

/**
 * struct mtk_g2d_surface - one operand of a G2D operation
 *
 * @handle: GEM handle naming the buffer, as returned by
 *	DRM_IOCTL_MODE_ADDFB2 (via drm_gem_fb_create_handle()) or
 *	DRM_IOCTL_PRIME_FD_TO_HANDLE.  Must be a handle this file owns.
 * @format: pixel format, an @enum mtk_g2d_format value
 * @pitch: stride of the surface in **bytes**, not pixels
 * @x: x origin of the rectangle within this surface, in pixels
 * @y: y origin of the rectangle within this surface, in pixels
 * @width: surface width in pixels, used to bound @y
 * @height: surface height in pixels, used to bound @y
 *
 * The buffer must be at least @pitch * @height bytes; anything less is
 * -EINVAL.  The engine is told a base address and a pitch and nothing else, so
 * the pitch and the declared height are the only things bounding the surface:
 * a caller that lies about them can have the engine write outside its buffer,
 * so both are checked against the real allocation size.
 *
 * @width is what bounds the rectangle horizontally: the engine has no
 * per-surface width register, so (x + rect_width) must fit within
 * @width * bytes_per_pixel as well as within @pitch.  @pitch may exceed
 * @width * bytes_per_pixel, because a stride is commonly padded - that is
 * normal and allowed.
 *
 * @x and @y are folded into the base address by the driver, because the block
 * has no offset register of any kind.  They are validated against this
 * surface's own @pitch and @format: the rectangle must fit within a single
 * row, origin included, i.e. (x + rect_width) * bytes_per_pixel <= @pitch.
 * An origin that starts partway along a row and runs off the end of it is
 * -EINVAL, not something to clamp.
 *
 * Note that the *fill* ioctl writes the rectangle and the *blit* ioctl reads
 * the source and writes the destination, so for a blit the source surface must
 * be readable and the destination writable.  Both must be linear, single-plane
 * and in a format this engine can encode; anything else is -EINVAL.
 */
struct mtk_g2d_surface {
	/** @handle: GEM handle naming the buffer. */
	__u32 handle;

	/** @format: pixel format, an @enum mtk_g2d_format value. */
	__u32 format;

	/** @pitch: stride in bytes. */
	__u32 pitch;

	/** @x: x origin of the rectangle, in pixels. */
	__u32 x;

	/** @y: y origin of the rectangle, in pixels. */
	__u32 y;

	/** @width: surface width in pixels. */
	__u32 width;

	/** @height: surface height in pixels. */
	__u32 height;
};

/**
 * struct mtk_g2d_blt - arguments for DRM_IOCTL_MTK_G2D_BLT
 *
 * @size: size of this struct, for forward compatibility.  Must be at least
 *	@MTK_G2D_BLT_MIN_SIZE on input; unknown trailing bytes are ignored, so
 *	a newer struct can be passed to an older driver.
 * @flags: must be zero.  No flags are defined; a non-zero value is -EINVAL
 *	rather than being ignored, so a typo cannot silently change behaviour.
 * @src: source surface, read by the engine
 * @dst: destination surface, written by the engine
 * @rect_width: rectangle width in pixels, shared by both surfaces
 * @rect_height: rectangle height in pixels, shared by both surfaces
 * @reserved: must be zero
 *
 * Copies @rect_width x @rect_height pixels from @src to @dst.  The origins
 * (@src.x, @src.y and @dst.x, @dst.y) are independent, so a rectangle can be
 * moved as well as copied.
 *
 * @rect_width and @rect_height apply to *both* surfaces: the engine has one
 * scan window, so a copy with different source and destination sizes cannot be
 * expressed and is not offered.  Both must be 1..2048.
 *
 * In-place operation is allowed when the two rectangles do not overlap, since
 * the engine's read/write ordering is unspecified and an overlapping in-place
 * copy would have no defined result; an overlap is -EINVAL.
 *
 * This ioctl blocks until the engine is idle.  It returns:
 *
 *   - 0: the copy completed.
 *   - -EINVAL: a malformed or unsupported argument.  Nothing was programmed, so
 *     the engine is untouched and no memory was written.
 *   - -ENOENT: a handle does not name a buffer this file owns.
 *   - -ENODEV: this DRM device has no G2D blitter.  A caller that can fall
 *     back to a software path should treat this as ordinary.
 *   - -EBUSY: the buffers could not be locked; another user holds them.
 *   - -EFAULT: the argument structure could not be copied from userspace.
 *   - -E2BIG: @size is smaller than this driver knows how to parse.
 *   - -ETIMEDOUT: the engine did not stop within its 100 ms budget.  The
 *     driver has attempted a warm reset, but that recovery is best-effort: the
 *     engine may still be running, so **the destination buffer may still be
 *     being written after this call returns**.  Treat this as "engine state is
 *     now unknown", do not reuse the destination, and stop using the blitter.
 *     The errno is surfaced rather than swallowed precisely so a caller can
 *     make that decision.
 *
 * There is deliberately no fence or async submit: the call is synchronous, so
 * a completion signal would have nothing to wait on.
 */
struct mtk_g2d_blt {
	/** @size: size of this struct; must be >= @MTK_G2D_BLT_MIN_SIZE. */
	__u32 size;

	/** @flags: must be zero. */
	__u32 flags;

	/** @src: source surface, read by the engine. */
	struct mtk_g2d_surface src;

	/** @dst: destination surface, written by the engine. */
	struct mtk_g2d_surface dst;

	/** @rect_width: rectangle width in pixels, shared by both surfaces. */
	__u32 rect_width;

	/** @rect_height: rectangle height in pixels, shared by both surfaces. */
	__u32 rect_height;

	/** @reserved: must be zero. */
	__u32 reserved;
};

/**
 * struct mtk_g2d_fill - arguments for DRM_IOCTL_MTK_G2D_FILL
 *
 * @size: size of this struct, for forward compatibility; unknown trailing
 *	bytes are ignored.
 * @flags: must be zero, as for @mtk_g2d_blt.
 * @dst: destination surface, written by the engine
 * @rect_width: rectangle width in pixels
 * @rect_height: rectangle height in pixels
 * @color: the raw constant written to the destination format, not a
 *	pre-converted DRM-style pixel value.  It is written to the engine's
 *	constant-colour register verbatim, so its meaning depends entirely on
 *	@dst.format.
 * @reserved: must be zero
 *
 * Fills @rect_width x @rect_height pixels at @dst's origin with @color.
 *
 * @dst.x / @dst.y give the origin of the filled rectangle, so a fill can
 * target a sub-rectangle rather than the whole surface.  @rect_width and
 * @rect_height must be 1..2048.
 *
 * This ioctl blocks until the engine is idle, and its errors are the same set
 * as @mtk_g2d_blt's - including -ETIMEDOUT, with the same warning that the
 * destination may still be written after the call returns.
 */
struct mtk_g2d_fill {
	/** @size: size of this struct; must be >= @MTK_G2D_FILL_MIN_SIZE. */
	__u32 size;

	/** @flags: must be zero. */
	__u32 flags;

	/** @dst: destination surface, written by the engine. */
	struct mtk_g2d_surface dst;

	/** @rect_width: rectangle width in pixels. */
	__u32 rect_width;

	/** @rect_height: rectangle height in pixels. */
	__u32 rect_height;

	/** @color: raw constant-colour value for @dst.format. */
	__u32 color;

	/** @reserved: must be zero. */
	__u32 reserved;
};

/*
 * Minimum accepted @size for each argument struct.
 *
 * Computed rather than spelled out, so it cannot drift from the layout, and
 * expressed with a struct that ends at the last required field: offsetof()
 * would need <stddef.h>, which a kernel build cannot include.  Each value must
 * be <= the sizeof() of the struct it describes - a caller always passes the
 * full struct, so a minimum larger than it would reject every call with -E2BIG.
 * That mistake happened once during development and is now static_assert()ed in
 * mtk-g2d-uapi.c.
 */
struct mtk_g2d_blt_min {
	__u32 size;
	__u32 flags;
	struct mtk_g2d_surface src;
	struct mtk_g2d_surface dst;
	__u32 rect_width;
	__u32 rect_height;
	__u32 reserved;
};

struct mtk_g2d_fill_min {
	__u32 size;
	__u32 flags;
	struct mtk_g2d_surface dst;
	__u32 rect_width;
	__u32 rect_height;
	__u32 color;
	__u32 reserved;
};

#define MTK_G2D_BLT_MIN_SIZE	((__u32)sizeof(struct mtk_g2d_blt_min))
#define MTK_G2D_FILL_MIN_SIZE	((__u32)sizeof(struct mtk_g2d_fill_min))

/*
 * Driver ioctls ride on the DRM command space: DRM_COMMAND_BASE (0x40) plus the
 * @mtk_g2d_ioctl_id, with 'd' as the ioctl type.  The numbers therefore never
 * collide with a core ioctl or with another driver's, and DRM_IOCTL_DEF_DRV()
 * can build the driver's dispatch table straight from these.
 */
#define DRM_IOCTL_MTK_G2D_GET_CAP	\
	DRM_IOWR(DRM_COMMAND_BASE + MTK_G2D_GET_CAP, struct mtk_g2d_get_cap)
#define DRM_IOCTL_MTK_G2D_BLT		\
	DRM_IOWR(DRM_COMMAND_BASE + MTK_G2D_BLT, struct mtk_g2d_blt)
#define DRM_IOCTL_MTK_G2D_FILL		\
	DRM_IOWR(DRM_COMMAND_BASE + MTK_G2D_FILL, struct mtk_g2d_fill)

#if defined(__cplusplus)
}
#endif

#endif /* _UAPI_MTK_G2D_H_ */