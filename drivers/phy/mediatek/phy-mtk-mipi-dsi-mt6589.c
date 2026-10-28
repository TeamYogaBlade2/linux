// SPDX-License-Identifier: GPL-2.0
/*
 * MT6589 MIPI TX (DSI D-PHY) PLL.
 *
 * The lane/top/bandgap registers match MT8173, but the MPPLL is different:
 * MIPITX_PLL_CON0 (0x50) carries the whole divider set (vendor Lenovo 3.4
 * kernel, mediatek/platform/mt6589/kernel/drivers/video/dsi_drv.c,
 * DSI_PHY_clk_setting()):
 *   [0] PLL_EN, [7:1] FBK_DIV, [9:8] PRE_DIV, [11:10] FBK_SEL,
 *   [13:12] TXDIV0, [15:14] TXDIV1, [31:16] loop filter (BIC/BIR/BP/BC/BR)
 * data rate = 26 MHz * (FBK_DIV + 1) * FBK_SEL * 2 / (TXDIV0 * TXDIV1 * PRE_DIV)
 * with the 2-bit codes 0/1/2 meaning x1/x2/x4.
 */

#include <linux/bitfield.h>
#include <linux/math.h>

#include "phy-mtk-io.h"
#include "phy-mtk-mipi-dsi.h"

#define MIPITX_DSI_CON		0x00
#define MIPITX_DSI_CLOCK_LANE	0x04
#define MIPITX_DSI_DATA_LANE3	0x14
#define MIPITX_DSI_TOP_CON	0x40
#define MIPITX_DSI_BG_CON	0x44
#define MIPITX_DSI_PLL_CON0	0x50
#define MIPITX_DSI_PLL_CHG	0x60
#define MIPITX_DSI_SW_CTRL	0x80
#define MIPITX_DSI_SW_CTRL_CON0	0x84

#define MT6589_PLL_EN		BIT(0)
#define MT6589_FBK_DIV		GENMASK(7, 1)
#define MT6589_PRE_DIV		GENMASK(9, 8)
#define MT6589_FBK_SEL		GENMASK(11, 10)
#define MT6589_TXDIV0		GENMASK(13, 12)
#define MT6589_TXDIV1		GENMASK(15, 14)
#define MT6589_LOOP_FILTER	GENMASK(31, 16)
/* Loop filter value the bootloader programs for this SoC. */
#define MT6589_LOOP_FILTER_DEF	0xac41

