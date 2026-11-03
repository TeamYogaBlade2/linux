// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6589 MEMPLL clock.
 *
 * WHERE MEMPLL LIVES -- verified, because it is easy to get wrong twice.
 *
 *  - The MEMPLL registers are inside the ddrphy_conf module (base
 *    +0x1001_1000h): MEMPLL0..MEMPLL12 at 0x1001_1600..0x1001_1630 and
 *    MEMPLL_DIVIDER at 0x1001_1640.  Datasheet 13.4.3 "DDRPHY Registers",
 *    module register list p. 708, per-register detail p. 729-741.
 *    MPLLPON0..3, the power-on debounce sequence registers, are at
 *    0x1001_15C0..0x1001_15CC (p. 708, detail p. 729f).
 *  - The downstream tree agrees.  mt_reg_base.h:13 DDRPHY_BASE is
 *    0xF0011000, and the preloader writes MEMPLL through
 *    DRAMC_WRITE_REG(val, (0x0180 << 2)) in
 *    preloader/src/drivers/mtk_pll.c -- 0xF0011000 + 0x600 = 0xF0011600
 *    = 0x1001_1600, i.e. MEMPLL0.  Register n is at offset 0x4 * n.
 *  - MEMPLL is NOT in apmixedsys.  The apmixedsys register list
 *    (datasheet p. 1349, 0x1020_9000..) has AP_PLL_CON0/1/2, PLL_HP_CON0,
 *    ARMPLL, MAINPLL, UNIVPLL, MMPLL, ISPPLL, MSDCPLL, TVDPLL and LVDSPLL
 *    and no MEMPLL block at all.  That is why
 *    clk-mt6589-apmixedsys.c cannot register it and registers MSDCPLL,
 *    TVDPLL and LVDSPLL as frequency-hopping channels 3, 4 and 5.
 *
 *    The "CH2" in that file refers to the frequency-hopping channel, not
 *    to the PLL: PLL_HP_CON0 at apmixedsys + 0x14 has MEMPLL_HP_EN at bit
 *    2 (datasheet p. 1349), and the downstream tree uses
 *    pll_id 2 for "MEMPLL default SSC enable" in
 *    kernel/core/mt_freqhopping.c:89.  Hopping control and the PLL itself
 *    are two different register windows, which is the whole trap here.
 *
 * WHY THIS DRIVER DOES NOT COMPUTE THE RATE FROM THE REGISTERS.
 *
 *  The obvious formula -- XTAL times (FBDIV + 1), divided by PREDIV and
 *  POSDIV -- does not hold on this block, and adopting it would put a
 *  confidently wrong number into the clock framework.  The verified
 *  reasons, in increasing order of how much they cost to discover:
 *
 *  1. MEMPLL0[24:18] is documented as "Feedback divide ratio of MEMPLL"
 *     (p. 730), but it is not PLL1's feedback divider.  The preloader
 *     writes MEMPLL0 with PREDIV/FBKSEL/POSDIV only and puts the feedback
 *     dividers for PLL2/3/4 in MEMPLL2/3/4 -- see the
 *     pllc1_prediv_1_0_n1 | pllc1_blp | pllc1_fbksel_1_0_n3 |
 *     pllc1_postdiv_1_0_n5 write to (0x0180 << 2) followed by
 *     mempll2_fbdiv_6_0 going to (0x0182 << 2) in mtk_pll.c.  So reading
 *     MEMPLL0's FBDIV field reads a field nothing programs.
 *
 *  2. The reference clock is not necessarily the crystal.
 *     MEMPLL1[12:11] RG_MEMPLL_REFCK_SEL selects 00 = Xtal clock or
 *     01 = "APLL + div (close loop)" (p. 730), and MEMPLL1[24]
 *     RG_MEMPLL_DIV_EN enables a divider for the "(APLL+DDS)" path.  With
 *     nothing in the register set saying which PLL or which divider, the
 *     multiplier is unknown.
 *
 *  3. The downstream frequency tables are not invertible.  In
 *     mtk_pll.c the DDS PCW for DDR300, DDR500, DDR800 and DDR1000 is the
 *     same value (0x00d89d89 << 1, 0x4e << 25), so the PCW in MEMPLL9
 *     cannot identify the frequency; and that source is guarded by
 *     #ifdef MEMPLL_CLK_* macros that no board in this tree defines, so
 *     the compiled configuration is itself unknown.
 *
 *  Therefore the rate is DECLARED by the board, not derived, and the
 *  default is the one value the datasheet states outright: 266.5 MHz.
 *  Datasheet p. 402, clk_mem_sel and clk_ddrphycfg_sel describe their
 *  clock as "(Maximum id 266.5MHz)" and list "MEMPLL_MCK_D4 (266.5/4)";
 *  the SoC summary gives "Memory clock up to 533MHz" and "LPDDR2 533MHz"
 *  / "DDR3 @ 533MHz" (p. 27, and the EMI chapter's 1066 Mbps per bit).
 *  MEMPLL_MCK 266.5 MHz with DDR data at 533 Mbps is consistent with all
 *  of that.
 *
 *  What this driver deliberately does NOT do is check the declared rate
 *  against the registers.  Attempting that from the fields available is
 *  how claim 1 above goes wrong: MEMPLL0[27:26] FBKSEL reads 1 (the /2
 *  setting in the field table) for the state the preloader leaves behind,
 *  while the same preloader's 266 MHz path writes 0xc0000000 into
 *  MEMPLL0, i.e. bits 27:26 = 11, an encoding the datasheet does not
 *  document for that field.  A rate decoder built on that table would be
 *  wrong by a factor of two on the reference case.
 *
 *  A board that runs its DRAM at another rate must say so with
 *  clock-frequency; nothing here guesses on its behalf.
 *
 * WHAT THE DRIVER DOES provide is the part that IS readable: enable state
 * (MEMPLL0[2] plus MEMPLL_DIVIDER[4]) and a debugfs dump of every field
 * the datasheet documents, so that the declared rate can be falsified on
 * real hardware rather than trusted.
 */
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

/* Offsets inside the ddrphy_conf window, datasheet p. 708. */
#define MEMPLL_CON(n)		(0x600 + 0x4 * (n))	/* 0x1001_1600 + 4n */
/* MPLLPON0..3, the power-on debounce sequence, are at 0x5c0..0x5cc.  They */
/* are documented (p. 708, p. 729f) but never read here: the sequence has   */
/* already run by the time Linux boots, and nothing in it is a clock field. */
#define MPLLPON_CON(n)		(0x5c0 + 0x4 * (n))	/* 0x1001_15C0 + 4n */
#define MEMPLL_DIVIDER		0x640			/* 0x1001_1640 */

/* MEMPLL0 -- MEMPLL Register Setting 0, datasheet p. 729f. */
#define MEMPLL0_FBKSEL		GENMASK(27, 26)
#define MEMPLL0_FBDIV		GENMASK(24, 18)
#define MEMPLL0_PREDIV		GENMASK(17, 16)
#define MEMPLL0_POSDIV		GENMASK(15, 14)
#define MEMPLL0_VCO_DIV_SEL	BIT(13)
#define MEMPLL0_MONCK_EN	BIT(5)
#define MEMPLL0_EN		BIT(2)

/* MEMPLL1 -- MEMPLL Register Setting 1, datasheet p. 730. */
#define MEMPLL1_DIV		GENMASK(31, 25)
#define MEMPLL1_DIV_EN		BIT(24)
#define MEMPLL1_BIAS_LPF_EN	BIT(15)
#define MEMPLL1_BIAS_EN		BIT(14)
#define MEMPLL1_REFCK_EN	BIT(13)
#define MEMPLL1_REFCK_SEL	GENMASK(12, 11)

/* MEMPLL_DIVIDER, datasheet p. 741. */
#define MEMPLL_DIV_ALL_CK_EN	BIT(4)		/* R_DMALLCLK_EN */
#define MEMPLL_DIV_1PLL_SYNC	BIT(2)		/* R_DM1PLL_SYNC_MODE */
#define MEMPLL_DIV_BYP_PLL3	BIT(1)		/* R_DMBY_PLL3 */
#define MEMPLL_DIV_BYP_PLL4	BIT(0)		/* R_DMBY_PLL4 */

static const char * const mempll_refck_sel_name[] = {
	"xtal", "apll+div", "reserved", "reserved",
};

struct mt6589_mempll {
	void __iomem *base;
	struct clk_hw hw;
	struct clk *clk;
	unsigned long rate;
	struct dentry *debugfs;
};

static inline struct mt6589_mempll *hw_to_mempll(struct clk_hw *hw)
{
	return container_of(hw, struct mt6589_mempll, hw);
}

static int mt6589_mempll_is_enabled(struct clk_hw *hw)
{
	struct mt6589_mempll *mempll = hw_to_mempll(hw);

	/*
	 * MEMPLL0[2] RG_MEMPLL_EN enables the PLL, but the output only
	 * reaches the SoC once MEMPLL_DIVIDER[4] R_DMALLCLK_EN is set --
	 * datasheet p. 741, "enables all output clocks of MEMPLL divider,
	 * except for feedback clocks to MEMPLL".  Both must be up, and
	 * reporting the first alone would claim a clock that is not
	 * reaching anything.
	 */
	if (!(readl(mempll->base + MEMPLL_CON(0)) & MEMPLL0_EN))
		return 0;

	return !!(readl(mempll->base + MEMPLL_DIVIDER) & MEMPLL_DIV_ALL_CK_EN);
}

static unsigned long mt6589_mempll_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	return hw_to_mempll(hw)->rate;
}

