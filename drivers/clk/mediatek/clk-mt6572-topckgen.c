// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025-2026 Roman Vivchar <rva333@protonmail.com>
 */

#include "clk-gate.h"
#include "clk-mtk.h"
#include "clk-mux.h"

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6572-clk.h>

#define CLK_CFG_0		0x00
/* DDR1 or DDR2 use 7th bit in the CLK_CFG_0 */
#define IS_DDR1_OR_DDR2		BIT(7)

static DEFINE_SPINLOCK(mt6572_topckgen_lock);

struct mt6572_topckgen_priv {
	struct clk_hw_onecell_data *clk_data;
	bool is_ddr3;
};

static const struct mtk_fixed_factor top_factors[] = {
	FACTOR(CLK_TOP_MAINPLL_D2, "mainpll_d2", "mainpll", 1, 2),
	FACTOR(CLK_TOP_MAINPLL_D3, "mainpll_d3", "mainpll", 1, 3),
	FACTOR(CLK_TOP_MAINPLL_D4, "mainpll_d4", "mainpll", 1, 4),
	FACTOR(CLK_TOP_MAINPLL_D5, "mainpll_d5", "mainpll", 1, 5),
	FACTOR(CLK_TOP_MAINPLL_D6, "mainpll_d6", "mainpll", 1, 6),
	FACTOR(CLK_TOP_MAINPLL_D7, "mainpll_d7", "mainpll", 1, 7),
	FACTOR(CLK_TOP_MAINPLL_D8, "mainpll_d8", "mainpll", 1, 8),
	FACTOR(CLK_TOP_MAINPLL_D10, "mainpll_d10", "mainpll", 1, 10),
	FACTOR(CLK_TOP_MAINPLL_D12, "mainpll_d12", "mainpll", 1, 12),
	FACTOR(CLK_TOP_MAINPLL_D20, "mainpll_d20", "mainpll", 1, 20),
	FACTOR(CLK_TOP_MAINPLL_D24, "mainpll_d24", "mainpll", 1, 24),
	
	FACTOR(CLK_TOP_UNIVPLL_D2, "univpll_d2", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL_D3, "univpll_d3", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIVPLL_D4, "univpll_d4", "univpll", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL_D5, "univpll_d5", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIVPLL_D6, "univpll_d6", "univpll", 1, 6),
	FACTOR(CLK_TOP_UNIVPLL_D7, "univpll_d7", "univpll", 1, 7),
	FACTOR(CLK_TOP_UNIVPLL_D8, "univpll_d8", "univpll", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL_D10, "univpll_d10", "univpll", 1, 10),
	FACTOR(CLK_TOP_UNIVPLL_D12, "univpll_d12", "univpll", 1, 12),
	FACTOR(CLK_TOP_UNIVPLL_D16, "univpll_d16", "univpll", 1, 16),
	FACTOR(CLK_TOP_UNIVPLL_D20, "univpll_d20", "univpll", 1, 20),
	FACTOR(CLK_TOP_UNIVPLL_D24, "univpll_d24", "univpll", 1, 24),
	FACTOR(CLK_TOP_UNIVPLL_D26, "univpll_d26", "univpll", 1, 26),
};

static const char * const uart_sel_parents[] = {
	"clk26m",
	"univpll_d24"
};

static const char * const emi2x_sel_parents[] = {
	"clk26m",
	"clk26m",
	"clk26m",
	"clk26m",
	"clk26m",
	"clk26m",
	"clk26m",
	"clk26m",
	"clk26m",
	"mainpll_d3",
	"mainpll_d4",
	"clk26m",
	"mainpll_d2"
};

static const char * const axi_sel_parents[] = {
	"clk26m",
	"clk26m",
	"mainpll_d10",
	"clk26m",
	"mainpll_d12"
};

static const char *const mfg_parents[] = {
	"mfg_pre_whpll_491m",
	"mfg_pre_whpll_500m",
	"mainpll_d3",
	"univpll_d2",
	"clk26m",
	"mainpll_d2",
	"clk26m",
	"mainpll_d2",
};

static const char *const mfg_pre_parents[] = {
	"univpll_d3",
	"mfg_sel"
};

