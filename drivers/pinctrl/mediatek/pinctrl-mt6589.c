// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: Biao Huang <biao.huang@mediatek.com>
 *
 * Author: akku <akkun11.open@gmail.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "pinctrl-mtk-common.h"
#include "pinctrl-mtk-mt6589.h"

static const struct mtk_pinctrl_devdata mt6589_pinctrl_data = {
	.pins = mtk_pins_mt6589,
	.npins = ARRAY_SIZE(mtk_pins_mt6589),
	// .grp_desc = mt2701_drv_grp,
	// .n_grp_cls = ARRAY_SIZE(mt2701_drv_grp),
	// .pin_drv_grp = mt2701_pin_drv,
	// .n_pin_drv_grps = ARRAY_SIZE(mt2701_pin_drv),
	// .spec_ies = mt2701_ies_set,
	// .n_spec_ies = ARRAY_SIZE(mt2701_ies_set),
	// .spec_pupd = mt2701_spec_pupd,
	// .n_spec_pupd = ARRAY_SIZE(mt2701_spec_pupd),
	// .spec_smt = mt2701_smt_set,
	// .n_spec_smt = ARRAY_SIZE(mt2701_smt_set),
	// .spec_pull_set = mtk_pctrl_spec_pull_set_samereg,
	// .spec_ies_smt_set = mtk_pconf_spec_set_ies_smt_range,
	// .spec_pinmux_set = mt2701_spec_pinmux_set,
	// .spec_dir_set = mt2701_spec_dir_set,
	// .dir_offset = 0x0000,
	// .pullen_offset = 0x0150,
	// .pullsel_offset = 0x0280,
	// .dout_offset = 0x0500,
	// .din_offset = 0x0630,
	// .pinmux_offset = 0x0760,
	// .type1_start = 280,
	// .type1_end = 280,
	// .port_shf = 4,
	// .port_mask = 0x1f,
	// .port_align = 4,
	// .mode_mask = 0xf,
	// .mode_per_reg = 5,
	// .mode_shf = 4,
	// .eint_hw = {
	// 	.port_mask = 6,
	// 	.ports     = 6,
	// 	.ap_num    = 169,
	// 	.db_cnt    = 16,
	// 	.db_time   = debounce_time_mt2701,
	// },
};

static const struct of_device_id mt6589_pctrl_match[] = {
	{ .compatible = "mediatek,mt6589-pinctrl", .data = &mt6589_pinctrl_data },
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
