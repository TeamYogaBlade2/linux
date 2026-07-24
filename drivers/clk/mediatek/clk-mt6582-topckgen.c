// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include "clk-gate.h"
#include "clk-mtk.h"
#include "clk-mux.h"
#include <linux/debugfs.h>
#include <linux/delay.h>

#include <dt-bindings/clock/mediatek,mt6582-clk.h>

static DEFINE_SPINLOCK(mt6582_top_clk_lock);

#define TOP_MUX_GATE_NOSR(_id, _name, _parents, _reg, _shift, _width, _gate, _flags) \
		MUX_GATE_CLR_SET_UPD_FLAGS(_id, _name, _parents, _reg,		\
			(_reg + 0x4), (_reg + 0x8), _shift, _width,		\
			_gate, 0, -1, _flags)

#define TOP_MUX_GATE(_id, _name, _parents, _reg, _shift, _width, _gate, _flags)	\
		TOP_MUX_GATE_NOSR(_id, _name, _parents, _reg, _shift, _width,	\
				  _gate, CLK_SET_RATE_PARENT | _flags)


static const struct mtk_fixed_factor top_fixed_divs[] = {
	FACTOR(CLK_TOP_SYSPLL_D2, "syspll_d2", "mainpll", 1, 2),
	FACTOR(CLK_TOP_SYSPLL_D3, "syspll_d3", "mainpll", 1, 3),
	FACTOR(CLK_TOP_SYSPLL_D5, "syspll_d5", "mainpll", 1, 5),
	FACTOR(CLK_TOP_SYSPLL_D7, "syspll_d7", "mainpll", 1, 7),
	FACTOR(CLK_TOP_SYSPLL1_D2, "syspll1_d2", "syspll_d2", 1, 2),
	FACTOR(CLK_TOP_SYSPLL1_D4, "syspll1_d4", "syspll_d2", 1, 4),
	FACTOR(CLK_TOP_SYSPLL1_D8, "syspll1_d8", "syspll_d2", 1, 8),
	FACTOR(CLK_TOP_SYSPLL1_D16, "syspll1_d16", "syspll_d2", 1, 16),
	FACTOR(CLK_TOP_SYSPLL2_D2, "syspll2_d2", "syspll_d3", 1, 2),
	FACTOR(CLK_TOP_SYSPLL2_D4, "syspll2_d4", "syspll_d3", 1, 4),
	FACTOR(CLK_TOP_SYSPLL2_D8, "syspll2_d8", "syspll_d3", 1, 8),
	FACTOR(CLK_TOP_SYSPLL3_D2, "syspll3_d2", "syspll_ck", 1, 8),
	FACTOR(CLK_TOP_SYSPLL3_D4, "syspll3_d4", "syspll_ck", 1, 16),
	FACTOR(CLK_TOP_SYSPLL4_D2, "syspll4_d2", "syspll_d5", 1, 2),

	FACTOR(CLK_TOP_UNIVPLL_D2, "univpll_d2", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL_D3, "univpll_d3", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIVPLL_D5, "univpll_d5", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIVPLL_D7, "univpll_d7", "univpll", 1, 7),

	FACTOR(CLK_TOP_UNIVPLL1_D4, "univpll1_d4", "univpll_d2", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL1_D8, "univpll1_d8", "univpll_d2", 1, 8),

	FACTOR(CLK_TOP_UNIVPLL2_D2, "univpll2_d2", "univpll_d3", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL2_D4, "univpll2_d4", "univpll_d3", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D8, "univpll2_d8", "univpll_d3", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL3_D2, "univpll3_d2", "univpll_ck", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL3_D4, "univpll3_d4", "univpll_ck", 1, 16),

	FACTOR(CLK_TOP_MSDCPLL_D2, "msdcpll_d2", "msdcpll", 1, 2),

	FACTOR(CLK_TOP_MMPLL_D2, "mmpll_d2", "mmpll", 1, 2),

	FACTOR(CLK_TOP_DMPLL, "dmpll_ck", "dmpll", 1, 2),
	FACTOR(CLK_TOP_DMPLL_D2, "dmpll_d2", "dmpll_ck", 1, 2),
	FACTOR(CLK_TOP_DMPLL_D4, "dmpll_d4", "dmpll_ck", 1, 4),
	FACTOR(CLK_TOP_DMPLL_X2, "dmpll_x2", "dmpll", 2, 1),
	
};


