// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */
 
#include <dt-bindings/clock/mt6582-clk.h>
#include <dt-bindings/reset/mt6582-resets.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "clk-cpumux.h"
#include "clk-gate.h"
#include "clk-mtk.h"
#include "reset.h"
#include <linux/delay.h>


/*INFRASYS*/


static const struct mtk_gate_regs infra_cg_regs = {
	.set_ofs = 0x0040,
	.clr_ofs = 0x0044,
	.sta_ofs = 0x0048,
};

#define GATE_ICG(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &infra_cg_regs, _shift, &mtk_clk_gate_ops_setclr)


static const char * const cpu_parents[] = {
	"clk26m",
	"armpll",
	"mainpll",
	"mmpll"
};


static const struct mtk_composite cpu_muxes[] = {
	MUX(CLK_INFRA_CPUSEL, "infra_cpu_sel", cpu_parents, 0x0000, 2, 2),
};


   
static const struct mtk_gate infra_gates[] = {
	GATE_ICG(CLK_INFRA_DBG, "dbgclk", "axi_sel", 0),
	GATE_ICG(CLK_INFRA_SMI, "smi_ck", "mm_sel", 1),
	GATE_ICG(CLK_INFRA_AUDIO, "audio_ck", "clk26m", 5),
	GATE_ICG(CLK_INFRA_EFUSE, "efuse_ck", "clk26m", 6),
	GATE_ICG(CLK_INFRA_L2C_SRAM, "l2c_sram_ck", "mm_sel", 7),
	GATE_ICG(CLK_INFRA_M4U, "m4u_ck", "mem_sel", 8),
	GATE_ICG(CLK_INFRA_MD1_CR4_AXI, "cr4_axi", "axi_sel", 9),
	GATE_ICG(CLK_INFRA_MD1_HWMIX_AXI, "hwmix_axi", "axi_sel", 10),
	GATE_ICG(CLK_INFRA_MD1_AHB, "md1_ahb", "axi_sel", 11),
	GATE_ICG(CLK_INFRA_CONNMCU, "connsys_bus", "univ_d3", 12),
	GATE_ICG(CLK_INFRA_TRNG, "trng_ck", "axi_sel", 13),
	GATE_ICG(CLK_INFRA_CPUM, "cpum_ck", "mem_sel", 15),
	GATE_ICG(CLK_INFRA_KP, "kp_ck", "axi_sel", 16),
	GATE_ICG(CLK_INFRA_CCIF0_AP_CTRL, "ccif0_ck", "axi_sel", 20),
	GATE_ICG(CLK_INFRA_PMICWRAP, "pmicwrap_ck", "clk26m", 23),
};     



static u16 infrasys_rst_ofs[] = { 0x30, 0x34, };


static const struct mtk_fixed_factor infra_fixed_divs[] = {
	FACTOR(CLK_INFRA_CLK_13M, "clk13m", "clk26m", 1, 2),
};

static u16 infra_idx_map[] = {
	/* INFRA_RST_0 */
	[INFRA_RST0_EMI_REG_RST]    = 0 * RST_NR_PER_BANK + 0,
	[INFRA_RST0_DRAMC0_AO_RST] = 0 * RST_NR_PER_BANK + 1,
	[INFRA_RST0_FHCTL_RST]  = 0 * RST_NR_PER_BANK + 2,
	[INFRA_RST0_AP_CIRQ_EINT_RST]  = 0 * RST_NR_PER_BANK + 3,
	[INFRA_RST0_APXGPT_RST]  = 0 * RST_NR_PER_BANK + 4,
	[INFRA_RST0_SCPSYS_RST]  = 0 * RST_NR_PER_BANK + 5,
	[INFRA_RST0_KP_RST]  = 0 * RST_NR_PER_BANK + 6,
	[INFRA_RST0_PMIC_WRAP_RST]  = 0 * RST_NR_PER_BANK + 7,
	[INFRA_RST0_MIPI_CONFIG_RST]  = 0 * RST_NR_PER_BANK + 8,
	/* INFRA_RST_1 */
	[INFRA_RST1_EMI_RST]  = 1 * RST_NR_PER_BANK + 0,
	[INFRA_RST1_DRAMC0_RST]  = 1 * RST_NR_PER_BANK + 2,
	[INFRA_RST1_APMIXEDSYS_RST]  = 1 * RST_NR_PER_BANK + 3,
	[INFRA_RST1_TRNG_RST]  = 1 * RST_NR_PER_BANK + 4,
	[INFRA_RST1_SYS_CIRQ_RST]  = 1 * RST_NR_PER_BANK + 5,
};


static const struct mtk_clk_rst_desc infra_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = infrasys_rst_ofs, 
	.rst_bank_nr = ARRAY_SIZE(infrasys_rst_ofs),
	.rst_idx_map = infra_idx_map,
	.rst_idx_map_nr = ARRAY_SIZE(infra_idx_map),
};


static const struct of_device_id of_match_clk_mt6582_infracfg[] = {
	{ 
		.compatible = "mediatek,mt6582-infracfg" 
	}, { 
		/* sentinel */ 
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6582_infracfg);

static int clk_mt6582_infracfg_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	void __iomem *base;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = mtk_alloc_clk_data(CLK_INFRA_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_register_reset_controller_with_dev(&pdev->dev, &infra_rst_desc);

	if (ret)
		goto free_clk_data;

	ret = mtk_clk_register_gates(&pdev->dev, node, infra_gates,
				     ARRAY_SIZE(infra_gates), clk_data);
	if (ret)
		goto free_clk_data;

	ret = mtk_clk_register_cpumuxes(&pdev->dev, node, cpu_muxes,
					ARRAY_SIZE(cpu_muxes), clk_data);
	if (ret)
		goto unregister_gates;

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret)
		goto unregister_cpumuxes;

	return 0;

unregister_cpumuxes:
	mtk_clk_unregister_cpumuxes(cpu_muxes, ARRAY_SIZE(cpu_muxes), clk_data);
unregister_gates:
	mtk_clk_unregister_gates(infra_gates, ARRAY_SIZE(infra_gates), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return ret;
}

static void clk_mt6582_infracfg_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_cpumuxes(cpu_muxes, ARRAY_SIZE(cpu_muxes), clk_data);
	mtk_clk_unregister_gates(infra_gates, ARRAY_SIZE(infra_gates), clk_data);
	mtk_free_clk_data(clk_data);
}

static struct platform_driver clk_mt6582_infracfg_drv = {
	.driver = {
		.name = "clk-mt6582-infracfg",
		.of_match_table = of_match_clk_mt6582_infracfg,
	},
	.probe = clk_mt6582_infracfg_probe,
	.remove = clk_mt6582_infracfg_remove,
};
builtin_platform_driver(clk_mt6582_infracfg_drv);

MODULE_DESCRIPTION("MediaTek MT6582 infracfg clocks driver");
MODULE_LICENSE("GPL");
