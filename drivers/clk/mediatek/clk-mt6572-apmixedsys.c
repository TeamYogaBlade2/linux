// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025-2026 Roman Vivchar <rva333@protonmail.com>
 */

#include "clk-fhctl.h"
#include "clk-gate.h"
#include "clk-mtk.h"
#include "clk-pll.h"
#include "clk-pllfh.h"

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6572-clk.h>

#define ARMPLL_OFFSET		0x100
#define MAINPLL_OFFSET		0x120
#define UNIVPLL_OFFSET		0x140
#define WHPLL_OFFSET		0x240

#define WHPLL_PATHSEL_CON 	0x254
#define RSV_RW0_CON1		0xf04

#define REG_CON0		0x0
#define REG_CON1		0x4
#define REG_PWR_CON0		0x10

#define CON0_RST_BAR		BIT(27)

#define WHPLL_ENABLE		BIT(0)
#define WHPLL_CONTROL		BIT(30) | BIT(31)

static int whpll_prepare(struct clk_hw *hw) {
	struct mtk_clk_pll *pll = to_mtk_clk_pll(hw);
	void __iomem *apmixed_base = pll->base_addr - WHPLL_OFFSET;
	
	/*
	 * WHPLL requires WHPLL_PATHSEL_CON and RSV_RW0_CON1 setup to enable
	 * output. Otherwise the GPU will get 0 HZ and hang.
	 */
	writel_relaxed(WHPLL_ENABLE, apmixed_base + WHPLL_PATHSEL_CON);
	writel_relaxed(WHPLL_CONTROL, apmixed_base + RSV_RW0_CON1);

	return mtk_pll_prepare(hw);
}

static void whpll_unprepare(struct clk_hw *hw) {
	struct mtk_clk_pll *pll = to_mtk_clk_pll(hw);
	void __iomem *apmixed_base = pll->base_addr - WHPLL_OFFSET;

	writel_relaxed(0, apmixed_base + WHPLL_PATHSEL_CON);
	writel_relaxed(0, apmixed_base + RSV_RW0_CON1);

	mtk_pll_unprepare(hw);
}

static const struct clk_ops whpll_ops = {
	.is_prepared	= mtk_pll_is_prepared,
	.prepare	= whpll_prepare,
	.unprepare	= whpll_unprepare,
	.recalc_rate	= mtk_pll_recalc_rate,
	.determine_rate = mtk_pll_determine_rate,
	.set_rate	= mtk_pll_set_rate,
};

#define PLL(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, _pcwbits, \
	    _fmin, _fmax, _div, _ops)                                     \
{                                                                         \
	.id = _id,                                                        \
	.name = _name,                                                    \
	.parent_name = "clk26m",                                          \
	.reg = (_base) + REG_CON0,                                        \
	.pwr_reg = (_base) + REG_PWR_CON0,                                \
	.en_mask = _en_mask,                                              \
	.rst_bar_mask = _rst_bar_mask,                                    \
	.pd_reg = (_base) + REG_CON1,                                     \
	.pd_shift = 24,                                                   \
	.pcw_reg = (_base) + REG_CON1,                                    \
	.pcw_chg_reg = (_base) + REG_CON1,                                \
	.pcwbits = _pcwbits,                                              \
	.flags = _flags,                                                  \
	.fmin = _fmin,                                                    \
	.fmax = _fmax,                                                    \
	.div_table = _div,                                                \
	.ops = _ops                                                       \
}

#define PLL_DIV(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, _pcwbits, \
		_fmin, _fmax, _div)                                           \
	PLL(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, _pcwbits,     \
	    _fmin, _fmax, _div, NULL)

#define PLL_NODIV(_id, _name, _base, _en_mask, _rst_bar_mask, _flags,     \
		  _pcwbits, _fmin, _fmax)                                 \
	PLL(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, _pcwbits, \
	    _fmin, _fmax, NULL, NULL)

static const struct mtk_pll_div_table armpll_mainpll_whpll_div_table[] = {
	/* VCO max 1989 MHz */
	{ .div = 0, .freq = 1989 * MHZ },
	/* VCO max 2002 MHz */
	{ .div = 1, .freq = 1001 * MHZ },
	/* VCO max 2080 MHz */
	{ .div = 2, .freq = 520 * MHZ },
	{ .div = 3, .freq = 260 * MHZ },
	{ .div = 4, .freq = 130 * MHZ },
	{ /* sentinel */ }
};

