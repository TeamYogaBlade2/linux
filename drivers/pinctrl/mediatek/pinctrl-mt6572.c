// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 NotMediaTek Inc.
 */

#include "pinctrl-mtk-common-v2.h"
#include <linux/module.h>
#include "pinctrl-mtk-mt6572.h"
#include "pinctrl-paris.h"

/* MT6572 have multiple bases to program pin configuration listed as the below:
 * gpio:0x10005000,     iocfg_t:0x10208000, iocfg_b:0x10209000,
 * iocfg_l:0x1020A000, iocfg_r:0x1020B000,
 * _i_base could be used to indicate what base the pin should be mapped into.
 */

#define PIN_FIELD_BASE(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits) \
	PIN_FIELD_CALC(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits, \
		       32, 0)

#define PINS_FIELD_BASE(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits) \
	PIN_FIELD_CALC(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits,  \
		       32, 1)

static const struct mtk_pin_field_calc mt6572_pin_mode_range[] = {
	PIN_FIELD_BASE(0, 7, 0, 0x0300, 0x10, 0, 4),
	PIN_FIELD_BASE(8, 15, 0, 0x0310, 0x10, 0, 4),
	PIN_FIELD_BASE(16, 23, 0, 0x0320, 0x10, 0, 4),
	PIN_FIELD_BASE(24, 31, 0, 0x0330, 0x10, 0, 4),
	PIN_FIELD_BASE(32, 39, 0, 0x0340, 0x10, 0, 4),
	PIN_FIELD_BASE(40, 47, 0, 0x0350, 0x10, 0, 4),
	PIN_FIELD_BASE(48, 55, 0, 0x0360, 0x10, 0, 4),
	PIN_FIELD_BASE(56, 63, 0, 0x0370, 0x10, 0, 4),
	PIN_FIELD_BASE(64, 71, 0, 0x0380, 0x10, 0, 4),
	PIN_FIELD_BASE(72, 79, 0, 0x0390, 0x10, 0, 4),
	PIN_FIELD_BASE(80, 87, 0, 0x03A0, 0x10, 0, 4),
	PIN_FIELD_BASE(88, 95, 0, 0x03B0, 0x10, 0, 4),
	PIN_FIELD_BASE(96, 103, 0, 0x03C0, 0x10, 0, 4),
	PIN_FIELD_BASE(104, 111, 0, 0x03D0, 0x10, 0, 4),
	PIN_FIELD_BASE(112, 119, 0, 0x03E0, 0x10, 0, 4),
	PIN_FIELD_BASE(120, 127, 0, 0x03F0, 0x10, 0, 4),
	PIN_FIELD_BASE(128, 135, 0, 0x0400, 0x10, 0, 4),
	PIN_FIELD_BASE(136, 143, 0, 0x0410, 0x10, 0, 4),
	PIN_FIELD_BASE(144, 151, 0, 0x0420, 0x10, 0, 4),
	PIN_FIELD_BASE(152, 153, 0, 0x0430, 0x10, 0, 4),
};

