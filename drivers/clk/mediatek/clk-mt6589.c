// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

#define GATE_INFRA(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &infra_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate_regs infra_cg_regs = {
	.set_ofs = 0x0040,
	.clr_ofs = 0x0044,
	.sta_ofs = 0x0048,
};

static const struct mtk_gate infra_clks[] = {
	// GATE_INFRA(CLK_INFRA_DBGCLK, "", "", 0),
	// GATE_INFRA(CLK_INFRA_SMI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_SPI0, "", "", 0),
	// GATE_INFRA(CLK_INFRA_AUDIO, "", "", 0),
	// GATE_INFRA(CLK_INFRA_CEC, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MFGAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_M4U, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD1MCUAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD1HWMIXAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD1AHB, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD2MCUAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD2HWMIXAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD2AHB, "", "", 0),
	// GATE_INFRA(CLK_INFRA_CPUM, "", "", 0),
	// GATE_INFRA(CLK_INFRA_KP, "", "", 0),
	// GATE_INFRA(CLK_INFRA_CCIF0, "", "", 0),
	// GATE_INFRA(CLK_INFRA_CCIF1, "", "", 0),
	// GATE_INFRA(CLK_INFRA_PMICSPI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_PMICWRAP, "", "", 0),
};

static const struct mtk_clk_desc infra_desc = {
	.clks = infra_clks,
	.num_clks = ARRAY_SIZE(infra_clks),
};

static const struct of_device_id of_match_clk_mt6589[] = {
	{ .compatible = "mediatek,mt6589-topckgen", .data = &topck_desc },
	{ .compatible = "mediatek,mt6589-infracfg", .data = &infra_desc },
	// { .compatible = "mediatek,mt6589-pericfg", .data = &peri_desc, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589);

static struct platform_driver clk_mt6589_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589",
		.of_match_table = of_match_clk_mt6589,
	},
};
module_platform_driver(clk_mt6589_drv);

MODULE_DESCRIPTION("MediaTek MT6589 main clocks driver");
MODULE_LICENSE("GPL");
