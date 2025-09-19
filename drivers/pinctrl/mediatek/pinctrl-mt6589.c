// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "pinctrl-mtk-common.h"

static const struct of_device_id mt6589_pctrl_match[] = {
	// { .compatible = "mediatek,mt6589-pinctrl", .data = &mt6589_pinctrl_data },
	{}
};
MODULE_DEVICE_TABLE(of, mt6589_pctrl_match);

static struct platform_driver mtk_pinctrl_driver = {
	.probe = mtk_pctrl_common_probe,
	.driver = {
		.name = "mediatek-mt6589-pinctrl",
		.of_match_table = mt6589_pctrl_match,
		.pm = pm_sleep_ptr(&mtk_eint_pm_ops),
	},
};

static int __init mtk_pinctrl_init(void)
{
	return platform_driver_register(&mtk_pinctrl_driver);
}
arch_initcall(mtk_pinctrl_init);
