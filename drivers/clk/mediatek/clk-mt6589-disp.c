// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

#define DISP_CG_CON0	0x0100
#define DISP_CG_SET0	0x0104
#define DISP_CG_CLR0	0x0108

#define DISP_CG_CON1	0x0110
#define DISP_CG_SET1	0x0114
#define DISP_CG_CLR1	0x0118

static const struct mtk_gate_regs disp0_cg_regs = {
	.set_ofs = DISP_CG_SET0,
	.clr_ofs = DISP_CG_CLR0,
	.sta_ofs = DISP_CG_CON0,
};

static const struct mtk_gate_regs disp1_cg_regs = {
	.set_ofs = DISP_CG_SET1,
	.clr_ofs = DISP_CG_CLR1,
	.sta_ofs = DISP_CG_CON1,
};

#define GATE_DISP0(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &disp_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_DISP1(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &disp_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate disp_clks[] = {
	GATE_DISP0(CLK_DISP0_LARB2_SMI, "disp0_larb2_smi", "disp_sel", 0),
	GATE_DISP0(CLK_DISP0_ROT_ENGINE, "disp0_rot_engine", "disp_sel", 1),
	GATE_DISP0(CLK_DISP0_ROT_SMI, "disp0_rot_smi", "disp_sel", 2),
	GATE_DISP0(CLK_DISP0_SCL, "disp0_scl", "disp_sel", 3),
	GATE_DISP0(CLK_DISP0_OVL_ENGINE, "disp0_ovl_engine", "disp_sel", 4),
	GATE_DISP0(CLK_DISP0_OVL_SMI, "disp0_ovl_smi", "disp_sel", 5),
	GATE_DISP0(CLK_DISP0_COLOR, "disp0_color", "disp_sel", 6),
	GATE_DISP0(CLK_DISP0_2DSHP, "disp0_2dshp", "disp_sel", 7),
	GATE_DISP0(CLK_DISP0_BLS, "disp0_bls", "disp_sel", 8),
	GATE_DISP0(CLK_DISP0_WDMA0_ENGINE, "disp0_wdma0_engine", "disp_sel", 9),
	GATE_DISP0(CLK_DISP0_WDMA0_SMI, "disp0_wdma0_smi", "disp_sel", 10),
	GATE_DISP0(CLK_DISP0_WDMA1_ENGINE, "disp0_wdma1_engine", "disp_sel", 11),
	GATE_DISP0(CLK_DISP0_WDMA1_SMI, "disp0_wdma1_smi", "disp_sel", 12),
	GATE_DISP0(CLK_DISP0_RDMA0_ENGINE, "disp0_rdma0_engine", "disp_sel", 13),
	GATE_DISP0(CLK_DISP0_RDMA0_SMI, "disp0_rdma0_smi", "disp_sel", 14),
	GATE_DISP0(CLK_DISP0_RDMA0_OUTPUT, "disp0_rdma0_output", "disp_sel", 15),
	GATE_DISP0(CLK_DISP0_RDMA1_ENGINE, "disp0_rdma1_engine", "disp_sel", 16),
	GATE_DISP0(CLK_DISP0_RDMA1_SMI, "disp0_rdma1_smi", "disp_sel", 17),
	GATE_DISP0(CLK_DISP0_RDMA1_OUTPUT, "disp0_rdma1_output", "disp_sel", 18),
	GATE_DISP0(CLK_DISP0_GAMMA_ENGINE, "disp0_gamma_engine", "disp_sel", 19),
	GATE_DISP0(CLK_DISP0_GAMMA_PIXEL, "disp0_gamma_pixel", "disp_sel", 20),
	GATE_DISP0(CLK_DISP0_CMDQ_ENGINE, "disp0_cmdq_engine", "disp_sel", 21),
	GATE_DISP0(CLK_DISP0_CMDQ_SMI, "disp0_cmdq_smi", "disp_sel", 22),
	GATE_DISP0(CLK_DISP0_G2D_ENGINE, "disp0_g2d_engine", "disp_sel", 23),
	GATE_DISP0(CLK_DISP0_G2D_SMI, "disp0_g2d_smi", "disp_sel", 24),

	GATE_DISP1(CLK_DISP1_DBI_ENGINE, "disp1_dbi_engine", "disp_sel", 0),
	GATE_DISP1(CLK_DISP1_DBI_SMI, "disp1_dbi_smi", "disp_sel", 1),
	GATE_DISP1(CLK_DISP1_DBI_OUTPUT, "disp1_dbi_output", "disp_sel", 2),
	GATE_DISP1(CLK_DISP1_DSI_ENGINE, "disp1_dsi_engine", "disp_sel", 3),
	GATE_DISP1(CLK_DISP1_DSI_DIGITAL, "disp1_dsi_digital", "disp_sel", 4),
	GATE_DISP1(CLK_DISP1_DSI_DIGITAL_LANE, "disp1_dsi_digital_lane", "disp_sel", 5),
	GATE_DISP1(CLK_DISP1_DPI0, "disp1_dpi0", "disp_sel", 6),
	GATE_DISP1(CLK_DISP1_DPI1, "disp1_dpi1", "disp_sel", 7),
	GATE_DISP1(CLK_DISP1_LCD, "disp1_lcd", "disp_sel", 8),
	GATE_DISP1(CLK_DISP1_SLCD, "disp1_slcd", "disp_sel", 9),
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
