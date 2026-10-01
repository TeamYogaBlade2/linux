// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_stream.c — validation of PRISMRV_CMD_ABI_STREAM_V1 command streams.
 *
 * Security model
 * --------------
 * All BOs live in one device-wide GPU address space (a single MMU
 * directory), and a GPU VA is just a number inside the stream.  Without
 * checks, client A could write client B's VA into its stream and have the
 * GPU read or write B's buffers.  Everything the executor *interprets*
 * is therefore copied into kernel memory, validated, and executed from a
 * kernel-owned snapshot BO that userspace can neither map nor modify:
 *
 *   layer 1  the command packets (this file: prismrv_validate_stream);
 *            every GPU address range is checked against the BOs the
 *            submitter listed (and so holds handles for);
 *   shaders  SET_PROG_VS/FS payloads are text in a closed instruction
 *            subset (mov/vmov/vmul/vmad/frcp/frsq/smp on r0-r255 and
 *            o0-o15).  The subset has no load/store/branch, so a shader
 *            cannot form or use an address; the executor must implement
 *            exactly this subset and bounds-check the register file and
 *            the smp slot;
 *   layer 2  the TA packets referenced by DRAW are copied into the same
 *            snapshot, structurally validated (prismrv_validate_ta) and
 *            the DRAW address is rewritten to point at the copy.
 *
 * What stays outside the snapshot is plain data the executor never
 * interprets as commands: textures and the render target (their extents
 * are validated; the executor must clamp coordinates into them).
 *
 * The packet layout is [u32 opcode][u32 payload_words][payload...];
 * see the documentation at the top of Mesa's prismrv_context.c.
 */
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <drm/drm_gem.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

enum {
	OP_NOP, OP_SET_RT, OP_SET_PROG_VS, OP_SET_PROG_FS, OP_SET_UNIFORMS,
	OP_DRAW, OP_BARRIER, OP_SET_TEXTURE, OP_SET_VIEWPORT, OP_SET_SCISSOR,
	OP_SET_BLEND, OP_SET_DEPTH, OP_SET_RASTER, OP_COUNT
};

#define MAX_PROG_WORDS		4096		/* 16 KiB of shader text */
#define MAX_PROG_LINES		2048
#define MAX_PROG_LINE_LEN	160
#define MAX_UNIFORM_VEC4	16
#define MAX_SURFACE_DIM		8192
#define MAX_TEXTURE_SLOTS	8
#define MAX_SCISSOR_COORD	16384
#define MAX_TA_BYTES		(256 * 1024)
#define MAX_TA_NCOMP		32		/* floats per vertex */
#define MAX_TA_VERTICES		65536

/* ---- shader text: closed instruction subset ------------------------ */

struct sp { const char *p, *end; };

static void sp_ws(struct sp *s)
{
	while (s->p < s->end && (*s->p == ' ' || *s->p == '\t'))
		s->p++;
}

static bool sp_char(struct sp *s, char c)
{
	sp_ws(s);
	if (s->p < s->end && *s->p == c) {
		s->p++;
		return true;
	}
	return false;
}

/* register: 'r' 0..255, or (if allow_out) 'o' 0..15 */
static bool sp_reg(struct sp *s, bool allow_out)
{
	char kind;
	u32 n = 0;
	unsigned int digits = 0;

	sp_ws(s);
	if (s->p >= s->end)
		return false;
	kind = *s->p;
	if (kind != 'r' && !(allow_out && kind == 'o'))
		return false;
	s->p++;
	while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
		n = n * 10 + (*s->p - '0');
		if (++digits > 3)
			return false;
		s->p++;
	}
	if (!digits)
		return false;
	return kind == 'r' ? n <= 255 : n <= 15;
}

