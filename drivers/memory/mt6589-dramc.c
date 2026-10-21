// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6589 DRAMC / DDRPHY.
 *
 * THE BLOCK IS SPLIT ACROSS THREE WINDOWS
 *
 * Datasheet 13.4.1: "The DRAMC command register is divided into three
 * partitions", and names them explicitly:
 *
 *   DRAMC AO   base +0x1000_4000
 *   DRAMC NAO  base +0x1020_F000
 *   DDRPHY     base +0x1001_1000
 *
 * Downstream agrees exactly: mt_reg_base.h:12-14 has
 * DRAMC0_BASE 0xF0004000, DDRPHY_BASE 0xF0011000 and
 * DRAMC_NAO_BASE 0xF020F000, and emi_bwl.h's DRAMC_READ()/DRAMC_WRITE()
 * macros OR or write all three windows at the same offset.  A register at
 * "offset X" therefore exists at X in each of the three, and only some of
 * them are implemented in each.  This driver reads one window at a time and
 * labels which one, rather than ORing three windows together the way the
 * downstream macro does -- that OR makes an unimplemented alias look like a
 * real value.
 *
 * The DDRPHY window also contains the MEMPLL registers at +0x600.., which
 * is why clk-mt6589-mempll.c claims 0x1001_1000 as well.  The two drivers
 * use devm_ioremap_resource on separate reg entries of separate nodes, so
 * they do not share the mapping.
 *
 * WHY THIS IS READ-ONLY
 *
 *  A DRAM controller is the one block where a wrong write does not produce
 *  a warning, it produces a board that stops.  Changing the DRAM clock
 *  requires re-deriving the whole timing set for the new period, and the
 *  only source for that on MT6589 is the preloader's tables in
 *  mtk_pll.c / custom_emi.c, which are compile-time #ifdef ladders keyed on
 *  macros (MEMPLL_CLK_400, DDRPHY_3PLL_MODE) that no board in this tree
 *  defines.  So:
 *
 *   - Nothing is written at probe time.
 *   - There is no DVFS or rate-change path.  Not "opt-in": absent.
 *   - The API exposes the configuration and the *derived* timing, nothing
 *     that mutates it.
 *
 *  If a future change wants a rate change it must refuse any frequency not
 *  in a table transcribed from a specific downstream configuration, and it
 *  must have a recovery path, because by construction a bad DRAM timing
 *  change is not recoverable from software running out of that same DRAM.
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#include <linux/soc/mediatek/mt6589-dramc.h>
#include <linux/soc/mediatek/mt6589-emi.h>

/* DRAMC AO registers, datasheet p. 651f (base +0x1000_4000). */
#define DRAMC_ACTIM0		0x000
#define DRAMC_CONF1		0x004
#define DRAMC_CONF2		0x008
#define DRAMC_DDR2CTL		0x07c
#define DRAMC_MRS		0x088
#define DRAMC_RKCFG		0x110
#define DRAMC_ACTIM1		0x1e8
#define DRAMC_LPDDR2		0x1e0
#define DRAMC_PERFCTL0		0x1ec

/* DRAMC_CONF1 fields, datasheet p. 653f. */
#define CONF1_TESTLP		BIT(27)
#define CONF1_SELFREF		BIT(26)
#define CONF1_TCMD		GENMASK(22, 20)
#define CONF1_CKEON		BIT(15)
#define CONF1_FW2R		BIT(13)
#define CONF1_BL4		BIT(10)
#define CONF1_MATYPE		GENMASK(9, 8)
#define CONF1_TRRD		GENMASK(7, 6)
#define CONF1_PAGDIS		BIT(3)
#define CONF1_DM64BITEN		BIT(0)

/* DRAMC_CONF2 fields, datasheet p. 654. */
#define CONF2_TEST2W		BIT(31)
#define CONF2_TEST2R		BIT(30)
#define CONF2_TEST1		BIT(29)
#define CONF2_REFTHD		GENMASK(26, 24)
#define CONF2_REFCNT		GENMASK(7, 0)

/* DRAMC_ACTIM0 fields, datasheet p. 652. */
#define ACTIM0_TRCD		GENMASK(31, 28)
#define ACTIM0_TRP		GENMASK(27, 24)
#define ACTIM0_TFAW		GENMASK(23, 20)
#define ACTIM0_TWR		GENMASK(19, 16)
#define ACTIM0_BL2		BIT(15)
#define ACTIM0_CL2		BIT(12)
#define ACTIM0_TWTR		GENMASK(11, 8)
#define ACTIM0_TRC		GENMASK(7, 4)
#define ACTIM0_TRAS		GENMASK(3, 0)

