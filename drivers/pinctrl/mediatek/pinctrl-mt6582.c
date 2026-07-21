// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */
 
#include <dt-bindings/pinctrl/mt65xx.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/regmap.h>



#include "pinctrl-mtk-mt6582.h"
#include "pinctrl-paris.h"


#define PIN_FIELD_BASE(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits) \
	PIN_FIELD_CALC(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits, \
		       16, 0)

#define PINS_FIELD_BASE(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits) \
	PIN_FIELD_CALC(s_pin, e_pin, i_base, s_addr, x_addrs, s_bit, x_bits,  \
		       16, 1)

#define PIN_FIELD_(_s_pin, _e_pin, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, 0, _s_addr, _x_addrs, _s_bit,	\
		       _x_bits, 16, 0)

#define PINS_FIELD_(_s_pin, _e_pin, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, 0, _s_addr, _x_addrs, _s_bit,	\
		       _x_bits, 16, 1)


/* ok */
static const struct mtk_pin_field_calc mt6582_pin_mode_range[] = {
	PIN_FIELD_CALC(0, 168, 0, 0x600, 0x10, 0, 3, 15, 0),
};
/* ok */
static const struct mtk_pin_field_calc mt6582_pin_dir_range[] = {
	PIN_FIELD_(0, 140, 0x00, 0x10, 0, 1),
	PIN_FIELD_(167, 168, 0xA0, 0x10, 7, 1),
};
/* ok */
static const struct mtk_pin_field_calc mt6582_pin_di_range[] = {
	PIN_FIELD_(0, 168, 0x500, 0x10, 0, 1),
};
/* ok */
static const struct mtk_pin_field_calc mt6582_pin_do_range[] = {
	PIN_FIELD_(0, 140, 0x400, 0x10, 0, 1),
	PIN_FIELD_(167, 168, 0x4A0, 0x10, 7, 1),
};
/*ok*/
static const struct mtk_pin_field_calc mt6582_pin_ies_range[] = {
	PINS_FIELD_BASE(0, 6, 0, 0x900, 0, 2, 1),
	PINS_FIELD_BASE(7, 10, 0, 0x900, 0x10, 3, 1),
	PINS_FIELD_BASE(11, 16, 0, 0x900, 0x10, 12, 1),
	PINS_FIELD_BASE(17, 20, 0, 0x900, 0x10, 13, 1),
	PINS_FIELD_BASE(21, 28, 0, 0x900, 0x10, 0, 1),
	PINS_FIELD_BASE(29, 38, 0, 0x900, 0x10, 1, 1),
	PINS_FIELD_BASE(39, 46, 0, 0x900, 0x10, 2, 1),
	PINS_FIELD_BASE(47, 49, 0, 0x900, 0x10, 4, 1),
	PINS_FIELD_BASE(50, 52, 0, 0x900, 0x10, 5, 1),
	PINS_FIELD_BASE(53, 56, 0, 0x900, 0x10, 6, 1),
	PINS_FIELD_BASE(57, 65, 0, 0x900, 0x10, 7, 1),
	PINS_FIELD_BASE(66, 71, 0, 0x900, 0x10, 8, 1),
	PINS_FIELD_BASE(72, 73, 0, 0x900, 0x10, 9, 1),
	PINS_FIELD_BASE(74, 75, 0, 0x900, 0x10, 10, 1),
	PINS_FIELD_BASE(76, 79, 0, 0x900, 0x10, 11, 1),
	PINS_FIELD_BASE(80, 85, 0, 0x900, 0x10, 14, 1),
	PINS_FIELD_BASE(84, 85, 0, 0x900, 0x10, 15, 1),
	PINS_FIELD_BASE(86, 87, 0, 0x910, 0x10, 0, 1),
	PINS_FIELD_BASE(88, 89, 0, 0x910, 0x10, 1, 1),
	PINS_FIELD_BASE(90, 91, 0, 0x910, 0x10, 2, 1),
	PINS_FIELD_BASE(92, 93, 0, 0x900, 0x10, 10, 1),
	PINS_FIELD_BASE(94, 98, 0, 0x910, 0x10, 2, 1),
	PINS_FIELD_BASE(99, 104, 0, 0x910, 0x10, 3, 1),
	PINS_FIELD_BASE(105, 107, 0, 0x910, 0x10, 4, 1),
	PINS_FIELD_BASE(108, 111, 0, 0x910, 0x10, 5, 1),
	PIN_FIELD_BASE(112, 113, 0, 0x910, 0x10, 6, 1),
	PIN_FIELD_BASE(114, 114, 0, 0xC90, 0x10, 14, 1),
	PIN_FIELD_BASE(115, 115, 0, 0xC80, 0x10, 14, 1),
	PIN_FIELD_BASE(116, 119, 0, 0xCA0, 0x10, 14, 1),
	PINS_FIELD_BASE(120, 123, 0, 0x910, 0x10, 7, 1),
	PIN_FIELD_BASE(124, 124, 0, 0xC50, 0x10, 14, 1),
	PIN_FIELD_BASE(125, 125, 0, 0xC40, 0x10, 14, 1),
	PIN_FIELD_BASE(126, 129, 0, 0xC60, 0x10, 14, 1),
	PIN_FIELD_BASE(130, 133, 0, 0xC20, 0x10, 14, 1),
	PIN_FIELD_BASE(135, 135, 0, 0xC10, 0x10, 14, 1),
	PIN_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 14, 1),
	PIN_FIELD_BASE(137, 140, 0, 0xC20, 0x10, 14, 1),
	PINS_FIELD_BASE(167, 168, 0, 0x900, 0x10, 10, 1),

};
/*ok*/
static const struct mtk_pin_field_calc mt6582_pin_smt_range[] = {
	PINS_FIELD_BASE(0, 6, 0, 0x920, 0x10, 2, 1),
	PINS_FIELD_BASE(7, 10, 0, 0x920, 0x10, 3, 1),
	PINS_FIELD_BASE(11, 16, 0, 0x920, 0x10, 12, 1),
	PINS_FIELD_BASE(17, 20, 0, 0x920, 0x10, 13, 1),
	PINS_FIELD_BASE(21, 28, 0, 0x920, 0x10, 0, 1),
	PINS_FIELD_BASE(29, 38, 0, 0x920, 0x10, 1, 1),
	PINS_FIELD_BASE(39, 46, 0, 0x920, 0x10, 2, 1),
	PINS_FIELD_BASE(47, 49, 0, 0x920, 0x10, 4, 1),
	PINS_FIELD_BASE(50, 52, 0, 0x920, 0x10, 5, 1),
	PINS_FIELD_BASE(53, 56, 0, 0x920, 0x10, 6, 1),
	PINS_FIELD_BASE(57, 65, 0, 0x920, 0x10, 7, 1),
	PINS_FIELD_BASE(66, 71, 0, 0x920, 0x10, 8, 1),
	PINS_FIELD_BASE(72, 73, 0, 0x920, 0x10, 9, 1),
	PINS_FIELD_BASE(74, 75, 0, 0x920, 0x10, 10, 1),
	PINS_FIELD_BASE(76, 79, 0, 0x920, 0x10, 11, 1),
	PINS_FIELD_BASE(80, 83, 0, 0x920, 0x10, 14, 1),
	PINS_FIELD_BASE(84, 85, 0, 0x920, 0x10, 15, 1),
	PINS_FIELD_BASE(86, 87, 0, 0x930, 0x10, 0, 1),
	PINS_FIELD_BASE(88, 89, 0, 0x930, 0x10, 1, 1),
	PINS_FIELD_BASE(90, 91, 0, 0x930, 0x10, 2, 1),
	PINS_FIELD_BASE(92, 93, 0, 0x920, 0x10, 10, 1),
	PINS_FIELD_BASE(94, 98, 0, 0x930, 0x10, 2, 1),
	PINS_FIELD_BASE(99, 104, 0, 0x930, 0x10, 3, 1),
	PINS_FIELD_BASE(105, 107, 0, 0x930, 0x10, 4, 1),
	PINS_FIELD_BASE(108, 111, 0, 0x930, 0x10, 5, 1),
	PINS_FIELD_BASE(112, 113, 0, 0x930, 0x10, 6, 1),
	PINS_FIELD_BASE(114, 114, 0, 0xC90, 0x10, 13, 1),
	PINS_FIELD_BASE(115, 115, 0, 0xC80, 0x10, 13, 1),
	PINS_FIELD_BASE(116, 119, 0, 0xCA0, 0x10, 13, 1),
	PINS_FIELD_BASE(120, 123, 0, 0x930, 0x10, 7, 1),
	PINS_FIELD_BASE(124, 124, 0, 0xC50, 0x10, 13, 1),
	PINS_FIELD_BASE(125, 125, 0, 0xC40, 0x10, 13, 1),
	PINS_FIELD_BASE(126, 129, 0, 0xC60, 0x10, 13, 1),
	PINS_FIELD_BASE(130, 133, 0, 0xC20, 0x10, 13, 1),
	PINS_FIELD_BASE(134, 134, 0, 0x930, 0x10, 8, 1),
	PINS_FIELD_BASE(135, 135, 0, 0xC10, 0x10, 13, 1),
	PINS_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 14, 1),
	PINS_FIELD_BASE(137, 140, 0, 0xC20, 0x10, 13, 1),
	/* 141-156 controlling in MIPI_tx MIPI ANA */

	PINS_FIELD_BASE(167, 168, 0, 0x920, 0x10, 10, 1),
	
};
/* ok */
static const struct mtk_pin_field_calc mt6582_pin_pu_range[] = {
	PIN_FIELD_BASE(0, 73, 0, 0x200, 0x10, 0, 1),
	PIN_FIELD_BASE(76, 91, 0, 0x240, 0x10, 12, 1),
	PIN_FIELD_BASE(94, 113, 0, 0x250, 0x10, 14, 1),
	PINS_FIELD_BASE(114, 114, 0, 0xC90, 0x10, 5, 1),
	PINS_FIELD_BASE(115, 115, 0, 0xC80, 0x10, 5, 1), 
	PINS_FIELD_BASE(116, 116, 0, 0xCA0, 0x10, 1, 2),
	PINS_FIELD_BASE(117, 117, 0, 0xCA0, 0x10, 3, 2),
	PINS_FIELD_BASE(118, 118, 0, 0xCA0, 0x10, 5, 2),
	PINS_FIELD_BASE(119, 119, 0, 0xCA0, 0x10, 7, 2),
	PIN_FIELD_BASE(120, 123, 0, 0x270, 0x10, 8, 1),
	PINS_FIELD_BASE(124, 124, 0, 0xC50, 0x10, 5, 1),
	PINS_FIELD_BASE(125, 125, 0, 0xC40, 0x10, 5, 1),
	PINS_FIELD_BASE(126, 126, 0, 0xC60, 0x10, 1, 1),
	PINS_FIELD_BASE(127, 127, 0, 0xC60, 0x10, 3, 1),
	PINS_FIELD_BASE(128, 128, 0, 0xC60, 0x10, 5, 1),
	PINS_FIELD_BASE(129, 129, 0, 0xC60, 0x10, 7, 1),
	PINS_FIELD_BASE(134, 134, 0, 0x280, 0x10, 6, 1),	 
	/* 141-146 MIPI */
};
/* ok */
static const struct mtk_pin_field_calc mt6582_pin_pd_range[] = {
	PIN_FIELD_BASE(0, 73, 0, 0x200, 0x10, 0, 1),
	PIN_FIELD_BASE(76, 91, 0, 0x240, 0x10, 12, 1),
	PIN_FIELD_BASE(94, 113, 0, 0x250, 0x10, 14, 1),
	PINS_FIELD_BASE(114, 114, 0, 0xC90, 0x10, 4, 1),
	PINS_FIELD_BASE(115, 115, 0, 0xC80, 0x10, 4, 1),
	PINS_FIELD_BASE(116, 116, 0, 0xCA0, 0x10, 0, 2),
	PINS_FIELD_BASE(117, 117, 0, 0xCA0, 0x10, 2, 2),
	PINS_FIELD_BASE(118, 118, 0, 0xCA0, 0x10, 4, 2),
	PINS_FIELD_BASE(119, 119, 0, 0xCA0, 0x10, 6, 2),
	PIN_FIELD_BASE(120, 123, 0, 0x270, 0x10, 8, 1),
	PINS_FIELD_BASE(124, 124, 0, 0xC50, 0x10, 4, 1),
	PINS_FIELD_BASE(125, 125, 0, 0xC40, 0x10, 4, 1),
	PINS_FIELD_BASE(125, 125, 0, 0xC40, 0x10, 5, 1),
	PINS_FIELD_BASE(126, 126, 0, 0xC60, 0x10, 0, 1),
	PINS_FIELD_BASE(127, 127, 0, 0xC60, 0x10, 2, 1),
	PINS_FIELD_BASE(128, 128, 0, 0xC60, 0x10, 4, 1),
	PINS_FIELD_BASE(129, 129, 0, 0xC60, 0x10, 6, 1),
	PINS_FIELD_BASE(134, 134, 0, 0x280, 0x10, 6, 1), 
	PINS_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 2, 1),   
	/* 141-146 MIPI */
};
/* ok */
static const struct mtk_pin_field_calc mt6582_pin_pupd_range[] = {
	PINS_FIELD_BASE(74, 74, 0, 0xCE0, 0x10, 2, 1),
	PINS_FIELD_BASE(75, 75, 0, 0xCF0, 0x10, 2, 1),
	PINS_FIELD_BASE(92, 92, 0, 0xCE0, 0x10, 6, 1),
	PINS_FIELD_BASE(93, 93, 0, 0xCF0, 0x10, 6, 1),
	PINS_FIELD_BASE(130, 133, 0, 0xC20, 0x10, 0, 1),
	/* 141-146 MIPI */
	PINS_FIELD_BASE(135, 135, 0, 0xC10, 0x10, 2, 1), 
	PINS_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 2, 1), 
	PINS_FIELD_BASE(137, 140, 0, 0xC20, 0x10, 2, 1),  
	PINS_FIELD_BASE(167, 167, 0, 0xCF0, 0x10, 0, 1),
	PINS_FIELD_BASE(168, 168, 0, 0xCF0, 0x10, 0, 1),
};
/*ok*/
static const struct mtk_pin_field_calc mt6582_pin_pullsel_range[] = {
	PIN_FIELD_BASE(0, 73, 0, 0x0200, 0x10, 0, 1),
	PIN_FIELD_BASE(76, 91, 0, 0x0240, 0x10, 12, 1),
	PIN_FIELD_BASE(94, 113, 0, 0x250, 0x10, 14, 1),
	PIN_FIELD_BASE(120, 123, 0, 0x0270, 0x10, 8, 1),
	PIN_FIELD_BASE(134, 134, 0, 0x0280, 0x10, 6, 1),
};
/*ok*/
static const struct mtk_pin_field_calc mt6582_pin_pullen_range[] = {
	PIN_FIELD_BASE(0, 73, 0, 0x0100, 0x10, 0, 1),
	PIN_FIELD_BASE(76, 91, 0, 0x0140, 0x10, 12, 1),
	PIN_FIELD_BASE(94, 113, 0, 0x150, 0x10, 14, 1),
	PIN_FIELD_BASE(120, 123, 0, 0x0170, 0x10, 8, 1),
	PIN_FIELD_BASE(134, 134, 0, 0x0180, 0x10, 6, 1),

};
/*ok*/
static const struct mtk_pin_field_calc mt6582_pin_r0_range[] = {
	PINS_FIELD_BASE(75, 75, 0, 0xCF0, 0x10, 0, 1),
	PINS_FIELD_BASE(74, 74, 0, 0xCE0, 0x10, 0, 1),
	PINS_FIELD_BASE(92, 92, 0, 0xCE0, 0x10, 4, 1),
	PINS_FIELD_BASE(93, 93, 0, 0xCE0, 0x10, 0, 1),
	PINS_FIELD_BASE(130, 133, 0, 0xC20, 0x10, 0, 1),
	PINS_FIELD_BASE(135, 135, 0, 0xC10, 0x10, 0, 1),
	PINS_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 0, 1),
	PINS_FIELD_BASE(137, 140, 0, 0xC20, 0x10, 0, 1),
	PINS_FIELD_BASE(167, 167, 0, 0xCF0, 0x10, 4, 1),
	PINS_FIELD_BASE(168, 168, 0, 0xCF0, 0x10, 8, 1),
};
/*ok*/
static const struct mtk_pin_field_calc mt6582_pin_r1_range[] = {
	PINS_FIELD_BASE(75, 75, 0, 0xCF0, 0x10, 1, 1),
	PINS_FIELD_BASE(74, 74, 0, 0xCE0, 0x10, 1, 1),
	PINS_FIELD_BASE(92, 92, 0, 0xCE0, 0x10, 5, 1),
	PINS_FIELD_BASE(93, 93, 0, 0xCE0, 0x10, 9, 1),
	PINS_FIELD_BASE(130, 133, 0, 0xC20, 0x10, 1, 1),
	PINS_FIELD_BASE(135, 135, 0, 0xC10, 0x10, 1, 1),
	PINS_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 1, 1),
	PINS_FIELD_BASE(137, 140, 0, 0xC20, 0x10, 1, 1),
	PINS_FIELD_BASE(167, 167, 0, 0xCF0, 0x10, 5, 1),
	PINS_FIELD_BASE(168, 168, 0, 0xCF0, 0x10, 9, 1),
};

