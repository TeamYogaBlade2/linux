// SPDX-License-Identifier: GPL-2.0+

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

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
	GATE_MM0(CLK_MM_SMI_COMMON, "mm_smi_common", "mm_smi_src", 0),
	GATE_MM0(CLK_MM_SMI_LARB0, "mm_smi_larb0", "mm_smi_src", 1),
	GATE_MM0(CLK_MM_CMDQ, "mm_cmdq", "mm_smi_src", 2),
	GATE_MM0(CLK_MM_SMI_CMDQ, "mm_smi_cmdq", "mm_smi_src", 3),
	GATE_MM0(CLK_MM_DISP_COLOR, "mm_disp_color", "mm_smi_src", 4),
	GATE_MM0(CLK_MM_DISP_BLS, "mm_disp_bls", "mm_smi_src", 5),
	GATE_MM0(CLK_MM_DISP_WDMA, "mm_disp_wdma", "mm_smi_src", 6),
	GATE_MM0(CLK_MM_DISP_RDMA, "mm_disp_rdma", "mm_smi_src", 7),
	GATE_MM0(CLK_MM_DISP_OVL, "mm_disp_ovl", "mm_smi_src", 8),
	GATE_MM0(CLK_MM_DISP_MDP_TDSHP, "mm_mdp_tdshp", "mm_smi_src", 9),
	GATE_MM0(CLK_MM_DISP_MDP_WROT, "mm_mdp_wrot", "mm_smi_src", 10),
	GATE_MM0(CLK_MM_DISP_MDP_WDMA, "mm_mdp_wdma", "mm_smi_src", 11),
	GATE_MM0(CLK_MM_DISP_MDP_RSZ1, "mm_mdp_rsz1", "mm_smi_src", 12),
	GATE_MM0(CLK_MM_DISP_MDP_RSZ0, "mm_mdp_rsz0", "mm_smi_src", 13),
	GATE_MM0(CLK_MM_DISP_MDP_RDMA, "mm_mdp_rdma", "mm_smi_src", 14),
	GATE_MM0(CLK_MM_DISP_MDP_BLS_26M, "mm_mdp_bls_26m", "mm_smi_src", 15),
	GATE_MM0(CLK_MM_CAM, "mm_cam", "mm_smi_src", 16),
	GATE_MM0(CLK_MM_SENINF, "mm_seninf", "mm_smi_src", 17),
	GATE_MM0(CLK_MM_CAMTG, "mm_camtg", "mm_smi_src", 18),
	GATE_MM0(CLK_MM_CODEC, "mm_codec", "mm_smi_src", 19),
	GATE_MM0(CLK_MM_DISP_FAKE_ENG, "mm_disp_fake_eng", "mm_smi_src", 20),
	GATE_MM0(CLK_MM_MUTEX_SLOW_CLOCK, "mm_mutex_slow_clock", "mm_smi_src", 21),

	GATE_MM1(CLK_MM_DSI_ENGINE, "dsi_engine", "mm_smi_src", 0),
	GATE_MM1(CLK_MM_DSI_DIGITAL, "dsi_digital", "mm_smi_src", 1),
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
