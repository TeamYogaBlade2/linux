// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */

#include <linux/module.h>
#include "pinctrl-mtk-mt6589.h"
#include "pinctrl-paris.h"

/*
 * GPIO_BASE: 0xF0005000
 * GPIO1_BASE: 0xF020C000
 * GPIOEXT_BASE: (0xC000) (PMIC GPIO base.)
 */

#define PIN_FIELD_BASE(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs, _s_bit, _x_bits, 16, 0)

#define PINS_FIELD_BASE(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs, _s_bit, _x_bits, 32, 1)

#define MODE_FIELD_BASE(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, _i_base, _s_addr, _x_addrs, _s_bit, _x_bits, 5, 0)

static const struct mtk_pin_field_calc mt6589_pin_mode_range[] = {
	MODE_FIELD_BASE(0, 43, 0, 0x0C00, 0x10, 0, 3),
	PINS_FIELD_BASE(44, 46, 0, 0x980, 0x00, 0, 4),
	PINS_FIELD_BASE(47, 49, 0, 0x9A0, 0x00, 4, 4),
	MODE_FIELD_BASE(50, 113, 0, 0x0C00 + (50/5)*0x10, 0x10, 0, 3),
	MODE_FIELD_BASE(114, 169, 1, 0x0C00, 0x10, 0, 3),
	MODE_FIELD_BASE(170, 231, 0, 0x0C00 + (170/5)*0x10, 0x10, 0, 3),
};

static const struct mtk_pin_field_calc mt6589_pin_dir_range[] = {
	PIN_FIELD_BASE(0, 113, 0, 0x0000, 0x10, 0, 1),
	PIN_FIELD_BASE(114, 169, 1, 0x0000, 0x10, 0, 1),
	PIN_FIELD_BASE(170, 231, 0, 0x0000 + (170/16)*0x10, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6589_pin_di_range[] = {
	PIN_FIELD_BASE(0, 43, 0, 0x0600, 0x10, 0, 1),
	PINS_FIELD_BASE(44, 46, 0, 0x990, 0x00, 0, 1),
	PINS_FIELD_BASE(47, 49, 0, 0x9B0, 0x00, 0, 1),
	PIN_FIELD_BASE(50, 231, 0, 0x0600 + (50/16)*0x10, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6589_pin_do_range[] = {
	PIN_FIELD_BASE(0, 231, 0, 0x0800, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6589_pin_ies_range[] = {
	PIN_FIELD_BASE(0, 113, 0, 0x0100, 0x10, 0, 1),
	PIN_FIELD_BASE(114, 169, 1, 0x0100, 0x10, 0, 1),
	PIN_FIELD_BASE(170, 231, 0, 0x0100 + (170/16)*0x10, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6589_pin_smt_range[] = {
};

static const struct mtk_pin_field_calc mt6589_pin_pu_range[] = {
	PIN_FIELD_BASE(0, 43, 0, 0x0200, 0x10, 0, 1),
	PINS_FIELD_BASE(44, 46, 0, 0x990, 0x00, 4, 1),
	PINS_FIELD_BASE(47, 49, 0, 0x9B0, 0x00, 4, 1),
	PIN_FIELD_BASE(50, 113, 0, 0x0200 + (50/16)*0x10, 0x10, 0, 1),
	PIN_FIELD_BASE(114, 169, 1, 0x0200, 0x10, 0, 1),
	PIN_FIELD_BASE(170, 231, 0, 0x0200 + (170/16)*0x10, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6589_pin_pd_range[] = {
	PIN_FIELD_BASE(0, 43, 0, 0x0400, 0x10, 0, 1),
	PINS_FIELD_BASE(44, 46, 0, 0x990, 0x00, 8, 1),
	PINS_FIELD_BASE(47, 49, 0, 0x9B0, 0x00, 8, 1),
	PIN_FIELD_BASE(50, 113, 0, 0x0400 + (50/16)*0x10, 0x10, 0, 1),
	PIN_FIELD_BASE(114, 169, 1, 0x0400, 0x10, 0, 1),
	PIN_FIELD_BASE(170, 231, 0, 0x0400 + (170/16)*0x10, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6589_pin_drv_range[] = {
};

static const struct mtk_pin_field_calc mt6589_pin_pupd_range[] = {
};

static const struct mtk_pin_field_calc mt6589_pin_r0_range[] = {
};

static const struct mtk_pin_field_calc mt6589_pin_r1_range[] = {
};

static const struct mtk_pin_reg_calc mt6589_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6589_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6589_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6589_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6589_pin_do_range),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt6589_pin_smt_range),
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt6589_pin_ies_range),
	[PINCTRL_PIN_REG_PU] = MTK_RANGE(mt6589_pin_pu_range),
	[PINCTRL_PIN_REG_PD] = MTK_RANGE(mt6589_pin_pd_range),
	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(mt6589_pin_drv_range),
	[PINCTRL_PIN_REG_PUPD] = MTK_RANGE(mt6589_pin_pupd_range),
	[PINCTRL_PIN_REG_R0] = MTK_RANGE(mt6589_pin_r0_range),
	[PINCTRL_PIN_REG_R1] = MTK_RANGE(mt6589_pin_r1_range),
};

static const char * const mt6589_pinctrl_register_base_names[] = {
	"gpio", "gpio1",
};

static const struct mtk_pin_soc mt6589_pinctrl_data = {
	.reg_cal = mt6589_reg_cals,
	.pins = mtk_pins_mt6589,
	.npins = ARRAY_SIZE(mtk_pins_mt6589),
	.ngrps = ARRAY_SIZE(mtk_pins_mt6589),
	// .eint_hw = &mt6589_eint_hw,
	// .gpio_m = 0,
	// .ies_present = true,
	.base_names = mt6589_pinctrl_register_base_names,
	.nbase_names = ARRAY_SIZE(mt6589_pinctrl_register_base_names),
	// .bias_set_combo = mtk_pinconf_bias_set_combo,
	// .bias_get_combo = mtk_pinconf_bias_get_combo,
	// .drive_set = mtk_pinconf_drive_set_raw,
	// .drive_get = mtk_pinconf_drive_get_raw,
	// .adv_pull_get = mtk_pinconf_adv_pull_get,
	// .adv_pull_set = mtk_pinconf_adv_pull_set,
};

static const struct of_device_id mt6589_pinctrl_match[] = {
	{ .compatible = "mediatek,mt6589-pinctrl", .data = &mt6589_pinctrl_data },
	{}
};
MODULE_DEVICE_TABLE(of, mt6589_pctrl_match);

static struct platform_driver mt6589_pinctrl_driver = {
	.probe = mtk_paris_pinctrl_probe,
	.driver = {
		.name = "mediatek-mt6589-pinctrl",
		.of_match_table = mt6589_pinctrl_match,
	},
};

static int __init mtk_pinctrl_init(void)
{
	return platform_driver_register(&mt6589_pinctrl_driver);
}
arch_initcall(mtk_pinctrl_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6589 Pinctrl Driver");
