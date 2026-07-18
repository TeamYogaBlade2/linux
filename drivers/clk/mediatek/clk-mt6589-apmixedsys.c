// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * Based on clk-mt2712-apmixedsys.c
 * Copyright (c) 2017 MediaTek Inc.
 *                    Weiyi Lu <weiyi.lu@mediatek.com>
 * Copyright (c) 2023 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/delay.h>

#include "clk-pll.h"
#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt6589-clk.h>

#define AP_PLL_CON0	0x0000
#define AP_PLL_CON1	0x0004
#define AP_PLL_CON2	0x0008
#define AP_PLL_CON3	0x000c

#define PLL_HP_CON0	0x0014

#define ARMPLL_CON0	0x0200
#define ARMPLL_CON1	0x0204
#define ARMPLL_CON2	0x0208
#define ARMPLL_PWR_CON0	0x0218

#define MAINPLL_CON0	0x021c
#define MAINPLL_CON1	0x0220
#define MAINPLL_CON2	0x0224
#define MAINPLL_PWR_CON0	0x0234

#define UNIVPLL_CON0	0x0238
#define MMPLL_CON0	0x0240
#define ISPPLL_CON0	0x0248

#define MSDCPLL_CON0	0x0250
#define MSDCPLL_CON1	0x0254
#define MSDCPLL_CON2	0x0258
#define MSDCPLL_PWR_CON0	0x0268

#define TVDPLL_CON0	0x026c
#define TVDPLL_CON1	0x0270
#define TVDPLL_CON2	0x0274
#define TVDPLL_CON3	0x0278
#define TVDPLL_PWR_CON0	0x0284

#define LVDSPLL_CON0	0x0288
#define LVDSPLL_CON1	0x028c
#define LVDSPLL_CON2	0x0290
#define LVDSPLL_CON3	0x0294
#define LVDSPLL_PWR_CON0	0x02a0

#define VOID_REG	0x0

#define CON0_MT6589_RST_BAR	BIT(27)

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,	\
			_pd_reg, _pd_shift, _pcw_reg, _pcw_shift,	\
			_ops, _fmax) {					\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = CON0_MT6589_RST_BAR,			\
		.fmax = _fmax,						\
		.pcwbits = _pcwbits,					\
		.pd_reg = _pd_reg,					\
		.pd_mask = 0x3,						\
		.pd_shift = _pd_shift,					\
		.tuner_reg = VOID_REG,					\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
		.ops = _ops,						\
	}

static int mt6589_lc_pll_set_rate(struct clk_hw *hw, unsigned long rate,
                                  unsigned long parent_rate)
{
    struct mtk_clk_pll *pll = to_mtk_clk_pll(hw);
    u32 pcw = 0;
    u32 postdiv;
    u32 mask, val;

    mtk_pll_calc_values(pll, &pcw, &postdiv, rate, parent_rate);

    /* LC PLL: write directly to CON0, no PCW_CHG trigger */
    val = readl(pll->base_addr);
    /* Clear postdiv field (2 bits) */
    mask = pll->data->pd_mask ?: 0x3;
    val &= ~(mask << pll->data->pd_shift);
    /* Clear FBKDIV field (pcwbits, from pcw_shift) */
    val &= ~(GENMASK(pll->data->pcw_shift + pll->data->pcwbits - 1,
                     pll->data->pcw_shift));
    /* Set new postdiv and pcw */
    val |= ((ffs(postdiv) - 1) << pll->data->pd_shift);
    val |= (pcw << pll->data->pcw_shift);

    writel(val, pll->base_addr);
    udelay(20); /* stabilize */

    return 0;
}

static const struct clk_ops mt6589_lc_pll_ops = {
    .is_prepared	= mtk_pll_is_prepared,
    .prepare		= mtk_pll_prepare,
    .unprepare		= mtk_pll_unprepare,
    .recalc_rate	= mtk_pll_recalc_rate,
    .determine_rate	= mtk_pll_determine_rate,
    .set_rate		= mt6589_lc_pll_set_rate,
};

static const struct clk_ops mt6589_fixed_lc_pll_ops = {
    .is_prepared	= mtk_pll_is_prepared,
    .prepare		= mtk_pll_prepare,
    .unprepare		= mtk_pll_unprepare,
    .recalc_rate	= mtk_pll_recalc_rate,
    .determine_rate	= mtk_pll_determine_rate,
    /* no .set_rate */
};