/* mux parents*/

static const char * const axi_parents[] = {
	"clk26m",
	"syspll1_d2",
	"syspll_d5",
	"syspll1_d4",
	"univpll_d5",
	"univpll2_d2",
	"dmpll_ck",
	"dmpll_d2"
};

static const char * const mem_parents[] = {
	"clk26m",
	"dmpll_ck"
};

static const char * const ddrphycfg_parents[] = {
	"clk26m",
	"syspll1_d8"
};

static const char * const mm_parents[] = {
	"clk26m",
	"vencpll_ck",
	"syspll1_d2",
	"syspll_d5",
	"syspll1_d4",
	"univpll_d5",
	"univpll2_d2",
	"dmpll_ck"
};



static const char * const pwm_parents[] = {
	"clk26m",
	"univpll2_d4",
	"univpll3_d2",
	"univpll1_d4",
};

static const char * const vdec_parents[] = {
	"clk26m",
	"syspll1_d2",
	"syspll_d5",
	"syspll1_d4",
	"univpll_d5",
	"univpll2_d2",
	"msdcpll_d2",
	"mmpll_d2"
};

static const char * const mfg_parents[] = {
	"clk26m",
	"mmpll_ck",
	"dmpll_x2",
	"msdcpll",
	"clk26m",
	"syspll_d3",
	"univpll_d3"
};

static const char * const camtg_parents[] = {
	"clk26m",
	"univpll_d26",
	"univpll2_d2",
	"syspll3_d2",
	"syspll3_d4",
	"msdcpll_d2",
	"mmpll_d2"
};

static const char * const uart_parents[] = {
	"clk26m",
	"univpll2_d8"
};

static const char * const spi_parents[] = {
	"clk26m",
	"syspll3_d2",
	"syspll4_d2",
	"univpll2_d4",
	"univpll1_d8"
};

static const char * const usb20_parents[] = {
	"clk26m",
	"univpll1_d8",
	"univpll3_d4"
};

static const char * const msdc30_0_parents[] = {
	"clk26m",
	"msdcpll_d2",
	"syspll2_d2",
	"syspll1_d4",
	"univpll1_d4",
	"univpll2_d4"
};


static const char * const msdc30_1_parents[] = {
	"clk26m",
	"msdcpll_d2",
	"syspll2_d2",
	"syspll1_d4",
	"univpll1_d4",
	"univpll2_d4"
};

static const char * const msdc30_2_parents[] = {
	"clk26m",
	"msdcpll_d2",
	"syspll2_d2",
	"syspll1_d4",
	"univpll1_d4",
	"univpll2_d4"
};

static const char * const audio_parents[] = {
/*	"f_f26m_ck",*/
	"clk26m",
	"syspll1_d16"
};

static const char * const aud_intbus_parents[] = {
	"clk26m",
	"syspll1_d4",
	"syspll3_d2",
	"syspll4_d2",
	"univpll3_d2",
	"univpll2_d4"
};

static const char * const pmicspi_parents[] = {
	"clk26m",
	"syspll1_d8",
	"syspll2_d4",
	"syspll4_d2",
	"syspll3_d4",
	"syspll2_d8",
	"syspll1_d16",
	"univpll3_d4",
	"univpll_d26",
	"dmpll_d2",
	"dmpll_d4"
};

static const char * const scp_parents[] = {
	"clk26m",
	"syspll1_d8",
	"dmpll_d2",
	"dmpll_d4"
};


