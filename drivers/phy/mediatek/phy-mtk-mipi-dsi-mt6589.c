// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 MIPI D-PHY driver
 *
 * Copyright (c) 2026 MediaTek Inc.
 * Based on downstream mt6589 dsi_drv.c
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>

#include "phy-mtk-io.h"
#include "phy-mtk-mipi-dsi.h"

/* MT6589 MIPI TX PHY registers (offsets relative to PHY base) */
#define MIPITX_DSI_CON			0x00
#define MIPITX_DSI_LANE0		0x04
#define MIPITX_DSI_LANE1		0x08
#define MIPITX_DSI_LANE2		0x0c
#define MIPITX_DSI_LANE3		0x10
#define MIPITX_DSI_CLK_LANE		0x14
#define MIPITX_DSI_TOP_CON		0x40
#define MIPITX_DSI_BG_CON		0x44
#define MIPITX_DSI_PLL_CON0		0x50
#define MIPITX_DSI_PLL_PWR		0x60
#define MIPITX_DSI_SW_CTRL		0x80
#define MIPITX_DSI_SW_CTRL_CON0		0x84
#define MIPITX_DSI_SW_CTRL_CON1		0x88

/* PLL configuration entry: raw register values from downstream pll_config[] */
struct mt6589_pll_config {
	u8 txdiv0;	/* TXDIV0 field (2 bits) */
	u8 txdiv1;	/* TXDIV1 field (2 bits) */
	u8 fbk_sel;	/* FBK_SEL  (2 bits) */
	u8 fbk_div;	/* FBK_DIV  (7 bits) */
	u8 pre_div;	/* PRE_DIV  (2 bits) */
	u8 rg_br;	/* RG_BR    (2 bits) */
	u8 rg_bc;	/* RG_BC    (2 bits) */
	u8 rg_bir;	/* RG_BIR   (4 bits) */
	u8 rg_bic;	/* RG_BIC   (4 bits) */
	u8 rg_bp;	/* RG_BP    (4 bits) */
};

/*
 * PLL configuration table (50 entries) taken from MT6589 downstream.
 * Each entry corresponds to a pre‑defined output frequency.
 */
static const struct mt6589_pll_config pll_config[] = {
	{1,1,1,0x1E,1,2,3,8,4,0xC},
	{1,1,1,0x0F,0,2,2,4,1,0xC},
	{1,1,1,0x20,1,2,3,9,4,0xC},
	{1,1,1,0x10,0,2,2,4,1,0xC},
	{1,1,1,0x22,1,2,3,9,4,0xC},
	{1,1,1,0x11,0,2,2,4,1,0xC},
	{1,1,1,0x24,1,2,3,0xA,5,0xC},
	{1,1,1,0x12,0,2,2,5,2,0xC},
	{1,1,1,0x26,1,2,3,0xA,4,0xC},
	{1,1,1,0x13,0,2,2,5,2,0xC},
	{1,1,1,0x28,1,2,3,0xA,4,0xC},
	{1,1,1,0x14,0,2,2,5,2,0xC},
	{1,1,1,0x2A,1,2,3,0xB,5,0xC},
	{1,1,1,0x15,0,2,2,6,2,0xC},
	{1,1,1,0x2C,1,2,3,0xB,5,0xC},
	{1,1,1,0x16,0,2,2,6,2,0xC},
	{1,1,1,0x2E,1,2,3,0xC,5,0xC},
	{1,1,1,0x17,0,2,2,6,2,0xC},
	{1,1,1,0x30,1,2,3,0xD,5,0xC},
	{1,1,1,0x18,0,2,2,6,2,0xC},
	{1,1,1,0x32,1,2,3,0xE,6,0xC},
	{1,1,1,0x19,0,2,2,6,2,0xC},
	{1,1,1,0x34,1,1,3,0x7,6,0x6},
	{1,1,1,0x1A,0,2,2,7,2,0xC},
	{1,1,1,0x36,1,1,3,0x7,7,0x6},
	{1,0,0,0x1B,0,2,2,5,2,0x8},
	{1,0,0,0x38,1,2,2,0xA,5,0x8},
	{1,0,1,0x1C,1,2,2,0xA,5,0x8},
	{1,0,1,0x3A,3,1,3,5,3,0x8},
	{1,0,1,0x0E,0,2,2,5,2,0x9},
	{1,0,1,0x3C,3,1,3,5,3,0x8},
	{1,0,1,0x1E,1,1,2,5,5,0x4},
	{1,0,1,0x3E,3,1,3,5,3,0x8},
	{1,0,1,0x0F,0,2,2,5,2,0xA},
	{1,0,1,0x40,3,1,3,5,3,0x8},
	{1,0,1,0x20,1,1,2,6,5,0x4},
	{1,0,1,0x42,3,1,3,6,3,0x8},
	{1,0,1,0x10,0,2,2,6,2,0xA},
	{1,0,1,0x44,3,1,3,6,3,0x8},
	{1,0,1,0x22,1,1,2,6,6,0x4},
	{1,0,1,0x46,3,1,3,6,4,0x8},
	{1,0,1,0x11,0,2,2,5,2,0xA},
	{1,0,1,0x48,3,1,3,6,4,0x8},
	{1,0,1,0x24,1,1,2,6,6,0x4},
	{1,0,1,0x4A,3,1,3,7,4,0x8},
	{1,0,1,0x12,0,2,2,5,2,0xC},
	{1,0,1,0x4C,3,1,3,7,4,0x8},
	{1,0,1,0x26,1,1,2,7,6,0x4},
	{1,0,1,0x4E,3,1,3,7,4,0x8},
	{1,0,1,0x13,0,2,2,5,2,0xC},
};

