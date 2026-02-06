// SPDX-License-Identifier: GPL-2.0+

#include "clk-gate.h"
#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt6572-clk.h>

#define CLK_CFG_0		0x00
#define CLK_CFG_0_SET		0x04
#define CLK_CFG_0_CLR		0x08

static DEFINE_SPINLOCK(mt6572_topckgen_lock);

static const struct mtk_fixed_clk topckgen_fixed_clks[] = {
	FIXED_CLK(CLK_TOP_SC_26M_SEL, "sc_26m_sel", "clk26m", 26 * MHZ),
};

static const struct mtk_fixed_factor topckgen_factors[] = {
	FACTOR(CLK_TOP_MPLL_D2, "mpll_d2", "mainpll", 1, 2),
	FACTOR(CLK_TOP_MPLL_D3, "mpll_d3", "mainpll", 1, 3),
	FACTOR(CLK_TOP_MPLL_D5, "mpll_d5", "mainpll", 1, 5),
	FACTOR(CLK_TOP_MPLL_D7, "mpll_d7", "mainpll", 1, 7),
	FACTOR(CLK_TOP_MPLL_D4, "mpll_d4", "mainpll", 1, 4),
	FACTOR(CLK_TOP_MPLL_D6, "mpll_d6", "mainpll", 1, 6),
	FACTOR(CLK_TOP_MPLL_D10, "mpll_d10", "mainpll", 1, 10),
	FACTOR(CLK_TOP_MPLL_D8, "mpll_d8", "mainpll", 1, 8),
	FACTOR(CLK_TOP_MPLL_D12, "mpll_d12", "mainpll", 1, 12),
	FACTOR(CLK_TOP_MPLL_D20, "mpll_d20", "mainpll", 1, 20),
	FACTOR(CLK_TOP_MPLL_D24, "mpll_d24", "mainpll", 1, 24),
	
	FACTOR(CLK_TOP_UPLL_D2, "upll_d2", "univpll", 1, 2),
	FACTOR(CLK_TOP_UPLL_D3, "upll_d3", "univpll", 1, 3),
	FACTOR(CLK_TOP_UPLL_D5, "upll_d5", "univpll", 1, 5),
	FACTOR(CLK_TOP_UPLL_D7, "upll_d7", "univpll", 1, 7),
	FACTOR(CLK_TOP_UPLL_D4, "upll_d4", "univpll", 1, 4),
	FACTOR(CLK_TOP_UPLL_D6, "upll_d6", "univpll", 1, 6),
	FACTOR(CLK_TOP_UPLL_D10, "upll_d10", "univpll", 1, 10),
	FACTOR(CLK_TOP_UPLL_D8, "upll_d8", "univpll", 1, 8),
	FACTOR(CLK_TOP_UPLL_D12, "upll_d12", "univpll", 1, 12),
	FACTOR(CLK_TOP_UPLL_D20, "upll_d20", "univpll", 1, 20),
	FACTOR(CLK_TOP_UPLL_D16, "upll_d16", "univpll", 1, 16),
	FACTOR(CLK_TOP_UPLL_D24, "upll_d24", "univpll", 1, 24),

	FACTOR(CLK_TOP_UNIV_48M_SEL, "univ_48m", "univpll", 1, 26),
};

static const char * const uart_sel_parents[] = {
	"clk26m",
	"upll_d24"
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
	"mpll_d3",
	"mpll_d4",
	"clk26m",
	"mpll_d2"
};

static const char * const axi_sel_parents[] = {
	"clk26m",
	"clk26m",
	"mpll_d10",
	"clk26m",
	"mpll_d12"
};

static const char *const mfg_mux_parents[] = {
	"mfg_pre_491m", // 3'b000: 491.52 MHz
	"mfg_pre_500m", // 3'b001: 500.5 MHz (whpll)
	"mpll_d3", // 3'b010: "mainpll output clock divided by 3 (designed for DDR 533MHz case)" from datasheet suggests this one should be mpll_d3, and previous one is some fixed clock? mainpll rate depends on the DRAM type...
	"upll_d2", // 3'b011: 624 MHz (upll is 1248 MHz)
	"clk26m", // 3'b1x0: 26 MHz
	"mpll_d2", // 3'b1x1: "mainpll output clock divided by 2 (designed for DDR 663MHz case)" from datasheet suggests this one should be mpll_d2, and previous one is some fixed clock? mainpll rate depends on the DRAM type...
	"clk26m", // 3'b1x0: 26 MHz
	"mpll_d2", // 3'b1x1: "mainpll output clock divided by 2 (designed for DDR 663MHz case)" from datasheet suggests this one should be mpll_d2, and previous one is some fixed clock? mainpll rate depends on the DRAM type...
};

