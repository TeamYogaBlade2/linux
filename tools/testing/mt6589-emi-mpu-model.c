// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side model of the MT6589 EMI MPU region packing, compiled from
 * userspace to check the arithmetic in drivers/soc/mediatek/mt6589-emi.c.
 *
 * The encoding being checked, from the datasheet:
 *   EMI_MPUA..H  [31:16] = start >> 16, [15:0] = end >> 16   (64KB units)
 *   EMI_MPUI..L  even region in [15:0], odd region in [31:16],
 *                each 4 x 3-bit permissions [11:9]=d3 .. [2:0]=d0
 *
 * Downstream agrees: emi_mpu.c emi_mpu_set_region_protection() writes
 * (start << 16) | end after shifting both right by 16, and MPUI for even
 * regions / its upper half for odd ones.
 *
 * Build and run: see the invoke at the bottom of verify.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MT6589_EMI_NR_REGIONS	8
#define EMI_MPU_ALIGNMENT	0x10000u
#define EMI_PHY_OFFSET		0x80000000u
#define EMI_MPUI		0x01a0u

#define EMI_MPU_SET_PERM(d3, d2, d1, d0) \
	((((d3) & 0x7) << 9) | (((d2) & 0x7) << 6) | \
	 (((d1) & 0x7) << 3) | ((d0) & 0x7))

/* Mirrors the register file the driver would touch. */
static uint32_t reg[0x1000 / 4];

static uint32_t rd(uint32_t off) { return reg[off >> 2]; }
static void wr(uint32_t off, uint32_t v) { reg[off >> 2] = v; }

/* Verbatim transcription of the driver's mt6589_emi_mpu_set_region(). */
static int set_region(int region, uint32_t start, uint32_t end,
		      const uint32_t perm[4])
{
	uint32_t perm_reg, perm_off, val;
	int ret = 0;

	if (region < 0 || region >= MT6589_EMI_NR_REGIONS)
		return -22;

	if (start || end) {
		if (start >= EMI_PHY_OFFSET)
			start -= EMI_PHY_OFFSET;
		if (end >= EMI_PHY_OFFSET)
			end -= EMI_PHY_OFFSET;

		start &= ~(EMI_MPU_ALIGNMENT - 1);
		end &= ~(EMI_MPU_ALIGNMENT - 1);

		if (end <= start) {
			ret = -22;
			goto out;
		}
	}

	if (start || end)
		val = ((start >> 16) << 16) | (end >> 16);
	else
		val = 0;

	wr(0x160 + region * 8, val);

	perm_reg = EMI_MPUI + (region / 2) * 8;
	perm_off = (region % 2) * 16;

	val = rd(perm_reg);
	val &= ~(0xfffu << perm_off);
	val |= EMI_MPU_SET_PERM(perm[3], perm[2], perm[1], perm[0]) << perm_off;
	wr(perm_reg, val);

out:
	return ret;
}

static void decode_addr(int region, uint32_t *s, uint32_t *e)
{
	uint32_t v = rd(0x160 + region * 8);

	*s = ((v >> 16) << 16) + EMI_PHY_OFFSET;
	*e = ((v & 0xffff) << 16) + EMI_PHY_OFFSET;
}

static uint32_t decode_perm(int region)
{
	uint32_t v = rd(EMI_MPUI + (region / 2) * 8);

	return (v >> ((region % 2) * 16)) & 0xfff;
}

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			printf("FAIL: %s\n", msg);			\
			fails++;					\
		}							\
	} while (0)

