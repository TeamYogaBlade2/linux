// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */

#include <dt-bindings/clock/mediatek,mt6582-clk.h>
#include <dt-bindings/reset/mediatek,mt6582-resets.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "clk-cpumux.h"
#include "clk-gate.h"
#include "clk-mtk.h"
#include "reset.h"


/*PERICFG*/

static DEFINE_SPINLOCK(mt6582_peri_clk_lock);

static const struct mtk_gate_regs peri0_cg_regs = {
	.set_ofs = 0x0008,
	.clr_ofs = 0x0010,
	.sta_ofs = 0x0018,
};


#define GATE_PERI0(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &peri0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)


static const struct mtk_gate peri_gates[] = {
	GATE_PERI0(CLK_PERI_SPI0, "spi0_ck", "spi0_sel", 25),
	GATE_PERI0(CLK_PERI_AUXADC, "auxadc_ck", "clk26m", 24),
	GATE_PERI0(CLK_PERI_I2C2, "i2c2_ck", "axi_sel", 23),
	GATE_PERI0(CLK_PERI_I2C1, "i2c1_ck", "axi_sel", 22),
	GATE_PERI0(CLK_PERI_I2C0, "i2c0_ck", "axi_sel", 21),
	GATE_PERI0(CLK_PERI_BTIF, "bitif_ck", "axi_sel", 20),
	GATE_PERI0(CLK_PERI_UART3, "uart3_ck", "axi_sel", 19),
	GATE_PERI0(CLK_PERI_UART2, "uart2_ck", "axi_sel", 18),
	GATE_PERI0(CLK_PERI_UART1, "uart1_ck", "axi_sel", 17),
	GATE_PERI0(CLK_PERI_UART0, "uart0_ck", "axi_sel", 16),
	GATE_PERI0(CLK_PERI_NLI, "nli_ck", "axi_sel", 15),
	GATE_PERI0(CLK_PERI_MSDC30_2, "msdc30_2_ck", "msdc30_2_sel", 14),
	GATE_PERI0(CLK_PERI_MSDC30_1, "msdc30_1_ck", "msdc30_1_sel", 13),
	GATE_PERI0(CLK_PERI_MSDC30_0, "msdc30_0_ck", "msdc30_0_sel", 12),
	GATE_PERI0(CLK_PERI_AP_DMA, "ap_dma_ck", "axi_sel", 11),
	GATE_PERI0(CLK_PERI_USB0, "usb0_ck", "usb20_sel", 10),
	GATE_PERI0(CLK_PERI_PWM, "pwm_ck", "axi_sel", 9),
	GATE_PERI0(CLK_PERI_PWM7, "pwm7_ck", "axisel_d4", 8),
	GATE_PERI0(CLK_PERI_PWM6, "pwm6_ck", "axisel_d4", 7),
	GATE_PERI0(CLK_PERI_PWM5, "pwm5_ck", "axisel_d4", 6),
	GATE_PERI0(CLK_PERI_PWM4, "pwm4_ck", "axisel_d4", 5),
	GATE_PERI0(CLK_PERI_PWM3, "pwm3_ck", "axisel_d4", 4),
	GATE_PERI0(CLK_PERI_PWM2, "pwm2_ck", "axisel_d4", 3),
	GATE_PERI0(CLK_PERI_PWM1, "pwm1_ck", "axisel_d4", 2),
	GATE_PERI0(CLK_PERI_THERM, "therm_ck", "axi_sel", 1),
	GATE_PERI0(CLK_PERI_NFI, "nfi_ck", "nfi2x_sel", 0),
};


static const char * const uart_ck_sel_parents[] = {
	"clk26m",		// 26MHz
	"uart_sel",		// 52MHz
};

static const struct mtk_composite peri_clks[] = {
	MUX(CLK_PERI_UART_SEL, "uart_ck_sel", uart_ck_sel_parents,
		0x40c, 0, 1),
};

static u16 peri_rst_ofs[] = { 0x0, 0x4, };

static u16 peri_idx_map[] = {
	[PERI_UART0_SW_RST] 	= 0,
	[PERI_UART1_SW_RST] 	= 1,
	[PERI_UART2_SW_RST] 	= 2,
	[PERI_UART3_SW_RST] 	= 3,
	[PERI_BTIF_SW_RST]	= 6,
	[PERI_PWM_SW_RST]	= 8,
	[PERI_AUXADC_SW_RST]	= 10,
	[PERI_DMA_SW_RST]	= 11,
	[PERI_NFI_SW_RST]	= 14,
	[PERI_NLI_SW_RST]	= 15,
	[PERI_THERM_SW_RST]	= 16,
	[PERI_MSDC2_SW_RST]	= 17,
	[PERI_MSDC0_SW_RST]	= 19,
	[PERI_MSDC1_SW_RST]	= 20,
	[PERI_I2C0_SW_RST]	= 22,
	[PERI_I2C1_SW_RST]	= 23,
	[PERI_I2C2_SW_RST]	= 24,
	[PERI_USB_SW_RST]	= 28,
	[PERI_SPI0_SW_RST]	= 32,
};

static const struct mtk_clk_rst_desc clk_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = peri_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(peri_rst_ofs),
	.rst_idx_map = peri_idx_map,
	.rst_idx_map_nr = ARRAY_SIZE(peri_idx_map),
};

static const struct of_device_id of_match_clk_mt6582_pericfg[] = {
	{ 
		.compatible = "mediatek,mt6582-pericfg" 
	}, { 
		/* sentinel */ 
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6582_pericfg);

static int clk_mt6582_pericfg_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data;
	struct device_node *node = pdev->dev.of_node;
	void __iomem *base;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = mtk_alloc_clk_data(CLK_PERI_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_register_reset_controller_with_dev(&pdev->dev, &clk_rst_desc);
	if (ret)
		goto free_clk_data;

	ret = mtk_clk_register_gates(&pdev->dev, node, peri_gates,
				     ARRAY_SIZE(peri_gates), clk_data);
	if (ret)
		goto free_clk_data;

	ret = mtk_clk_register_composites(&pdev->dev, peri_clks,
					  ARRAY_SIZE(peri_clks), base,
					  &mt6582_peri_clk_lock, clk_data);
	if (ret)
		goto unregister_gates;

	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (ret)
		goto unregister_composites;

	return 0;

unregister_composites:
	mtk_clk_unregister_composites(peri_clks, ARRAY_SIZE(peri_clks), clk_data);
unregister_gates:
	mtk_clk_unregister_gates(peri_gates, ARRAY_SIZE(peri_gates), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return ret;
}

static void clk_mt6582_pericfg_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	of_clk_del_provider(node);
	mtk_clk_unregister_composites(peri_clks, ARRAY_SIZE(peri_clks), clk_data);
	mtk_clk_unregister_gates(peri_gates, ARRAY_SIZE(peri_gates), clk_data);
	mtk_free_clk_data(clk_data);
}

static struct platform_driver clk_mt6582_pericfg_drv = {
	.driver = {
		.name = "clk-mt6582-pericfg",
		.of_match_table = of_match_clk_mt6582_pericfg,
	},
	.probe = clk_mt6582_pericfg_probe,
	.remove = clk_mt6582_pericfg_remove,
};
module_platform_driver(clk_mt6582_pericfg_drv);

MODULE_DESCRIPTION("MediaTek MT6582 pericfg clocks driver");
MODULE_LICENSE("GPL");