/* Convert raw divider field to actual division factor */
static u8 div_factor(u8 raw)
{
	switch (raw) {
	case 0: return 1;
	case 1: return 2;
	case 2:
	case 3: return 4;
	default: return 4;
	}
}

/* Calculate the MIPI lane data rate (Hz) produced by a PLL configuration */
static unsigned long mt6589_pll_rate(const struct mt6589_pll_config *cfg)
{
	unsigned int fbk = cfg->fbk_div + 1;
	unsigned int fbk_sel = div_factor(cfg->fbk_sel);
	unsigned int pre = div_factor(cfg->pre_div);
	unsigned int div1 = div_factor(cfg->txdiv0);
	unsigned int div2 = div_factor(cfg->txdiv1);
	unsigned long rate;

	/* Lane data rate (Mbps) = 26 * fbk * fbk_sel * 2 / (8 * div1 * div2 * pre) */
	rate = 26 * fbk * fbk_sel * 2;
	rate /= 8 * div1 * div2 * pre;
	/* Convert to Hz */
	return rate * 1000000UL;
}

/* Find the table entry whose output rate is closest to the target */
static const struct mt6589_pll_config *mt6589_pll_find_closest(unsigned long rate_hz)
{
	unsigned long best_diff = ULONG_MAX;
	const struct mt6589_pll_config *best = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(pll_config); i++) {
		unsigned long r = mt6589_pll_rate(&pll_config[i]);
		long diff = abs((long)(r - rate_hz));

		if (diff < best_diff) {
			best_diff = diff;
			best = &pll_config[i];
		}
	}
	return best;
}

/* Program the PLL dividers and analog tuning values */
static void mt6589_mipi_tx_set_pll(struct mtk_mipi_tx *mipi_tx,
				   const struct mt6589_pll_config *cfg)
{
	void __iomem *base = mipi_tx->regs;
	u32 val;

	/* Assemble PLL_CON0 value without PLL_EN (bit0) */
	val = (cfg->rg_br  << 30) |
	      (cfg->rg_bc  << 28) |
	      (cfg->rg_bp  << 24) |
	      (cfg->rg_bir << 20) |
	      (cfg->rg_bic << 16) |
	      (cfg->txdiv1 << 14) |
	      (cfg->txdiv0 << 12) |
	      (cfg->fbk_sel << 10) |
	      (cfg->pre_div <<  8) |
	      (cfg->fbk_div <<  1);

	writel(val, base + MIPITX_DSI_PLL_CON0);
	/* Now enable the PLL */
	val |= BIT(0);
	writel(val, base + MIPITX_DSI_PLL_CON0);
}

/* --- Clock operations for the PLL --- */

static int mt6589_mipi_tx_pll_prepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;
	const struct mt6589_pll_config *cfg;

	cfg = mt6589_pll_find_closest(mipi_tx->data_rate);
	if (!cfg)
		return -EINVAL;

	dev_dbg(mipi_tx->dev, "PLL prepare: target %lu Hz, selected entry rate %lu Hz\n",
		(unsigned long)mipi_tx->data_rate,
		(unsigned long)mt6589_pll_rate(cfg));

	/* Step 1: Enable bandgap and LDO core */
	writel(0x3, base + MIPITX_DSI_BG_CON);	/* BG_CORE_EN | BG_CKEN */
	writel(0x3, base + MIPITX_DSI_CON);	/* LDOCORE_EN | CKG_LDOOUT_EN */
	usleep_range(30, 100);

	/* Step 2: PLL power-up sequence (mimic downstream 0x400 -> PLL setup -> 0x600) */
	writel(0x400, base + MIPITX_DSI_PLL_PWR);
	mt6589_mipi_tx_set_pll(mipi_tx, cfg);
	usleep_range(20, 100);
	writel(0x600, base + MIPITX_DSI_PLL_PWR);
	usleep_range(20, 100);

	return 0;
}