static const struct mtk_pin_field_calc mt6582_pin_drv_range[] = {
	PINS_FIELD_BASE(0, 4, 0, 0xB20, 0x10, 0, 3),
	PINS_FIELD_BASE(5, 10, 0, 0xB20, 0x10, 4, 3),
	PINS_FIELD_BASE(11, 16, 0, 0xB40, 0x10, 12, 3),
	PINS_FIELD_BASE(17, 20, 0, 0xB50, 0x10, 0, 3),
	PINS_FIELD_BASE(21, 25, 0, 0xB00, 0x10, 0, 3),
	PINS_FIELD_BASE(26, 28, 0, 0xB00, 0x10, 12, 3),
	PINS_FIELD_BASE(29, 34, 0, 0xB00, 0x10, 4, 3),
	PINS_FIELD_BASE(35, 38, 0, 0xB00, 0x10, 8, 3),
	PINS_FIELD_BASE(39, 42, 0, 0xB10, 0x10, 4, 3),
	PINS_FIELD_BASE(43, 46, 0, 0xB10, 0x10, 8, 3),
	PINS_FIELD_BASE(47, 49, 0, 0xB20, 0x10, 8, 3),
	PINS_FIELD_BASE(50, 65, 0, 0xB30, 0x10, 0, 3),
	PINS_FIELD_BASE(66, 73, 0, 0xB30, 0x10, 4, 3),
	PINS_FIELD_BASE(74, 75, 0, 0xB30, 0x10, 8, 3),
	PINS_FIELD_BASE(76, 79, 0, 0xB30, 0x10, 12, 3),
	PINS_FIELD_BASE(80, 83, 0, 0xB50, 0x10, 12, 3),
	PINS_FIELD_BASE(84, 84, 0, 0xB60, 0x10, 0, 3),
	PINS_FIELD_BASE(90, 91, 0, 0xB40, 0x10, 4, 3),
	PINS_FIELD_BASE(92, 93, 0, 0xB30, 0x10, 8, 3),
	PINS_FIELD_BASE(94, 95, 0, 0xB40, 0x10, 0, 3),
	PINS_FIELD_BASE(96, 98, 0, 0xB40, 0x10, 4, 3),
	PINS_FIELD_BASE(99, 104, 0, 0xB40, 0x10, 8, 3),
	PINS_FIELD_BASE(105, 107, 0, 0xB50, 0x10, 4, 3),
	PINS_FIELD_BASE(108, 113, 0, 0xB50, 0x10, 8, 3),
	PINS_FIELD_BASE(114, 114, 0, 0xC90, 0x10, 8, 3),
	PINS_FIELD_BASE(115, 115, 0, 0xC80, 0x10, 8, 3),
	PINS_FIELD_BASE(116, 119, 0, 0xCA0, 0x10, 8, 3),
	PINS_FIELD_BASE(120, 123, 0, 0xB60, 0x10, 12, 3),
	PINS_FIELD_BASE(124, 124, 0, 0xC50, 0x10, 8, 3),
	PINS_FIELD_BASE(125, 125, 0, 0xC40, 0x10, 8, 3),
	PINS_FIELD_BASE(126, 129, 0, 0xC60, 0x10, 8, 3),
	PINS_FIELD_BASE(130, 134, 0, 0xC20, 0x10, 8, 3),
	PINS_FIELD_BASE(135, 135, 0, 0xC10, 0x10, 8, 3),
	PINS_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 8, 3),
	PINS_FIELD_BASE(137, 140, 0, 0xC20, 0x10, 8, 3),
	PINS_FIELD_BASE(167, 168, 0, 0xB30, 0x10, 8, 3),
};

