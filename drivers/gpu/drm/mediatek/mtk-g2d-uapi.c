// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * Userspace interface to the G2D 2D blitter.
 *
 * This file is the whole of the G2D userspace plumbing: it resolves and
 * validates a request, takes the locks and references that keep the buffers
 * alive for the duration, programs the engine through mtk-g2d.c and waits for
 * it to stop.  It used to be split across here and mtk_drm_drv.c, with the
 * framebuffer-resolving and locking half living in the display driver because
 * that is where the ioctls were registered.  The ioctls are no longer on the
 * display device - the display DRM must not own a block it does not program
 * - so the whole of it lives here, next to the ABI it implements.
 *
 * The G2D block owns a DRM render node of its own, created here:
 * mtk_g2d_register_drm() allocates a &drm_device with no parent and
 * registers it with .ioctls = mtk_g2d_ioctls and DRIVER_GEM | DRIVER_RENDER.
 * That is what makes GEM handles in this file mean what they say - they are
 * looked up against *this* device's namespace, so the address resolved from
 * one belongs to the DMA space of the G2D platform device and to no other
 * engine's.
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
 *    here (G2D is a DMA engine, and the display path hands it DMA addresses),
 *    and a raw fd would be a second, parallel way of naming the same buffers
 *    that the GEM namespace already covers.
 *
 * The file's job is *strictness*.  Every bound the engine imposes is
 * re-checked here and reported as -EINVAL, rather than being clipped to what
 * the buffers can supply: for an ABI, silently doing less than was requested
 * is a bug the caller cannot see.  By the time the engine is programmed the
 * request is known to fit, so nothing is ever clipped and the two layers
 * cannot disagree about what was accepted.
 */

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_prime.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/types.h>

#include <drm/mtk_g2d.h>

#include "mtk-g2d.h"

/**
 * struct mtk_g2d_uapi_surf - one operand, resolved and validated
 * @obj: the GEM object, referenced for as long as this structure lives
 * @addr: DMA address of the first byte of the buffer
 * @size: real size of the allocation in bytes, from obj->size
 * @pitch: stride in bytes
 * @fmt: engine CLRFMT this buffer is programmed with
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
	dma_addr_t addr;
	size_t size;
	u32 pitch;
	enum g2d_format fmt;
	u32 bpp;
	u32 width;
	u32 height;
	u32 x;
	u32 y;
};

/**
 * mtk_g2d_uapi_map_format - map a @mtk_g2d_format onto a DRM fourcc.
 *
 * Returns a format the DRM side recognises, or NULL if there is none.
 *
 * The two directions are deliberately separate mappings rather than one
 * table walked backwards: the engine names its encodings, DRM names its own,
 * and the honest awkward case is MTK_G2D_PARGB8888 - the engine can encode a
 * pre-multiplied format and DRM has no fourcc for it, since
 * DRM_FORMAT_ARGB8888 names non-premultiplied alpha.  Rather than let the two
 * silently disagree about what the bytes mean, PARGB8888 is reported as
 * *unsupported* here and is excluded from the capability bitmap.  A caller
 * wanting pre-multiplied alpha has no representation for it yet; getting that
 * wrong would corrupt colours, which is a much worse outcome than a format
 * being unavailable.
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
 * mtk_g2d_uapi_to_clrfmt - the DRM fourcc that came out of the mapping above,
 *	turned back into the engine's own CLRFMT encoding.
 * @format: a DRM fourcc
 * @out: CLRFMT, written on success
 *
 * The inverse of mtk_g2d_uapi_map_format().  It is a switch rather than an
 * index because the engine's enum is not dense - it has a hole where
 * PARGB8888 sits, and no fourcc to go with it - so walking an array of four
 * entries by fourcc would be the wrong shape.
 *
 * The conversions are format-identities, not approximations: no swap bit
 * (RB_SWP, BYTE_SWP) or flip is programmed here, so a BGR variant of any of
 * these would be a different layout and is deliberately absent rather than
 * approximated.
 *
 * Returns 0, or -EINVAL if @format has no CLRFMT encoding.
 */
