// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */
 
#include "clk-gate.h"
#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <dt-bindings/clock/mediatek,mt6582-clk.h>

#include "clk-mtk.h"
#include "clk-pll.h"
#include <linux/delay.h>

#define MT6582_PLL_FMAX		(1690 * MHZ)
#define INTEGER_BITS		7
#define CON0_MT6582_RST_BAR    BIT(24)

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits, _pd_reg, \
			_pd_shift, _pcw_reg, _pcw_shift) {	\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = CON0_MT6582_RST_BAR,			\
		.fmax = MT6582_PLL_FMAX,				\
		.pcwbits = _pcwbits,					\
		.pd_reg = _pd_reg,					\
		.pd_shift = _pd_shift,					\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
	}

static const struct mtk_pll_data plls[] = {
	PLL(CLK_APMIXED_ARMPLL, "armpll", 0x0200, 0x020c, 0x00000001, 0, 21, 0x0200, 19, 0x0204, 0),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", 0x0210, 0x021c, 0x78000001, HAVE_RST_BAR, 21, 0x210, 19, 0x0214, 0),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", 0x0220, 0x022c, 0xFC000001, HAVE_RST_BAR, 21, 0x0220, 19, 0x0224, 0),
	PLL(CLK_APMIXED_MMPLL, "mmpll", 0x0230, 0x023c, 0x00000001, 0, 21, 0x0230, 4, 0x0234, 0),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", 0x0240, 0x024c, 0x00000001, PLL_AO, 21, 0x0240, 4, 0x0244, 0),
};

static const struct mtk_fixed_factor apmixed_factors[] = {
	FACTOR(CLK_APMIXED_UNIV48M, "univ48m", "univpll", 1, 26),
	FACTOR(CLK_APMIXED_USB48M, "usb48m", "univpll", 1, 26),
};


static int clk_mt6582_apmixed_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	int r;
	struct device_node *node = pdev->dev.of_node;

	clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_plls(&pdev->dev, plls, ARRAY_SIZE(plls), clk_data);
	if (r)
		goto free_clk_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r) {
		dev_err(&pdev->dev, "Cannot register clock provider: %d\n", r);
		goto unregister_plls;
	}

	mtk_clk_register_factors(apmixed_factors, 
				 ARRAY_SIZE(apmixed_factors), clk_data);

	return 0;

unregister_plls:
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static void clk_mt6582_apmixed_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(plls, ARRAY_SIZE(plls), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt6582_apmixed[] = {
	{ 
		.compatible = "mediatek,mt6582-apmixedsys"
	}, {
		/* sentinel */
	}

};

static struct platform_driver clk_mt6582_apmixed_drv = {
	.probe = clk_mt6582_apmixed_probe,
	.remove = clk_mt6582_apmixed_remove,
	.driver = {
		.name = "clk-mt6582-apmixed",
		.of_match_table = of_match_clk_mt6582_apmixed,
	},
};

builtin_platform_driver(clk_mt6582_apmixed_drv);


/* ddr_phy part */
static const struct mtk_pll_data plls_ddrphy[] = {
	PLL(CLK_DDRPHY_VENCPLL, "vencpll", 0x0800, 0x080c, 0x00000001, 0, 21, 0x0800, 4, 0x0804, 0),
};

static int clk_mt6582_ddrphy_probe(struct platform_device *pdev)
{

	struct clk_hw_onecell_data *clk_data;
	int r;
	struct device_node *node = pdev->dev.of_node;

	clk_data = mtk_alloc_clk_data(CLK_DDRPHY_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	r = mtk_clk_register_plls(&pdev->dev, plls_ddrphy, ARRAY_SIZE(plls_ddrphy), clk_data);
	if (r)
		goto free_clk_data;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r) {
		dev_err(&pdev->dev, "Cannot register clock provider: %d\n", r);
		goto unregister_plls;
	}

	return 0;

unregister_plls:
	mtk_clk_unregister_plls(plls_ddrphy, ARRAY_SIZE(plls_ddrphy), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static void clk_mt6582_ddrphy_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_plls(plls_ddrphy, ARRAY_SIZE(plls_ddrphy), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt6582_ddrphy[] = {
	{ 
		.compatible = "mediatek,mt6582-ddrphy"
	}, {
		/* sentinel */
	}

};

static struct platform_driver clk_mt6582_ddrphy_drv = {
	.probe = clk_mt6582_ddrphy_probe,
	.remove = clk_mt6582_ddrphy_remove,
	.driver = {
		.name = "clk-mt6582-ddrphy",
		.of_match_table = of_match_clk_mt6582_ddrphy,
	},
};

builtin_platform_driver(clk_mt6582_ddrphy_drv);


MODULE_DESCRIPTION("MediaTek MT6582 apmixedsys clocks driver");
MODULE_LICENSE("GPL");
