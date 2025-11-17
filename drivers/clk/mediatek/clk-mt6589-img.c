// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

static const struct mtk_gate_regs img_cg_regs = {
	.set_ofs = 0x0004,
	.clr_ofs = 0x0008,
	.sta_ofs = 0x0000,
};

#define GATE_IMG(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &img_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate img_clks[] = {
	// GATE_IMG(CLK_IMAGE_LARB3_SMI, "", "", 0),
	// GATE_IMG(CLK_IMAGE_LARB4_SMI, "", "", 2),
	// GATE_IMG(CLK_IMAGE_COMMN_SMI, "", "", 4),
	// GATE_IMG(CLK_IMAGE_CAM_SMI, "", "", 5),
	// GATE_IMG(CLK_IMAGE_CAM_CAM, "", "", 6),
	// GATE_IMG(CLK_IMAGE_SEN_TG, "", "", 7),
	// GATE_IMG(CLK_IMAGE_SEN_CAM, "", "", 8),
	// GATE_IMG(CLK_IMAGE_JPGD_SMI, "", "", 9),
	// GATE_IMG(CLK_IMAGE_JPGD_JPG, "", "", 10),
	// GATE_IMG(CLK_IMAGE_JPGE_SMI, "", "", 11),
	// GATE_IMG(CLK_IMAGE_JPGE_JPG, "", "", 12),
	// GATE_IMG(CLK_IMAGE_FPC, "", "", 13),
};

static const struct mtk_clk_desc img_desc = {
	.clks = img_clks,
	.num_clks = ARRAY_SIZE(img_clks),
};

static const struct of_device_id of_match_clk_mt6589_img[] = {
	{
		.compatible = "mediatek,mt6589-imgsys",
		.data = &img_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_img);

static struct platform_driver clk_mt6589_img_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-img",
		.of_match_table = of_match_clk_mt6589_img,
	},
};
module_platform_driver(clk_mt6589_img_drv);

MODULE_DESCRIPTION("MediaTek MT6589 imgsys clocks driver");
MODULE_LICENSE("GPL");