static const struct mtk_pin_field_calc mt6582_pin_sr_range[] = {
	/*MSDC2*/
	PIN_FIELD_BASE(114, 114, 0, 0xC90, 0x10, 12, 1),
	PIN_FIELD_BASE(115, 115, 0, 0xC80, 0x10, 12, 1),
	PINS_FIELD_BASE(116, 119, 0, 0xCA0, 0x10, 12, 1),
	/*MSDC1*/
	PIN_FIELD_BASE(124, 124, 0, 0xC50, 0x10, 12, 1),
	PIN_FIELD_BASE(125, 125, 0, 0xC40, 0x10, 12, 1),
	PINS_FIELD_BASE(126, 129, 0, 0xC60, 0x10, 12, 1),
	/*MSDC20*/
	PINS_FIELD_BASE(130, 133, 0, 0xC20, 0x10, 12, 1),
	PIN_FIELD_BASE(135, 135, 0/*2*/, 0xC00, 0x10, 12, 1),
	PIN_FIELD_BASE(136, 136, 0, 0xC00, 0x10, 12, 1),
	PINS_FIELD_BASE(137, 140, 0, 0xC20, 0x10, 12, 1),
};

static const unsigned int mt6582_pull_type[] = {
	 MTK_PULL_PULLSEL_TYPE	/*	0	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	1	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	2	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	3	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	4	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	5	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	6	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	7	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	8	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	9	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	10	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	11	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	12	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	13	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	14	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	15	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	16	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	17	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	18	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	19	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	20	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	21	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	22	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	23	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	24	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	25	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	26	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	27	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	28	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	29	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	30	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	31	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	32	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	33	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	34	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	35	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	36	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	37	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	38	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	39	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	40	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	41	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	42	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	43	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	44	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	45	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	46	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	47	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	48	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	49	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	50	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	51	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	52	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	53	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	54	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	55	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	56	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	57	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	58	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	59	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	60	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	61	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	62	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	63	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	64	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	65	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	66	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	67	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	68	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	69	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	70	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	71	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	72	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	73	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	74	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	75	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	76	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	77	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	78	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	79	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	80	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	81	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	82	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	83	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	84	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	85	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	86	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	87	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	88	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	89	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	90	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	91	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	92	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	93	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	94	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	95	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	96	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	97	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	98	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	99	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	100	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	101	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	102	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	103	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	104	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	105	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	106	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	107	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	108	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	109	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	110	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	111	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	112	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	113	*/,
	 MTK_PULL_PU_PD_TYPE	/*	114	*/,
	 MTK_PULL_PU_PD_TYPE	/*	115	*/,
	 MTK_PULL_PU_PD_TYPE	/*	116	*/,
	 MTK_PULL_PU_PD_TYPE	/*	117	*/,
	 MTK_PULL_PU_PD_TYPE	/*	118	*/,
	 MTK_PULL_PU_PD_TYPE	/*	119	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	120	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	121	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	122	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	123	*/,
	 MTK_PULL_PU_PD_TYPE	/*	124	*/,
	 MTK_PULL_PU_PD_TYPE	/*	125	*/,
	 MTK_PULL_PU_PD_TYPE	/*	126	*/,
	 MTK_PULL_PU_PD_TYPE	/*	127	*/,
	 MTK_PULL_PU_PD_TYPE	/*	128	*/,
	 MTK_PULL_PU_PD_TYPE	/*	129	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	130	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	131	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	132	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	133	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	134	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	135	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	136	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	137	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	138	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	139	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	140	*/,
	 MTK_PULL_PU_PD_TYPE	/*	141	*/,
	 MTK_PULL_PU_PD_TYPE	/*	142	*/,
	 MTK_PULL_PU_PD_TYPE	/*	143	*/,
	 MTK_PULL_PU_PD_TYPE	/*	144	*/,
	 MTK_PULL_PU_PD_TYPE	/*	145	*/,
	 MTK_PULL_PU_PD_TYPE	/*	146	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	147	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	148	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	149	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	150	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	151	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	152	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	153	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	154	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	155	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	156	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	157	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	158	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	159	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	160	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	161	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	162	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	163	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	164	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	165	*/,
	 MTK_PULL_PULLSEL_TYPE	/*	166	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	167	*/,
	 MTK_PULL_PUPD_R1R0_TYPE	/*	168	*/,

};

