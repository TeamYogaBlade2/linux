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

static const struct mtk_pin_soc mt6589_pinctrl_data = {
	// .reg_cal = mt6589_reg_cals,
	.pins = mtk_pins_mt6589,
	.npins = ARRAY_SIZE(mtk_pins_mt6589),
	.ngrps = ARRAY_SIZE(mtk_pins_mt6589),
	// .eint_hw = &mt6589_eint_hw,
	// .gpio_m = 0,
	// .ies_present = true,
	// .base_names = mt6589_pinctrl_register_base_names,
	// .nbase_names = ARRAY_SIZE(mt6589_pinctrl_register_base_names),
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