static const struct clk_ops mt6589_mempll_ops = {
	.is_enabled	= mt6589_mempll_is_enabled,
	.recalc_rate	= mt6589_mempll_recalc_rate,
};

static int mt6589_mempll_dump_show(struct seq_file *s, void *unused)
{
	struct mt6589_mempll *mempll = s->private;
	u32 con0, con1, divider;
	unsigned int i;

	con0 = readl(mempll->base + MEMPLL_CON(0));
	con1 = readl(mempll->base + MEMPLL_CON(1));
	divider = readl(mempll->base + MEMPLL_DIVIDER);

	seq_printf(s, "declared rate      : %lu Hz\n", mempll->rate);
	seq_printf(s, "MEMPLL0            : 0x%08x\n", con0);
	seq_printf(s, "  EN               : %u\n", !!(con0 & MEMPLL0_EN));
	seq_printf(s, "  PREDIV           : %lu\n",
		   FIELD_GET(MEMPLL0_PREDIV, con0));
	seq_printf(s, "  POSDIV           : %lu\n",
		   FIELD_GET(MEMPLL0_POSDIV, con0));
	seq_printf(s, "  VCO_DIV_SEL      : %u\n",
		   !!(con0 & MEMPLL0_VCO_DIV_SEL));
	seq_printf(s, "  FBKSEL           : %lu\n",
		   FIELD_GET(MEMPLL0_FBKSEL, con0));
	seq_printf(s, "  FBDIV            : %lu (not PLL1's divider, see header)\n",
		   FIELD_GET(MEMPLL0_FBDIV, con0));
	seq_printf(s, "  MONCK_EN         : %u\n",
		   !!(con0 & MEMPLL0_MONCK_EN));
	seq_printf(s, "MEMPLL1            : 0x%08x\n", con1);
	seq_printf(s, "  REFCK_SEL        : %lu (%s)\n",
		   FIELD_GET(MEMPLL1_REFCK_SEL, con1),
		   mempll_refck_sel_name[FIELD_GET(MEMPLL1_REFCK_SEL, con1)]);
	seq_printf(s, "  REFCK_EN         : %u\n",
		   !!(con1 & MEMPLL1_REFCK_EN));
	seq_printf(s, "  BIAS_EN          : %u\n", !!(con1 & MEMPLL1_BIAS_EN));
	seq_printf(s, "  BIAS_LPF_EN      : %u\n",
		   !!(con1 & MEMPLL1_BIAS_LPF_EN));
	seq_printf(s, "  DIV_EN           : %u\n", !!(con1 & MEMPLL1_DIV_EN));
	seq_printf(s, "  DIV              : %lu\n", FIELD_GET(MEMPLL1_DIV, con1));
	seq_printf(s, "MEMPLL_DIVIDER     : 0x%08x\n", divider);
	seq_printf(s, "  ALL_CK_EN        : %u\n",
		   !!(divider & MEMPLL_DIV_ALL_CK_EN));
	seq_printf(s, "  1PLL_SYNC_MODE   : %u\n",
		   !!(divider & MEMPLL_DIV_1PLL_SYNC));
	seq_printf(s, "  BYP_PLL3         : %u\n",
		   !!(divider & MEMPLL_DIV_BYP_PLL3));
	seq_printf(s, "  BYP_PLL4         : %u\n",
		   !!(divider & MEMPLL_DIV_BYP_PLL4));

	seq_puts(s, "\nper-PLL feedback dividers (MEMPLL2/3/4, p. 731f)\n");
	for (i = 2; i <= 4; i++) {
		u32 con = readl(mempll->base + MEMPLL_CON(i));

		seq_printf(s, "  MEMPLL%u          : 0x%08x FBDIV %lu PREDIV %lu\n",
			   i, con, FIELD_GET(GENMASK(8, 2), con),
			   FIELD_GET(GENMASK(1, 0), con));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6589_mempll_dump);

static int mt6589_mempll_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct mt6589_mempll *mempll;
	struct resource *res;
	u32 rate;
	int ret;

	mempll = devm_kzalloc(dev, sizeof(*mempll), GFP_KERNEL);
	if (!mempll)
		return -ENOMEM;

	/*
	 * Map the ddrphy_conf page without reserving it.  The same
	 * 0x1001_1000 page is also described by the DRAMC node (as its
	 * "ddrphy" window), and this driver only reads MEMPLL0..12 at +0x600
	 * to report them through debugfs - it never writes, and it does not
	 * program the PLL.  Reserving the region would make one of the two
	 * drivers lose to the other purely on probe order: this clock must
	 * win, because topckgen's clkph_mck hangs off it and mempll_mck_d4
	 * would report 0 Hz without it.  So take a plain devm_ioremap() over
	 * the first reg entry and leave region ownership to DRAMC, which
	 * tolerates -EBUSY on this window.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -ENODEV,
				     "no ddrphy_conf reg entry\n");
	mempll->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!mempll->base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map ddrphy_conf window\n");

	/*
	 * 266.5 MHz is the datasheet's own figure for this clock
	 * (clk_mem_sel/clk_ddrphycfg_sel "Maximum id 266.5MHz", and
	 * "MEMPLL_MCK_D4 (266.5/4)", p. 402).  See the header for why it is
	 * declared instead of decoded from MEMPLL0.
	 */
	rate = 266500000;
	ret = of_property_read_u32(dev->of_node, "clock-frequency", &rate);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret, "invalid clock-frequency\n");
	if (!rate)
		return dev_err_probe(dev, -EINVAL, "clock-frequency must not be zero\n");

	mempll->rate = rate;

	init.name = "mempll";
	init.ops = &mt6589_mempll_ops;
	init.flags = CLK_GET_RATE_NOCACHE;

	mempll->hw.init = &init;

	mempll->clk = devm_clk_register(dev, &mempll->hw);
	if (IS_ERR(mempll->clk))
		return dev_err_probe(dev, PTR_ERR(mempll->clk),
				     "failed to register MEMPLL\n");

	ret = of_clk_add_provider(dev->of_node, of_clk_src_simple_get,
				  mempll->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add clk provider\n");

	platform_set_drvdata(pdev, mempll);

	mempll->debugfs = debugfs_create_file("mempll", 0444, NULL, mempll,
					      &mt6589_mempll_dump_fops);

	dev_info(dev, "MEMPLL %lu Hz, enabled %u (read-only)\n",
		 mempll->rate, mt6589_mempll_is_enabled(&mempll->hw));

	return 0;
}

static void mt6589_mempll_remove(struct platform_device *pdev)
{
	struct mt6589_mempll *mempll = platform_get_drvdata(pdev);

	debugfs_remove(mempll->debugfs);
	of_clk_del_provider(pdev->dev.of_node);
}

static const struct of_device_id mt6589_mempll_of_match[] = {
	{ .compatible = "mediatek,mt6589-mempll" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6589_mempll_of_match);

static struct platform_driver mt6589_mempll_drv = {
	.probe	= mt6589_mempll_probe,
	.remove	= mt6589_mempll_remove,
	.driver	= {
		.name		= "mt6589-mempll",
		.of_match_table	= mt6589_mempll_of_match,
	},
};
module_platform_driver(mt6589_mempll_drv);

MODULE_DESCRIPTION("MediaTek MT6589 MEMPLL clock");
MODULE_LICENSE("GPL");