static const char * const msdc_sel_parents[] = {
	"mainpll_d12",
	"mainpll_d10",
	"mainpll_d8",
	"univpll_d7",
	"mainpll_d7",
	"mainpll_d8",
	"clk26m",
	"univpll_d6"
};

static const char * const spi_nand_sel_parents[] = {
	"mainpll_d24",
	"mainpll_d20",
	"univpll_d20",
	"univpll_d16",
	"univpll_d12",
	"univpll_d10",
	"mainpll_d12",
	"mainpll_d10",
};

static const char * const cam_sel_parents[] = {
	"univpll_d26",
	"univpll_d6"
};

static const char * const pwm_mm_sel_parents[] = {
	"clk26m",
	"univpll_d12"
};

static const char * const spm_52m_sel_parents[] = {
	"clk26m",
	"univpll_d24"
};

static const char * const pmic_spi_sel_ddr2_parents[] = {
	"mainpll_d24",
	"univpll_d26",
	"univpll_d16",
	"clk26m"
};

static const char * const pmic_spi_sel_ddr3_parents[] = {
	"mainpll_d20",
	"univpll_d26",
	"univpll_d16",
	"clk26m"
};

static const char * const aud_intbus_sel_ddr2_parents[] = {
	"clk26m",
	"clk26m",
	"mainpll_d24",
	"clk26m",
	"mainpll_d12"
};

static const char * const aud_intbus_sel_ddr3_parents[] = {
	"clk26m",
	"clk26m",
	"mainpll_d20",
	"clk26m",
	"mainpll_d10"
};

static const char * const spinfi_pre_sel_parents[] = {
	"clk26m",
	"spinfi_sel"
};

static const struct mtk_composite top_ddr2_muxes[] = {
	MUX(CLK_TOP_PMIC_SPI_SEL, "pmic_spi_sel", pmic_spi_sel_ddr2_parents,
	    CLK_CFG_0, 24, 2),
	MUX(CLK_TOP_AUD_INTBUS_SEL, "aud_intbus_sel",
	    aud_intbus_sel_ddr2_parents, CLK_CFG_0, 27, 3),
};

static const struct mtk_composite top_ddr3_muxes[] = {
	MUX(CLK_TOP_PMIC_SPI_SEL, "pmic_spi_sel", pmic_spi_sel_ddr3_parents,
	    CLK_CFG_0, 24, 2),
	MUX(CLK_TOP_AUD_INTBUS_SEL, "aud_intbus_sel",
	    aud_intbus_sel_ddr3_parents, CLK_CFG_0, 27, 3),
};

static const struct mtk_composite top_muxes[] = {
	MUX(CLK_TOP_UART0_SEL, "uart0_sel", uart_sel_parents, CLK_CFG_0, 0, 1),
	MUX_FLAGS(CLK_TOP_EMI2X_SEL, "emi2x_sel", emi2x_sel_parents, CLK_CFG_0,
		  1, 4, CLK_IS_CRITICAL),
	MUX_FLAGS(CLK_TOP_AXI_SEL, "axi_sel", axi_sel_parents, CLK_CFG_0, 5, 3,
		  CLK_IS_CRITICAL),
	MUX_FLAGS(CLK_TOP_MFG_SEL, "mfg_sel", mfg_parents, CLK_CFG_0, 8,
		  3, CLK_SET_RATE_PARENT),
	MUX(CLK_TOP_MSDC0_SEL, "msdc0_sel", msdc_sel_parents, CLK_CFG_0, 11, 3),
	MUX(CLK_TOP_SPINFI_SEL, "spinfi_sel", spi_nand_sel_parents, CLK_CFG_0,
	    14, 3),
	MUX(CLK_TOP_CAM_SEL, "cam_sel", cam_sel_parents, CLK_CFG_0, 17, 1),
	MUX(CLK_TOP_PWM_MM_SEL, "pwm_mm_sel", pwm_mm_sel_parents, CLK_CFG_0, 18,
	    1),
	MUX(CLK_TOP_UART1_SEL, "uart1_sel", uart_sel_parents, CLK_CFG_0, 19, 1),
	MUX(CLK_TOP_MSDC1_SEL, "msdc1_sel", msdc_sel_parents, CLK_CFG_0, 20, 3),
	MUX_FLAGS(CLK_TOP_SPM_52M_SEL, "spm_52m_sel", spm_52m_sel_parents,
		  CLK_CFG_0, 23, 1, CLK_IS_CRITICAL),
	MUX(CLK_TOP_SPINFI_PRE_SEL, "spinfi_pre_sel", spinfi_pre_sel_parents,
	    CLK_CFG_0, 30, 1),
	MUX_FLAGS(CLK_TOP_MFG_PRE_SEL, "mfg_pre_sel", mfg_pre_parents, CLK_CFG_0,
		  31, 1, CLK_SET_RATE_PARENT),
};