static const char *const mfg_gf_parents[] = {
	"upll_d3", // 0 = 416 MHz
	"mfg_mux_sel" // 1 = the mux
};

static const char * const msdc0_sel_parents[] = {
	"mpll_d12",
	"mpll_d10",
	"mpll_d8",
	"upll_d7",
	"mpll_d7",
	"mpll_d8",
	"clk26m",
	"upll_d6"
};

static const char * const spi_nand_sel_parents[] = {
	"mpll_d24",
	"mpll_d20",
	"upll_d20",
	"upll_d16",
	"upll_d12",
	"upll_d10",
	"mpll_d12",
	"mpll_d10",
};

static const char * const cam_sel_parents[] = {
	"univ_48m",
	"upll_d6"
};

static const char * const pwm_mm_sel_parents[] = {
	"clk26m",
	"upll_d12"
};

static const char * const msdc1_sel_parents[] = {
	"mpll_d12",
	"mpll_d10",
	"mpll_d8",
	"upll_d7",
	"mpll_d7",
	"mpll_d8",
	"clk26m",
	"upll_d6"
};

static const char * const spm_52m_sel_parents[] = {
	"clk26m",
	"upll_d24"
};

static const char * const pmic_spi_sel_ddr2_parents[] = {
	"mpll_d24",
	"univ_48m",
	"upll_d16",
	"clk26m"
};

static const char * const pmic_spi_sel_ddr3_parents[] = {
	"mpll_d20",
	"univ_48m",
	"upll_d16",
	"clk26m"
};

static const char * const audio_intbus_sel_ddr2_parents[] = {
	"clk26m",
	"clk26m",
	"mpll_d24",
	"clk26m",
	"mpll_d12"
};

static const char * const audio_intbus_sel_ddr3_parents[] = {
	"clk26m",
	"clk26m",
	"mpll_d20",
	"clk26m",
	"mpll_d10"
};

static const char * const spinfi_pre_sel_parents[] = {
	"clk26m",
	"spinfi_sel"
};

static const struct mtk_composite topckgen_ddr2_muxes[] = {
	MUX(CLK_TOP_PMIC_SPI_SEL, "pmic_spi_sel", pmic_spi_sel_ddr2_parents, CLK_CFG_0, 24, 2),
	MUX(CLK_TOP_AUDIO_INTBUS_SEL, "audio_intbus_sel", audio_intbus_sel_ddr2_parents, CLK_CFG_0, 27, 3),
};

static const struct mtk_composite topckgen_ddr3_muxes[] = {
	MUX(CLK_TOP_PMIC_SPI_SEL, "pmic_spi_sel", pmic_spi_sel_ddr3_parents, CLK_CFG_0, 24, 2),
	MUX(CLK_TOP_AUDIO_INTBUS_SEL, "audio_intbus_sel", audio_intbus_sel_ddr3_parents, CLK_CFG_0, 27, 3),
};

static const struct mtk_composite topckgen_muxes[] = {
	MUX(CLK_TOP_UART0_SEL, "uart0_sel", uart_sel_parents, CLK_CFG_0, 0, 1),
	MUX_FLAGS(CLK_TOP_EMI2X_SEL, "emi2x_sel", emi2x_sel_parents, CLK_CFG_0,
		  1, 4, CLK_IS_CRITICAL),
	MUX_FLAGS(CLK_TOP_AXI_SEL, "axi_sel", axi_sel_parents, CLK_CFG_0, 5, 3,
		  CLK_IS_CRITICAL),
	MUX_FLAGS(CLK_TOP_MFG_MUX_SEL, "mfg_mux_sel", mfg_mux_parents, CLK_CFG_0,
		  8, 3, CLK_SET_RATE_PARENT),
	MUX(CLK_TOP_MSDC0_SEL, "msdc0_sel", msdc0_sel_parents, CLK_CFG_0, 11,
	    3),
	MUX(CLK_TOP_SPINFI_SEL, "spinfi_sel", spi_nand_sel_parents, CLK_CFG_0,
	    14, 3),
	MUX(CLK_TOP_CAM_SEL, "cam_sel", cam_sel_parents, CLK_CFG_0, 17, 1),
	MUX(CLK_TOP_PWM_MM_SEL, "pwm_mm_sel", pwm_mm_sel_parents, CLK_CFG_0, 18,
	    1),
	MUX(CLK_TOP_UART1_SEL, "uart1_sel", uart_sel_parents, CLK_CFG_0, 19, 1),
	MUX(CLK_TOP_MSDC1_SEL, "msdc1_sel", msdc1_sel_parents, CLK_CFG_0, 20,
	    3),
	MUX_FLAGS(CLK_TOP_SPM_52M_SEL, "spm_52m_sel", spm_52m_sel_parents,
		  CLK_CFG_0, 23, 1, CLK_IS_CRITICAL),
	MUX(CLK_TOP_SPINFI_PRE_SEL, "spinfi_pre_sel", spinfi_pre_sel_parents,
	    CLK_CFG_0, 30, 1),
	MUX_FLAGS(CLK_TOP_MFG_SEL, "mfg_sel", mfg_gf_parents, CLK_CFG_0, 31, 1,
		  CLK_SET_RATE_PARENT),
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

#define GATE_TOP0(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &top0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_TOP0_INV(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &top0_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

#define GATE_TOP1(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &top1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate top_ddr2_clks[] = {
	GATE_TOP0(CLK_TOP_DBI_BCLK, "dbi_bclk", "mpll_d12", 5),
};