static void mt6589_mipi_tx_pll_unprepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;

	dev_dbg(mipi_tx->dev, "PLL unprepare\n");

	/* Restore default divider settings (mask 0xF0FE, value 0x26) */
	mtk_phy_update_bits(base + MIPITX_DSI_PLL_CON0, 0xF0FE, 0x26);
	/* Disable PLL */
	mtk_phy_clear_bits(base + MIPITX_DSI_PLL_CON0, BIT(0));

	/* Power down analog blocks */
	writel(0, base + MIPITX_DSI_PLL_PWR);
	writel(0, base + MIPITX_DSI_CON);
	writel(0, base + MIPITX_DSI_BG_CON);
}

static int mt6589_mipi_tx_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				       unsigned long parent_rate)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);

	dev_dbg(mipi_tx->dev, "set rate: %lu Hz\n", rate);
	mipi_tx->data_rate = rate;
	return 0;
}

static unsigned long mt6589_mipi_tx_pll_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	return mipi_tx->data_rate;
}

static int mt6589_mipi_tx_pll_determine_rate(struct clk_hw *hw,
					     struct clk_rate_request *req)
{
	req->rate = clamp_val(req->rate, 50000000, 1250000000);
	return 0;
}

static const struct clk_ops mt6589_mipi_tx_pll_ops = {
	.prepare = mt6589_mipi_tx_pll_prepare,
	.unprepare = mt6589_mipi_tx_pll_unprepare,
	.set_rate = mt6589_mipi_tx_pll_set_rate,
	.recalc_rate = mt6589_mipi_tx_pll_recalc_rate,
	.determine_rate = mt6589_mipi_tx_pll_determine_rate,
};

/* --- PHY signal enable / disable --- */

static void mt6589_mipi_tx_power_on_signal(struct phy *phy)
{
	struct mtk_mipi_tx *mipi_tx = phy_get_drvdata(phy);
	void __iomem *base = mipi_tx->regs;

	/* Enable software control over PHY lanes */
	writel(1, base + MIPITX_DSI_SW_CTRL);

	/* Enable LDO output on all data lanes and clock lane */
	writel(1, base + MIPITX_DSI_LANE0);
	writel(1, base + MIPITX_DSI_LANE1);
	writel(1, base + MIPITX_DSI_LANE2);
	writel(1, base + MIPITX_DSI_LANE3);
	writel(1, base + MIPITX_DSI_CLK_LANE);

	/* Enable HS bias */
	writel(0x2, base + MIPITX_DSI_TOP_CON);	/* BIT(1) */
	usleep_range(20, 100);

	/* Clear an unknown TOP_CON bit (BIT(11) in downstream) */
	mtk_phy_clear_bits(base + MIPITX_DSI_TOP_CON, BIT(11));

	/* Set DP/DN to mark-1 state: first 0x200, then 0x600 */
	writel(0x200, base + MIPITX_DSI_SW_CTRL_CON0);
	usleep_range(20, 100);
	writel(0x600, base + MIPITX_DSI_SW_CTRL_CON0);

	/* Return control to DSI host */
	writel(0, base + MIPITX_DSI_SW_CTRL);
}

static void mt6589_mipi_tx_power_off_signal(struct phy *phy)
{
	struct mtk_mipi_tx *mipi_tx = phy_get_drvdata(phy);
	void __iomem *base = mipi_tx->regs;

	/* Take software control */
	writel(1, base + MIPITX_DSI_SW_CTRL);

	/* Disable all lane LDOs */
	writel(0, base + MIPITX_DSI_LANE0);
	writel(0, base + MIPITX_DSI_LANE1);
	writel(0, base + MIPITX_DSI_LANE2);
	writel(0, base + MIPITX_DSI_LANE3);
	writel(0, base + MIPITX_DSI_CLK_LANE);

	/* Disable HS bias */
	writel(0, base + MIPITX_DSI_TOP_CON);

	/* Clear SW control registers (including HS TX data ready / data) */
	writel(0, base + MIPITX_DSI_SW_CTRL_CON0);
	writel(0, base + MIPITX_DSI_SW_CTRL_CON1);

	/* Power down PLL block */
	writel(0, base + MIPITX_DSI_PLL_PWR);

	/* Release software control */
	writel(0, base + MIPITX_DSI_SW_CTRL);
}

/* Platform data exported for binding */
const struct mtk_mipitx_data mt6589_mipitx_data = {
	.mppll_preserve = 0, /* not required? */
	.mipi_tx_clk_ops = &mt6589_mipi_tx_pll_ops,
	.mipi_tx_enable_signal = mt6589_mipi_tx_power_on_signal,
	.mipi_tx_disable_signal = mt6589_mipi_tx_power_off_signal,
};
EXPORT_SYMBOL_GPL(mt6589_mipitx_data);

MODULE_DESCRIPTION("MediaTek MT6589 MIPI TX PHY driver");
MODULE_LICENSE("GPL");
