// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include "pinctrl-paris.h"
#include "pinctrl-mtk-mt6589.h"

/* GPIO0 */
#define DRV_CON0	0x0500
#define DRV_CON1	0x0510
#define DRV_CON2	0x0520
#define DRV_CON3	0x0530
#define DRV_CON4	0x0540
#define DRV_CON5	0x0550

/* GPIO1 */
#define DRV_CON6	0x0560
#define DRV_CON7	0x0570
#define DRV_CON8	0x0580
#define DRV_CON9	0x0590

/* GPIO0 */
#define DRV_CON10	0x05a0
#define DRV_CON11	0x05b0
#define DRV_CON12	0x05c0

static const struct mtk_pin_field_calc mt6589_pin_mode_range[] = {
	PIN_FIELD_CALC(0, 43, 0, 0x0c00, 0x10, 0, 3, 16, 0),
	PIN_FIELD_CALC(44, 46, 0, 0x0980, 0x10, 0, 4, 16, 0),
	PIN_FIELD_CALC(47, 49, 0, 0x09a0, 0x10, 0, 4, 16, 0),
	PIN_FIELD_CALC(50, 231, 0, 0x0ca0, 0x10, 0, 3, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_dir_range[] = {
	PIN_FIELD_CALC(0, 231, 0, 0x0000, 0x10, 0, 1, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_di_range[] = {
	PIN_FIELD_CALC(0, 231, 0, 0x0800, 0x10, 0, 1, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_do_range[] = {
	PIN_FIELD_CALC(0, 231, 0, 0x0a00, 0x10, 0, 1, 16, 0),
};



static const struct mtk_pin_field_calc mt6589_pin_smt_range[] = {
	PIN_FIELD_CALC(0, 113, 0, 0x0300, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(114, 169, 1, 0x0370, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x03a0, 0x10, 10, 1, 16, 0),
};


static const struct mtk_pin_field_calc mt6589_pin_ies_range[] = {
	PIN_FIELD_CALC(0, 113, 0, 0x0100, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(114, 169, 1, 0x0170, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x01a0, 0x10, 10, 1, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_pullen_range[] = {
	PIN_FIELD_CALC(0, 113, 0, 0x0200, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(114, 169, 1, 0x0270, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x02a0, 0x10, 10, 1, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_pullsel_range[] = {
	PIN_FIELD_CALC(0, 113, 0, 0x0400, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(114, 169, 1, 0x0470, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x04a0, 0x10, 10, 1, 16, 0),
};

static const struct mtk_pin_reg_calc mt6589_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6589_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6589_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6589_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6589_pin_do_range),
//	[PINCTRL_PIN_REG_SR] = MTK_RANGE(),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt6589_pin_smt_range),
//	[PINCTRL_PIN_REG_PD] = no
//	[PINCTRL_PIN_REG_PU] = no
//	[PINCTRL_PIN_REG_E4] = MTK_RANGE(),
//	[PINCTRL_PIN_REG_E8] = MTK_RANGE(),
//	[PINCTRL_PIN_REG_TDSEL] = MTK_RANGE(), // 0x0700 DSEL
//	[PINCTRL_PIN_REG_RDSEL] = MTK_RANGE(), // 0x0700 DSEL
//	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(),
//	[PINCTRL_PIN_REG_PUPD] = MTK_RANGE(), // yes
//	[PINCTRL_PIN_REG_R0] = MTK_RANGE(), // 0x04f0 MSDC_R0
//	[PINCTRL_PIN_REG_R1] = no
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt6589_pin_ies_range),
	[PINCTRL_PIN_REG_PULLEN] = MTK_RANGE(mt6589_pin_pullen_range),
	[PINCTRL_PIN_REG_PULLSEL] = MTK_RANGE(mt6589_pin_pullsel_range),
//	[PINCTRL_PIN_REG_DRV_EN] = MTK_RANGE(),
//	[PINCTRL_PIN_REG_DRV_E0] = not adv
//	[PINCTRL_PIN_REG_DRV_E1] = not adv
//	[PINCTRL_PIN_REG_DRV_ADV] = not adv
//	[PINCTRL_PIN_REG_RSEL] = MTK_RANGE(),
};

// DINV, BIAS

static const char * const mt6589_pinctrl_register_base_names[] = {
	"gpio", "gpio1",
};

static const struct mtk_eint_hw mt6589_eint_hw = {
	.port_mask = 7,
	.ports     = 6,
	.ap_num    = 192,
	.db_cnt    = 16,
	.db_time   = debounce_time_mt6795,
};

static const struct mtk_pin_soc mt6589_pinctrl_data = {
	.reg_cal = mt6589_reg_cals,
	.pins = mtk_pins_mt6589,
	.npins = ARRAY_SIZE(mtk_pins_mt6589),
//	.grps = noneed
	.ngrps = ARRAY_SIZE(mtk_pins_mt6589),
//	.funcs = noneed
//	.nfuncs = noneed
//	.eint_regs = noneed
	.eint_hw = &mt6589_eint_hw,
//	.eint_pin = noneed
	.gpio_m = 0,
//	.ies_present = noneed
	.base_names = mt6589_pinctrl_register_base_names,
	.nbase_names = ARRAY_SIZE(mt6589_pinctrl_register_base_names),
//	.pull_type =
//	.pin_rsel =
//	.npin_rsel =
//	.bias_disable_set =
//	.bias_disable_get =
//	.bias_set =
//	.bias_get =
//	.bias_set_combo =
//	.bias_get_combo =
//	.drive_set =
//	.drive_get =
//	.adv_pull_set =
//	.adv_pull_get =
//	.adv_drive_set =
//	.adv_drive_get =
//	.driver_data = noneed
};

static const struct of_device_id mt6589_pinctrl_match[] = {
	{ .compatible = "mediatek,mt6589-pinctrl", .data = &mt6589_pinctrl_data },
	{},
};
MODULE_DEVICE_TABLE(of, mt6589_pinctrl_match);

static struct platform_driver mt6589_pinctrl_driver = {
	.probe = mtk_paris_pinctrl_probe,
	.driver = {
		.name = "mediatek-mt6589-pinctrl",
		.of_match_table = mt6589_pinctrl_match,
	},
};

static int __init mt6589_pinctrl_init(void)
{
	return platform_driver_register(&mt6589_pinctrl_driver);
}
arch_initcall(mt6589_pinctrl_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek MT6589 Pinctrl Driver");
