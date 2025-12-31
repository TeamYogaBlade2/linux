// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

#define VDEC_CKEN_SET	0x0000
#define VDEC_CKEN_CLR	0x0004
#define LARB_CKEN_SET	0x0008
#define LARB_CKEN_CLR	0x000c

/*
 * downstream
    {
        .name = __stringify(CG_VDEC0),
        .set_addr = VDEC_CKEN_CLR,
        .clr_addr = VDEC_CKEN_SET,
        .mask = 0x00000001,
        .ops = &vdec_cg_grp_ops,
        .sys = &syss[SYS_VDE],
    }, {
        .name = __stringify(CG_VDEC1),
        .set_addr = LARB_CKEN_CLR,
        .clr_addr = LARB_CKEN_SET,
        .mask = 0x00000001,
        .ops = &vdec_cg_grp_ops,
        .sys = &syss[SYS_VDE],
    }
 */

static const struct mtk_gate_regs vdec_cg_regs = {
	.set_ofs = VDEC_CKEN_SET,
	.clr_ofs = VDEC_CKEN_CLR,
	.sta_ofs = VDEC_CKEN_SET, /* many SoCs use set reg as sta reg */
};

static const struct mtk_gate_regs larb_cg_regs = {
	.set_ofs = LARB_CKEN_SET,
	.clr_ofs = LARB_CKEN_CLR,
	.sta_ofs = LARB_CKEN_SET,
};

#define GATE_VDEC(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &vdec_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

#define GATE_LARB(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &larb_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate vdec_clks[] = {
	GATE_VDEC(CLK_VDEC0_VDE, "vdec0_vde", "vdec_sel", 0),
	GATE_LARB(CLK_VDEC1_SMI, "vdec0_smi", "vdec_sel", 0),
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