/* '#' immediate: decimal or 0x hex, value must fit u32 */
static bool sp_imm(struct sp *s, u32 limit)
{
	u64 v = 0;
	unsigned int digits = 0;
	bool hex = false;

	sp_ws(s);
	if (s->p >= s->end || *s->p != '#')
		return false;
	s->p++;
	if (s->end - s->p > 2 && s->p[0] == '0' && s->p[1] == 'x') {
		hex = true;
		s->p += 2;
	}
	while (s->p < s->end) {
		char c = *s->p;
		unsigned int d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (hex && c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (hex && c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			break;
		v = v * (hex ? 16 : 10) + d;
		if (v > U32_MAX)
			return false;
		digits++;
		s->p++;
	}
	return digits && v <= limit;
}

static bool sp_swizzle(struct sp *s)
{
	unsigned int i;

	sp_ws(s);
	if (s->end - s->p < 13 || strncmp(s->p, "swizzle(", 8))
		return false;
	s->p += 8;
	for (i = 0; i < 4; i++, s->p++)
		if (*s->p != 'x' && *s->p != 'y' && *s->p != 'z' && *s->p != 'w')
			return false;
	if (*s->p != ')')
		return false;
	s->p++;
	return true;
}

static bool prog_line_ok(const char *line, size_t len)
{
	struct sp s = { line, line + len };
	char mn[8];
	unsigned int n = 0, nsrc;

	sp_ws(&s);
	if (s.p == s.end || *s.p == '#')
		return true;			/* blank or comment */

	while (s.p < s.end && *s.p >= 'a' && *s.p <= 'z') {
		if (n >= sizeof(mn) - 1)
			return false;
		mn[n++] = *s.p++;
	}
	mn[n] = '\0';

	if (!strcmp(mn, "mov")) {
		if (!sp_reg(&s, false) || !sp_char(&s, ',') ||
		    !sp_imm(&s, U32_MAX))
			return false;
	} else if (!strcmp(mn, "vmov")) {
		if (!sp_reg(&s, true) || !sp_char(&s, ',') ||
		    !sp_reg(&s, false) || !sp_char(&s, ',') || !sp_swizzle(&s))
			return false;
	} else if (!strcmp(mn, "smp")) {
		if (!sp_reg(&s, false) || !sp_char(&s, ',') ||
		    !sp_reg(&s, false) || !sp_char(&s, ',') ||
		    !sp_imm(&s, MAX_TEXTURE_SLOTS - 1))
			return false;
	} else {
		if (!strcmp(mn, "vmul"))
			nsrc = 2;
		else if (!strcmp(mn, "vmad"))
			nsrc = 3;
		else if (!strcmp(mn, "frcp") || !strcmp(mn, "frsq"))
			nsrc = 1;
		else
			return false;		/* unknown mnemonic */
		if (!sp_reg(&s, true))
			return false;
		while (nsrc--)
			if (!sp_char(&s, ',') || !sp_reg(&s, false))
				return false;
	}
	sp_ws(&s);
	return s.p == s.end || *s.p == '#';	/* nothing but a trailing comment */
}

/* payload: NUL-terminated text, NUL padded to a word boundary */
static int prismrv_validate_program(const u32 *p, u32 words)
{
	const char *t = (const char *)p;
	size_t len = 0, max = (size_t)words * 4, pos;
	unsigned int lines = 0;

	while (len < max && t[len])
		len++;
	if (len == max || !len)
		return -EINVAL;			/* unterminated / empty */
	for (pos = len; pos < max; pos++)
		if (t[pos])
			return -EINVAL;		/* junk after the terminator */

	for (pos = 0; pos < len;) {
		size_t e = pos;

		while (e < len && t[e] != '\n') {
			unsigned char c = t[e];

			if ((c < 0x20 && c != '\t') || c > 0x7e)
				return -EINVAL;
			e++;
		}
		if (e - pos > MAX_PROG_LINE_LEN || ++lines > MAX_PROG_LINES)
			return -EINVAL;
		if (!prog_line_ok(t + pos, e - pos))
			return -EINVAL;
		pos = e + 1;
	}
	return 0;
}

/* ---- layer 2 (TA packets) ------------------------------------------ */

enum { TA_VGT_STATE = 1, TA_INDEX_RANGE, TA_VERTEX_ARRAY, TA_END };

/**
 * prismrv_validate_ta() - validate one TA packet stream.
 *
 * Exactly VGT_STATE, INDEX_RANGE, VERTEX_ARRAY, END in that order.  The
 * vertex array is inline float data whose size must match its header and
 * which the index range must lie inside.
 */
int prismrv_validate_ta(const u32 *ta, size_t words)
{
	u32 mode = 0, first = 0, count = 0, vcount = 0;
	unsigned int stage = 0;
	size_t pos = 0;

	if (!words || words * 4 > MAX_TA_BYTES)
		return -EINVAL;

	while (pos < words) {
		u32 op, n;
		const u32 *p;

		if (words - pos < 2)
			return -EINVAL;
		op = ta[pos];
		n = ta[pos + 1];
		pos += 2;
		if (n > words - pos)
			return -EINVAL;
		p = ta + pos;
		pos += n;

		if (op != stage + 1)
			return -EINVAL;		/* wrong order / repeat */
		stage = op;

		switch (op) {
		case TA_VGT_STATE:
			if (n != 1 || p[0] > 2)
				return -EINVAL;
			mode = p[0];
			break;
		case TA_INDEX_RANGE:
			if (n != 2)
				return -EINVAL;
			first = p[0];
			count = p[1];
			break;
		case TA_VERTEX_ARRAY: {
			u32 ncomp;

			if (n < 4)
				return -EINVAL;
			ncomp = p[0];
			if (!ncomp || ncomp > MAX_TA_NCOMP || p[1] != 0 ||
			    p[2] != ncomp || (n - 3) % ncomp)
				return -EINVAL;
			vcount = (n - 3) / ncomp;
			break;
		}
		case TA_END:
			if (n != 0 || pos != words)
				return -EINVAL;
			break;
		default:
			return -EINVAL;
		}
	}
	if (stage != TA_END)
		return -EINVAL;

	/* the range must be non-empty, whole primitives, inside the array */
	if (!count || count > MAX_TA_VERTICES || vcount > MAX_TA_VERTICES ||
	    count % (mode + 1) ||
	    (u64)first + count > vcount)
		return -EINVAL;
	return 0;
}

/* ---- layer 1 -------------------------------------------------------- */

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

/* the BO containing [va, va+len); *off is the offset inside it */
static struct drm_gem_object *prismrv_find_bo(struct drm_gem_object **bos,
					      unsigned int num_bos, u64 va,
					      u64 len, u64 *off)
{
	unsigned int i;
	u64 end;

	if (check_add_overflow(va, len, &end))
		return NULL;

	for (i = 0; i < num_bos; i++) {
		u64 start, bo_end;

		if (!bos[i])
			continue;
		start = prismrv_bo_gpuva(bos[i]);
		bo_end = start + bos[i]->size;
		if (va >= start && end <= bo_end) {
			if (off)
				*off = va - start;
			return bos[i];
		}
	}
	return NULL;
}

/* a w x h surface with the given row stride, starting at va */
static bool prismrv_surface_ok(struct drm_gem_object **bos,
			       unsigned int num_bos, u32 va, u32 w, u32 h,
			       u32 stride, u32 fmt)
{
	unsigned int bpp = prismrv_fmt_bpp(fmt);

	if (!bpp || !w || !h || w > MAX_SURFACE_DIM || h > MAX_SURFACE_DIM)
		return false;
	if (stride < (u64)w * bpp)
		return false;
	return prismrv_find_bo(bos, num_bos, va, (u64)stride * h, NULL);
}

static bool prismrv_finite(u32 bits)
{
	return ((bits >> 23) & 0xff) != 0xff;	/* not Inf/NaN */
}

/**
 * prismrv_stream_next() - iterate the packets of a validated stream.
 * @pos: in/out word index; 0 to start
 * @op/@n/@payload: the packet
 *
 * Return: false at the end of the stream.
 */
bool prismrv_stream_next(const u32 *stream, size_t words, size_t *pos,
			 u32 *op, u32 *n, const u32 **payload)
{
	if (*pos + 2 > words)
		return false;
	*op = stream[*pos];
	*n = stream[*pos + 1];
	*payload = stream + *pos + 2;
	*pos += 2 + *n;
	return *pos <= words;
}

/**
 * prismrv_validate_stream() - check a command stream snapshot.
 * @stream: kernel copy of the stream
 * @words: its length in 32-bit words
 * @bos: BOs the submitter listed (NULL entries are skipped)
 *
 * Return: 0 if every packet is well formed, every address range is
 * inside a listed BO and every state value is in range, -EINVAL
 * otherwise.
 */
int prismrv_validate_stream(struct prismrv_device *pv, const u32 *stream,
			    size_t words, struct drm_gem_object **bos,
			    unsigned int num_bos)
{
	size_t pos = 0;
	u64 ta_total = 0;

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
			if (n == 0 || n > MAX_PROG_WORDS ||
			    prismrv_validate_program(p, n))
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
			if (!p[2] || p[2] % 4 || p[2] > MAX_TA_BYTES ||
			    !prismrv_find_bo(bos, num_bos, va, p[2], NULL))
				return -EINVAL;
			ta_total += p[2];
			if (ta_total > PRISMRV_STREAM_MAX_BYTES)
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

		case OP_SET_VIEWPORT:	/* scale[3], translate[3] (f32) */
			if (n != 6)
				return -EINVAL;
			for (u32 i = 0; i < 6; i++)
				if (!prismrv_finite(p[i]))
					return -EINVAL;
			break;

		case OP_SET_SCISSOR:	/* enable, minx, miny, maxx, maxy */
			if (n != 5 || p[0] > 1)
				return -EINVAL;
			if (p[0] && (p[1] > p[3] || p[2] > p[4] ||
				     p[3] > MAX_SCISSOR_COORD ||
				     p[4] > MAX_SCISSOR_COORD))
				return -EINVAL;
			break;

		case OP_SET_BLEND:	/* enable, rgb f/s/d, a f/s/d, mask */
			/* values are Gallium PIPE_BLEND_* / PIPE_BLENDFACTOR_* */
			if (n != 8 || p[0] > 1 || p[1] > 4 || p[2] > 0x1f ||
			    p[3] > 0x1f || p[4] > 4 || p[5] > 0x1f ||
			    p[6] > 0x1f || p[7] > 0xf)
				return -EINVAL;
			break;

		case OP_SET_DEPTH:	/* test, write, PIPE_FUNC_* */
			if (n != 3 || p[0] > 1 || p[1] > 1 || p[2] > 7)
				return -EINVAL;
			break;

		case OP_SET_RASTER:	/* PIPE_FACE_*, front_ccw */
			if (n != 2 || p[0] > 3 || p[1] > 1)
				return -EINVAL;
			break;

		default:
			/* unknown opcode: its payload might carry addresses */
			return -EINVAL;
		}
	}
	return 0;
}