static const struct mtk_gate_regs top0_cg_regs = {
	.sta_ofs = 0x20,
	.set_ofs = 0x50,
	.clr_ofs = 0x80,
};

static const struct mtk_gate_regs top1_cg_regs = {
	.sta_ofs = 0x24,
	.set_ofs = 0x54,
	.clr_ofs = 0x84,
};

#define GATE_TOP0(_id, _name, _parent, _shift)               \
	GATE_MTK(_id, _name, _parent, &top0_cg_regs, _shift, \
		 &mtk_clk_gate_ops_setclr)

#define GATE_TOP0_INV(_id, _name, _parent, _shift)           \
	GATE_MTK(_id, _name, _parent, &top0_cg_regs, _shift, \
		 &mtk_clk_gate_ops_setclr_inv)

#define GATE_TOP1(_id, _name, _parent, _shift)               \
	GATE_MTK(_id, _name, _parent, &top1_cg_regs, _shift, \
		 &mtk_clk_gate_ops_setclr)

#define GATE_TOP1_CRITICAL(_id, _name, _parent, _shift)            \
	GATE_MTK_FLAGS(_id, _name, _parent, &top1_cg_regs, _shift, \
		       &mtk_clk_gate_ops_setclr, CLK_IS_CRITICAL)

static const struct mtk_gate top_ddr2_gates[] = {
	GATE_TOP0(CLK_TOP_DBI_BCLK, "dbi_bclk", "mainpll_d12", 5),
	GATE_TOP1(CLK_TOP_PWM, "pwm", "mainpll_d24", 9),
};

static const struct mtk_gate top_ddr3_gates[] = {
	GATE_TOP0(CLK_TOP_DBI_BCLK, "dbi_bclk", "mainpll_d10", 5),
	GATE_TOP1(CLK_TOP_PWM, "pwm", "mainpll_d20", 9),
};

