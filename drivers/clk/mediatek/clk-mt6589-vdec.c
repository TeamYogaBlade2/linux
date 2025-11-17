// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

static const struct mtk_gate_regs vdec_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0000,
};

static const struct mtk_gate_regs larb_cg_regs = {
	.set_ofs = 0x000c,
	.clr_ofs = 0x0008,
};

#define GATE_VDEC(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &vdec_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

#define GATE_LARB(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &larb_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate vdec_clks[] = {
	// GATE_VDEC(CLK_VDEC0_VDE, "", "", 0),
	// GATE_LARB(CLK_VDEC1_SMI, "", "", 0),
};

static const struct mtk_clk_desc vdec_desc = {
	.clks = vdec_clks,
	.num_clks = ARRAY_SIZE(vdec_clks),
};

static const struct of_device_id of_match_clk_mt6589_vdec[] = {
	{
		.compatible = "mediatek,mt6589-vdecsys",
		.data = &vdec_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_vdec);

static struct platform_driver clk_mt6589_vdec_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-vdec",
		.of_match_table = of_match_clk_mt6589_vdec,
	},
};
module_platform_driver(clk_mt6589_vdec_drv);

MODULE_DESCRIPTION("MediaTek MT6589 video decoder clocks driver");
MODULE_LICENSE("GPL");
