// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2026 Roman Vivchar <rva333@protonmail.com>
 */

#include <linux/module.h>
#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6572-clk.h>

static const struct mtk_gate_regs audio_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x0,
	.sta_ofs = 0x0,
};

#define GATE_AUDIO(_id, _name, _parent, _shift)               \
	GATE_MTK(_id, _name, _parent, &audio_cg_regs, _shift, \
		 &mtk_clk_gate_ops_no_setclr)

static const struct mtk_gate audio_clks[] = {
	/* AUDIO0 */
	GATE_AUDIO(CLK_AUD_AFE, "aud_afe", "axi_sel", 2),
	GATE_AUDIO(CLK_AUD_I2S, "aud_i2s", "axi_sel", 8),
	GATE_AUDIO(CLK_AUD_ADC, "aud_adc", "axi_sel", 24),
	GATE_AUDIO(CLK_AUD_DAC, "aud_dac", "axi_sel", 25),
	GATE_AUDIO(CLK_AUD_DAC_PREDIS, "aud_dac_predis", "axi_sel", 26),
	GATE_AUDIO(CLK_AUD_TML, "aud_tml", "axi_sel", 27),
};

static const struct mtk_clk_desc audio_desc = {
	.clks = audio_clks,
	.num_clks = ARRAY_SIZE(audio_clks),
};

static const struct of_device_id of_match_clk_mt6572_aud[] = {
	{ .compatible = "mediatek,mt6572-audsys", .data = &audio_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6572_aud);

static struct platform_driver clk_mt6572_aud_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6572-aud",
		.of_match_table = of_match_clk_mt6572_aud,
	},
};
module_platform_driver(clk_mt6572_aud_drv);

MODULE_DESCRIPTION("MediaTek MT6572 audio clocks driver");
MODULE_LICENSE("GPL");