static const struct mtk_pll_data plls[] = {
	PLL_DIV(CLK_APMIXED_ARMPLL, "armpll", ARMPLL_OFFSET, 0x00000011, 0,
		PLL_AO, 21, 1001 * MHZ, 1989 * MHZ,
		armpll_mainpll_whpll_div_table),

	PLL_DIV(CLK_APMIXED_MAINPLL, "mainpll", MAINPLL_OFFSET, 0x00000011,
		CON0_RST_BAR, PLL_AO | HAVE_RST_BAR, 21, 1000 * MHZ, 1989 * MHZ,
		armpll_mainpll_whpll_div_table),

	PLL_NODIV(CLK_APMIXED_UNIVPLL, "univpll", UNIVPLL_OFFSET, 0x30000011,
		  CON0_RST_BAR, HAVE_RST_BAR, 7, 1248 * MHZ, 1248 * MHZ),

	PLL(CLK_APMIXED_WHPLL, "whpll", WHPLL_OFFSET, 0x00000011, 0, 0, 21,
	    1001 * MHZ, 1989 * MHZ, armpll_mainpll_whpll_div_table, &whpll_ops),
};

enum fh_pll_id {
	FH_ARMPLL,
	FH_MAINPLL,
	FH_NR_FH,
};

#define FH(_pllid, _fhid, _offset) \
{								\
	.data = {						\
		.pll_id = _pllid,				\
		.fh_id = _fhid,					\
		.fh_ver = FHCTL_PLLFH_V1,			\
		.fhx_offset = _offset,				\
		.dds_mask = GENMASK(20, 0),			\
		.slope0_value = 0x6003c97,			\
		.slope1_value = 0x6003c97,			\
		.sfstrx_en = BIT(2),				\
		.frddsx_en = BIT(1),				\
		.fhctlx_en = BIT(0),				\
		.tgl_org = BIT(31),				\
		.dvfs_tri = BIT(31),				\
		.pcwchg = BIT(31),				\
		.dt_val = 0x0,					\
		.df_val = 0x9,					\
		.updnlmt_shft = 16,				\
		.msk_frddsx_dys = GENMASK(23, 20),		\
		.msk_frddsx_dts = GENMASK(19, 16),		\
	},							\
}

static struct mtk_pllfh_data pllfhs[] = {
	FH(CLK_APMIXED_ARMPLL, FH_ARMPLL, 0x00),
	FH(CLK_APMIXED_MAINPLL, FH_MAINPLL, 0x14),
};

static const struct mtk_gate_regs univpll_cg_regs = {
	.set_ofs = UNIVPLL_OFFSET,
	.clr_ofs = UNIVPLL_OFFSET,
	.sta_ofs = UNIVPLL_OFFSET,
};

#define GATE_UNIVPLL(_id, _name, _parent, _shift)               \
	GATE_MTK(_id, _name, _parent, &univpll_cg_regs, _shift, \
		 &mtk_clk_gate_ops_no_setclr)

static const struct mtk_gate apmixed_gates[] = {
	GATE_UNIVPLL(CLK_APMIXED_USB48M, "univpll_usb48m", "univpll_d26", 26),
};

static int clk_mt6572_apmixed_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	const u8 *fhctl_node = "mediatek,mt6572-fhctl";
	int r;

	clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	fhctl_parse_dt(fhctl_node, pllfhs, ARRAY_SIZE(pllfhs));

	r = mtk_clk_register_pllfhs(&pdev->dev, plls, ARRAY_SIZE(plls), pllfhs,
				    ARRAY_SIZE(pllfhs), clk_data);
	if (r)
		goto free_apmixed_data;

	r = mtk_clk_register_gates(&pdev->dev, node, apmixed_gates,
				   ARRAY_SIZE(apmixed_gates), clk_data);
	if (r)
		goto unregister_plls;

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_gates;

	platform_set_drvdata(pdev, clk_data);

	return r;

unregister_gates:
	mtk_clk_unregister_gates(apmixed_gates, ARRAY_SIZE(apmixed_gates),
				 clk_data);
unregister_plls:
	mtk_clk_unregister_pllfhs(plls, ARRAY_SIZE(plls), pllfhs,
				  ARRAY_SIZE(pllfhs), clk_data);
free_apmixed_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static void clk_mt6572_apmixed_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_gates(apmixed_gates, ARRAY_SIZE(apmixed_gates),
				 clk_data);
	mtk_clk_unregister_pllfhs(plls, ARRAY_SIZE(plls), pllfhs,
				  ARRAY_SIZE(pllfhs), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_mt6572_apmixedsys[] = {
	{ .compatible = "mediatek,mt6572-apmixedsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt6572_apmixedsys);

static struct platform_driver clk_mt6572_apmixedsys = {
	.probe = clk_mt6572_apmixed_probe,
	.remove = clk_mt6572_apmixed_remove,
	.driver = {
		.name = "clk-mt6572-apmixedsys",
		.of_match_table = of_match_mt6572_apmixedsys,
	},
};
module_platform_driver(clk_mt6572_apmixedsys);

MODULE_DESCRIPTION("MediaTek MT6572 apmixedsys clock driver");
MODULE_LICENSE("GPL");
