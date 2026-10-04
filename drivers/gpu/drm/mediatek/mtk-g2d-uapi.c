// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * Userspace interface to the G2D 2D blitter.
 *
 * These ioctls are deliberately thin.  Everything that decides whether an
 * operation is *safe* - resolving a buffer, taking a reference to it, locking
 * it, validating the geometry against what the registers can hold, and waiting
 * for the engine to stop - already exists in the DRM-side helper layer
 * (mtk_g2d_drm_blt_rect() / mtk_g2d_drm_fill() in mtk_drm_drv.c).  This file
 * adds the ABI on top: validate the arguments strictly, then hand them over.
 *
 * Three things this file does *not* do, each for a reason:
 *
 *  - It does not re-declare the engine's limits.  A copy of G2D_PITCH_MAX and
 *    friends would be a second source of truth that silently rots; the accessors
 *    in mtk-g2d.h below make mtk-g2d.c the only place a number is written
 *    down, and this file asks it rather than restating it.  That is what keeps
 *    "the ABI accepted a pitch the register truncates" from being possible.
 *
 *  - It does not import DMA-BUFs or map memory.  Buffers are named by GEM
 *    handle, which covers a DRM-allocated buffer directly and a buffer shared
 *    with another driver after DRM_IOCTL_PRIME_FD_TO_HANDLE has imported its fd.
 *    A GEM handle pins the allocation for the life of the file, and the prime
 *    import path already rejects a non-contiguous scatterlist - the one property
 *    the engine cannot work without, since it takes a single base address and
 *    a pitch with no descriptor.
 *
 *  - It does not use a DMA-BUF fd in the ABI.  A user pointer is meaningless
 *    here (G2D is a DMA engine and the display path has no IOMMU, so hardware
 *    consumes physical addresses), and a raw fd would be a second, parallel way
 *    of naming the same buffers that the GEM namespace already covers.
 *
 * The one behaviour this file does add on top of the helper is *strictness*.
 * mtk_g2d_drm_blt_rect() clips an over-large rectangle down to what the buffers
 * can supply, which is right for an internal caller that asked for "copy what
 * you can" and wrong for an ABI, where silently doing less than was requested
 * is a bug the caller cannot see.  So every bound is re-checked here and
 * reported as -EINVAL.  By the time the helper runs, the request is known to
 * fit: nothing is ever clipped, and the two layers cannot disagree about what
 * was accepted.
 *
 * A side effect worth naming, because it is a real benefit rather than a
 * convenience: the helper's locking, reference-taking and format validation
 * only work on a &struct drm_framebuffer, so this file builds a framebuffer
 * view of each GEM object.  That is what lets the audited buffer handling be
 * reused instead of reimplemented - but it also means an ioctl buffer is
 * *required* to be a single-plane, linear framebuffer, which is exactly what the
 * engine needs anyway.
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/types.h>

#include <drm/mtk_g2d.h>

#include "mtk-g2d.h"
#include "mtk_drm_drv.h"

/**
 * struct mtk_g2d_uapi_surf - one operand, resolved and validated
 * @obj: the GEM object, referenced for as long as this structure lives
 * @fb: the framebuffer view the helper layer takes
 * @addr: DMA address of the first byte of the buffer
 * @size: real size of the allocation in bytes, from obj->size
 * @pitch: stride in bytes
 * @bpp: bytes per pixel, from the format
 * @width, @height: surface dimensions in pixels, as declared by userspace
 * @x, @y: origin of the rectangle within the surface, in pixels
 *
 * @size is the load-bearing field.  The engine is given a base address, a
 * pitch and a scan window size and nothing else: it has no idea where the
 * buffer ends.  So the pitch and the declared height - both numbers userspace
 * supplied - would otherwise be the *only* things bounding the destination, and
 * a caller declaring a 0x2000 pitch for a 4 KiB buffer would have the engine
 * write 0x2000 * height bytes into it, with the hardware none the wiser.  Every
 * rectangle is therefore also checked against @size.
 */
struct mtk_g2d_uapi_surf {
	struct drm_gem_object *obj;
	struct drm_framebuffer fb;
	dma_addr_t addr;
	size_t size;
	u32 pitch;
	u32 bpp;
	u32 width;
	u32 height;
	u32 x;
	u32 y;
};

