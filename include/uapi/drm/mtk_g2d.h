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
 * They are on a DRM *render* node belonging to G2D itself, and not on the
 * mediatek-drm card node.  They used to be on the display card node, because
 * there was nowhere else for them to be: G2D owned no DRM device, so its ioctls
 * were hung off the display device - which made a modesetting card the only way
 * to reach a block that has nothing to do with modesetting.  G2D now has its own
 * device with its own GEM namespace and its own DMA identity, which is what lets
 * a display buffer be shared with it safely (see @mtk_g2d_surface).
 *
 * Buffer references
 *
 * Every surface is named by a GEM handle *in the G2D node's namespace*, not by
 * a raw DMA-BUF fd and never by a user pointer:
 *
 *   - A user pointer is meaningless here.  G2D is a DMA engine and the buffer
 *     is handed to hardware, not read by the kernel, so the hardware is given
 *     the buffer's DMA address and a CPU address has no hardware meaning.
 *   - A DMA-BUF fd is *how a buffer gets here*, not how it is named.  Buffers
 *     this device allocated arrive as GEM handles directly; buffers shared with
 *     the display pipeline arrive as GEM handles after
 *     DRM_IOCTL_PRIME_FD_TO_HANDLE on this node has imported the fd.  One
 *     namespace, no second import path to keep in sync.
 *
 * A GEM handle also pins the buffer for the lifetime of the file: handles are
 * looked up with drm_gem_object_lookup(), which takes a reference, and the
 * caller cannot free the backing pages while a handle exists.  That is what
 * makes it safe for the engine to be programmed with the buffer's address.
 *
 * Formats
 *
 * Surfaces are described with DRM fourccs (DRM_FORMAT_RGB565, DRM_FORMAT_RGB888,
 * DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888).  The engine has its own CLRFMT
 * encodings, but those are a hardware detail: the driver converts from the
 * fourcc inside the kernel, so a buffer's format is described the same way
 * whichever device allocated it.  Anything else is -EINVAL.
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
 * The wait is bounded, so a call cannot hang indefinitely, and the destination
 * is never left owned by an engine the driver has lost track of - see
 * @mtk_g2d_blt's description of -ETIMEDOUT.  There is consequently no fence,
 * no async submit and no queue - a caller that wants to batch work must
 * serialise the calls itself.
 *
 * Being synchronous is also what keeps the buffers alive for the operation
 * without any extra machinery: the driver holds a reference and the
 * reservation lock on both operands from before the first register write
 * until after the engine is idle, so there is no window in which the caller
 * could unmap a buffer the hardware is still using.
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
 * enum mtk_g2d_format_index - indices into the @formats bitmap
 *
 * Formats are named by DRM fourcc in the ioctl arguments - @mtk_g2d_surface's
 * @format takes a DRM_FORMAT_* value directly - but DRM fourccs are a sparse
 * 32-bit space, so they cannot index a bitmap.  This enum exists only to give
 * @mtk_g2d_cap_info::formats something to count.
 *
 * Place new values at the end.  Do not re-order, replace or remove.
 */
enum mtk_g2d_format_index {
	/** @MTK_G2D_FMT_RGB565: DRM_FORMAT_RGB565, 2 bytes per pixel. */
	MTK_G2D_FMT_RGB565 = 0,

	/** @MTK_G2D_FMT_ARGB8888: DRM_FORMAT_ARGB8888, 4 bytes per pixel. */
	MTK_G2D_FMT_ARGB8888,

	/** @MTK_G2D_FMT_RGB888: DRM_FORMAT_RGB888, packed 24bpp. */
	MTK_G2D_FMT_RGB888,

	/** @MTK_G2D_FMT_XRGB8888: DRM_FORMAT_XRGB8888, 4 bytes per pixel. */
	MTK_G2D_FMT_XRGB8888,

	/** @__MTK_G2D_FORMAT_COUNT: number of formats, not a format. */
	__MTK_G2D_FORMAT_COUNT,
};

/**
 * Formats not offered, and why
 *
 * The engine's CLRFMT field can encode a pre-multiplied 8888 format, and this
 * header deliberately does not expose it.  DRM has no fourcc for
 * pre-multiplied alpha: DRM_FORMAT_ARGB8888 names *non*-premultiplied alpha,
 * so a caller passing that fourcc means the bytes are straight, and a
 * premultiplied interpretation of the same bytes is a different image.  Naming
 * it DRM_FORMAT_ARGB8888 would be silently wrong and naming it with a private
 * value would fork the format namespace for one engine.
 *
 * The bitblt path is unaffected: it moves pixels without interpreting them, so
 * a pre-multiplied buffer would copy correctly, which is exactly why leaving it
 * out is about honesty rather than capability.  It is also the format whose
 * alpha channel the engine applies in hardware on the fill path, where the
 * driver would have no way to tell a caller whether @color was premultiplied.
 * Both of those are reasons to add it later as a proper DRM format modifier, not
 * to guess here.
 */

/**
 * struct mtk_g2d_cap_info - returned by @MTK_G2D_GET_CAP / @MTK_G2D_CAP_INFO
 *
 * @formats: bitmap of supported formats, indexed by @mtk_g2d_format_index
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
 *
 * There is no @version field.  A struct whose size is encoded in the ioctl
 * number cannot be grown compatibly by a *newer driver* talking to an *older
 * caller*: the caller's ioctl number was built from its own sizeof(), the
 * driver dispatches on its own, and the two numbers differ, so the call is
 * rejected before the struct is ever copied.  Adding a field is therefore a
 * clean break, and pretending otherwise with a version field would only buy
 * the illusion of compatibility.  See @mtk_g2d_get_cap.
 */