static const struct mtk_gate top_gates[] = {
	GATE_TOP0(CLK_TOP_PWM_MM, "pwm_mm", "pwm_mm_sel", 0),
	GATE_TOP0(CLK_TOP_CAM_MM, "cam_mm", "cam_sel", 1),
	GATE_TOP0(CLK_TOP_MFG_MM, "mfg_mm", "mfg_sel", 2),
	GATE_TOP0(CLK_TOP_SPM_52M, "spm_52m", "spm_52m_sel", 3),
	GATE_TOP0_INV(CLK_TOP_MIPI_26M_DBG, "mipi_26m_dbg", "clk26m", 4),
	GATE_TOP0_INV(CLK_TOP_SC_26M, "sc_26m", "clk26m", 6),
	GATE_TOP0_INV(CLK_TOP_SC_MEM, "sc_mem", "clk26m", 7),
	GATE_TOP0(CLK_TOP_DBI_PAD0, "dbi_pad0", "dbi_bclk", 16),
	GATE_TOP0(CLK_TOP_DBI_PAD1, "dbi_pad1", "dbi_bclk", 17),
	GATE_TOP0(CLK_TOP_DBI_PAD2, "dbi_pad2", "dbi_bclk", 18),
	GATE_TOP0(CLK_TOP_DBI_PAD3, "dbi_pad3", "dbi_bclk", 19),
	GATE_TOP0_INV(CLK_TOP_MFG_PRE_491M, "mfg_pre_whpll_491m", "mainpll_d3", 20),
	GATE_TOP0_INV(CLK_TOP_MFG_PRE_500M, "mfg_pre_whpll_500m", "whpll", 21),
	GATE_TOP0_INV(CLK_TOP_ARMDCM, "armdcm", "clk26m", 31),

	GATE_TOP1(CLK_TOP_EFUSE, "efuse", "clk26m", 0),
	GATE_TOP1(CLK_TOP_THERMAL, "thermal", "clk26m", 1),
	GATE_TOP1(CLK_TOP_APDMA, "apdma", "axi_sel", 2),
	GATE_TOP1(CLK_TOP_I2C0, "i2c0", "axi_sel", 3),
	GATE_TOP1(CLK_TOP_I2C1, "i2c1", "axi_sel", 4),
	GATE_TOP1(CLK_TOP_NFI, "nfi", "axi_sel", 6),
	GATE_TOP1(CLK_TOP_NFI_ECC, "nfi_ecc", "axi_sel", 7),
	GATE_TOP1(CLK_TOP_DEBUGSYS, "debugsys", "axi_sel", 8),
	GATE_TOP1(CLK_TOP_UART0, "uart0", "uart0_sel", 10),
	GATE_TOP1(CLK_TOP_UART1, "uart1", "uart1_sel", 11),
	GATE_TOP1(CLK_TOP_BTIF, "btif", "axi_sel", 12),
	GATE_TOP1(CLK_TOP_USB, "usb", "axi_sel", 13),
	GATE_TOP1(CLK_TOP_FHCTL, "fhctl", "clk26m", 14),
	GATE_TOP1(CLK_TOP_SPINFI, "spinfi", "spinfi_sel", 16),
	GATE_TOP1(CLK_TOP_MSDC0, "msdc0", "msdc0_sel", 17),
	GATE_TOP1(CLK_TOP_MSDC1, "msdc1", "msdc1_sel", 18),
	GATE_TOP1(CLK_TOP_PMIC_SPI, "pmic_spi", "pmic_spi_sel", 20),
	GATE_TOP1(CLK_TOP_SEJ, "sej", "clk26m", 21),
	GATE_TOP1(CLK_TOP_MEMSLP_DLYER, "memslp_dlyer", "clk26m", 22),
	GATE_TOP1_CRITICAL(CLK_TOP_APXGPT, "apxgpt", "clk26m", 24),
	GATE_TOP1(CLK_TOP_AUD, "aud", "aud_intbus_sel", 25),
	GATE_TOP1_CRITICAL(CLK_TOP_SPM, "spm", "clk26m", 26),
	GATE_TOP1(CLK_TOP_PMIC_26M, "pmic_26m", "clk26m", 29),
	GATE_TOP1(CLK_TOP_AUXADC, "auxadc", "clk26m", 30),
};

static int clk_mt6572_topckgen_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *clk_data;
	struct mt6572_topckgen_priv *priv;
	struct clk_hw *hw;
	void __iomem *base;
	int r;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	clk_data = mtk_alloc_clk_data(CLK_TOP_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		r = PTR_ERR(base);
		goto free_top_data;
	}

	priv->clk_data = clk_data;
	priv->is_ddr3 = !(readl(base + CLK_CFG_0) & IS_DDR1_OR_DDR2);

	r = mtk_clk_register_factors(top_factors, ARRAY_SIZE(top_factors),
				     clk_data);
	if (r)
		goto free_top_data;

	r = mtk_clk_register_composites(&pdev->dev, top_muxes,
					ARRAY_SIZE(top_muxes), base,
					&mt6572_topckgen_lock, clk_data);
	if (r)
		goto unregister_factors;

	r = mtk_clk_register_gates(&pdev->dev, node, top_gates,
				   ARRAY_SIZE(top_gates), clk_data);
	if (r)
		goto unregister_muxes;

	if (priv->is_ddr3) {
		r = mtk_clk_register_composites(dev, top_ddr3_muxes,
						ARRAY_SIZE(top_ddr3_muxes),
						base, &mt6572_topckgen_lock,
						clk_data);
		if (r)
			goto unregister_gates;

		r = mtk_clk_register_gates(dev, node, top_ddr3_gates,
					   ARRAY_SIZE(top_ddr3_gates),
					   clk_data);
		if (r)
			goto unregister_dram_muxes;
		/* When DDR3, MultiMedia source is mainpll / 5 */
		hw = devm_clk_hw_register_fixed_factor(dev, "smi_mm", "mainpll",
						       0, 1, 5);
	} else {
		r = mtk_clk_register_composites(dev, top_ddr2_muxes,
						ARRAY_SIZE(top_ddr2_muxes),
						base, &mt6572_topckgen_lock,
						clk_data);
		if (r)
			goto unregister_gates;

		r = mtk_clk_register_gates(dev, node, top_ddr2_gates,
					   ARRAY_SIZE(top_ddr2_gates),
					   clk_data);
		if (r)
			goto unregister_dram_muxes;

		/* When DDR2, MultiMedia source is mainpll / 6 */
		hw = devm_clk_hw_register_fixed_factor(dev, "smi_mm", "mainpll",
						       0, 1, 6);
	}

	if (IS_ERR(hw)) {
		r = PTR_ERR(hw);
		goto unregister_dram_gates;
	}

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		goto unregister_dram_gates;

	platform_set_drvdata(pdev, clk_data);

	return r;