/* DRAMC_LPDDR2 fields, datasheet p. 672. */
#define LPDDR2_ADRDECEN		BIT(31)
#define LPDDR2_LPDDR2EN		BIT(28)
#define LPDDR2_WDATRGO		BIT(27)

/* EMI_CONA address mapping, datasheet p. 585 (EMI window, +0x000). */
#define EMI_CONA		0x000
#define EMI_CONA_DUAL_RANK_EN	BIT(17)
#define EMI_CONA_ROW2ND		GENMASK(15, 14)
#define EMI_CONA_ROW		GENMASK(13, 12)
#define EMI_CONA_BAN2ND		BIT(9)
#define EMI_CONA_BAN		BIT(8)
#define EMI_CONA_COL2ND		GENMASK(7, 6)
#define EMI_CONA_COL		GENMASK(5, 4)
#define EMI_CONA_CHN_POS	GENMASK(3, 2)
/*
 * DW32_EN is bit 1, not bit 0.  The datasheet's EMI_CONA field table
 * (p. 585) lists "1 DW32_EN", and its bits 3:2 CHN_POS sits above it.
 * Downstream's comments in preloader mt_emi.c describe the same value
 * 0x0000212A as "dw32_en=1 ... 2Gb x16 + 2Gb x16", and the low nibble
 * 0xA breaks down as DW32_EN=1 (bit 1) with CHN_POS=2 (bits 3:2) -- so
 * reading bit 0 instead would report every board as 16 bits wide.
 */
#define EMI_CONA_DW32_EN	BIT(1)

static const char * const mt6589_dramc_window_name[] = {
	"AO",		/* 0x1000_4000 */
	"NAO",		/* 0x1020_F000 */
	"DDRPHY",	/* 0x1001_1000 */
};

struct mt6589_dramc {
	struct device *dev;
	void __iomem *base[MT6589_DRAMC_NR_WINDOWS];
	struct clk *clk;	/* mempll_mck_d4, informational only */
	unsigned long clk_rate;
	bool has_emi;
	void __iomem *emi;
	struct dentry *debugfs;
};

static u32 mt6589_dramc_read(struct mt6589_dramc *dc, int win, u32 off)
{
	if (!dc->base[win])
		return 0;

	return readl(dc->base[win] + off);
}

/*
 * Turn CONF1[9:8] MATYPE into a column address bit count, datasheet p. 653:
 * 00 = 8 bits, 01 = 9, 10 = 10, 11 = 11.
 */
static unsigned int mt6589_dramc_col_bits(u32 matype)
{
	return 8 + matype;
}

/* EMI_CONA row bits are encoded the same way, datasheet p. 585. */
static unsigned int mt6589_dramc_row_bits(u32 field)
{
	return 12 + field;
}

static unsigned int mt6589_dramc_bank_bits(u32 field)
{
	return 2 + field;
}

/**
 * mt6589_dramc_get_info() - read the DRAM geometry and timing from hardware
 * @dc: DRAMC handle
 * @info: filled in with the decoded configuration
 *
 * The geometry is decoded from EMI_CONA (address mapping) and DRAMC_CONF1
 * (column width, bus width), and the size is derived from those.  The timing
 * fields are converted back into the DRAMC clock cycles the datasheet says
 * they encode; they are NOT converted into nanoseconds, because that needs
 * the DRAMC clock rate and the FDIV2 relationship, and reporting a time
 * derived from a guessed rate is worse than reporting the raw count.
 *
 * Returns 0, or -ENODEV if the EMI window was not provided (geometry is
 * unavailable without it, but the timings still are).
 */
int mt6589_dramc_get_info(struct mt6589_dramc *dc,
			  struct mt6589_dramc_info *info)
{
	u32 cona, conf1, conf2, actim0, lpddr2;

	memset(info, 0, sizeof(*info));

	conf1 = mt6589_dramc_read(dc, MT6589_DRAMC_WIN_AO, DRAMC_CONF1);
	conf2 = mt6589_dramc_read(dc, MT6589_DRAMC_WIN_AO, DRAMC_CONF2);
	actim0 = mt6589_dramc_read(dc, MT6589_DRAMC_WIN_AO, DRAMC_ACTIM0);
	lpddr2 = mt6589_dramc_read(dc, MT6589_DRAMC_WIN_AO, DRAMC_LPDDR2);

