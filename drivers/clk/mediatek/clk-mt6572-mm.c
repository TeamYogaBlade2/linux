// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2025-2026 Roman Vivchar <rva333@protonmail.com>
 */

#include "clk-mtk.h"
#include "clk-gate.h"

#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mediatek,mt6572-clk.h>

static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
	.sta_ofs = 0x110,
};

#define GATE_MM0(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &mm0_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr)

#define GATE_MM1(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &mm1_cg_regs, _shift,	\
		&mtk_clk_gate_ops_setclr)


static const struct mtk_gate mm_clks[] = {
	GATE_MM0(CLK_MM_SMI_COMMON, "mm_smi_common", "smi_mm", 0),
	GATE_MM0(CLK_MM_SMI_LARB0, "mm_smi_larb0", "smi_mm", 1),
	GATE_MM0(CLK_MM_CMDQ, "mm_cmdq", "smi_mm", 2),
	GATE_MM0(CLK_MM_SMI_CMDQ, "mm_smi_cmdq", "smi_mm", 3),
	GATE_MM0(CLK_MM_DISP_COLOR, "mm_disp_color", "smi_mm", 4),
	GATE_MM0(CLK_MM_DISP_BLS, "mm_disp_bls", "smi_mm", 5),
	GATE_MM0(CLK_MM_DISP_WDMA, "mm_disp_wdma", "smi_mm", 6),
	GATE_MM0(CLK_MM_DISP_RDMA, "mm_disp_rdma", "smi_mm", 7),
	GATE_MM0(CLK_MM_DISP_OVL, "mm_disp_ovl", "smi_mm", 8),
	GATE_MM0(CLK_MM_DISP_MDP_TDSHP, "mm_mdp_tdshp", "smi_mm", 9),
	GATE_MM0(CLK_MM_DISP_MDP_WROT, "mm_mdp_wrot", "smi_mm", 10),
	GATE_MM0(CLK_MM_DISP_MDP_WDMA, "mm_mdp_wdma", "smi_mm", 11),
	GATE_MM0(CLK_MM_DISP_MDP_RSZ1, "mm_mdp_rsz1", "smi_mm", 12),
	GATE_MM0(CLK_MM_DISP_MDP_RSZ0, "mm_mdp_rsz0", "smi_mm", 13),
	GATE_MM0(CLK_MM_DISP_MDP_RDMA, "mm_mdp_rdma", "smi_mm", 14),
	GATE_MM0(CLK_MM_DISP_MDP_BLS_26M, "mm_mdp_bls_26m", "clk26m", 15),
	GATE_MM0(CLK_MM_CAM, "mm_cam", "smi_mm", 16),
	GATE_MM0(CLK_MM_SENINF, "mm_seninf", "smi_mm", 17),
	GATE_MM0(CLK_MM_CAMTG, "mm_camtg", "cam_mm", 18),
	GATE_MM0(CLK_MM_CODEC, "mm_codec", "smi_mm", 19),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG, "mm_disp_fake_eng", "smi_mm", 20),
	GATE_MM0(CLK_MM_MUTEX_SLOW_CLOCK, "mm_mutex_slow_clock", "clk32k", 21),

	GATE_MM1(CLK_MM_DSI_ENGINE, "mm_dsi_engine", "smi_mm", 0),
	GATE_MM1(CLK_MM_DSI_DIGITAL, "mm_dsi_digital", "mipi_tx0_pll", 1),
	GATE_MM1(CLK_MM_DPI_ENGINE, "mm_dpi_engine", "smi_mm", 2),
	GATE_MM1(CLK_MM_DPI_IF, "mm_dpi_if", "mipi_tx0_pll", 3),
	GATE_MM1(CLK_MM_DBI_ENGINE, "mm_dbi_engine", "smi_mm", 4),
	GATE_MM1(CLK_MM_DBI_SMI, "mm_dbi_smi", "smi_mm", 5),
	GATE_MM1(CLK_MM_DBI_IF, "mm_dbi_if", "dbi_bclk", 6),
};

static const struct mtk_clk_desc mm_desc = {
	.clks = mm_clks,
	.num_clks = ARRAY_SIZE(mm_clks),
};

static const struct platform_device_id clk_mt6572_mm_id_table[] = {
	{ .name = "clk-mt6572-mm", .driver_data = (kernel_ulong_t)&mm_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt6572_mm_id_table);

static struct platform_driver clk_mt6572_mm = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt6572-mm",
	},
	.id_table = clk_mt6572_mm_id_table,
};
module_platform_driver(clk_mt6572_mm);

MODULE_DESCRIPTION("MediaTek MT6572 MultiMedia clock driver");
MODULE_LICENSE("GPL");