struct mtk_g2d_cap_info {
	/** @formats: bitmap of supported formats, by @mtk_g2d_format_index. */
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
 * @cap_info: the &mtk_g2d_cap_info to fill, in place
 *
 * The struct is the ioctl's argument and nothing is pointed at, so the core
 * copies it in before the handler runs and back out after it returns.  There is
 * no size, no version and no pointer: the ioctl number already encodes
 * sizeof(struct mtk_g2d_get_cap), which is the only size negotiation that works
 * across a driver change.
 *
 * The read-only nature of this ioctl means it needs no authentication: any
 * process that can open the node can ask what the engine can do.
 */
struct mtk_g2d_get_cap {
	/** @cap_info: capability and limit report, filled by the driver. */
	struct mtk_g2d_cap_info cap_info;
};

/**
 * struct mtk_g2d_surface - one operand of a G2D operation
 *
 * @handle: GEM handle naming the buffer, in *this* device's namespace: either
 *	a handle returned by DRM_IOCTL_PRIME_FD_TO_HANDLE on the G2D node, or
 *	one it allocated itself.  A handle belonging to the display DRM device
 *	is -ENOENT here, and that is not an oversight - see below.
 * @format: pixel format, a DRM fourcc (DRM_FORMAT_RGB565, DRM_FORMAT_RGB888,
 *	DRM_FORMAT_ARGB8888 or DRM_FORMAT_XRGB8888)
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
 *
 * Why the handle must come from this node
 *
 * G2D is not the device that allocated the buffer, so it cannot be handed the
 * display device's address for it.  With the M4U in the path an address is an
 * IOVA in the page table of the engine that asked for the mapping, and the two
 * engines' pages are not the same mapping.  To use a display buffer, export it
 * (PRIME_HANDLE_TO_FD on the display node) and import it here
 * (PRIME_FD_TO_HANDLE on this node); the import maps it into G2D's address
 * space and the handle below then resolves to a G2D IOVA.
 *
 * @format is a DRM fourcc and not a driver-private encoding because the buffer
 * has a real, public format independent of who programmed it.  The driver's
 * CLRFMT is a hardware detail and is chosen from the fourcc inside the kernel;
 * a caller does not need to know it exists.  Only the four formats the engine
 * can encode are accepted, and the rest are -EINVAL rather than approximated.
 */
struct mtk_g2d_surface {
	/** @handle: GEM handle naming the buffer. */
	__u32 handle;

	/** @format: pixel format, a DRM fourcc. */
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
 * The two surfaces must be two different buffers: a blit of one buffer onto
 * itself is -EINVAL, even when the rectangles are far apart.  The engine's
 * read/write ordering is unspecified, so an overlapping in-place copy has no
 * defined result, and with differing pitches and origins it is not decidable
 * from these arguments whether the rectangles overlap at all - row n of the
 * source and row n of the destination sit at different strides, so the rows
 * whose byte ranges intersect are not the rows whose y ranges do.  A client
 * that wants to move data within one buffer should do it with two buffers and
 * a copy, or in software.
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
 *   - -ETIMEDOUT: the engine did not stop within its 100 ms budget.  The
 *     driver then warm-resets it and polls several times more.  If the engine
 *     is idle by then the reset worked, the buffers are safe, and only this call
 *     failed.  If it is *still* busy the driver cannot stop it: the hardware may
 *     be writing the address last programmed, and no register write documented
 *     here can prevent that.  In that case the driver declares the engine
 *     **wedged**, which means two things for a caller:
 *
 *       1. **The source and destination of this operation may still be read
 *          or written by the engine indefinitely.**  The driver does not
 *          release them - it cannot promise the operation is over - but it
 *          cannot keep them alive either, so that memory must be treated as
 *          belonging to the engine.  Do not unmap it, and do not hand the same
 *          buffer to anything else.
 *       2. **Every later BLT and FILL returns -ETIMEDOUT immediately**, without
 *          programming a register or waiting on the hardware.  The blitter is
 *          unusable until the device is unbound; treat it as permanently lost
 *          and fall back to a software path.
 *
 *     The errno is surfaced rather than swallowed precisely so a caller can
 *     make that decision.  A -ETIMEDOUT is not a transient error to retry:
 *     after a wedge the retry cannot succeed, because nothing was submitted.
 *
 * There is deliberately no fence or async submit: the call is synchronous, so
 * a completion signal would have nothing to wait on.
 *
 * There is no @size field.  An earlier revision of this header had one, and
 * claimed that "unknown trailing bytes are ignored, so a newer struct can be
 * passed to an older driver".  That was false, and the ioctl number is what
 * makes it false: DRM_IOCTL_MTK_G2D_BLT is built with
 * _IOWR(..., struct mtk_g2d_blt), which encodes sizeof(struct mtk_g2d_blt) into
 * the number itself.  A caller built against a newer, larger struct and a
 * driver built against the older, smaller one therefore issue *different*
 * numbers, and the driver's table has no entry for the caller's number, so the
 * call fails with -ENOTTY before the struct is ever copied.  A @size field
 * cannot rescue that; it can only mislead a reader into believing a
 * compatibility that does not exist.
 *
 * Growing a struct is a clean break, and that is the honest description.  The
 * same reasoning removed the size/version dance from @mtk_g2d_get_cap.
 */
struct mtk_g2d_blt {
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
 * as @mtk_g2d_blt's - including -ETIMEDOUT, with the same warning about a
 * wedged engine and a destination that may still be written.
 */
struct mtk_g2d_fill {
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