static int mtk_mipi_tx_mt6589_pll_prepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;
	/* (TXDIV0, TXDIV1) codes giving x1, x2, x4, x8, x16 */
	static const u8 div_codes[][2] = { {0, 0}, {1, 0}, {1, 1}, {2, 1}, {2, 2} };
	u64 rate = mipi_tx->data_rate;
	unsigned int i, div = 1;
	u32 fbk, con0, reg;

	/* Keep the VCO (rate * post divider) around 1..2.1 GHz like the vendor. */
	for (i = 0; i < ARRAY_SIZE(div_codes); i++, div <<= 1)
		if (rate * div >= 1000000000ULL)
			break;
	if (i == ARRAY_SIZE(div_codes)) {
		i--;
		div >>= 1;
	}

	/* FBK_SEL = x2, PRE_DIV = x1: rate * div = 104 MHz * (FBK_DIV + 1) */
	fbk = DIV_ROUND_CLOSEST_ULL(rate * div, 104000000) - 1;
	if (fbk > 127)
		return -EINVAL;

	dev_info(mipi_tx->dev, "prepare: %u bps, div x%u, fbk %u -> %llu bps\n",
		 mipi_tx->data_rate, div, fbk,
		 div_u64(104000000ULL * (fbk + 1), div));

	con0 = FIELD_PREP(MT6589_LOOP_FILTER, MT6589_LOOP_FILTER_DEF) |
	       FIELD_PREP(MT6589_TXDIV1, div_codes[i][1]) |
	       FIELD_PREP(MT6589_TXDIV0, div_codes[i][0]) |
	       FIELD_PREP(MT6589_FBK_SEL, 1) |
	       FIELD_PREP(MT6589_PRE_DIV, 0) |
	       FIELD_PREP(MT6589_FBK_DIV, fbk);

	mtk_phy_set_bits(base + MIPITX_DSI_SW_CTRL, BIT(0));	/* SW mode */
	mtk_phy_set_bits(base + MIPITX_DSI_BG_CON, 0x3);
	mtk_phy_set_bits(base + MIPITX_DSI_CON, 0x3);
	usleep_range(1000, 1100);
	mtk_phy_update_bits(base + MIPITX_DSI_PLL_CHG, 0x600, 0x400);
	writel(con0, base + MIPITX_DSI_PLL_CON0);
	mtk_phy_set_bits(base + MIPITX_DSI_PLL_CON0, MT6589_PLL_EN);
	usleep_range(1000, 1100);
	mtk_phy_update_bits(base + MIPITX_DSI_PLL_CHG, 0x600, 0x600);
	usleep_range(1000, 1100);

	for (reg = MIPITX_DSI_CLOCK_LANE; reg <= MIPITX_DSI_DATA_LANE3; reg += 4)
		mtk_phy_set_bits(base + reg, BIT(0));

	mtk_phy_set_bits(base + MIPITX_DSI_TOP_CON, BIT(1));
	usleep_range(1000, 1100);
	mtk_phy_clear_bits(base + MIPITX_DSI_TOP_CON, BIT(11));

	/* DP/DN to mark-1, then hand the pads back to the DSI controller. */
	mtk_phy_update_bits(base + MIPITX_DSI_SW_CTRL_CON0, 0x600, 0x200);
	usleep_range(1000, 1100);
	mtk_phy_update_bits(base + MIPITX_DSI_SW_CTRL_CON0, 0x600, 0x600);
	mtk_phy_clear_bits(base + MIPITX_DSI_SW_CTRL, BIT(0));

	return 0;
}

static void mtk_mipi_tx_mt6589_pll_unprepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *mipi_tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = mipi_tx->regs;
	u32 reg;

	mtk_phy_clear_bits(base + MIPITX_DSI_PLL_CON0, MT6589_PLL_EN);
	for (reg = MIPITX_DSI_CLOCK_LANE; reg <= MIPITX_DSI_DATA_LANE3; reg += 4)
		mtk_phy_clear_bits(base + reg, BIT(0));
	mtk_phy_clear_bits(base + MIPITX_DSI_TOP_CON, BIT(1));
	mtk_phy_clear_bits(base + MIPITX_DSI_CON, 0x3);
	mtk_phy_clear_bits(base + MIPITX_DSI_BG_CON, 0x3);
}

static int mtk_mipi_tx_mt6589_pll_determine_rate(struct clk_hw *hw,
						 struct clk_rate_request *req)
{
	req->rate = clamp_val(req->rate, 50000000, 1000000000);

	return 0;
}

static const struct clk_ops mtk_mipi_tx_mt6589_pll_ops = {
	.prepare = mtk_mipi_tx_mt6589_pll_prepare,
	.unprepare = mtk_mipi_tx_mt6589_pll_unprepare,
	.determine_rate = mtk_mipi_tx_mt6589_pll_determine_rate,
	.set_rate = mtk_mipi_tx_pll_set_rate,
	.recalc_rate = mtk_mipi_tx_pll_recalc_rate,
};

/* Lanes are powered together with the PLL (vendor order). */
static void mtk_mipi_tx_mt6589_signal_nop(struct phy *phy)
{
}

const struct mtk_mipitx_data mt6589_mipitx_data = {
	.mppll_preserve = 0,
	.mipi_tx_clk_ops = &mtk_mipi_tx_mt6589_pll_ops,
	.mipi_tx_enable_signal = mtk_mipi_tx_mt6589_signal_nop,
	.mipi_tx_disable_signal = mtk_mipi_tx_mt6589_signal_nop,
};