/**
 * mtk_g2d_uapi_map_format - map a @mtk_g2d_format onto a DRM fourcc.
 *
 * Returns a format the helper layer recognises, or NULL if there is none.
 *
 * The helper maps DRM fourccs onto CLRFMT in one direction only, so an ioctl
 * that takes engine-native formats has to come back the other way.  The
 * mapping is one-to-one over the four formats both sides have in common.
 *
 * MTK_G2D_PARGB8888 is the honest awkward case: the engine can encode a
 * pre-multiplied format, and DRM has no fourcc for it - DRM_FORMAT_ARGB8888
 * names non-premultiplied alpha.  Rather than let the two silently disagree
 * about what the bytes mean, PARGB8888 is reported as *unsupported* here and is
 * excluded from the capability bitmap.  A caller wanting pre-multiplied alpha
 * has no representation for it yet; getting that wrong would corrupt colours,
 * which is a much worse outcome than a format being unavailable.
 */
static const struct drm_format_info *
mtk_g2d_uapi_map_format(u32 format)
{
	switch (format) {
	case MTK_G2D_RGB565:
		return drm_format_info(DRM_FORMAT_RGB565);
	case MTK_G2D_ARGB8888:
		return drm_format_info(DRM_FORMAT_ARGB8888);
	case MTK_G2D_RGB888:
		return drm_format_info(DRM_FORMAT_RGB888);
	case MTK_G2D_XRGB8888:
		return drm_format_info(DRM_FORMAT_XRGB8888);
	default:
		/* Includes MTK_G2D_PARGB8888: see above. */
		return NULL;
	}
}

/**
 * mtk_g2d_uapi_resolve - resolve and fully validate one operand.
 * @file_priv: file the handle must belong to
 * @s: the surface as userspace described it
 * @rect_w, @rect_h: the rectangle size, shared by both operands
 * @out: resolved operand, with a reference held on success
 *
 * Returns 0, or a negative errno.  On every error @out->obj is left NULL, so
 * the caller can run one unconditional cleanup on every path - which is what
 * makes "a failed import cannot leak a reference" a property of the shape of
 * the code rather than something to remember per path.
 *
 * The checks, and why each one is here rather than left to the hardware:
 *
 *  1. @format must be one the engine can encode *and* that has a DRM fourcc
 *     (mtk_g2d_uapi_map_format()).  -EINVAL otherwise.
 *
 *  2. @pitch must be 1..max_pitch and a whole number of pixels.  The pitch
 *     registers hold 14 bits, so an over-large pitch would be silently
 *     truncated to a different - possibly zero - pitch.  Refuse, do not round.
 *
 *  3. The rectangle must fit within one row, origin included:
 *     (x + rect_w) * bpp <= pitch.  Checking only rect_w * bpp <= pitch would
 *     leave a rectangle starting partway along a row free to run off the end of
 *     it, and for a destination that is a write into memory nobody handed over.
 *
 *  4. The rectangle must fit within the declared surface: x + rect_w <= width
 *     and y + rect_h <= height.  The engine has no per-surface width register,
 *     so nothing else would catch this.
 *
 *  5. The rectangle must fit within the *real allocation*:
 *     (y + rect_h) * pitch <= size.  This is the memory-safety check, and the
 *     only one that uses a number the kernel owns rather than one userspace
 *     supplied.  All of it is u64: on this SoC dma_addr_t is 32 bits (LPAE and
 *     HIGHMEM are off), so a 32-bit computation would wrap for a large
 *     pitch * height and pass a check it should have failed.
 *
 *  6. The start address must carry the format's alignment.  The block has no
 *     offset register, so the origin is folded into the base address in
 *     software, and the data sheet requires 2-byte alignment for RGB565 and
 *     4-byte for the 8888 formats.
 *
 *  7. The last byte touched must be representable as a DMA address, so the
 *     hardware is never handed an address that wrapped.
 *
 *  8. The buffer must be contiguous in DMA address space.  The engine takes one
 *     base address plus a pitch and cannot follow a scatterlist, so a buffer
 *     whose segments are merely consecutive in length but not in address would
 *     be read from the wrong place.  GEM-allocated buffers satisfy this; an
 *     imported one is checked here because the prime import path only guarantees
 *     it for the default import helper.
 */
static int mtk_g2d_uapi_resolve(struct drm_device *dev,
				struct drm_file *file_priv,
				const struct mtk_g2d_surface *s,
				u32 rect_w, u32 rect_h,
				struct mtk_g2d_uapi_surf *out)
{
	struct drm_gem_dma_object *dma_obj;
	const struct drm_format_info *info;
	u32 max_pitch, max_width, max_height, addr_align;
	u64 row_end, last, start;
	int ret;

	memset(out, 0, sizeof(*out));

	/* 1. Format, before any arithmetic needs a bytes-per-pixel. */
	info = mtk_g2d_uapi_map_format(s->format);
	if (!info)
		return -EINVAL;