static const struct mtk_pin_field_calc mt6572_pin_dir_range[] = {
	PIN_FIELD_BASE(0, 31, 0, 0x0000, 0x10, 0, 1),
	PIN_FIELD_BASE(32, 63, 0, 0x0010, 0x10, 0, 1),
	PIN_FIELD_BASE(64, 95, 0, 0x0020, 0x10, 0, 1),
	PIN_FIELD_BASE(96, 127, 0, 0x0030, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 153, 0, 0x0040, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_di_range[] = {
	PIN_FIELD_BASE(0, 31, 0, 0x0200, 0x10, 0, 1),
	PIN_FIELD_BASE(32, 63, 0, 0x0210, 0x10, 0, 1),
	PIN_FIELD_BASE(64, 95, 0, 0x0220, 0x10, 0, 1),
	PIN_FIELD_BASE(96, 127, 0, 0x0230, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 153, 0, 0x0240, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_do_range[] = {
	PIN_FIELD_BASE(0, 31, 0, 0x0100, 0x10, 0, 1),
	PIN_FIELD_BASE(32, 63, 0, 0x0110, 0x10, 0, 1),
	PIN_FIELD_BASE(64, 95, 0, 0x0120, 0x10, 0, 1),
	PIN_FIELD_BASE(96, 127, 0, 0x0130, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 153, 0, 0x0140, 0x10, 0, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_ies_range[] = {
	PINS_FIELD_BASE(0, 1, 3, 0x0000, 0x10, 0, 1),
	PINS_FIELD_BASE(2, 3, 3, 0x0000, 0x10, 1, 1),
	PINS_FIELD_BASE(4, 6, 3, 0x0000, 0x10, 2, 1),
	PINS_FIELD_BASE(7, 7, 3, 0x0000, 0x10, 4, 1),
	PINS_FIELD_BASE(8, 11, 3, 0x0000, 0x10, 3, 1),
	PINS_FIELD_BASE(12, 13, 3, 0x0000, 0x10, 4, 1),
	PINS_FIELD_BASE(14, 31, 3, 0x0000, 0x10, 5, 1),
	PINS_FIELD_BASE(32, 39, 3, 0x0000, 0x10, 6, 1),
	PINS_FIELD_BASE(40, 43, 2, 0x0000, 0x10, 2, 1),
	PINS_FIELD_BASE(44, 49, 2, 0x0000, 0x10, 0, 1),
	PINS_FIELD_BASE(50, 53, 2, 0x0000, 0x10, 1, 1),
	PINS_FIELD_BASE(54, 54, 2, 0x0000, 0x10, 0, 1),
	PINS_FIELD_BASE(55, 58, 4, 0x0000, 0x10, 0, 1),
	PINS_FIELD_BASE(59, 60, 4, 0x0000, 0x10, 1, 1),
	PINS_FIELD_BASE(61, 62, 4, 0x0000, 0x10, 2, 1),
	PINS_FIELD_BASE(63, 66, 4, 0x0000, 0x10, 3, 1),
	PINS_FIELD_BASE(87, 88, 4, 0x0000, 0x10, 4, 1),
	PINS_FIELD_BASE(89, 90, 4, 0x0000, 0x10, 5, 1),
	PINS_FIELD_BASE(91, 96, 4, 0x0000, 0x10, 6, 1),
	PIN_FIELD_BASE(97, 102, 4, 0x0000, 0x10, 7, 1),
	PINS_FIELD_BASE(103, 104, 4, 0x0000, 0x10, 13, 1),
	PINS_FIELD_BASE(105, 106, 4, 0x0000, 0x10, 14, 1),
	PIN_FIELD_BASE(107, 112, 1, 0x0000, 0x10, 0, 1),
	PINS_FIELD_BASE(113, 114, 1, 0x0000, 0x10, 6, 1),
	PINS_FIELD_BASE(115, 120, 1, 0x0010, 0x10, 0, 1),
	PINS_FIELD_BASE(121, 124, 1, 0x0010, 0x10, 1, 1),
	PINS_FIELD_BASE(126, 127, 1, 0x0010, 0x10, 2, 1),
	PIN_FIELD_BASE(128, 129, 1, 0x0000, 0x10, 13, 1),
	PINS_FIELD_BASE(130, 133, 1, 0x0000, 0x10, 7, 1),
	PINS_FIELD_BASE(134, 136, 1, 0x0000, 0x10, 8, 1),
	PINS_FIELD_BASE(137, 141, 1, 0x0000, 0x10, 9, 1),
	PINS_FIELD_BASE(142, 145, 1, 0x0000, 0x10, 10, 1),
	PINS_FIELD_BASE(146, 148, 1, 0x0000, 0x10, 11, 1),
	PINS_FIELD_BASE(149, 153, 1, 0x0000, 0x10, 12, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_smt_range[] = {
	PINS_FIELD_BASE(0, 1, 3, 0x0020, 0x10, 0, 1),
	PINS_FIELD_BASE(2, 3, 3, 0x0020, 0x10, 1, 1),
	PINS_FIELD_BASE(4, 6, 3, 0x0020, 0x10, 2, 1),
	PINS_FIELD_BASE(7, 7, 3, 0x0020, 0x10, 4, 1),
	PINS_FIELD_BASE(8, 11, 3, 0x0020, 0x10, 3, 1),
	PINS_FIELD_BASE(12, 13, 3, 0x0020, 0x10, 4, 1),
	PINS_FIELD_BASE(14, 31, 3, 0x0020, 0x10, 5, 1),
	PINS_FIELD_BASE(32, 39, 3, 0x0020, 0x10, 6, 1),
	PINS_FIELD_BASE(40, 43, 2, 0x0020, 0x10, 2, 1),
	PINS_FIELD_BASE(44, 49, 2, 0x0020, 0x10, 0, 1),
	PINS_FIELD_BASE(50, 53, 2, 0x0020, 0x10, 1, 1),
	PINS_FIELD_BASE(54, 54, 2, 0x0020, 0x10, 0, 1),
	PINS_FIELD_BASE(55, 58, 4, 0x0020, 0x10, 0, 1),
	PINS_FIELD_BASE(59, 60, 4, 0x0020, 0x10, 1, 1),
	PINS_FIELD_BASE(61, 62, 4, 0x0020, 0x10, 2, 1),
	PINS_FIELD_BASE(63, 66, 4, 0x0020, 0x10, 3, 1),
	PINS_FIELD_BASE(87, 88, 4, 0x0020, 0x10, 4, 1),
	PINS_FIELD_BASE(89, 90, 4, 0x0020, 0x10, 5, 1),
	PINS_FIELD_BASE(91, 96, 4, 0x0020, 0x10, 6, 1),
	PIN_FIELD_BASE(97, 102, 4, 0x0020, 0x10, 7, 1),
	PINS_FIELD_BASE(103, 104, 4, 0x0020, 0x10, 13, 1),
	PINS_FIELD_BASE(105, 106, 4, 0x0020, 0x10, 14, 1),
	PINS_FIELD_BASE(107, 112, 1, 0x0040, 0x10, 0, 1),
	PINS_FIELD_BASE(113, 114, 1, 0x0040, 0x10, 6, 1),
	PINS_FIELD_BASE(115, 120, 1, 0x0050, 0x10, 0, 1),
	PINS_FIELD_BASE(121, 124, 1, 0x0050, 0x10, 1, 1),
	PINS_FIELD_BASE(126, 127, 1, 0x0050, 0x10, 2, 1),
	PIN_FIELD_BASE(128, 129, 1, 0x0040, 0x10, 8, 1),
	PINS_FIELD_BASE(130, 133, 1, 0x0040, 0x10, 2, 1),
	PINS_FIELD_BASE(134, 136, 1, 0x0040, 0x10, 3, 1),
	PINS_FIELD_BASE(137, 141, 1, 0x0040, 0x10, 4, 1),
	PINS_FIELD_BASE(142, 145, 1, 0x0040, 0x10, 5, 1),
	PINS_FIELD_BASE(146, 148, 1, 0x0040, 0x10, 6, 1),
	PINS_FIELD_BASE(149, 153, 1, 0x0040, 0x10, 7, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_pu_pd_range[] = {
	PIN_FIELD_BASE(0, 6, 3, 0x0040, 0x10, 0, 1),
	PIN_FIELD_BASE(7, 7, 3, 0x0040, 0x10, 11, 1),
	PIN_FIELD_BASE(8, 11, 3, 0x0040, 0x10, 7, 1),
	PIN_FIELD_BASE(12, 13, 3, 0x0040, 0x10, 12, 1),
	PIN_FIELD_BASE(14, 39, 3, 0x0050, 0x10, 0, 1),
	PIN_FIELD_BASE(40, 43, 2, 0x0040, 0x10, 21, 1),
	PIN_FIELD_BASE(54, 54, 2, 0x0040, 0x10, 20, 1),
	PIN_FIELD_BASE(55, 66, 4, 0x0040, 0x10, 0, 1),
	PIN_FIELD_BASE(87, 106, 4, 0x0040, 0x10, 12, 1),
	PIN_FIELD_BASE(107, 114, 1, 0x0070, 0x10, 0, 1),
	PIN_FIELD_BASE(115, 127, 1, 0x0090, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 129, 1, 0x0080, 0x10, 0, 1),
	PIN_FIELD_BASE(130, 153, 1, 0x0070, 0x10, 8, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_drv_range[] = {
	/*
	[DRV_GRP0] = { 4, 16, 4, 1 }, // index 0
	[DRV_GRP1] = { 4, 16, 4, 2 }, // index 3 (probably), TODO: figure out if this should be actually used, and not group 0
	[DRV_GRP2] = { 2, 8, 2, 1 }, // index 1
	[DRV_GRP3] = { 2, 8, 2, 2 }, // unused
	[DRV_GRP4] = { 2, 16, 2, 1 }, // index 2
	*/
	PINS_FIELD_BASE(0, 1, 3, 0x0080, 0x10, 0, 2),
	PINS_FIELD_BASE(2, 3, 3, 0x0080, 0x10, 2, 2),
	PINS_FIELD_BASE(4, 6, 3, 0x0080, 0x10, 4, 2),
	PINS_FIELD_BASE(8, 11, 3, 0x0080, 0x10, 6, 2),
	PINS_FIELD_BASE(7, 7, 3, 0x0080, 0x10, 8, 2),
	PINS_FIELD_BASE(12, 13, 3, 0x0080, 0x10, 8, 2),
	PINS_FIELD_BASE(14, 31, 3, 0x0080, 0x10, 10, 2),
	PINS_FIELD_BASE(32, 39, 3, 0x0080, 0x10, 12, 2),
	PINS_FIELD_BASE(40, 43, 2, 0x0060, 0x10, 8, 2),
	PINS_FIELD_BASE(44, 49, 2, 0x0060, 0x10, 0, 3),
	PINS_FIELD_BASE(50, 53, 2, 0x0060, 0x10, 3, 3),
	PINS_FIELD_BASE(54, 54, 2, 0x0060, 0x10, 6, 2),
	PINS_FIELD_BASE(55, 60, 4, 0x0060, 0x10, 0, 3),
	PINS_FIELD_BASE(61, 66, 4, 0x0060, 0x10, 4, 2),
	PINS_FIELD_BASE(87, 88, 4, 0x0060, 0x10, 6, 2),
	PINS_FIELD_BASE(89, 90, 4, 0x0060, 0x10, 8, 2),
	PINS_FIELD_BASE(91, 96, 4, 0x0060, 0x10, 10, 3),
	PINS_FIELD_BASE(97, 100, 4, 0x0060, 0x10, 14, 2),
	PINS_FIELD_BASE(101, 102, 4, 0x0060, 0x10, 16, 2),
	PINS_FIELD_BASE(103, 104, 4, 0x0060, 0x10, 18, 2),
	PINS_FIELD_BASE(107, 112, 1, 0x00D0, 0x04, 0, 2),
	PINS_FIELD_BASE(115, 120, 1, 0x00D4, 0x04, 0, 2),
	PINS_FIELD_BASE(121, 124, 1, 0x00D4, 0x04, 2, 2),
	PINS_FIELD_BASE(126, 127, 1, 0x00D4, 0x04, 4, 2),
	PINS_FIELD_BASE(128, 129, 1, 0x00D0, 0x04, 14, 2),
	PINS_FIELD_BASE(130, 133, 1, 0x00D0, 0x04, 2, 2),
	PINS_FIELD_BASE(134, 136, 1, 0x00D0, 0x04, 4, 2),
	PINS_FIELD_BASE(137, 141, 1, 0x00D0, 0x04, 6, 2),
	PINS_FIELD_BASE(142, 145, 1, 0x00D0, 0x04, 8, 2),
	PINS_FIELD_BASE(146, 148, 1, 0x00D0, 0x04, 10, 2),
	PINS_FIELD_BASE(149, 153, 1, 0x00D0, 0x04, 12, 2),
};

static const struct mtk_pin_field_calc mt6572_pin_pupd_range[] = {
	PIN_FIELD_BASE(0, 6, 3, 0x0060, 0x10, 0, 1),
	PIN_FIELD_BASE(7, 7, 3, 0x0060, 0x10, 11, 1),
	PIN_FIELD_BASE(8, 11, 3, 0x0060, 0x10, 7, 1),
	PIN_FIELD_BASE(12, 13, 3, 0x0060, 0x10, 12, 1),
	PIN_FIELD_BASE(14, 39, 3, 0x0070, 0x10, 0, 1),
	PIN_FIELD_BASE(40, 43, 2, 0x0050, 0x10, 21, 1),
	PIN_FIELD_BASE(44, 54, 2, 0x0050, 0x10, 0, 1),
	PIN_FIELD_BASE(55, 66, 4, 0x0050, 0x10, 0, 1),
	PIN_FIELD_BASE(87, 104, 4, 0x0050, 0x10, 12, 1),
	PIN_FIELD_BASE(107, 112, 1, 0x00A0, 0x10, 0, 1),
	PIN_FIELD_BASE(115, 127, 1, 0x00C0, 0x10, 0, 1),
	PIN_FIELD_BASE(128, 129, 1, 0x00B0, 0x10, 0, 1),
	PIN_FIELD_BASE(130, 148, 1, 0x00A0, 0x10, 8, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_r0_range[] = {
	PIN_FIELD_BASE(44, 44, 2, 0x0040, 0x10, 0, 1),
	PIN_FIELD_BASE(45, 45, 2, 0x0040, 0x10, 2, 1),
	PIN_FIELD_BASE(46, 46, 2, 0x0040, 0x10, 4, 1),
	PIN_FIELD_BASE(47, 47, 2, 0x0040, 0x10, 6, 1),
	PIN_FIELD_BASE(48, 48, 2, 0x0040, 0x10, 8, 1),
	PIN_FIELD_BASE(49, 49, 2, 0x0040, 0x10, 10, 1),
	PIN_FIELD_BASE(50, 50, 2, 0x0040, 0x10, 12, 1),
	PIN_FIELD_BASE(51, 51, 2, 0x0040, 0x10, 14, 1),
	PIN_FIELD_BASE(52, 52, 2, 0x0040, 0x10, 16, 1),
	PIN_FIELD_BASE(53, 53, 2, 0x0040, 0x10, 18, 1),
};

static const struct mtk_pin_field_calc mt6572_pin_r1_range[] = {
	PIN_FIELD_BASE(44, 44, 2, 0x0040, 0x10, 1, 1),
	PIN_FIELD_BASE(45, 45, 2, 0x0040, 0x10, 3, 1),
	PIN_FIELD_BASE(46, 46, 2, 0x0040, 0x10, 5, 1),
	PIN_FIELD_BASE(47, 47, 2, 0x0040, 0x10, 7, 1),
	PIN_FIELD_BASE(48, 48, 2, 0x0040, 0x10, 9, 1),
	PIN_FIELD_BASE(49, 49, 2, 0x0040, 0x10, 11, 1),
	PIN_FIELD_BASE(50, 50, 2, 0x0040, 0x10, 13, 1),
	PIN_FIELD_BASE(51, 51, 2, 0x0040, 0x10, 15, 1),
	PIN_FIELD_BASE(52, 52, 2, 0x0040, 0x10, 17, 1),
	PIN_FIELD_BASE(53, 53, 2, 0x0040, 0x10, 19, 1),
};

static const struct mtk_pin_reg_calc mt6572_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6572_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6572_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6572_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6572_pin_do_range),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt6572_pin_smt_range),
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt6572_pin_ies_range),
	[PINCTRL_PIN_REG_PULLEN] = MTK_RANGE(mt6572_pin_pu_pd_range),
	[PINCTRL_PIN_REG_PULLSEL] = MTK_RANGE(mt6572_pin_pupd_range),
	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(mt6572_pin_drv_range),
	[PINCTRL_PIN_REG_R0] = MTK_RANGE(mt6572_pin_r0_range),
	[PINCTRL_PIN_REG_R1] = MTK_RANGE(mt6572_pin_r1_range),
};

static const char * const mt6572_pinctrl_register_base_names[] = {
	"gpio", "iocfg_t", "iocfg_b", "iocfg_l", "iocfg_r",
};

static const struct mtk_eint_hw mt6572_eint_hw = {
	.port_mask = 7,
	.ports     = 7,
	.ap_num    = 26,
	.db_cnt    = 26,
	.db_time   = debounce_time_mt2701,
};

static const struct mtk_pin_soc mt6572_data = {
	.reg_cal = mt6572_reg_cals,
	.pins = mtk_pins_mt6572,
	.npins = ARRAY_SIZE(mtk_pins_mt6572),
	.ngrps = ARRAY_SIZE(mtk_pins_mt6572),
	.eint_hw = &mt6572_eint_hw,
	.gpio_m = 0,
	.ies_present = true,
	.base_names = mt6572_pinctrl_register_base_names,
	.nbase_names = ARRAY_SIZE(mt6572_pinctrl_register_base_names),
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.drive_set = mtk_pinconf_drive_set_rev1,
	.drive_get = mtk_pinconf_drive_get_rev1,
	.adv_pull_get = mtk_pinconf_adv_pull_get,
	.adv_pull_set = mtk_pinconf_adv_pull_set,
};

static const struct of_device_id mt6572_pinctrl_of_match[] = {
	{ .compatible = "mediatek,mt6572-pinctrl", .data = &mt6572_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6572_pinctrl_of_match);

static struct platform_driver mt6572_pinctrl_driver = {
	.driver = {
		.name = "mt6572-pinctrl",
		.of_match_table = mt6572_pinctrl_of_match,
	},
	.probe = mtk_paris_pinctrl_probe,
};

static int __init mt6572_pinctrl_init(void)
{
	return platform_driver_register(&mt6572_pinctrl_driver);
}
arch_initcall(mt6572_pinctrl_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MediaTek MT6572 Pinctrl Driver");