static const struct mtk_mux top_muxes[] = {
	/* CLK_CFG_0 */
	TOP_MUX_GATE_NOSR(CLK_TOP_AXI_SEL, "axi_sel", axi_parents,
		0x0040, 0, 3, 7, CLK_IS_CRITICAL),
	TOP_MUX_GATE_NOSR(CLK_TOP_MEM_SEL, "mem_sel", mem_parents,
		0x0040, 8, 1, 15, CLK_IS_CRITICAL),
	TOP_MUX_GATE_NOSR(CLK_TOP_DDRPHYCFG_SEL, "ddrphycfg_sel",
		ddrphycfg_parents, 0x0040, 16, 1, 23, CLK_IS_CRITICAL),
	TOP_MUX_GATE(CLK_TOP_MM_SEL, "mm_sel", mm_parents,
		0x0040, 24, 3, 31, 0),
	/* CLK_CFG_1 */
	TOP_MUX_GATE(CLK_TOP_PWM_SEL, "pwm_sel", pwm_parents,
		0x0050, 0, 2, 7, 0),
	TOP_MUX_GATE(CLK_TOP_VDEC_SEL, "vdec_sel", vdec_parents,
		0x0050, 8, 4, 15, 0),
	TOP_MUX_GATE(CLK_TOP_MFG_SEL, "mfg_sel", mfg_parents,
		0x0050, 16, 3, 23, 0),
	TOP_MUX_GATE(CLK_TOP_CAMTG_SEL, "camtg_sel", camtg_parents,
		0x0050, 24, 3, 31, 0),
	/* CLK_CFG_2 */
	TOP_MUX_GATE(CLK_TOP_UART_SEL, "uart_sel", uart_parents,
		0x0060, 0, 1, 7, 0),
	TOP_MUX_GATE(CLK_TOP_SPI_SEL, "spi_sel", spi_parents,
		0x0060, 8, 3, 15, 0),
	TOP_MUX_GATE(CLK_TOP_USB20_SEL, "usb20_sel", usb20_parents,
		0x0060, 16, 2, 23, 0),
	TOP_MUX_GATE(CLK_TOP_MSDC30_0_SEL, "msdc30_0_sel", msdc30_0_parents,
		0x0060, 24, 3, 31, 0),
	/* CLK_CFG_3 */
	TOP_MUX_GATE(CLK_TOP_MSDC30_1_SEL, "msdc30_1_sel", msdc30_1_parents,
		0x0070, 0, 3, 7, 0),
	TOP_MUX_GATE(CLK_TOP_MSDC30_2_SEL, "msdc30_2_sel", msdc30_2_parents,
		0x0070, 8, 3, 15, 0),
	TOP_MUX_GATE(CLK_TOP_AUDIO_SEL, "audio_sel", audio_parents,
		0x0070, 16, 1, 23, 0),
	TOP_MUX_GATE(CLK_TOP_AUD_INTBUS_SEL, "aud_intbus_sel", aud_intbus_parents,
		0x0070, 24, 3, 31, 0),
	/* CLK_CFG_4 */
	TOP_MUX_GATE_NOSR(CLK_TOP_PMICSPI_SEL, "pmicspi_sel", pmicspi_parents,
		0x0080, 0, 4, 7, CLK_IS_CRITICAL | CLK_SET_RATE_PARENT),
	TOP_MUX_GATE(CLK_TOP_SCP_SEL, "scp_sel", scp_parents,
		0x0080, 8, 2, 15, 0),
	
};





static int clk_mt6582_topckgen_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) return PTR_ERR(base);

	clk_data = mtk_alloc_clk_data(CLK_TOP_NR_CLK);
	if (!clk_data) return -ENOMEM;

								
	mtk_clk_register_factors(top_fixed_divs, (int)ARRAY_SIZE(top_fixed_divs), clk_data);

	
	mtk_clk_register_muxes(&pdev->dev, top_muxes, (int)ARRAY_SIZE(top_muxes), 
                       node, &mt6582_top_clk_lock, clk_data);

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get, clk_data);
}

static const struct of_device_id of_match_clk_mt6582_topckgen[] = {
	{ 	.compatible = "mediatek,mt6582-topckgen"
	}, {
		/* sentinel */
	}
};

static struct platform_driver clk_mt6582_topckgen_drv = {
	.probe = clk_mt6582_topckgen_probe,
	.driver = {
		.name = "clk-mt6582-topckgen",
		.of_match_table = of_match_clk_mt6582_topckgen,
	},
};
builtin_platform_driver(clk_mt6582_topckgen_drv);


MODULE_DESCRIPTION("MediaTek MT6582 topckgen clocks driver");
MODULE_LICENSE("GPL");