	out->bpp = info->cpp[0];
	out->pitch = s->pitch;
	out->width = s->width;
	out->height = s->height;
	out->x = s->x;
	out->y = s->y;

	/* 2. Pitch: bounded by the register, and a whole number of pixels. */
	max_pitch = mtk_g2d_max_pitch();
	if (!s->pitch || s->pitch > max_pitch)
		return -EINVAL;
	if (s->pitch % out->bpp)
		return -EINVAL;

	max_width = mtk_g2d_max_width();
	max_height = mtk_g2d_max_height();

	/*
	 * Scan window bounds, from the engine rather than restated here.  Zero is
	 * rejected explicitly rather than left to fall out of the arithmetic
	 * below: with a zero height the "last byte touched" sum would subtract one
	 * row from an unsigned value and wrap, which happens to be caught, but
	 * relying on that is how a real bound quietly stops being one.
	 */
	if (!rect_w || !rect_h)
		return -EINVAL;
	if (rect_w > max_width || rect_h > max_height)
		return -EINVAL;

	/* 3. One row, origin included. */
	row_end = (u64)(s->x + rect_w) * out->bpp;
	if (row_end > s->pitch)
		return -EINVAL;

	/* 4. Within the declared surface.  u64 so x + rect_w cannot wrap. */
	if ((u64)s->x + rect_w > s->width)
		return -EINVAL;
	if ((u64)s->y + rect_h > s->height)
		return -EINVAL;

	/*
	 * Look the buffer up last, so every purely numeric rejection happens
	 * without touching the object table at all.
	 */
	out->obj = drm_gem_object_lookup(file_priv, s->handle);
	if (!out->obj)
		return -ENOENT;

	dma_obj = to_drm_gem_dma_obj(out->obj);
	out->addr = dma_obj->dma_addr;
	out->size = out->obj->size;

	/*
	 * 8. Contiguity.  For an imported buffer the scatterlist is the truth
	 * about the address space.  For a GEM-allocated one there is no
	 * scatterlist at all, and the allocation is a single coherent mapping of
	 * obj->size bytes by construction, so there is nothing left to check:
	 * @size *is* the mapped length.
	 */
	if (drm_gem_is_imported(out->obj) && dma_obj->sgt) {
		if (drm_prime_get_contiguous_size(dma_obj->sgt) < out->size) {
			ret = -EINVAL;
			goto err_put;
		}
	}

	/* 5. The real allocation, which is the check that actually protects it. */
	if ((u64)(s->y + rect_h) * s->pitch > out->size) {
		ret = -EINVAL;
		goto err_put;
	}

	start = (u64)out->addr + (u64)s->y * s->pitch + (u64)s->x * out->bpp;

	/* 6. Start-address alignment, per format. */
	if (mtk_g2d_addr_align(s->format, &addr_align)) {
		ret = -EINVAL;
		goto err_put;
	}
	/*
	 * Narrow before the modulo.  start is u64 so that the offset arithmetic
	 * below cannot wrap on the way to the range check, but ARM's EABI has no
	 * 64-bit divide helper (__aeabi_uldivmod), so a 64-bit % here is an
	 * unresolved external symbol at link time.  Narrowing is not a
	 * truncation of the value being checked: check 5 above has already
	 * bounded (y + rect_h) * pitch by out->size, and check 7 below rejects
	 * any address that does not fit dma_addr_t, which is 32-bit in this
	 * configuration.  So at this point start fits in dma_addr_t by
	 * construction, and testing the low 32 bits is the whole test.
	 */
	if ((dma_addr_t)start % addr_align) {
		ret = -EINVAL;
		goto err_put;
	}

	/* 7. No wrap in the address the engine is handed. */
	last = start + row_end - 1 + (u64)(rect_h - 1) * s->pitch;
	if (last > (u64)(dma_addr_t)~0ULL) {
		ret = -EINVAL;
		goto err_put;
	}

	/*
	 * Build the framebuffer view the helper layer takes.  offsets[0] is 0
	 * because the origin is expressed in pixels and folded into the address
	 * by the engine driver; the helper adds offsets[0] to the object's DMA
	 * address itself, so a non-zero value here would double-count the
	 * origin and shift every access.
	 *
	 * The view borrows the GEM object, which this function holds a reference
	 * to for the whole operation, so the helper taking its own reference on
	 * top is safe and the stack frame outlives it.
	 */
	out->fb.dev = dev;
	out->fb.format = info;
	out->fb.pitches[0] = s->pitch;
	out->fb.offsets[0] = 0;
	out->fb.width = s->width;
	out->fb.height = s->height;
	out->fb.obj[0] = out->obj;