static const struct mtk_pin_reg_calc mt6582_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6582_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6582_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6582_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6582_pin_do_range),
	[PINCTRL_PIN_REG_SR] = MTK_RANGE(mt6582_pin_sr_range),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt6582_pin_smt_range),
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt6582_pin_ies_range),
	[PINCTRL_PIN_REG_PU] = MTK_RANGE(mt6582_pin_pu_range),
	[PINCTRL_PIN_REG_PD] = MTK_RANGE(mt6582_pin_pd_range),
	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(mt6582_pin_drv_range),
	[PINCTRL_PIN_REG_PUPD] = MTK_RANGE(mt6582_pin_pupd_range),
	[PINCTRL_PIN_REG_R0] = MTK_RANGE(mt6582_pin_r0_range),
	[PINCTRL_PIN_REG_R1] = MTK_RANGE(mt6582_pin_r1_range),
	[PINCTRL_PIN_REG_PULLEN] = MTK_RANGE(mt6582_pin_pullen_range),
	[PINCTRL_PIN_REG_PULLSEL] = MTK_RANGE(mt6582_pin_pullsel_range),
};

static const char * const mt6582_pinctrl_register_base_names[] = {
	"gpio", 
};


static const struct mtk_eint_hw mt6582_eint_hw = {
	.port_mask = 0xf,
	.ports     = 6,
	.ap_num    = 169,
	.db_cnt    = 16,
	.db_time   = debounce_time_mt6795,
};


