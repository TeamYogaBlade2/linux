// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6582-clk.h>
#include <dt-bindings/reset/mediatek,mt6582-resets.h>

static const struct mtk_gate_regs mfg_cg_regs = {
	.sta_ofs = 0x0,
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
};

#define GATE_MFG(_id, _name, _parent, _shift)               \
	GATE_MTK(_id, _name, _parent, &mfg_cg_regs, _shift, \
		 &mtk_clk_gate_ops_setclr)

static const struct mtk_gate mfg_clks[] = {
	GATE_DUMMY(CLK_DUMMY, "g3d_dummy"),
	GATE_MFG(CLK_MFG_BG3D, "mfg_bg3d", "mfg_sel", 0),
};

static u16 rst_ofs[] = { 0xc, };

static const struct mtk_clk_rst_desc clk_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(rst_ofs),
};

static const struct mtk_clk_desc mfg_desc = {
	.clks = mfg_clks,
	.num_clks = ARRAY_SIZE(mfg_clks),
	.rst_desc = &clk_rst_desc,
};

static const struct of_device_id of_match_clk_mt6582_mfg[] = {
	{
		.compatible = "mediatek,mt6582-mfgcfg",
		.data = &mfg_desc,
	},
	{
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6582_mfg);

static struct platform_driver clk_mt6582_mfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6582-mfg",
		.of_match_table = of_match_clk_mt6582_mfg,
	},
};
module_platform_driver(clk_mt6582_mfg_drv);

MODULE_DESCRIPTION("MediaTek MT6582 GPU mfg clocks driver");
MODULE_LICENSE("GPL");