	return 0;

err_put:
	drm_gem_object_put(out->obj);
	out->obj = NULL;

	return ret;
}

static void mtk_g2d_uapi_release(struct mtk_g2d_uapi_surf *surf)
{
	if (surf->obj)
		drm_gem_object_put(surf->obj);
	surf->obj = NULL;
}

/**
 * mtk_g2d_ioctl_get_cap - DRM_IOCTL_MTK_G2D_GET_CAP
 *
 * Reports what the engine can do and the hard limits it enforces, so a client
 * can lay out a buffer and compute a pitch without hardcoding register widths.
 * The limits are read back out of the engine driver rather than duplicated
 * here, so this ioctl cannot drift from what the engine will actually accept.
 *
 * Deliberately unauthenticated: a client needs the limits before it can decide
 * how to allocate anything, which is long before it would authenticate, and the
 * call changes no state.
 */
static int mtk_g2d_ioctl_get_cap(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct mtk_g2d_cap_info cap = {};
	struct mtk_g2d_get_cap *arg = data;
	void __user *user_cap;
	u32 fmt;

	/*
	 * drm_ioctl() has already copied the fixed-size argument in and will
	 * copy it back out, so this only ever touches kernel memory.  The only
	 * pointer to chase is the one inside the struct.
	 */
	if (arg->size < sizeof(*arg))
		return -EINVAL;

	user_cap = u64_to_user_ptr(arg->cap_info);

	/*
	 * No pointer, no copy: just report the size the caller should use.  This
	 * is what lets a client size its buffer without knowing the layout.
	 */
	if (!user_cap) {
		arg->size = sizeof(cap);
		return 0;
	}

	/*
	 * -E2BIG, not -EINVAL, when the caller's buffer cannot hold what the
	 * driver would write.  It means "retry with a bigger buffer", which is
	 * what makes the struct extensible in both directions.
	 */
	if (arg->size < sizeof(cap))
		return -E2BIG;

	cap.version = MTK_G2D_CAP_INFO_VERSION;
	for (fmt = 0; fmt < __MTK_G2D_FORMAT_COUNT; fmt++) {
		if (mtk_g2d_uapi_map_format(fmt))
			cap.formats |= BIT(fmt);
	}

	cap.max_pitch = mtk_g2d_max_pitch();
	cap.max_width = mtk_g2d_max_width();
	cap.max_height = mtk_g2d_max_height();
	cap.min_width = 1;
	cap.min_height = 1;
	cap.reserved = 0;

	if (copy_to_user(user_cap, &cap, sizeof(cap)))
		return -EFAULT;

	/* Report what was actually written, so a future version can differ. */
	arg->size = sizeof(cap);

	return 0;
}

/**
 * mtk_g2d_ioctl_blt - DRM_IOCTL_MTK_G2D_BLT
 *
 * Blocking: returns only once the engine has stopped.  See the -ETIMEDOUT
 * documentation in the UAPI header for what a caller owes in that case - in
 * particular the destination may still be written after this returns, which is
 * why the errno is passed through unchanged rather than folded into a generic
 * failure.
 */
static int mtk_g2d_ioctl_blt(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct mtk_drm_private *priv = dev->dev_private;
	struct mtk_g2d_uapi_surf src = {}, dst = {};
	struct mtk_g2d_blt arg;
	int ret;

	/* Already copied in by drm_ioctl(); see mtk_g2d_ioctl_get_cap(). */
	arg = *(struct mtk_g2d_blt *)data;

	/*
	 * Versioned.  The ioctl number's encoded size is fixed at the size this
	 * driver knows, so @size can only be smaller (a caller passing a newer,
	 * longer struct) - never larger - and a smaller one is refused rather
	 * than read past.
	 */
	if (arg.size < MTK_G2D_BLT_MIN_SIZE)
		return -E2BIG;

	/* No flags are defined; a typo must not silently change behaviour. */
	if (arg.flags)
		return -EINVAL;

	/* Scan window: one size for both ports, non-zero. */
	if (!arg.rect_width || !arg.rect_height)
		return -EINVAL;

	/* -ENODEV rather than -EINVAL: a missing blitter is a normal config. */
	if (!priv->g2d)
		return -ENODEV;

	ret = mtk_g2d_uapi_resolve(dev, file_priv, &arg.src,
				   arg.rect_width, arg.rect_height, &src);
	if (ret)
		return ret;

	ret = mtk_g2d_uapi_resolve(dev, file_priv, &arg.dst,
				   arg.rect_width, arg.rect_height, &dst);
	if (ret)
		goto err_src;

	/*
	 * Validated strictly, so the helper has nothing to clip and nothing to
	 * refuse.  It owns buffer references, locking and the wait for idle.
	 */
	ret = mtk_g2d_drm_blt_rect(priv->g2d, &src.fb,
				   arg.src.x, arg.src.y,
				   &dst.fb,
				   arg.dst.x, arg.dst.y,
				   arg.rect_width, arg.rect_height);

	if (ret)
		drm_dbg(dev, "G2D blit of %ux%u failed: %d\n",
			arg.rect_width, arg.rect_height, ret);

err_src:
	mtk_g2d_uapi_release(&dst);
	mtk_g2d_uapi_release(&src);

	return ret;
}

