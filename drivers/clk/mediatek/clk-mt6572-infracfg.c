// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025-2026 Roman Vivchar <rva333@protonmail.com>
 */

#include "clk-mtk.h"

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6572-clk.h>

static const char * const cpu_mux_parents[] = {
	"clk26m",
	"armpll",
	"univpll",
	"mainpll_d2"
};

static const struct mtk_composite infra_muxes[] = {
	MUX(CLK_INFRA_CPUSEL, "cpu_mux", cpu_mux_parents, 0x0, 2, 2),
};

static const struct mtk_clk_desc infracfg_clks = {
	.composite_clks = infra_muxes,
	.num_composite_clks = ARRAY_SIZE(infra_muxes),
};

static const struct of_device_id of_match_mt6572_infracfg[] = {
	{ .compatible = "mediatek,mt6572-infracfg_ao", .data = &infracfg_clks },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt6572_infracfg);

static struct platform_driver clk_mt6572_infracfg = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6572-infracfg",
		.of_match_table = of_match_mt6572_infracfg,
	},
};
module_platform_driver(clk_mt6572_infracfg);

MODULE_DESCRIPTION("MediaTek MT6572 infracfg clock driver");
MODULE_LICENSE("GPL");

