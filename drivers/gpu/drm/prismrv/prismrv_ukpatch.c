// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* prismrv_ukpatch.c - see prismrv_ukpatch.h */
#include "prismrv_ukpatch.h"

#ifdef PRISMRV_UKP_HOST
#include <errno.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#else
#include <linux/errno.h>
#include <linux/array_size.h>
#endif

#define UKP_PLACEHOLDER 0xDEADBEEFu

enum ukp_kind {
	UKP_CONST,	/* literal the loader writes (layout independent) */
	UKP_VA,		/* va of role */
	UKP_SHR4,	/* va >> 4 (16-byte units: emitpds / PDS program operands) */
	UKP_HEAP8M,	/* va & ~0x7FFFFF (8 MiB heap region base) */
	UKP_SHR4_20,	/* (va >> 4) | 0x20000000 */
};

/*
 * Master-uKernel `mov rN,#imm32` sites.  imm layout (verified): imm[20:0]=w[20:0], imm[25:21]=w[40:36],
 * imm[31:26]=w[49:44]; destination register = w[27:21].  Meaning of each site, from the uKernel code
 * and the loader: analysis/UKERNEL_IMMEDIATES.md.  NOTE site 1190 is the CONSTANT 0x01001000 (an
 * emitpds descriptor word, like 1200/10789) - it is NOT the HostCtl address.
 */
static const struct { u16 instr; u8 dest; u8 kind; u8 role; u32 val; } uk_imm_sites[] = {
	{ 1190,  0,  UKP_CONST,   0,              0x01001000 },
	{ 1192,  2,  UKP_SHR4,    20,             0 },	/* PDS program (role 20) */
	{ 1193,  4,  UKP_VA,      UKP_R_TA3DCTL,  0 },	/* TA3DCtl (0x190 object); code reads its dwords 1 and 9 */
	{ 1200,  0,  UKP_CONST,   0,              0x00800900 },
	{ 1202,  2,  UKP_SHR4,    21,             0 },
	{ 1214,  0,  UKP_VA,      28,             0 },
	{ 1216,  0,  UKP_CONST,   0,              0x00000006 },
	{ 1255,  5,  UKP_HEAP8M,  UKP_R_CCB,      0 },	/* kernel-data heap base */
	{ 1292,  4,  UKP_VA,      47,             0 },
	{ 10022, 7,  UKP_SHR4,    25,             0 },
	{ 10023, 8,  UKP_CONST,   0,              0x12022207 },
	{ 10111, 5,  UKP_CONST,   0,              0x2200E105 },
	{ 10116, 4,  UKP_SHR4,    27,             0 },
	{ 10789, 20, UKP_CONST,   0,              0x02800900 },
	{ 10792, 18, UKP_SHR4,    29,             0 },
	{ 11289, 17, UKP_SHR4_20, 23,             0 },
	{ 11290, 18, UKP_CONST,   0,              0x0A028201 },
};

#define UK_SLAVE_FIRST_INSTR	11951	/* slave object holds instr 11951.. */
#define UK_SLAVE_SIZE_INSTR	1754	/* low dword = slave size + 1 */
#define UK_SLAVE_VA_INSTR	11965	/* low dword = ((slave va & 0x7FFFF) >> 3) | 8 */

enum rel_kind { REL_PTR, REL_SHR4_FIELD, REL_HI23_FIELD, REL_LO23_OR };

/*
 * Words the loader writes into the other uploaded objects.  `word = va(target)+add` (REL_PTR), or the
 * bit field [shift +: width] of the stock word is replaced by ((va(target)+add) >> 4) (REL_SHR4_FIELD).
 */
static const struct { u8 role; u16 off; u8 kind; u8 target; u16 add; u8 shift; u8 width; } rel_words[] = {
	{ 5, 0x000, REL_PTR, 5, 0, 0, 32 },  { 5, 0x004, REL_PTR, 4, 0, 0, 32 },
	{ 5, 0x008, REL_PTR, 2, 0, 0, 32 },  { 5, 0x0a0, REL_PTR, 8, 0, 0, 32 },
	{ 5, 0x0c4, REL_PTR, 1, 0, 0, 32 },  { 5, 0x0d4, REL_PTR, 6, 0, 0, 32 },
	{ 5, 0x134, REL_PTR, 27, 0x10, 0, 32 }, { 5, 0x138, REL_PTR, 27, 0x14, 0, 32 },
	{ 5, 0x13c, REL_PTR, 27, 0x34, 0, 32 }, { 5, 0x140, REL_PTR, 27, 0x30, 0, 32 },
	{ 5, 0x144, REL_PTR, 9, 0, 0, 32 },  { 5, 0x148, REL_PTR, 10, 0, 0, 32 },
	{ 5, 0x14c, REL_PTR, 11, 0, 0, 32 },
	{ 18, 0x010, REL_PTR, 5, 0, 0, 32 }, { 18, 0x020, REL_SHR4_FIELD, 17, 0, 5, 22 },
	{ 20, 0x000, REL_PTR, 5, 0, 0, 32 },
	{ 22, 0x010, REL_SHR4_FIELD, 5, 0xe0, 4, 19 },	/* = (va(TA3DCtl) & 0x7FFFFF) | 0xE8 */
	{ 22, 0x014, REL_HI23_FIELD, 1, 0, 6, 2 },	/* heap-region index: (va(kernel-data obj) >> 23) & 3 */
	{ 23, 0x000, REL_SHR4_FIELD, 22, 0, 5, 19 },
	{ 25, 0x000, REL_PTR, 5, 0, 0, 32 }, { 25, 0x010, REL_SHR4_FIELD, 24, 0, 5, 23 },
	{ 27, 0x000, REL_SHR4_FIELD, 26, 0, 5, 19 },
	{ 40, 0x000, REL_SHR4_FIELD, 42, 0, 0, 30 }, { 40, 0x00c, REL_PTR, 41, 0, 0, 32 },
	{ 40, 0x014, REL_SHR4_FIELD, 42, 0x50, 0, 28 }, { 40, 0x01c, REL_SHR4_FIELD, 42, 0xa0, 0, 30 },
	{ 42, 0x020, REL_SHR4_FIELD, 43, 0, 5, 19 }, { 42, 0x070, REL_SHR4_FIELD, 43, 0x80, 5, 19 },
	{ 42, 0x0c0, REL_SHR4_FIELD, 43, 0x110, 5, 19 },
};