	info->conf1 = conf1;
	info->conf2 = conf2;
	info->actim0 = actim0;
	info->lpddr2 = lpddr2;

	info->type = (lpddr2 & LPDDR2_LPDDR2EN) ? "LPDDR2" : "DDR3/mDDR";
	info->bus_width = (conf1 & CONF1_DM64BITEN) ? 64 : 32;
	info->burst_length = (conf1 & CONF1_BL4) ? 4 : 8;
	info->page_mode = !(conf1 & CONF1_PAGDIS);
	info->cke = !!(conf1 & CONF1_CKEON);
	info->self_refresh = !!(conf1 & CONF1_SELFREF);
	info->column_bits = mt6589_dramc_col_bits(FIELD_GET(CONF1_MATYPE, conf1));

	info->trcd = FIELD_GET(ACTIM0_TRCD, actim0) + 1;
	info->trp = FIELD_GET(ACTIM0_TRP, actim0) + 1;
	info->tfaw = 8 + FIELD_GET(ACTIM0_TFAW, actim0);
	info->twr = FIELD_GET(ACTIM0_TWR, actim0);
	info->twtr = 3 + FIELD_GET(ACTIM0_TWTR, actim0);
	info->trc = FIELD_GET(ACTIM0_TRC, actim0);	/* see the dump for the +1 */
	info->tras = FIELD_GET(ACTIM0_TRAS, actim0);
	info->trrd = 1 + FIELD_GET(CONF1_TRRD, conf1);
	info->refcnt = FIELD_GET(CONF2_REFCNT, conf2);
	info->refthd = FIELD_GET(CONF2_REFTHD, conf2);

	info->clock_rate = dc->clk_rate;

	if (!dc->emi) {
		info->size_bytes = 0;
		return -ENODEV;
	}

	cona = readl(dc->emi + EMI_CONA);
	info->cona = cona;
	info->dual_rank = !!(cona & EMI_CONA_DUAL_RANK_EN);
	info->row_bits = mt6589_dramc_row_bits(FIELD_GET(EMI_CONA_ROW, cona));
	info->bank_bits = mt6589_dramc_bank_bits(FIELD_GET(EMI_CONA_BAN, cona));
	info->emi_col_bits = FIELD_GET(EMI_CONA_COL, cona) + 8;
	info->ram_width = (cona & EMI_CONA_DW32_EN) ? 32 : 16;

	if (info->dual_rank) {
		info->row2nd_bits =
			mt6589_dramc_row_bits(FIELD_GET(EMI_CONA_ROW2ND, cona));
		info->bank2nd_bits =
			mt6589_dramc_bank_bits(FIELD_GET(EMI_CONA_BAN2ND, cona));
		info->emi_col2nd_bits = FIELD_GET(EMI_CONA_COL2ND, cona) + 8;
	}

	/*
	 * Capacity of one rank = 2^row * 2^bank * 2^col addresses, each
	 * carrying one bus word of (bus_width / 8) bytes.  Checked against
	 * the datasheet's own worked example: downstream mt_emi.c annotates
	 * 0x00002128 as "dw32_en=0, row 14bit, bank 3bit, column 10bit :
	 * 2Gb x16", and with DW32_EN=0 (16-bit bus) this gives
	 * 2^27 * 2 = 256 MiB = 2Gb, which is the part size it names.
	 *
	 * The second rank is NOT assumed to mirror the first: EMI_CONA
	 * carries ROW2ND, BAN2ND and COL2ND for it precisely because it can
	 * differ, so it is sized from its own fields when dual rank mode is
	 * on.  (CHN_POS, EMI_CONA[3:2], is left out: it only says where in
	 * the AXI address the channel bit sits, which does not change the
	 * total.)
	 */
	info->size_bytes = (1ULL << info->row_bits) *
			   (1ULL << info->bank_bits) *
			   (1ULL << info->column_bits) *
			   (info->bus_width / 8);

	if (info->dual_rank)
		info->size_bytes += (1ULL << info->row2nd_bits) *
				    (1ULL << info->bank2nd_bits) *
				    (1ULL << info->emi_col2nd_bits) *
				    (info->bus_width / 8);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6589_dramc_get_info);

struct mt6589_dramc *mt6589_dramc_get(struct device *dev)
{
	struct platform_device *pdev;

	pdev = to_platform_device(dev->parent);
	if (!pdev || !pdev->dev.driver)
		return ERR_PTR(-EPROBE_DEFER);

