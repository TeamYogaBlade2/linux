// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

static const struct mtk_gate_regs disp0_cg_regs = {
	.set_ofs = 0x0104,
	.clr_ofs = 0x0108,
	.sta_ofs = 0x0100,
};

static const struct mtk_gate_regs disp1_cg_regs = {
	.set_ofs = 0x0114,
	.clr_ofs = 0x0118,
	.sta_ofs = 0x0110,
};

#define GATE_DISP0(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &disp_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

#define GATE_DISP1(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &disp_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate disp_clks[] = {
	// GATE_DISP0(CLK_DISP0_LARB2_SMI, "", "", 0),
	// GATE_DISP0(CLK_DISP0_ROT_ENGINE, "", "", 1),
	// GATE_DISP0(CLK_DISP0_ROT_SMI, "", "", 2),
	// GATE_DISP0(CLK_DISP0_SCL, "", "", 3),
	// GATE_DISP0(CLK_DISP0_OVL_ENGINE, "", "", 4),
	// GATE_DISP0(CLK_DISP0_OVL_SMI, "", "", 5),
	// GATE_DISP0(CLK_DISP0_COLOR, "", "", 6),
	// GATE_DISP0(CLK_DISP0_2DSHP, "", "", 7),
	// GATE_DISP0(CLK_DISP0_BLS, "", "", 8),
	// GATE_DISP0(CLK_DISP0_WDMA0_ENGINE, "", "", 9),
	// GATE_DISP0(CLK_DISP0_WDMA0_SMI, "", "", 10),
	// GATE_DISP0(CLK_DISP0_WDMA1_ENGINE, "", "", 11),
	// GATE_DISP0(CLK_DISP0_WDMA1_SMI, "", "", 12),
	// GATE_DISP0(CLK_DISP0_RDMA0_ENGINE, "", "", 13),
	// GATE_DISP0(CLK_DISP0_RDMA0_SMI, "", "", 14),
	// GATE_DISP0(CLK_DISP0_RDMA0_OUTPUT, "", "", 15),
	// GATE_DISP0(CLK_DISP0_RDMA1_ENGINE, "", "", 16),
	// GATE_DISP0(CLK_DISP0_RDMA1_SMI, "", "", 17),
	// GATE_DISP0(CLK_DISP0_RDMA1_OUTPUT, "", "", 18),
	// GATE_DISP0(CLK_DISP0_GAMMA_ENGINE, "", "", 19),
	// GATE_DISP0(CLK_DISP0_GAMMA_PIXEL, "", "", 20),
	// GATE_DISP0(CLK_DISP0_CMDQ_ENGINE, "", "", 21),
	// GATE_DISP0(CLK_DISP0_CMDQ_SMI, "", "", 22),
	// GATE_DISP0(CLK_DISP0_G2D_ENGINE, "", "", 23),
	// GATE_DISP0(CLK_DISP0_G2D_SMI, "", "", 24),
	//
	// GATE_DISP1(CLK_DISP1_DBI_ENGINE, "", "", 0),
	// GATE_DISP1(CLK_DISP1_DBI_SMI, "", "", 1),
	// GATE_DISP1(CLK_DISP1_DBI_OUTPUT, "", "", 2),
	// GATE_DISP1(CLK_DISP1_DSI_ENGINE, "", "", 3),
	// GATE_DISP1(CLK_DISP1_DSI_DIGITAL, "", "", 4),
	// GATE_DISP1(CLK_DISP1_DSI_DIGITAL_LANE, "", "", 5),
	// GATE_DISP1(CLK_DISP1_DPI0, "", "", 6),
	// GATE_DISP1(CLK_DISP1_DPI1, "", "", 7),
	// GATE_DISP1(CLK_DISP1_LCD, "", "", 8),
	// GATE_DISP1(CLK_DISP1_SLCD, "", "", 9),
};

static const struct mtk_clk_desc disp_desc = {
	.clks = disp_clks,
	.num_clks = ARRAY_SIZE(disp_clks),
};

static const struct of_device_id of_match_clk_mt6589_disp[] = {
	{
		.compatible = "mediatek,mt6589-dispsys",
		.data = &disp_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_disp);

static struct platform_driver clk_mt6589_disp_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-disp",
		.of_match_table = of_match_clk_mt6589_disp,
	},
};
module_platform_driver(clk_mt6589_disp_drv);

MODULE_DESCRIPTION("MediaTek MT6589 display clocks driver");
MODULE_LICENSE("GPL");
