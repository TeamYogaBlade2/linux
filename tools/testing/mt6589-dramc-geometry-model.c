// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side model of the MT6589 DRAM geometry decode in
 * drivers/memory/mt6589-dramc.c, checked against concrete EMI_CONA values
 * that have known part descriptions.
 *
 * The test vectors come from two places, and every expectation below is
 * traceable to one of them:
 *
 *  - downstream preloader/src/drivers/mt_emi.c, which annotates its own
 *    example values:
 *      0x00002128 "dw32_en=0, row 14bit, bank 3bit, column 10bit : 2Gb x16"
 *      0x0000212A "dw32_en=1, row 14bit, bank 3bit, column 10bit : 2Gb x16 + 2Gb x16"
 *      0x11303128 "dw32_en=0, row 15bit, bank 3bit, column 10bit : 4Gb x16"
 *      0x0000A3AE / 0x0002A3AE "one rank" / "dual rank"
 *  - the datasheet's EMI_CONA field table (p. 585), which is where the field
 *    positions come from: ROW[13:12], BAN[8], COL[5:4], DW32_EN[1],
 *    DUAL_RANK_EN[17], and ROW2ND[15:14], BAN2ND[9], COL2ND[7:6] for rank 2.
 *
 * DW32_EN being bit 1 and not bit 0 is the trap this model exists to catch:
 * 0x2128 and 0x212A differ only in that bit, and reading bit 0 would report
 * every one of these boards as a 16-bit bus.
 *
 * Build with "gcc -Wall -Wextra" and run it; it needs no kernel headers.
 * It is a host model, so it is not built by the kernel build.
 */
#include <stdio.h>
#include <stdint.h>

#define ROW(f)   (12 + (f))
#define BAN(f)   (2 + (f))
#define COL(f)   (8 + (f))

static unsigned long long size_of(uint32_t cona)
{
	unsigned int row   = ROW((cona >> 12) & 3);
	unsigned int bank  = BAN((cona >> 8) & 1);
	unsigned int col   = COL((cona >> 4) & 3);
	unsigned int width = (cona & (1u << 1)) ? 32 : 16;   /* DW32_EN is bit 1 */
	unsigned long long b = (1ULL << row) * (1ULL << bank) * (1ULL << col) * (width / 8);

	if ((cona >> 17) & 1) {
		unsigned int r2 = ROW((cona >> 14) & 3);
		unsigned int b2 = BAN((cona >> 9) & 1);
		unsigned int c2 = COL((cona >> 6) & 3);

		b += (1ULL << r2) * (1ULL << b2) * (1ULL << c2) * (width / 8);
	}
	return b;
}

#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void)
{
	int fails = 0;

	/* downstream mt_emi.c: "one rank:0x0000A3AE. dual rank = 0x0002A3AE" */
	CHECK(size_of(0x0000A3AE) == 512ULL << 20, "0xA3AE single rank = 512 MiB");
	/* ... and DUAL_RANK_EN (bit 17) doubles it: 1 GiB total. */
	CHECK(size_of(0x0002A3AE) == 1024ULL << 20, "0x2A3AE dual rank = 1024 MiB");

	/* "0x00002128 //dw32_en=0, row 14bit, bank 3bit, column 10bit : 2Gb x16" */
	CHECK(size_of(0x00002128) == (1ULL << 31) / 8, "0x2128 = 2Gb x16 = 256 MiB");

	/* "0x0000212A //dw32_en=1, row 14bit, bank 3bit, column 10bit : 2Gb x16 + 2Gb x16"
	 * Two x16 parts on a 32-bit bus: same addresses, twice the bus word.
	 */
	CHECK(size_of(0x0000212A) == (1ULL << 31) / 8 * 2, "0x212A = 2 x 2Gb x16 = 512 MiB");

	/* "0x11303128 //dw32_en=0, row 15bit, bank 3bit, column 10bit : 4Gb x16" */
	CHECK(size_of(0x11303128) == (1ULL << 32) / 8, "0x11303128 = 4Gb x16 = 512 MiB");

	/* DW32_EN must be bit 1: 0x2128 vs 0x212A differ only in that bit and
	 * the part count, so a decoder reading bit 0 would report both as 16b.
	 */
	CHECK(((0x212A >> 1) & 1) == 1, "DW32_EN is bit 1");
	CHECK((0x2128 & 1) == 0 && (0x212A & 1) == 0, "bit 0 is clear in both");

	/* The board's memory node is 1 GiB (mt6589-lenovo-blade.dtsi
	 * reg = <0x80000000 0x40000000>).  Two 512 MiB ranks of 4Gb x32 on a
	 * 32-bit bus: row 14, bank 3, col 10, DW32_EN=1, DUAL_RANK_EN=1, with
	 * rank 2 given the same geometry.
	 *
	 * This is also what the board's bootloader is driven with:
	 * da-boot.c runs it as --dram-size-per-rank 0x20000000 --dram-ranks 2,
	 * i.e. 512 MiB per rank.  2^27 addresses per rank therefore has to be
	 * 4 bytes each, which is DW32_EN set - not the DRAMC bus width, which
	 * is 64 here because it sums both 32-bit ranks.  Sizing each rank from
	 * the bus width instead reports 2 GiB for this 1 GiB board.
	 */
	{
		uint32_t c = (2u << 12) | (1u << 8) | (2u << 4) | (1u << 1) |
			     (1u << 17) | (2u << 14) | (1u << 9) | (2u << 6);
		printf("row14 x32 dual rank -> %llu MiB\n", size_of(c) >> 20);
		CHECK(size_of(c) == 1ULL << 30, "row14 x32 dual rank = 1 GiB (board)");
		/* Per rank must equal the bootloader's --dram-size-per-rank. */
		CHECK(size_of(c) / 2 == 0x20000000ULL,
		      "each rank = 0x20000000 (da-boot --dram-size-per-rank)");
	}

	if (!fails)
		printf("PASS: all DRAM geometry checks\n");
	return fails != 0;
}