static const struct mtk_pll_data plls[] = {
	PLL(CLK_APMIXED_ARMPLL, "armpll", ARMPLL_CON0, ARMPLL_PWR_CON0, 0x80000001,
		PLL_AO, 21, ARMPLL_CON1, 24, ARMPLL_CON1, 0, NULL, 1508 * MHZ),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", MAINPLL_CON0, MAINPLL_PWR_CON0, 0xf0000001,
		HAVE_RST_BAR, 21, MAINPLL_CON0, 6, MAINPLL_CON1, 0, NULL, 1768 * MHZ),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", UNIVPLL_CON0, VOID_REG, 0xf3000001,
		HAVE_RST_BAR, 7, UNIVPLL_CON0, 6, UNIVPLL_CON0, 8, &mt6589_fixed_lc_pll_ops, 1248 * MHZ),
	PLL(CLK_APMIXED_MMPLL, "mmpll", MMPLL_CON0, VOID_REG, 0xf0000001,
		HAVE_RST_BAR, 7, MMPLL_CON0, 6, MMPLL_CON0, 8, &mt6589_fixed_lc_pll_ops, 1690 * MHZ),
	PLL(CLK_APMIXED_ISPPLL, "isppll", ISPPLL_CON0, VOID_REG, 0x80000001,
		0, 7, ISPPLL_CON0, 6, ISPPLL_CON0, 8, &mt6589_lc_pll_ops, 1664 * MHZ),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", MSDCPLL_CON0, MSDCPLL_PWR_CON0, 0x80000001,
		0, 21, MSDCPLL_CON0, 6, MSDCPLL_CON1, 0, NULL, 1664 * MHZ),
	PLL(CLK_APMIXED_TVDPLL,  "tvdpll",  TVDPLL_CON0, TVDPLL_PWR_CON0, 0x80000001,
		0, 21, TVDPLL_CON0, 6, TVDPLL_CON1, 0, NULL, 2376UL * MHZ),
	PLL(CLK_APMIXED_LVDSPLL, "lvdspll", LVDSPLL_CON0, LVDSPLL_PWR_CON0, 0x80000001,
		0, 21, LVDSPLL_CON0, 6, LVDSPLL_CON1, 0, NULL, 1440 * MHZ),
};

/* TODO: convert to gate */
static const struct mtk_fixed_factor pll_divs[] = {
	FACTOR(CLK_APMIXED_ARMPLL_1300M, "armpll_1300m", "mainpll", 1, 1),

	FACTOR(CLK_APMIXED_MAINPLL_806M, "mainpll_806m", "mainpll", 1, 2),
	FACTOR(CLK_APMIXED_MAINPLL_537P3M, "mainpll_537p3m", "mainpll", 1, 3),
	FACTOR(CLK_APMIXED_MAINPLL_322P4M, "mainpll_322p4m", "mainpll", 1, 5),
	FACTOR(CLK_APMIXED_MAINPLL_230P3M, "mainpll_230p3m", "mainpll", 1, 7),

	FACTOR(CLK_APMIXED_UNIVPLL_624M, "univpll_624m", "univpll", 1, 2),
	FACTOR(CLK_APMIXED_UNIVPLL_416M, "univpll_416m", "univpll", 1, 3),
	FACTOR(CLK_APMIXED_UNIVPLL_249P6M, "univpll_249p6m", "univpll", 1, 5),
	FACTOR(CLK_APMIXED_UNIVPLL_178P3M, "univpll_178p3m", "univpll", 1, 7),
	FACTOR(CLK_APMIXED_UNIVPLL_48M, "univpll_48m", "univpll", 1, 26),
	FACTOR(CLK_APMIXED_UNIVPLL_USB_48M, "univpll_usb_48m", "univpll", 1, 26),

	FACTOR(CLK_APMIXED_MMPLL_DIV2, "mmpll_div2", "mmpll", 1, 2),
	FACTOR(CLK_APMIXED_MMPLL_DIV3, "mmpll_div3", "mmpll", 1, 3),
	FACTOR(CLK_APMIXED_MMPLL_DIV5, "mmpll_div5", "mmpll", 1, 5),
	FACTOR(CLK_APMIXED_MMPLL_DIV7, "mmpll_div7", "mmpll", 1, 7),

	FACTOR(CLK_APMIXED_ISPPLL_208M, "isppll_208m", "isppll", 1, 8),

	FACTOR(CLK_APMIXED_MSDCPLL_208M, "msdcpll_208m", "msdcpll", 1, 8),

	FACTOR(CLK_APMIXED_TVDPLL_148P5M, "tvdpll_148p5m", "tvdpll", 1, 16),

	FACTOR(CLK_APMIXED_LVDSPLL_180M, "lvdspll_180m", "lvdspll", 1, 8),
};

static const struct mtk_clk_desc apmixed_desc = {
	.plls = plls,
	.num_plls = ARRAY_SIZE(plls),
	.factor_clks = pll_divs,
	.num_factor_clks = ARRAY_SIZE(pll_divs),

};

static const struct of_device_id of_match_clk_mt6589_apmixed[] = {
	{ .compatible = "mediatek,mt6589-apmixedsys", .data = &apmixed_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_apmixed);

static struct platform_driver clk_mt6589_apmixed_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-apmixed",
		.of_match_table = of_match_clk_mt6589_apmixed,
	},
};
module_platform_driver(clk_mt6589_apmixed_drv);

MODULE_DESCRIPTION("MediaTek MT6589 apmixedsys clocks driver");
MODULE_LICENSE("GPL");
