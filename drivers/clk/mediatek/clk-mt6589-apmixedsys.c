// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
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

#include "clk-pll.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mt6589-clk.h>

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
			_fmax) {					\
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
		.pd_shift = _pd_shift,					\
		.tuner_reg = VOID_REG,					\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
	}

static const struct mtk_pll_data plls[] = {
	PLL(CLK_APMIXED_ARMPLL, "armpll", ARMPLL_CON0, ARMPLL_PWR_CON0, 0x80000001,
		PLL_AO, 21, ARMPLL_CON1, 24, ARMPLL_CON1, 0, 1300000000),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", MAINPLL_CON0, MAINPLL_PWR_CON0, 0xf0000001,
		HAVE_RST_BAR, 21, MAINPLL_CON0, 6, MAINPLL_CON1, 0, 1612000000),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", MSDCPLL_CON0, MSDCPLL_PWR_CON0, 0x80000001,
		0, 21, MSDCPLL_CON0, 6, MSDCPLL_CON1, 0, 1664000000),
	PLL(CLK_APMIXED_TVDPLL,  "tvdpll",  TVDPLL_CON0, TVDPLL_PWR_CON0, 0x80000001,
		0, 21, TVDPLL_CON0, 6, TVDPLL_CON1, 0, 2376000000),
	PLL(CLK_APMIXED_LVDSPLL, "lvdspll", LVDSPLL_CON0, LVDSPLL_PWR_CON0, 0x80000001,
		0, 21, LVDSPLL_CON0, 6, LVDSPLL_CON1, 0, 1300000000),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", UNIVPLL_CON0, VOID_REG, 0xf3000001,
		HAVE_RST_BAR, 7, UNIVPLL_CON0, 6, UNIVPLL_CON0, 8, 1248000000),
	PLL(CLK_APMIXED_MMPLL, "mmpll", MMPLL_CON0, VOID_REG, 0xf0000001,
		HAVE_RST_BAR, 7, MMPLL_CON0, 6, MMPLL_CON0, 8, 1430000000),
	PLL(CLK_APMIXED_ISPPLL, "isppll", ISPPLL_CON0, VOID_REG, 0x80000001,
		0, 7, ISPPLL_CON0, 7, ISPPLL_CON0, 8, 1664000000),
};

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
