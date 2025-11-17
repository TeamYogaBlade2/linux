// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

static const struct mtk_gate_regs aud_cg_regs = {
	.sta_ofs = 0x0000,
};

#define GATE_AUDIO(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &aud_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate aud_clks[] = {
	// GATE_AUDIO(CLK_AUDIO_AFE, "audio_afe", "", <TODO>),
	// GATE_AUDIO(CLK_AUDIO_I2S, "audio_i2s", "", <TODO>),
};

static const struct mtk_clk_desc aud_desc = {
	.clks = aud_clks,
	.num_clks = ARRAY_SIZE(aud_clks),
};

static const struct of_device_id of_match_clk_mt6589_aud[] = {
	{
		.compatible = "mediatek,mt6589-audsys",
		.data = &aud_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_aud);

static struct platform_driver clk_mt6589_aud_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-aud",
		.of_match_table = of_match_clk_mt6589_aud,
	},
};
module_platform_driver(clk_mt6589_aud_drv);

MODULE_DESCRIPTION("MediaTek MT6589 audio clocks driver");
MODULE_LICENSE("GPL");