static const struct mtk_gate top_ddr3_clks[] = {
	GATE_TOP0(CLK_TOP_DBI_BCLK, "dbi_bclk", "mpll_d10", 5),
};

static const struct mtk_gate top_clks[] = {
	GATE_TOP0(CLK_TOP_PWM_MM, "pwm_mm", "pwm_mm_sel", 0),
	GATE_TOP0(CLK_TOP_CAM_MM, "cam_mm", "cam_sel", 1),
	GATE_TOP0(CLK_TOP_MFG_MM, "mfg_mm", "mfg_mux_sel", 2),
	GATE_TOP0(CLK_TOP_SPM_52M, "spm_52m", "spm_52m_sel", 3),
	GATE_TOP0_INV(CLK_TOP_MIPI_26M_DBG, "mipi_26m_dbg", "clk26m", 4),
	GATE_TOP0_INV(CLK_TOP_SC_26M, "sc_26m", "clk26m", 6),
	GATE_TOP0_INV(CLK_TOP_SC_MEM, "sc_mem", "clk26m", 7), // unk parent
	GATE_TOP0(CLK_TOP_DBI_PAD0, "dbi_pad0", "dbi_bclk", 16),
	GATE_TOP0(CLK_TOP_DBI_PAD1, "dbi_pad1", "dbi_bclk", 17),
	GATE_TOP0(CLK_TOP_DBI_PAD2, "dbi_pad2", "dbi_bclk", 18),
	GATE_TOP0(CLK_TOP_DBI_PAD3, "dbi_pad3", "dbi_bclk", 19),
	GATE_TOP0_INV(CLK_TOP_MFG_PRE_491M, "mfg_pre_491m", "mpll_d3", 20), // unk parent, but i tend to think it's mainpll/3
	GATE_TOP0_INV(CLK_TOP_MFG_PRE_500M, "mfg_pre_500m", "whpll", 21),
	GATE_TOP0_INV(CLK_TOP_ARMDCM, "armdcm", "clk26m", 31), // unk parent

	GATE_TOP1(CLK_TOP_EFUSE, "efuse", "clk26m", 0),
	GATE_TOP1(CLK_TOP_THERMAL, "thermal", "clk26m", 1),
	GATE_TOP1(CLK_TOP_APDMA, "apdma", "axi_sel", 2),
	GATE_TOP1(CLK_TOP_I2C0, "i2c0", "axi_sel", 3),
	GATE_TOP1(CLK_TOP_I2C1, "i2c1", "axi_sel", 4),
	GATE_TOP1(CLK_TOP_NFI, "nfi", "axi_sel", 6),
	GATE_TOP1(CLK_TOP_NFI_ECC, "nfi_ecc", "axi_sel", 7),
	GATE_TOP1(CLK_TOP_DEBUGSYS, "debugsys", "axi_sel", 8),
	GATE_TOP1(CLK_TOP_PWM, "pwm", "axi_sel", 9),
	GATE_TOP1(CLK_TOP_UART0, "uart0", "uart0_sel", 10),
	GATE_TOP1(CLK_TOP_UART1, "uart1", "uart1_sel", 11),
	GATE_TOP1(CLK_TOP_BTIF, "btif", "axi_sel", 12),
	GATE_TOP1(CLK_TOP_USB, "usb", "axi_sel", 13),
	GATE_TOP1(CLK_TOP_FHCTL, "fhctl", "clk26m", 14),
	GATE_TOP1(CLK_TOP_SPINFI, "spinfi", "spinfi_sel", 16),
	GATE_TOP1(CLK_TOP_MSDC0, "msdc0", "msdc0_sel", 17),
	GATE_TOP1(CLK_TOP_MSDC1, "msdc1", "msdc1_sel", 18),
	GATE_TOP1(CLK_TOP_PMIC_WRAP, "pmic_wrap", "pmic_spi_sel", 20),
	GATE_TOP1(CLK_TOP_SEJ, "sej", "clk26m", 21), // MT_CG_SYS_26M, AXIBUS, let it be clk26m
	GATE_TOP1(CLK_TOP_MEMSLP_DLYER, "memslp_dlyer", "clk26m", 22), // unk parent
	GATE_TOP1(CLK_TOP_APXGPT, "apxgpt", "clk26m", 24),
	GATE_TOP1(CLK_TOP_AUDIO, "audio", "audio_intbus_sel", 25),
	GATE_TOP1(CLK_TOP_SPM, "spm", "clk26m", 26),
	GATE_TOP1(CLK_TOP_PMIC_SPI, "pmic_spi", "clk26m", 29),
	GATE_TOP1(CLK_TOP_AUXADC, "auxadc", "clk26m", 30),
};

