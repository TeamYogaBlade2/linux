// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "clk-pll.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mt6589-clk.h>

/* FIXME: MT6589 config */
#define MT8590_PLL_FMAX		(2000 * MHZ)
#define CON0_MT6589_RST_BAR	BIT(27)

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, \
			_flags, _pcwbits, _pd_reg, _pd_shift, \
			_tuner_reg, _pcw_reg, _pcw_shift) {	\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = CON0_MT6589_RST_BAR,			\
		.fmax = MT8590_PLL_FMAX,				\
		.pcwbits = _pcwbits,					\
		.pd_reg = _pd_reg,					\
		.pd_shift = _pd_shift,					\
		.tuner_reg = _tuner_reg,				\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
	}

// FIXME: pd_reg/pd_shift, comments are from tooniis port. 
// pd is Post-Divider, not Power-Down

static const struct mtk_pll_data plls[] = {
	PLL(CLK_APMIXED_ARMPLL,  "armpll",  0x0200, 0x0218, 0x80000001,
//		0, 21, 0x0204, 24, 0x0, 0x0204, 0),
		PLL_AO, 21, 0x204, 24, 0x0, 0x0204, 0),
/*
	PLL(CLK_APMIXED_MAINPLL, "mainpll", 0x021C, 0x0234, 0xF0000001,
//		HAVE_RST_BAR, 21, 0x21c, 6, 0x0, 0x0220, 0),
		HAVE_RST_BAR, 21, <unknown>, <unknown>, 0x0, 0x0220, 0),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", 0x0250, 0x0268, 0x80000001,
//		0, 21, 0x250, 6, 0x0, 0x0254, 0),
		0, 21, <unknown>, <unknown>, 0x0, 0x0254, 0),
	PLL(CLK_APMIXED_TVDPLL,  "tvdpll",  0x026C, 0x0284, 0x80000001,
//		0, 21, 0x26c, 6, 0x0, 0x0270, 0),
		0, 21, <unknown>, <unknown>, 0x0, 0x0270, 0),
	PLL(CLK_APMIXED_LVDSPLL, "lvdspll", 0x0288, 0x02A0, 0x80000001,
//		0, 21, 0x288, 6, 0x0, 0x028C, 0),
		0, 21, <unknown>, <unknown>, 0x0, 0x028C, 0),
/*
	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x0238, <unknown>, 0xF3000001,
		HAVE_RST_BAR, 7, <unknown>, <unknown>, 0x0, 0x0238, 8),
//	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x0238, <unknown>, 0xF3000001,
//		HAVE_RST_BAR, 7, 0x238, 6, 0x0, 0x238, 8),
	PLL(CLK_APMIXED_MMPLL,   "mmpll",   0x0240, <unknown>, 0xF0000001,
		HAVE_RST_BAR, 7, <unknown>, <unknown>, 0x0, 0x0240, 8),
//	PLL(CLK_APMIXED_MMPLL,   "mmpll",   0x0240, <unknown>, 0xF0000001,
//		HAVE_RST_BAR, 7, 0x240, 6, 0x0, 0x240, 8),
	PLL(CLK_APMIXED_ISPPLL,  "isppll",   0x0248, <unknown>, 0x80000001,
		0, 7, <unknown>, <unknown>, 0x0, 0x0248, 8),
//	PLL(CLK_APMIXED_ISPPLL,  "isppll",   0x0248, <unknown>, 0x80000001,
//		0, 7, 0x240, 7, 0x0, 0x240, 8),
*/
};

/* init logic from MT2712 */

static int clk_mt6589_apmixed_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	int r;
	struct device_node *node = pdev->dev.of_node;

	clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_plls(node, plls, ARRAY_SIZE(plls), clk_data);
	if (r)
		goto free_clk_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r) {
		dev_err(&pdev->dev, "Cannot register clock provider: %d\n", r);
		goto unregister_plls;
	}

	return 0;

unregister_plls:
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static void clk_mt6589_apmixed_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt6589_apmixed[] = {
	{ .compatible = "mediatek,mt6589-apmixedsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_apmixed);

static struct platform_driver clk_mt6589_apmixed_drv = {
	.probe = clk_mt6589_apmixed_probe,
	.remove = clk_mt6589_apmixed_remove,
	.driver = {
		.name = "clk-mt6589-apmixed",
		.of_match_table = of_match_clk_mt6589_apmixed,
	},
};
module_platform_driver(clk_mt6589_apmixed_drv)

MODULE_DESCRIPTION("MediaTek MT6589 apmixedsys clocks driver");
MODULE_LICENSE("GPL");