/**
 * mtk_g2d_ioctl_fill - DRM_IOCTL_MTK_G2D_FILL
 *
 * Blocking, with the same -ETIMEDOUT contract as @mtk_g2d_ioctl_blt.
 */
static int mtk_g2d_ioctl_fill(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct mtk_drm_private *priv = dev->dev_private;
	struct mtk_g2d_uapi_surf dst = {};
	struct mtk_g2d_fill arg;
	int ret;

	arg = *(struct mtk_g2d_fill *)data;

	if (arg.size < MTK_G2D_FILL_MIN_SIZE)
		return -E2BIG;
	if (arg.flags)
		return -EINVAL;
	if (!arg.rect_width || !arg.rect_height)
		return -EINVAL;
	if (!priv->g2d)
		return -ENODEV;

	ret = mtk_g2d_uapi_resolve(dev, file_priv, &arg.dst,
				   arg.rect_width, arg.rect_height, &dst);
	if (ret)
		return ret;

	ret = mtk_g2d_drm_fill(priv->g2d, &dst.fb,
			       arg.dst.x, arg.dst.y,
			       arg.rect_width, arg.rect_height, arg.color);

	if (ret)
		drm_dbg(dev, "G2D fill of %ux%u failed: %d\n",
			arg.rect_width, arg.rect_height, ret);

	mtk_g2d_uapi_release(&dst);

	return ret;
}

/**
 * mtk_g2d_ioctls - the driver ioctl table
 *
 * Non-static and referenced from mtk_drm_drv.c's &drm_driver.  DRM_IOCTL_DEF_DRV
 * places each entry at its own ioctl number, so these cannot collide with a
 * core DRM ioctl or with another driver's.
 *
 * The flags are the access control:
 *
 *   - GET_CAP carries no flags, so it is available to any client that can open
 *     the node.  Querying limits changes nothing and has to be possible before
 *     a client authenticates.
 *
 *   - BLT and FILL carry DRM_AUTH and deliberately not DRM_MASTER.  DRM_AUTH is
 *     the important one: an unauthenticated process should not be able to make
 *     the display hardware write into buffers.  DRM_MASTER is withheld because
 *     this is not display state - requiring modeset master would exclude the
 *     render-node clients that have no master at all, which is where a
 *     compositor's helper would live.
 */
/*
 * The count is a compile-time constant the driver file needs but cannot
 * compute for itself, since sizeof() on an extern array of unknown bound is an
 * incomplete type.  This assertion is what stops the header's count and this
 * table from drifting apart, which would otherwise silently truncate the table
 * or leave a gap in the ioctl numbering.
 */
static_assert(MTK_G2D_NR_IOCTLS == 3,
	      "MTK_G2D_NR_IOCTLS must match the mtk_g2d_ioctls table below");

/*
 * A minimum-size constant larger than the struct it describes would reject
 * every call with -E2BIG, because a caller always passes the full struct.  This
 * caught exactly that once, when the minimum was computed by summing field
 * sizes in the wrong order and came out four bytes over.
 */
static_assert(MTK_G2D_BLT_MIN_SIZE <= sizeof(struct mtk_g2d_blt),
	      "MTK_G2D_BLT_MIN_SIZE exceeds sizeof(struct mtk_g2d_blt)");
static_assert(MTK_G2D_FILL_MIN_SIZE <= sizeof(struct mtk_g2d_fill),
	      "MTK_G2D_FILL_MIN_SIZE exceeds sizeof(struct mtk_g2d_fill)");

const struct drm_ioctl_desc mtk_g2d_ioctls[] = {
	DRM_IOCTL_DEF_DRV(MTK_G2D_GET_CAP, mtk_g2d_ioctl_get_cap, 0),
	DRM_IOCTL_DEF_DRV(MTK_G2D_BLT, mtk_g2d_ioctl_blt, DRM_AUTH),
	DRM_IOCTL_DEF_DRV(MTK_G2D_FILL, mtk_g2d_ioctl_fill, DRM_AUTH),
};