/**
 * prismrv_stream_snapshot_size() - bytes needed for the snapshot BO.
 *
 * Layout: [stream][TA copy 0][TA copy 1]...  Call only on a stream that
 * passed prismrv_validate_stream().
 */
size_t prismrv_stream_snapshot_size(const u32 *stream, size_t words)
{
	size_t pos = 0, total = ALIGN(words * 4, 16);
	u32 op, n;
	const u32 *p;

	while (prismrv_stream_next(stream, words, &pos, &op, &n, &p))
		if (op == OP_DRAW)
			total += ALIGN(p[2], 16);
	return total;
}

/**
 * prismrv_stream_snapshot_fill() - build the snapshot in @out.
 * @snap_va: GPU VA of @out's BO
 * @read_bo: copies @len bytes at @off of a listed BO into @dst
 *
 * Copies the stream, then for each DRAW copies the TA packets out of the
 * user's BO, validates the copy (not the user memory) and rewrites the
 * DRAW address to the copy.  @out must hold prismrv_stream_snapshot_size()
 * bytes.
 */
int prismrv_stream_snapshot_fill(const u32 *stream, size_t words,
				 struct drm_gem_object **bos,
				 unsigned int num_bos, u32 snap_va, void *out,
				 prismrv_read_bo_fn read_bo, void *ctx)
{
	u32 *o = out;
	size_t pos = 0, data = ALIGN(words * 4, 16);
	u32 op, n;
	const u32 *p;

	memset(out, 0, prismrv_stream_snapshot_size(stream, words));
	memcpy(out, stream, words * 4);

	while (prismrv_stream_next(o, words, &pos, &op, &n, &p)) {
		struct drm_gem_object *bo;
		u32 *dp = o + (p - o);
		u64 va, off;
		int ret;

		if (op != OP_DRAW)
			continue;
		va = ((u64)p[1] << 32) | p[0];
		bo = prismrv_find_bo(bos, num_bos, va, p[2], &off);
		if (!bo)
			return -EINVAL;
		ret = read_bo(ctx, bo, off, (u8 *)out + data, p[2]);
		if (ret)
			return ret;
		ret = prismrv_validate_ta((u32 *)((u8 *)out + data), p[2] / 4);
		if (ret)
			return ret;
		va = (u64)snap_va + data;
		dp[0] = lower_32_bits(va);
		dp[1] = upper_32_bits(va);
		data += ALIGN(p[2], 16);
	}
	return 0;
}
