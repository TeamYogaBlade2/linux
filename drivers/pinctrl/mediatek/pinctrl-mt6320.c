// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: Hongzhou.Yang <hongzhou.yang@mediatek.com>
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * MediaTek MT6320 PMIC Pinctrl Driver based on pinctrl-mt6397.c
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/mfd/mt6397/core.h>

#include "pinctrl-mtk-common.h"
#include "pinctrl-mtk-mt6320.h"

#define MT6320_PIN_REG_BASE  0xc000

static const struct mtk_pinctrl_devdata mt6320_pinctrl_data = {
	.pins = mtk_pins_mt6320,
	.npins = ARRAY_SIZE(mtk_pins_mt6320),
	.dir_offset = (MT6320_PIN_REG_BASE + 0x000),
	.ies_offset = MTK_PINCTRL_NOT_SUPPORT,
	.smt_offset = MTK_PINCTRL_NOT_SUPPORT,
	.pullen_offset = (MT6320_PIN_REG_BASE + 0x020),
	.pullsel_offset = (MT6320_PIN_REG_BASE + 0x040),
	.dout_offset = (MT6320_PIN_REG_BASE + 0x080),
	.din_offset = (MT6320_PIN_REG_BASE + 0x0a0),
	.pinmux_offset = (MT6320_PIN_REG_BASE + 0x0c0),
	.type1_start = 49,
	.type1_end = 49,
	.port_shf = 3,
	.port_mask = 0x3,
	.port_align = 2,
	.mode_mask = 0xf,
	.mode_per_reg = 5,
	.mode_shf = 4,
};

static int mt6320_pinctrl_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6320;

	mt6320 = dev_get_drvdata(pdev->dev.parent);
	return mtk_pctrl_init(pdev, &mt6320_pinctrl_data, mt6320->regmap);
}

static const struct of_device_id mt6320_pctrl_match[] = {
	{ .compatible = "mediatek,mt6320-pinctrl", },
	{ },
};

static struct platform_driver mtk_pinctrl_driver = {
	.probe = mt6320_pinctrl_probe,
	.driver = {
		.name = "mediatek-mt6320-pinctrl",
		.of_match_table = mt6320_pctrl_match,
	},
};

builtin_platform_driver(mtk_pinctrl_driver);
