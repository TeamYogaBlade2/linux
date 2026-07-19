// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6582-clk.h>

static const struct mtk_gate_regs img_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_IMG(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &img_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate img_clks[] = {
	//GATE_DUMMY(CLK_DUMMY, "img_dummy"),
    	GATE_IMG(CLK_IMG_LARB2_SMI, "img_larb2_smi", "mm_sel", 0),
	GATE_IMG(CLK_IMG_SMI, "img_smi", "mm_sel", 5),
	GATE_IMG(CLK_IMG_CAM, "img_cam", "mm_sel", 6),
	GATE_IMG(CLK_IMG_SEN_TG, "img_sen_tg", "mm_sel", 7),
	GATE_IMG(CLK_IMG_SEN_CAM, "img_sen_cam", "mm_sel", 8),
	GATE_IMG(CLK_IMG_VENC_JPEGENC, "img_venc_jpegenc", "mm_sel", 9),
};

static const struct mtk_clk_desc img_desc = {
	.clks = img_clks,
	.num_clks = ARRAY_SIZE(img_clks),
};

static const struct of_device_id of_match_clk_mt6582_img[] = {
	{
		.compatible = "mediatek,mt6582-imgsys",
		.data = &img_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6582_img);

static struct platform_driver clk_mt6582_img_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6582-img",
		.of_match_table = of_match_clk_mt6582_img,
	},
};
module_platform_driver(clk_mt6582_img_drv);

MODULE_DESCRIPTION("MediaTek MT6582 imgsys clocks driver");
MODULE_LICENSE("GPL");
