// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

static const struct mtk_gate_regs mfg_cg_regs = {
	.set_ofs = 0x0000,
	.clr_ofs = 0x0004,
	.sta_ofs = 0x0008,
};

#define GATE_MFG(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &mfg_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate mfg_clks[] = {
	// GATE_MFG(CLK_MFG_AXI, "mfg_axi", "", 0),
	// GATE_MFG(CLK_MFG_MEM, "mfg_mem", "", 1),
	// GATE_MFG(CLK_MFG_G3D, "mfg_g3d", "", 2),
	// GATE_MFG(CLK_MFG_HYD, "mfg_hyd", "", 3),
};

static const struct mtk_clk_desc mfg_desc = {
	.clks = mfg_clks,
	.num_clks = ARRAY_SIZE(mfg_clks),
};

static const struct of_device_id of_match_clk_mt6589_mfg[] = {
	{
		.compatible = "mediatek,mt6589-mfgsys",
		.data = &mfg_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_mfg);

static struct platform_driver clk_mt6589_mfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-mfg",
		.of_match_table = of_match_clk_mt6589_mfg,
	},
};
module_platform_driver(clk_mt6589_mfg_drv);

MODULE_DESCRIPTION("MediaTek MT6589 mfgsys clocks driver");
MODULE_LICENSE("GPL");
