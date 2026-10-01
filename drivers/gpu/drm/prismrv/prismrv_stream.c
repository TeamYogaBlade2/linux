// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_stream.c — validation of PRISMRV_CMD_ABI_STREAM_V1 command streams.
 *
 * Security model
 * --------------
 * All BOs live in one device-wide GPU address space (a single MMU
 * directory), and a GPU VA is just a number inside the stream.  Without
 * checks, client A could write client B's VA into its stream and have the
 * GPU read or write B's buffers.  The kernel therefore
 *
 *   1. copies the stream out of the user-writable BO into kernel memory
 *      (so it cannot change after validation),
 *   2. validates every GPU address range in that copy against the BOs
 *      the submitter listed (and thereby owns a handle for), and
 *   3. gives the GPU a kernel-owned snapshot of the validated copy
 *      (see prismrv_submit.c), never the user BO.
 *
 * Only addresses in layer 1 exist: the TA packet BO (layer 2) carries
 * vertex data and is bounded by the DRAW {address, length} range checked
 * here.  The executor must still bounds-check inside that range.
 *
 * The packet layout is [u32 opcode][u32 payload_words][payload...];
 * see the documentation at the top of Mesa's prismrv_context.c.
 */
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <drm/drm_gem.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

enum {
	OP_NOP, OP_SET_RT, OP_SET_PROG_VS, OP_SET_PROG_FS, OP_SET_UNIFORMS,
	OP_DRAW, OP_BARRIER, OP_SET_TEXTURE, OP_SET_VIEWPORT, OP_SET_SCISSOR,
	OP_SET_BLEND, OP_SET_DEPTH, OP_SET_RASTER, OP_COUNT
};

#define MAX_PROG_WORDS		4096		/* 16 KiB of shader text */
#define MAX_UNIFORM_VEC4	16
#define MAX_SURFACE_DIM		8192
#define MAX_TEXTURE_SLOTS	8

static unsigned int prismrv_fmt_bpp(u32 fmt)
{
	switch (fmt) {
	case PRISMRV_FMT_RGBA8_UNORM:
	case PRISMRV_FMT_BGRA8_UNORM:
		return 4;
	case PRISMRV_FMT_RGBA32F:
		return 16;
	default:
		return 0;
	}
}

/* [va, va+len) must lie entirely inside one of the submitted BOs */
static bool prismrv_range_in_bos(struct drm_gem_object **bos,
				 unsigned int num_bos, u64 va, u64 len)
{
	unsigned int i;
	u64 end;

	if (check_add_overflow(va, len, &end))
		return false;

	for (i = 0; i < num_bos; i++) {
		u64 start, bo_end;

		if (!bos[i])
			continue;
		start = prismrv_bo_gpuva(bos[i]);
		bo_end = start + bos[i]->size;
		if (va >= start && end <= bo_end)
			return true;
	}
	return false;
}

/* a w x h surface with the given row stride, starting at va */
static bool prismrv_surface_ok(struct drm_gem_object **bos,
			       unsigned int num_bos, u32 va, u32 w, u32 h,
			       u32 stride, u32 fmt)
{
	unsigned int bpp = prismrv_fmt_bpp(fmt);
	u64 len;

	if (!bpp || !w || !h || w > MAX_SURFACE_DIM || h > MAX_SURFACE_DIM)
		return false;
	if (stride < (u64)w * bpp)
		return false;
	len = (u64)stride * h;
	return prismrv_range_in_bos(bos, num_bos, va, len);
}

/**
 * prismrv_validate_stream() - check a command stream snapshot.
 * @stream: kernel copy of the stream
 * @words: its length in 32-bit words
 * @bos: BOs the submitter listed (NULL entries are skipped)
 *
 * Return: 0 if every packet is well formed and every address range is
 * inside a listed BO, -EINVAL otherwise.
 */
int prismrv_validate_stream(struct prismrv_device *pv, const u32 *stream,
			    size_t words, struct drm_gem_object **bos,
			    unsigned int num_bos)
{
	size_t pos = 0;

	while (pos < words) {
		u32 op, n;
		const u32 *p;

		if (words - pos < 2)
			return -EINVAL;
		op = stream[pos];
		n = stream[pos + 1];
		pos += 2;
		if (n > words - pos)
			return -EINVAL;
		p = stream + pos;
		pos += n;

		switch (op) {
		case OP_NOP:
		case OP_BARRIER:
			if (n)
				return -EINVAL;
			break;

		case OP_SET_RT:		/* w, h, va, stride, format */
			if (n != 5)
				return -EINVAL;
			if (p[2] == 0 && p[3] == 0)
				break;		/* no render target */
			if (!prismrv_surface_ok(bos, num_bos, p[2], p[0], p[1],
						p[3], p[4]))
				return -EINVAL;
			break;

		case OP_SET_PROG_VS:
		case OP_SET_PROG_FS:
			if (n == 0 || n > MAX_PROG_WORDS)
				return -EINVAL;
			break;

		case OP_SET_UNIFORMS:	/* stage, vec4 * k */
			if (n < 1 || (n - 1) % 4 || (n - 1) / 4 > MAX_UNIFORM_VEC4 ||
			    p[0] > 1)
				return -EINVAL;
			break;

		case OP_DRAW: {		/* va_lo, va_hi, len, first */
			u64 va;

			if (n != 4)
				return -EINVAL;
			va = ((u64)p[1] << 32) | p[0];
			if (!p[2] || p[2] % 4 ||
			    !prismrv_range_in_bos(bos, num_bos, va, p[2]))
				return -EINVAL;
			break;
		}

		case OP_SET_TEXTURE:	/* slot, w, h, va, stride, format */
			if (n != 6 || p[0] >= MAX_TEXTURE_SLOTS)
				return -EINVAL;
			if (!prismrv_surface_ok(bos, num_bos, p[3], p[1], p[2],
						p[4], p[5]))
				return -EINVAL;
			break;

		case OP_SET_VIEWPORT:
			if (n != 6)
				return -EINVAL;
			break;
		case OP_SET_SCISSOR:
			if (n != 5)
				return -EINVAL;
			break;
		case OP_SET_BLEND:
			if (n != 8)
				return -EINVAL;
			break;
		case OP_SET_DEPTH:
			if (n != 3)
				return -EINVAL;
			break;
		case OP_SET_RASTER:
			if (n != 2)
				return -EINVAL;
			break;

		default:
			/* unknown opcode: its payload might carry addresses */
			return -EINVAL;
		}
	}
	return 0;
}