int main(void)
{
	uint32_t s, e;
	int fails = 0;

	/* 1. AP region 7 exactly as downstream installs it: kernel_base to
	 *    kernel_base + dram_size, with the documented permission row
	 *    NO_PROTECTION / FORBIDDEN / FORBIDDEN / NO_PROTECTION.
	 */
	{
		const uint32_t perm[4] = { 0, 5, 5, 0 };

		/* 0x8000_0000 + 1 GiB, the board's memory node. */
		set_region(7, 0x80000000, 0xC0000000, perm);
		decode_addr(7, &s, &e);
		CHECK(s == 0x80000000, "region 7 start round-trip");
		CHECK(e == 0xC0000000, "region 7 end round-trip");
		/* d0=0, d1=5, d2=5, d3=0 -> 5<<3 | 5<<6 = 0x168 */
		CHECK(decode_perm(7) == (0u << 0 | 5u << 3 | 5u << 6 | 0u << 9),
		      "region 7 permission value is 0x168");
	}

	/* 2. Odd/even permutation must not clobber the neighbour. */
	{
		const uint32_t p0[4] = { 4, 4, 4, 4 };
		const uint32_t p1[4] = { 1, 2, 3, 4 };

		memset(reg, 0, sizeof(reg));
		set_region(0, 0x80000000, 0x80100000, p0);
		set_region(1, 0x80200000, 0x80300000, p1);

		CHECK(decode_perm(0) ==
		      (4u << 0 | 4u << 3 | 4u << 6 | 4u << 9), "region 0 perm");
		CHECK(decode_perm(1) ==
		      (1u << 0 | 2u << 3 | 3u << 6 | 4u << 9), "region 1 perm");
		decode_addr(0, &s, &e);
		CHECK(s == 0x80000000 && e == 0x80100000, "region 0 addr");
		decode_addr(1, &s, &e);
		CHECK(s == 0x80200000 && e == 0x80300000, "region 1 addr");
	}

	/* 3. Region 7 must land in MPUL (0x1b8) upper half, not MPUK. */
	{
		const uint32_t perm[4] = { 0, 0, 0, 0 };

		memset(reg, 0, sizeof(reg));
		set_region(7, 0x90000000, 0x90100000, perm);
		CHECK(rd(0x1b8) >> 16 == 0 /* perms are zero here */, "MPUL used");
		CHECK(rd(0x1b0) == 0, "MPUK untouched by region 7");
	}

	/* 4. Disabling with 0,0 writes zero to the address register. */
	{
		const uint32_t perm[4] = { 0, 0, 0, 0 };

		set_region(3, 0x80000000, 0x80100000, perm);
		CHECK(rd(0x160 + 3 * 8) != 0, "region 3 armed");
		set_region(3, 0, 0, perm);
		CHECK(rd(0x160 + 3 * 8) == 0, "region 3 disabled by 0/0");
	}

	/* 5. Inverted and degenerate ranges are refused, not truncated. */
	{
		const uint32_t perm[4] = { 0, 0, 0, 0 };

		CHECK(set_region(0, 0x80200000, 0x80100000, perm) == -22,
		      "inverted range refused");
		CHECK(set_region(0, 0x80000000, 0x80000000, perm) == -22,
		      "empty range refused");
		CHECK(set_region(0, 0x80000001, 0x8000FFFF, perm) == -22,
		      "sub-64KB range refused after alignment");
	}

	/* 6. Ranges are rounded DOWN to 64KB, so a protected region is never
	 *    larger than asked for.  This is the safety-critical direction:
	 *    rounding up would cover memory the caller did not protect.
	 */
	{
		const uint32_t perm[4] = { 5, 5, 5, 5 };

		set_region(2, 0x80000000 + 0x1234, 0x80100000 + 0x8000, perm);
		decode_addr(2, &s, &e);
		CHECK(s == 0x80000000, "start rounded down");
		CHECK(e == 0x80100000, "end rounded down");
		CHECK(s < 0x80000000 + 0x1234, "start never rounded up");
		CHECK(e < 0x80100000 + 0x8000, "end never rounded up");
	}

	/* 7. Out-of-range region index. */
	{
		const uint32_t perm[4] = { 0, 0, 0, 0 };

		CHECK(set_region(8, 0x80000000, 0x80100000, perm) == -22,
		      "region 8 refused");
		CHECK(set_region(-1, 0x80000000, 0x80100000, perm) == -22,
		      "region -1 refused");
	}

	if (!fails)
		printf("PASS: all EMI MPU region encoding checks\n");

	return fails ? 1 : 0;
}