static int mtk_g2d_uapi_to_clrfmt(u32 format, enum g2d_format *out)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
		*out = g2d_clrfmt_rgb565;
		return 0;
	case DRM_FORMAT_RGB888:
		*out = g2d_clrfmt_rgb888;
		return 0;
	case DRM_FORMAT_ARGB8888:
		*out = g2d_clrfmt_argb8888;
		return 0;
	case DRM_FORMAT_XRGB8888:
		*out = g2d_clrfmt_xrgb8888;
		return 0;
	default:
		return -EINVAL;
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
static int mtk_g2d_uapi_resolve(struct drm_file *file_priv,
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

	ret = mtk_g2d_uapi_to_clrfmt(info->format, &out->fmt);
	if (ret)
		return ret;

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

	/*
	 * 3. One row, origin included.
	 *
	 * The widen is on each operand *before* the add, not on the sum:
	 * (u64)(x + rect_w) evaluates x + rect_w in u32 first, so a large x
	 * wraps there and the check then compares a small number against the
	 * pitch and passes.  Both x and rect_w are u32 from userspace, so this
	 * is reachable, and it is a check that exists to keep the engine inside
	 * the buffer.
	 */
	row_end = ((u64)s->x + rect_w) * out->bpp;
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

	/*
	 * 5. The real allocation, which is the check that actually protects it.
	 *
	 * Same widen-before-add reasoning as check 3: (u64)(y + rect_h) would
	 * wrap the sum in u32 first, and this is the check that is supposed to
	 * stop the engine writing past the end of a buffer.
	 */
	if (((u64)s->y + rect_h) * s->pitch > out->size) {
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

/*
 * ---------------------------------------------------------------------------
 * Lifetime.
 *
 * The engine reads and writes memory that userspace owns, asynchronously with
 * respect to userspace's view of it.  Two things therefore have to be true for
 * the whole of an operation, and both are established here rather than assumed:
 *
 *   - The buffers cannot be freed or remapped underneath us.  The GEM object
 *     reference taken in mtk_g2d_uapi_resolve() stops the allocation being
 *     freed, and the reservation lock stops anything that mutates the mapping -
 *     a vmap/vunmap of the same object, a prime export, another driver taking
 *     it for its own use - from proceeding while the engine is reading it.
 *     This is the same pairing drm_gem_vmap()/drm_gem_vunmap() use, for the
 *     same reason.
 *
 *   - The engine has stopped before the locks are dropped.  mtk_g2d_blt_rect()
 *     and mtk_g2d_fill() both block in g2d_wait_idle() until G2D_STATUS reads
 *     idle before they return, so releasing the locks afterwards is not a
 *     race: there is nothing left running that could touch the memory.  If it
 *     does not go idle, the engine is wedged shut rather than released - see
 *     the timeout note below.
 *
 * That is why this is synchronous and installs no fence.  A fence would let
 * the source be released earlier, which is a real benefit, but it would also
 * mean the engine outliving this call - and the display path here has no
 * fencing infrastructure to hang one on, because nothing in the OVL/RDMA path
 * ever installs one either.  A correct synchronous wait is the honest choice:
 * it is bounded, it cannot hang, and it leaves no window in which a buffer is
 * unlocked while the engine is still running.
 *
 * Locking order.  A blit takes two reservation locks, and two concurrent
 * blits of the same pair of buffers in opposite directions would deadlock if
 * the order depended on which buffer was passed first.  It does not:
 * mtk_g2d_uapi_lock_pair() always takes them in reservation-object pointer
 * order, which is one global order over every buffer in the system.  The
 * reservation locks are therefore the only locks held across the engine
 * operation, and they are always released in the reverse of the order taken.
 *
 * No engine lock is taken here: mtk_g2d_blt_rect()/mtk_g2d_fill() take
 * g2d->lock internally around register programming, and this code never programs
 * a register itself, so there is nothing here that could race with another user
 * of the engine.
 */

/**
 * mtk_g2d_uapi_lock - take a surface's reservation object.
 * @surf: surface to lock
 * @owner: set to the reservation object actually locked, for the pair case
 *
 * drm_gem_lock() is dma_resv_lock(obj->resv, NULL), and a NULL acquire context
 * is documented as legal only for locking a reservation object against itself.
 * That is exactly the wrong property for a pair of buffers: a NULL context
 * carries no record of what this task already holds, so if the two surfaces
 * turn out to be the same object the second lock blocks on a mutex the first
 * one is still holding, and the task sleeps forever.  A pair lock therefore
 * uses a real ww_acquire_ctx.  The same-object case cannot be reached from
 * here any more - mtk_g2d_ioctl_blt() refuses it outright - but the short
 * circuit below stays, because a single lock is still the right answer for one
 * object and it costs nothing to not depend on the caller's check.
 *
 * Not interruptible, deliberately: what is being waited on is another G2D user
 * finishing a copy that is itself bounded, so bailing out with -EINTR halfway
 * through owning one of two buffers would cost more state to unwind than it
 * saves.
 */
static int mtk_g2d_uapi_lock(struct mtk_g2d_uapi_surf *surf,
			     struct ww_acquire_ctx *ctx)
{
	if (!surf->obj)
		return 0;

	return dma_resv_lock(surf->obj->resv, ctx);
}

static void mtk_g2d_uapi_unlock(struct mtk_g2d_uapi_surf *surf)
{
	if (surf->obj)
		dma_resv_unlock(surf->obj->resv);
}

/**
 * mtk_g2d_uapi_lock_pair - lock two surfaces in a deadlock-free order.
 * @a: first surface
 * @b: second surface
 *
 * Both buffers must be held for the whole operation - the engine reads the
 * source while it writes the destination, so locking only one would leave the
 * other exposed.
 *
 * The order is always reservation-object pointer order, which is a single
 * global order over every buffer in the system.  Ordering by "which argument it
 * was" would not be: A->B and B->A are both reachable, and the second thread
 * would take the first lock the first thread is holding and wait for it
 * forever.
 *
 * Returns 0, or the error from the first lock that could not be taken, in
 * which case anything already taken has been released again.
 */
static int mtk_g2d_uapi_lock_pair(struct mtk_g2d_uapi_surf *a,
				  struct mtk_g2d_uapi_surf *b)
{
	struct ww_acquire_ctx ctx;
	struct mtk_g2d_uapi_surf *first, *second;
	int ret;

	/*
	 * One reservation object means one lock is already enough, and taking
	 * it twice is a self-deadlock.  Not an error case, just one lock
	 * instead of two.
	 */
	if (a->obj && a->obj == b->obj) {
		first = a;
		second = NULL;
	} else {
		/* Deterministic global order, independent of argument position. */
		if (a->obj && b->obj && a->obj->resv > b->obj->resv) {
			first = b;
			second = a;
		} else {
			first = a;
			second = b;
		}
	}

	/*
	 * The acquire context is a transaction, not bookkeeping the caller
	 * owns.  ww_acquire_init() ... ww_acquire_fini() brackets it, and
	 * ww_acquire_fini() while @ctx still holds locks is premature: it
	 * releases the transaction's lockdep state and clears its acquired
	 * count while the mutexes stay locked, so a later ww_mutex_lock() on
	 * this class no longer knows the task already holds them and can no
	 * longer order against them.  The documented order is init, lock every
	 * object, ww_acquire_done(), release the locks, then fini - so fini()
	 * belongs to the unlock side, not to the end of the lock side, and the
	 * lock side must close the transaction with ww_acquire_done() first.
	 */
	ww_acquire_init(&ctx, &reservation_ww_class);

	ret = mtk_g2d_uapi_lock(first, &ctx);
	if (ret)
		goto err_fini;

	if (second) {
		ret = mtk_g2d_uapi_lock(second, &ctx);

		/*
		 * reservation_ww_class is a DEFINE_WD_CLASS, so the ww mutex
		 * *dies* rather than waiting when it finds a cycle: -EDEADLK
		 * is how it reports contention, not a failure of the request.
		 * Returning it up would turn an ordinary concurrent blit into a
		 * spurious error, so the pair is retried on the slowpath.
		 *
		 * The die case has a hard requirement, from dma_resv.h and
		 * ww_mutex.h alike: *everything* @ctx holds must be released
		 * before dma_resv_lock_slow() is called on the contended
		 * object, and it is forbidden to call the slowpath with any
		 * other mutex of this context still held.  So @first goes back
		 * first, then the slowpath claims the object that died, and
		 * only then is @first re-taken.  Both buffers are held when this
		 * returns, which the engine requires: it reads the source while
		 * it writes the destination.
		 *
		 * Ordering cannot simply be restarted from the top here - that
		 * is the same cycle that just produced the -EDEADLK.  Backing
		 * off one object and waiting for it is what breaks it, and
		 * ww_mutex.h explicitly allows the remaining mutexes to be
		 * re-acquired with ww_mutex_lock() afterwards.
		 */
		if (ret == -EDEADLK) {
			mtk_g2d_uapi_unlock(first);

			/* Cannot fail: the slowpath is an uninterruptible wait. */
			dma_resv_lock_slow(second->obj->resv, &ctx);

			ret = mtk_g2d_uapi_lock(first, &ctx);
			if (ret) {
				/*
				 * Only reachable if @first itself is now
				 * contended, which the ww class cannot report as
				 * -EDEADLK without dying again, and a dying
				 * mutex cannot be waited on.  Release what is
				 * held so the transaction can still be closed.
				 */
				dma_resv_unlock(second->obj->resv);
				goto err_fini;
			}
		}
	}

	ww_acquire_done(&ctx);

	return 0;

err_fini:
	/*
	 * Nothing is held on this path, so the transaction is simply closed.
	 */
	ww_acquire_fini(&ctx);

	return ret;
}

/**
 * mtk_g2d_uapi_unlock_pair - release both, in the reverse of the order taken.
 * @a: first surface passed to the lock
 * @b: second surface passed to the lock
 *
 * Strict reverse of the order mtk_g2d_uapi_lock_pair() took them in, which is
 * what makes the order it chose a real one rather than a convention.
 */
static void mtk_g2d_uapi_unlock_pair(struct mtk_g2d_uapi_surf *a,
				     struct mtk_g2d_uapi_surf *b)
{
	if (a->obj && a->obj == b->obj) {
		mtk_g2d_uapi_unlock(a);
		return;
	}

	/* Strict reverse of the order mtk_g2d_uapi_lock_pair() took them in. */
	if (a->obj && b->obj && a->obj->resv > b->obj->resv) {
		mtk_g2d_uapi_unlock(a);
		mtk_g2d_uapi_unlock(b);
	} else {
		mtk_g2d_uapi_unlock(b);
		mtk_g2d_uapi_unlock(a);
	}
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
 * Blocking: returns only once the engine has stopped, or has been declared
 * unrecoverable.  See the -ETIMEDOUT documentation in the UAPI header for what
 * that means - in particular that the destination may still be written after
 * the call returns - and why that is now a state no further ioctl can reach.
 */
static int mtk_g2d_ioctl_blt(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct mtk_g2d_uapi_surf src = {}, dst = {};
	struct mtk_g2d_drm_private *priv = dev->dev_private;
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
	if (!priv || !priv->g2d)
		return -ENODEV;

	ret = mtk_g2d_uapi_resolve(file_priv, &arg.src,
				   arg.rect_width, arg.rect_height, &src);
	if (ret)
		return ret;

	ret = mtk_g2d_uapi_resolve(file_priv, &arg.dst,
				   arg.rect_width, arg.rect_height, &dst);
	if (ret)
		goto err_src;

	/*
	 * A blit of one buffer onto itself is refused rather than analysed.
	 *
	 * The engine reads a row and writes a row with no ordering this driver
	 * can specify, so an overlapping in-place copy has no defined result at
	 * all.  Whether two rectangles overlap cannot be answered from the
	 * arguments alone either: with different pitches, and with the two
	 * origins at different y, row n of the source is at base + (y + n) *
	 * pitch + x * bpp and row n of the destination is at a different
	 * offset again, so comparing origins inside a row - which is all the
	 * geometry would otherwise allow - decides a question about rows it
	 * cannot see.  Getting that right means either walking every row or
	 * proving non-overlap with per-row address arithmetic, and a false
	 * "no overlap" is a silent corruption of the buffer, not an error.
	 *
	 * So the safe rule for v1: two operands must be two GEM objects.
	 */
	if (src.obj == dst.obj) {
		ret = -EINVAL;
		goto err_dst;
	}

	/* Both buffers are held for the whole engine operation. */
	ret = mtk_g2d_uapi_lock_pair(&src, &dst);
	if (ret)
		goto err_dst;

	/*
	 * Validated strictly, so the engine has nothing to clip and nothing to
	 * refuse: every bound above is already against the real allocation.
	 */
	ret = mtk_g2d_blt_rect(priv->g2d,
			       src.addr, src.pitch, src.fmt, src.x, src.y,
			       dst.addr, dst.pitch, dst.fmt, dst.x, dst.y,
			       arg.rect_width, arg.rect_height);

	mtk_g2d_uapi_unlock_pair(&src, &dst);

	if (ret)
		drm_dbg(dev, "G2D blit of %ux%u failed: %d\n",
			arg.rect_width, arg.rect_height, ret);

err_dst:
	mtk_g2d_uapi_release(&dst);
err_src:
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
	struct mtk_g2d_uapi_surf dst = {};
	struct mtk_g2d_drm_private *priv = dev->dev_private;
	struct mtk_g2d_fill arg;
	int ret;

	arg = *(struct mtk_g2d_fill *)data;

	if (arg.size < MTK_G2D_FILL_MIN_SIZE)
		return -E2BIG;
	if (arg.flags)
		return -EINVAL;
	if (!arg.rect_width || !arg.rect_height)
		return -EINVAL;

	if (!priv || !priv->g2d)
		return -ENODEV;

	ret = mtk_g2d_uapi_resolve(file_priv, &arg.dst,
				   arg.rect_width, arg.rect_height, &dst);
	if (ret)
		return ret;

	ret = mtk_g2d_uapi_lock(&dst, NULL);
	if (ret)
		goto err_dst;

	ret = mtk_g2d_fill(priv->g2d, dst.addr, dst.pitch, dst.fmt,
			   dst.x, dst.y, arg.rect_width, arg.rect_height,
			   arg.color);

	mtk_g2d_uapi_unlock(&dst);

	if (ret)
		drm_dbg(dev, "G2D fill of %ux%u failed: %d\n",
			arg.rect_width, arg.rect_height, ret);

err_dst:
	mtk_g2d_uapi_release(&dst);

	return ret;
}

/**
 * mtk_g2d_ioctls - the driver ioctl table
 *
 * Non-static and intended for a &drm_driver belonging to the G2D node itself.
 * No such driver exists yet, so nothing references this table; DRM_IOCTL_DEF_DRV
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
 *     the engine write into buffers.  DRM_MASTER is withheld because a
 *     compositor's helper opens a render node and has no master at all, and
 *     that is exactly the client this exists for.
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

/*
 * ---------------------------------------------------------------------------
 * The DRM device.
 *
 * G2D gets a device of its own rather than being reached through the display
 * DRM device, for one reason: the addresses it programs have to come from a
 * DMA space G2D itself owns.  As long as the ioctls lived on the display
 * device, a handle resolved in *that* device's namespace and yielded an
 * address that device's dma_dev had produced - which is correct only while
 * there is no IOMMU in the path, because then physical and DMA addresses
 * coincide.  The moment either engine gets an M4U port, the two diverge and
 * the borrower is handed a number the borrower cannot translate.
 *
 * A device of its own fixes that at the root: GEM handles here are this
 * device's own, dma_addr values come from this device's dma_dev, and a
 * buffer shared with the display pipeline arrives as a dma-buf fd that this
 * device imports and maps into *its* address space.  Each engine then reads
 * an address from the page table that engine has.
 *
 * What it deliberately does not have:
 *
 *  - No KMS.  There are no connectors, CRTCs or planes; DRIVER_MODESET is not
 *    set and there is no mode_config, no fbops and no fbdev emulation.  A
 *    display device can have a render node as well as a primary one; this one
 *    has *only* a render node, because there is nothing a primary node could
 *    show.
 *
 *  - No master.  DRIVER_MASTER is not set, so no ioctl here requires it, and
 *    drm_dev_set_master() has nothing to hand out.  A compositor's helper
 *    opens a render node and has no master at all, which is exactly the client
 *    this exists to serve.
 *
 *  - No fences.  The operations are synchronous, so there is nothing to wait
 *    on and no timeline to install.
 */

/*
 * Major/minor 1/0 is this driver's own slot in the DRM minor registry.  It
 * has to differ from the display driver's (mtk_drm_drv.c, also 1/0) only in
 * the sense that they are separate entries in separate drivers; the core
 * allocates the render minor number itself from a global registry, so the
 * (major, minor) pair is an identity here, not an allocation, and two drivers
 * may hold the same values.
 */
#define MTK_G2D_DRM_NAME	"mtk-g2d"
#define MTK_G2D_DRM_DESC	"MediaTek MT6589 G2D"
#define MTK_G2D_DRM_MAJOR	1
#define MTK_G2D_DRM_MINOR	0

/*
 * The core GEM mmap handler, and nothing KMS: there is no fbops here and
 * nothing can be mapped through a framebuffer, only through a GEM handle.
 * This is the same shape the display driver uses (DEFINE_DRM_GEM_FOPS there,
 * DEFINE_DRM_GEM_DMA_FOPS here because this device's GEM objects are DMA
 * objects, not shmem ones).
 */
DEFINE_DRM_GEM_DMA_FOPS(mtk_g2d_fops);

static const struct drm_driver mtk_g2d_drm_driver = {
	/*
	 * DRIVER_RENDER and DRIVER_GEM, and nothing else.  Deliberately no
	 * DRIVER_MODESET (no KMS, no primary node), no DRIVER_MASTER
	 * (render-node clients have no master) and no DRIVER_ATOMIC (there is
	 * no display state).
	 */
	.driver_features	= DRIVER_GEM | DRIVER_RENDER,

	DRM_GEM_DMA_DRIVER_OPS,

	.fops			= &mtk_g2d_fops,

	/*
	 * The only ioctls this node answers are the G2D ones above.
	 * DRM_IOCTL_DEF_DRV places each at its own number in the
	 * DRM_COMMAND_BASE space, so they cannot collide with a core ioctl or
	 * another driver's; PRIME, GEM and the dumb-buffer ioctls come from
	 * the core and are not listed here.
	 */
	.ioctls			= mtk_g2d_ioctls,
	.num_ioctls		= ARRAY_SIZE(mtk_g2d_ioctls),

	.name			= MTK_G2D_DRM_NAME,
	.desc			= MTK_G2D_DRM_DESC,
	.major			= MTK_G2D_DRM_MAJOR,
	.minor			= MTK_G2D_DRM_MINOR,
};

void *mtk_g2d_register_drm(struct device *dev, struct mtk_g2d *g2d)
{
	struct mtk_g2d_drm_private *priv;
	struct drm_device *drm;
	int ret;

	/*
	 * Allocated here rather than as a field of the &drm_device, because
	 * this tree's drm_dev_alloc() takes no private size - mtk_drm_drv.c
	 * does the same, assigning drm->dev_private from its own structure.
	 *
	 * Not devres, deliberately.  devres would free it when the platform
	 * device is unbound, and unbind happens at remove() *after*
	 * drm_dev_unregister(), so the ordering happens to be right - but an
	 * open file can still hold the node at that point, and the ioctl
	 * handlers dereference dev->dev_private without a reference of their
	 * own.  It is instead released from mtk_g2d_unregister_drm(), which
	 * runs when the last reference to the DRM device goes away.
	 */
	priv = kzalloc_obj(*priv);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	drm = drm_dev_alloc(&mtk_g2d_drm_driver, dev);
	if (IS_ERR(drm)) {
		ret = PTR_ERR(drm);
		goto err_free_priv;
	}

	/*
	 * dev_private is what every ioctl handler in this file reads to find
	 * the engine, so it must be set before the device is registered:
	 * drm_dev_register() publishes the minor, and from that moment the
	 * node can be opened and the ioctls reached.  Assigning it after
	 * registration would leave a window in which an ioctl saw NULL.
	 */
	drm->dev_private = priv;
	priv->g2d = g2d;

	ret = drm_dev_register(drm, 0);
	if (ret) {
		drm->dev_private = NULL;
		drm_dev_put(drm);
		goto err_free_priv;
	}

	return drm;

err_free_priv:
	kfree(priv);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(mtk_g2d_register_drm);

void mtk_g2d_unregister_drm(void *drm)
{
	struct mtk_g2d_drm_private *priv;
	struct drm_device *dev = drm;

	/* NULL when probe never got as far as registering. */
	if (!dev)
		return;

	drm_dev_unregister(dev);

	/*
	 * Taken out here rather than through devres.  drm_dev_put() drops the
	 * driver's reference, which is the last one once drm_dev_unregister()
	 * has seen every file close, so the free cannot race a handler that
	 * has already read dev->dev_private.
	 */
	priv = dev->dev_private;
	dev->dev_private = NULL;
	drm_dev_put(dev);
	kfree(priv);
}
EXPORT_SYMBOL_GPL(mtk_g2d_unregister_drm);