static const struct mtk_pin_soc mt6582_data = {
	.reg_cal = mt6582_reg_cals,
	.pins = mtk_pins_mt6582,
	.npins = ARRAY_SIZE(mtk_pins_mt6582),
	.ngrps = ARRAY_SIZE(mtk_pins_mt6582),
	.eint_hw = &mt6582_eint_hw,
	.nfuncs = 8,
	.gpio_m = 0,
	.base_names = mt6582_pinctrl_register_base_names,
	.nbase_names = ARRAY_SIZE(mt6582_pinctrl_register_base_names),
	.pull_type = mt6582_pull_type,
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.drive_set = mtk_pinconf_drive_set_rev1,
	.drive_get = mtk_pinconf_drive_get_rev1,
	.adv_drive_get = mtk_pinconf_adv_drive_get_raw,
	.adv_drive_set = mtk_pinconf_adv_drive_set_raw,
};

static const struct of_device_id mt6582_pinctrl_of_match[] = {
	{ .compatible = "mediatek,mt6582-pinctrl", .data = &mt6582_data },
	{ }
};

static struct platform_driver mt6582_pinctrl_driver = {
	.driver = {
		.name = "mt6582-pinctrl",
		.of_match_table = mt6582_pinctrl_of_match,
		.pm = pm_sleep_ptr(&mtk_paris_pinctrl_pm_ops),
	},
	.probe = mtk_paris_pinctrl_probe,
};

static int __init mt6582_pinctrl_init(void)
{
	return platform_driver_register(&mt6582_pinctrl_driver);
}
arch_initcall(mt6582_pinctrl_init);

MODULE_DESCRIPTION("MediaTek MT6582 Pinctrl Driver");