static int clk_mt6572_topckgen_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	struct clk_hw *hw;
	void __iomem *base;
	bool is_ddr3;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	is_ddr3 = !(readl(base + CLK_CFG_0) & BIT(7));

	clk_data = mtk_alloc_clk_data(CLK_TOP_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_fixed_clks(topckgen_fixed_clks,
				    ARRAY_SIZE(topckgen_fixed_clks), clk_data);
	mtk_clk_register_factors(topckgen_factors, ARRAY_SIZE(topckgen_factors),
				 clk_data);
	mtk_clk_register_composites(dev, topckgen_muxes,
				    ARRAY_SIZE(topckgen_muxes), base,
				    &mt6572_topckgen_lock, clk_data);
	mtk_clk_register_gates(dev, node, top_clks, ARRAY_SIZE(top_clks),
			       clk_data);

	if (is_ddr3) {
		mtk_clk_register_composites(dev, topckgen_ddr3_muxes,
					    ARRAY_SIZE(topckgen_ddr3_muxes),
					    base, &mt6572_topckgen_lock,
					    clk_data);
		mtk_clk_register_gates(dev, node, top_ddr3_clks,
				       ARRAY_SIZE(top_ddr3_clks), clk_data);
		hw = clk_hw_register_fixed_factor(NULL, "mm_smi_src", "mainpll",
						  0, 1, 5);
	} else {
		mtk_clk_register_composites(dev, topckgen_ddr2_muxes,
					    ARRAY_SIZE(topckgen_ddr2_muxes),
					    base, &mt6572_topckgen_lock,
					    clk_data);
		mtk_clk_register_gates(dev, node, top_ddr2_clks,
				       ARRAY_SIZE(top_ddr2_clks), clk_data);
		hw = clk_hw_register_fixed_factor(NULL, "mm_smi_src", "mainpll",
						  0, 1, 6);
	}

	if (IS_ERR(hw)) {
		ret = PTR_ERR(hw);
		dev_err(dev, "failed to register mm_smi_src clock: %d", ret);
		goto free_data;
	}

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret) {
		dev_err(dev, "failed to register clock provider: %d\n", ret);
		goto free_data;
	}

	return 0;

free_data:
	mtk_free_clk_data(clk_data);
	return ret;
}

static const struct mtk_clk_desc topckgen_desc = {
	.fixed_clks = topckgen_fixed_clks,
	.num_fixed_clks = ARRAY_SIZE(topckgen_fixed_clks),
	.factor_clks = topckgen_factors,
	.num_factor_clks = ARRAY_SIZE(topckgen_factors),
	.clks = top_clks,
	.num_clks = ARRAY_SIZE(top_clks),
	.composite_clks = topckgen_muxes,
	.num_composite_clks = ARRAY_SIZE(topckgen_muxes),
	.clk_lock = &mt6572_topckgen_lock,
};

static const struct of_device_id of_match_mt6572_topckgen[] = {
	{ .compatible = "mediatek,mt6572-topckgen" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt6572_topckgen);

static struct platform_driver clk_mt6572_topckgen = {
	.probe = clk_mt6572_topckgen_probe,
	.driver = {
		.name = "clk-mt6572-topckgen",
		.of_match_table = of_match_mt6572_topckgen,
	},
};
module_platform_driver(clk_mt6572_topckgen);

MODULE_DESCRIPTION("MediaTek MT6572 topckgen clock driver");
MODULE_LICENSE("GPL");