unregister_dram_gates:
	if (priv->is_ddr3)
		mtk_clk_unregister_gates(top_ddr3_gates,
					 ARRAY_SIZE(top_ddr3_gates),
					 priv->clk_data);
	else
		mtk_clk_unregister_gates(top_ddr2_gates,
					 ARRAY_SIZE(top_ddr2_gates),
					 priv->clk_data);

unregister_dram_muxes:
	if (priv->is_ddr3)
		mtk_clk_unregister_composites(
			top_ddr3_muxes, ARRAY_SIZE(top_ddr3_muxes), clk_data);
	else
		mtk_clk_unregister_composites(
			top_ddr2_muxes, ARRAY_SIZE(top_ddr2_muxes), clk_data);

unregister_gates:
	mtk_clk_unregister_gates(top_gates, ARRAY_SIZE(top_gates), clk_data);
unregister_muxes:
	mtk_clk_unregister_composites(top_muxes, ARRAY_SIZE(top_muxes),
				      clk_data);
unregister_factors:
	mtk_clk_unregister_factors(top_factors, ARRAY_SIZE(top_factors),
				   clk_data);
free_top_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static void clk_mt6572_topckgen_remove(struct platform_device *pdev)
{
	struct mt6572_topckgen_priv *priv = platform_get_drvdata(pdev);
	struct clk_hw_onecell_data *clk_data = priv->clk_data;
	struct device_node *node = pdev->dev.of_node;

	of_clk_del_provider(node);

	if (priv->is_ddr3) {
		mtk_clk_unregister_gates(top_ddr3_gates,
					 ARRAY_SIZE(top_ddr3_gates),
					 priv->clk_data);
		mtk_clk_unregister_composites(top_ddr3_muxes,
					      ARRAY_SIZE(top_ddr3_muxes),
					      priv->clk_data);
	} else {
		mtk_clk_unregister_gates(top_ddr2_gates,
					 ARRAY_SIZE(top_ddr2_gates),
					 priv->clk_data);
		mtk_clk_unregister_composites(top_ddr2_muxes,
					      ARRAY_SIZE(top_ddr2_muxes),
					      priv->clk_data);
	}

	mtk_clk_unregister_gates(top_gates, ARRAY_SIZE(top_gates), clk_data);
	mtk_clk_unregister_composites(top_muxes, ARRAY_SIZE(top_muxes),
				      clk_data);
	mtk_clk_unregister_factors(top_factors, ARRAY_SIZE(top_factors),
				   clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_mt6572_topckgen[] = {
	{ .compatible = "mediatek,mt6572-topckgen" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt6572_topckgen);

static struct platform_driver clk_mt6572_topckgen = {
	.probe = clk_mt6572_topckgen_probe,
	.remove = clk_mt6572_topckgen_remove,
	.driver = {
		.name = "clk-mt6572-topckgen",
		.of_match_table = of_match_mt6572_topckgen,
	},
};
module_platform_driver(clk_mt6572_topckgen);

MODULE_DESCRIPTION("MediaTek MT6572 topckgen clock driver");
MODULE_LICENSE("GPL");
