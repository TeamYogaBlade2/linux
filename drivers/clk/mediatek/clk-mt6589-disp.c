// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Akari Tsuyukusa <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6589-clk.h>

#define DISP_CG_CON0	0x0100
#define DISP_CG_SET0	0x0104
#define DISP_CG_CLR0	0x0108

#define DISP_CG_CON1	0x0110
#define DISP_CG_SET1	0x0114
#define DISP_CG_CLR1	0x0118

#define DISP_SW_RST_B	0x0140

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
	GATE_MTK(_id, _name, _parent, &disp0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_DISP1(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &disp1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate disp_clks[] = {
	GATE_DISP0(CLK_DISP0_LARB2_SMI, "disp0_larb2_smi", "smi_sel", 0), /* mt8135 */
	GATE_DISP0(CLK_DISP0_ROT_ENGINE, "disp0_rot_engine", "disp_sel", 1),
	GATE_DISP0(CLK_DISP0_ROT_SMI, "disp0_rot_smi", "smi_sel", 2), /* mt8135 */
	GATE_DISP0(CLK_DISP0_SCL, "disp0_scl", "disp_sel", 3),
	GATE_DISP0(CLK_DISP0_OVL_ENGINE, "disp0_ovl_engine", "disp_sel", 4),
	GATE_DISP0(CLK_DISP0_OVL_SMI, "disp0_ovl_smi", "smi_sel", 5), /* mt8135 */
	GATE_DISP0(CLK_DISP0_COLOR, "disp0_color", "disp_sel", 6),
	GATE_DISP0(CLK_DISP0_2DSHP, "disp0_2dshp", "disp_sel", 7),
	GATE_DISP0(CLK_DISP0_BLS, "disp0_bls", "disp_sel", 8),
	GATE_DISP0(CLK_DISP0_WDMA0_ENGINE, "disp0_wdma0_engine", "disp_sel", 9),
	GATE_DISP0(CLK_DISP0_WDMA0_SMI, "disp0_wdma0_smi", "smi_sel", 10), /* mt8135 */
	GATE_DISP0(CLK_DISP0_WDMA1_ENGINE, "disp0_wdma1_engine", "disp_sel", 11),
	GATE_DISP0(CLK_DISP0_WDMA1_SMI, "disp0_wdma1_smi", "smi_sel", 12), /* mt8135 */
	GATE_DISP0(CLK_DISP0_RDMA0_ENGINE, "disp0_rdma0_engine", "disp_sel", 13),
	GATE_DISP0(CLK_DISP0_RDMA0_SMI, "disp0_rdma0_smi", "smi_sel", 14), /* mt8135 */
	GATE_DISP0(CLK_DISP0_RDMA0_OUTPUT, "disp0_rdma0_output", "clk_null", 15), /* mt8135 */
	GATE_DISP0(CLK_DISP0_RDMA1_ENGINE, "disp0_rdma1_engine", "disp_sel", 16),
	GATE_DISP0(CLK_DISP0_RDMA1_SMI, "disp0_rdma1_smi", "smi_sel", 17), /* mt8135 */
	GATE_DISP0(CLK_DISP0_RDMA1_OUTPUT, "disp0_rdma1_output", "clk_null", 18), /* mt8135 */
	GATE_DISP0(CLK_DISP0_GAMMA_ENGINE, "disp0_gamma_engine", "disp_sel", 19),
	GATE_DISP0(CLK_DISP0_GAMMA_PIXEL, "disp0_gamma_pixel", "clk_null", 20), /* mt8135 */
	GATE_DISP0(CLK_DISP0_CMDQ_ENGINE, "disp0_cmdq_engine", "disp_sel", 21),
	GATE_DISP0(CLK_DISP0_CMDQ_SMI, "disp0_cmdq_smi", "smi_sel", 22), /* mt8135 */
	GATE_DISP0(CLK_DISP0_G2D_ENGINE, "disp0_g2d_engine", "disp_sel", 23),
	GATE_DISP0(CLK_DISP0_G2D_SMI, "disp0_g2d_smi", "smi_sel", 24), /* mt8135 */

	GATE_DISP1(CLK_DISP1_DBI_ENGINE, "disp1_dbi_engine", "disp_sel", 0),
	GATE_DISP1(CLK_DISP1_DBI_SMI, "disp1_dbi_smi", "smi_sel", 1), /* maybe */
	GATE_DISP1(CLK_DISP1_DBI_OUTPUT, "disp1_dbi_output", "disp_sel", 2),
	GATE_DISP1(CLK_DISP1_DSI_ENGINE, "disp1_dsi_engine", "disp_sel", 3),
	GATE_DISP1(CLK_DISP1_DSI_DIGITAL, "disp1_dsi_digital", "disp_sel", 4),
	GATE_DISP1(CLK_DISP1_DSI_DIGITAL_LANE, "disp1_dsi_digital_lane", "disp_sel", 5),
	GATE_DISP1(CLK_DISP1_DPI0, "disp1_dpi0", "disp_sel", 6),
	GATE_DISP1(CLK_DISP1_DPI1, "disp1_dpi1", "disp_sel", 7),
	GATE_DISP1(CLK_DISP1_LCD, "disp1_lcd", "disp_sel", 8),
	GATE_DISP1(CLK_DISP1_SLCD, "disp1_slcd", "disp_sel", 9),
};

static u16 disp_rst_ofs[] = { DISP_SW_RST_B };

static const struct mtk_clk_rst_desc disp_clk_rst_desc = {
	.version = MTK_RST_SIMPLE_RSTB,
	.rst_bank_ofs = disp_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(disp_rst_ofs),
};

static const struct mtk_clk_desc disp_desc = {
	.clks = disp_clks,
	.num_clks = ARRAY_SIZE(disp_clks),
	.rst_desc = &disp_clk_rst_desc,
};

static const struct platform_device_id clk_mt6589_disp_id_table[] = {
	{ .name = "clk-mt6589-disp", .driver_data = (kernel_ulong_t)&disp_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt6589_disp_id_table);

static struct platform_driver clk_mt6589_disp_drv = {
	.id_table = clk_mt6589_disp_id_table,
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt6589-disp",
	},
};
module_platform_driver(clk_mt6589_disp_drv);

MODULE_DESCRIPTION("MediaTek MT6589 display clocks driver");
MODULE_LICENSE("GPL");