static const struct prismrv_ukp_obj *find(const struct prismrv_ukp_obj *o, unsigned int n, u32 role)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		if (o[i].role == role)
			return &o[i];
	return NULL;
}

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

static int is_mov_imm(u64 w) { return (w >> 56) == 0xfc && ((w >> 52) & 0xf) == 2; }
static u32 imm_dec(u64 w) { return (u32)(w & 0x1fffff) | (u32)(((w >> 36) & 0x1f) << 21) | (u32)(((w >> 44) & 0x3f) << 26); }
static u64 imm_enc(u64 w, u32 v)
{
	w &= ~(0x1fffffULL | (0x1fULL << 36) | (0x3fULL << 44));
	return w | (v & 0x1fffff) | ((u64)((v >> 21) & 0x1f) << 36) | ((u64)((v >> 26) & 0x3f) << 44);
}

int prismrv_ukp_apply(const struct prismrv_ukp_obj *objs, unsigned int n, const char **why)
{
	const struct prismrv_ukp_obj *uk = find(objs, n, UKP_R_UKERNEL), *sl = find(objs, n, UKP_R_UKSLAVE);
	unsigned int i;
	u8 *img;

#define FAIL(msg) do { if (why) *why = msg; return -EINVAL; } while (0)
	if (!uk || !sl || uk->size < 8u * 11951 || sl->size < 8u * (UK_SLAVE_VA_INSTR - UK_SLAVE_FIRST_INSTR + 1))
		FAIL("uKernel master/slave object missing or too small");
	img = uk->data;

	for (i = 0; i < ARRAY_SIZE(uk_imm_sites); i++) {
		u32 instr = uk_imm_sites[i].instr, want = uk_imm_sites[i].val;
		const struct prismrv_ukp_obj *t = NULL;
		u64 w = rd64(img + 8 * instr);

		if (uk_imm_sites[i].kind != UKP_CONST) {
			t = find(objs, n, uk_imm_sites[i].role);
			if (!t)
				FAIL("uKernel patch target object missing");
		}
		switch (uk_imm_sites[i].kind) {
		case UKP_VA:	  want = t->va; break;
		case UKP_SHR4:	  want = t->va >> 4; break;
		case UKP_HEAP8M:  want = t->va & ~0x7FFFFFu; break;
		case UKP_SHR4_20: want = (t->va >> 4) | 0x20000000u; break;
		default: break;
		}
		if (!is_mov_imm(w) || ((w >> 21) & 0x7f) != uk_imm_sites[i].dest)
			FAIL("uKernel site is not the expected mov-imm32: not the stock image");
		if (imm_dec(w) != UKP_PLACEHOLDER && imm_dec(w) != want)
			FAIL("uKernel site is neither 0xDEADBEEF nor the expected value: unknown image");
		wr64(img + 8 * instr, imm_enc(w, want));
	}

	/* slave size / slave address words (raw `phas`-class words, upper dword untouched) */
	{
		u8 *p = img + 8 * UK_SLAVE_SIZE_INSTR;
		u32 lo = rd32(p), want = sl->size + 1;

		if (lo != 0 && lo != want)
			FAIL("uKernel instr 1754 is not the stock image");
		wr32(p, want);
		p = (u8 *)sl->data + 8 * (UK_SLAVE_VA_INSTR - UK_SLAVE_FIRST_INSTR);
		lo = rd32(p); want = ((sl->va & 0x7FFFFu) >> 3) | 8;
		if (lo != 0 && lo != want)
			FAIL("uKernel slave instr 11965 is not the stock image");
		wr32(p, want);
	}

	for (i = 0; i < ARRAY_SIZE(rel_words); i++) {
		const struct prismrv_ukp_obj *o = find(objs, n, rel_words[i].role);
		const struct prismrv_ukp_obj *t = find(objs, n, rel_words[i].target);
		u32 w, field, mask, a;

		if (!o || !t || rel_words[i].off + 4u > o->size)
			FAIL("relocation object missing");
		w = rd32((u8 *)o->data + rel_words[i].off);
		a = t->va + rel_words[i].add;
		if (rel_words[i].kind == REL_PTR) {
			wr32((u8 *)o->data + rel_words[i].off, a);
			continue;
		}
		mask = rel_words[i].width >= 32 ? 0xffffffffu : (1u << rel_words[i].width) - 1;
		field = rel_words[i].kind == REL_HI23_FIELD ? (t->va >> 23) & mask : (a >> 4) & mask;
		w = (w & ~(mask << rel_words[i].shift)) | (field << rel_words[i].shift);
		wr32((u8 *)o->data + rel_words[i].off, w);
	}
	return 0;
#undef FAIL
}