	return platform_get_drvdata(pdev) ?: ERR_PTR(-EPROBE_DEFER);
}
EXPORT_SYMBOL_GPL(mt6589_dramc_get);

static int mt6589_dramc_show(struct seq_file *s, void *unused)
{
	struct mt6589_dramc *dc = s->private;
	struct mt6589_dramc_info info;
	int ret;
	int w;

	ret = mt6589_dramc_get_info(dc, &info);

	seq_printf(s, "clocks: mempll_mck_d4 %lu Hz\n", info.clock_rate);

	seq_puts(s, "\nraw registers\n");
	for (w = 0; w < MT6589_DRAMC_NR_WINDOWS; w++) {
		seq_printf(s, "  %-6s CONF1 0x%08x CONF2 0x%08x ACTIM0 0x%08x\n",
			   mt6589_dramc_window_name[w],
			   mt6589_dramc_read(dc, w, DRAMC_CONF1),
			   mt6589_dramc_read(dc, w, DRAMC_CONF2),
			   mt6589_dramc_read(dc, w, DRAMC_ACTIM0));
	}

	seq_printf(s, "\ntype        : %s\n", info.type);
	seq_printf(s, "bus width   : %u bits\n", info.bus_width);
	seq_printf(s, "burst length: %u\n", info.burst_length);
	seq_printf(s, "page mode   : %s\n", info.page_mode ? "on" : "off");
	seq_printf(s, "CKE         : %s\n", info.cke ? "on" : "off");
	seq_printf(s, "self refresh: %s\n", info.self_refresh ? "on" : "off");

	if (ret)
		seq_puts(s, "\nEMI window not mapped: geometry unavailable\n");
	else
		seq_printf(s, "size        : %llu MiB (%llu bytes)%s\n",
			   info.size_bytes >> 20, info.size_bytes,
			   info.dual_rank ? " dual rank" : "");

	if (!ret) {
		seq_printf(s, "row bits    : %u\n", info.row_bits);
		seq_printf(s, "bank bits   : %u\n", info.bank_bits);
		seq_printf(s, "column bits : %u (CONF1 MATYPE), %u (EMI_CONA)\n",
			   info.column_bits, info.emi_col_bits);
		seq_printf(s, "rank width  : %u bits (EMI_CONA DW32_EN)\n",
			   info.ram_width);
		/*
		 * The size above is computed from the DRAMC-side bus width,
		 * not from DW32_EN, and the two disagree on this board
		 * (64 vs 32).  Which one is the per-rank width could not be
		 * settled from the sources available: downstream mt_emi.c
		 * annotates "one rank: 0x0000A3AE. dual rank = 0x0002A3AE"
		 * (so dual is 2x single), while its other annotation
		 * "dw32_en=1 ... 2Gb x16 + 2Gb x16" reads as two 16-bit
		 * ranks in one 32-bit bus - the first implies DW32_EN is a
		 * per-rank width, the second that it is the aggregate.
		 * Flagging the disagreement rather than silently picking one.
		 */
		if (info.ram_width != info.bus_width)
			seq_printf(s, "WARNING: size uses bus width %u, not rank width %u\n",
				   info.bus_width, info.ram_width);
		/*
		 * A mismatch here is not cosmetic: EMI decodes the address and
		 * DRAMC drives the pins, so if they disagree the controller
		 * and the DRAM part are configured for different geometry.
		 */
		if (info.column_bits != info.emi_col_bits)
			seq_puts(s, "WARNING: CONF1 MATYPE and EMI_CONA COL disagree\n");
	}

	seq_puts(s, "\ntiming, in DRAMC clock cycles (datasheet p. 652, 653, 654)\n");
	seq_printf(s, "  tRCD  %u  tRP %u  tRC %u  tRAS %u\n",
		   info.trcd, info.trp, info.trc, info.tras);
	seq_printf(s, "  tFAW  %u  tWR %u  tWTR %u  tRRD %u\n",
		   info.tfaw, info.twr, info.twtr, info.trrd);
	seq_printf(s, "  refresh: REFCNT %u -> period %u DRAMC cycles, REFTHD %u\n",
		   info.refcnt, info.refcnt * 16, info.refthd);

	/*
	 * These fields are counts, not times: converting needs the DRAMC
	 * clock and the FDIV2 (DDR2CTL[0]) relationship, and the clock this
	 * driver can see is mempll_mck_d4, which is not the DRAMC clock when
	 * FDIV2 divides it.  Reporting the counts is the honest option.
	 */
	seq_printf(s, "  DDR2CTL 0x%08x (FDIV2 bit0 = %u)\n",
		   mt6589_dramc_read(dc, MT6589_DRAMC_WIN_AO, DRAMC_DDR2CTL),
		   mt6589_dramc_read(dc, MT6589_DRAMC_WIN_AO, DRAMC_DDR2CTL) & 1);

	seq_puts(s, "\nread-only: this driver never writes these registers.\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6589_dramc);

static int mt6589_dramc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6589_dramc *dc;
	int i;

	dc = devm_kzalloc(dev, sizeof(*dc), GFP_KERNEL);
	if (!dc)
		return -ENOMEM;

	dc->dev = dev;

	/*
	 * The bindings list the three windows in AO, NAO, DDRPHY order.
	 * NAO and DDRPHY are optional so that a board can expose timings
	 * without granting access to the window that holds MEMPLL.
	 */
	for (i = 0; i < MT6589_DRAMC_NR_WINDOWS; i++) {
		dc->base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(dc->base[i])) {
			int err = PTR_ERR(dc->base[i]);

			/*
			 * The DDRPHY window (0x1001_1000) is also claimed by
			 * the MEMPLL clock provider, which maps the same page
			 * to reach MEMPLL0..12 at +0x600.  Both go through
			 * devm_platform_ioremap_resource(), which reserves the
			 * region, so whichever probes second gets -EBUSY -
			 * not -ENXIO.  The window is optional here (nothing
			 * but the pad/DLL dump needs it), so treat a busy
			 * region like an absent one rather than failing the
			 * whole controller probe over it.  The reverse order
			 * is fine: the clock driver has no such fallback, so
			 * MEMPLL must win, and it is registered from DT as an
			 * always-on provider.
			 *
			 * Note this is only safe because the region really is
			 * shared read-only: neither driver writes to it.
			 */
			if (i > 0 && (err == -ENXIO || err == -EBUSY)) {
				dc->base[i] = NULL;
				continue;
			}
			return dev_err_probe(dev, err,
					     "failed to map window %s\n",
					     mt6589_dramc_window_name[i]);
		}
	}

	/* Optional: only informational, so a missing clock is not fatal. */
	dc->clk = devm_clk_get_optional_enabled(dev, "ddr");
	if (IS_ERR(dc->clk))
		return dev_err_probe(dev, PTR_ERR(dc->clk), "failed to get ddr clock\n");
	if (dc->clk)
		dc->clk_rate = clk_get_rate(dc->clk);

	/*
	 * Optional EMI window, needed only for the address-mapping geometry
	 * (EMI_CONA).  It is borrowed from the EMI driver rather than mapped
	 * again from a second reg entry, so that 0x1020_3000 has exactly one
	 * owner.  It is optional: without it the raw timing registers are
	 * still readable, which is the more useful half.
	 */
	if (of_find_property(dev->of_node, "mediatek,emi", NULL))
		dc->emi = mt6589_emi_iomem();

	platform_set_drvdata(pdev, dc);

	dc->debugfs = debugfs_create_file("dramc", 0444, NULL, dc,
					  &mt6589_dramc_fops);

	dev_info(dev, "%s DRAM, mempll_mck_d4 %lu Hz (read-only)\n",
		 mt6589_dramc_read(dc, MT6589_DRAMC_WIN_AO,
				   DRAMC_LPDDR2) & LPDDR2_LPDDR2EN ?
		 "LPDDR2" : "DDR3/mDDR", dc->clk_rate);

	return 0;
}

static void mt6589_dramc_remove(struct platform_device *pdev)
{
	struct mt6589_dramc *dc = platform_get_drvdata(pdev);

	debugfs_remove(dc->debugfs);
}

static const struct of_device_id mt6589_dramc_of_match[] = {
	{ .compatible = "mediatek,mt6589-dramc" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6589_dramc_of_match);

static struct platform_driver mt6589_dramc_driver = {
	.probe	= mt6589_dramc_probe,
	.remove	= mt6589_dramc_remove,
	.driver	= {
		.name		= "mt6589-dramc",
		.of_match_table	= mt6589_dramc_of_match,
	},
};
module_platform_driver(mt6589_dramc_driver);

MODULE_DESCRIPTION("MediaTek MT6589 DRAMC / DDRPHY (read-only)");
MODULE_LICENSE("GPL");
