// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 *
 * Based on pinctrl-mt2701.c
 * Copyright (c) 2015 MediaTek Inc.
 * Author: Biao Huang <biao.huang@mediatek.com>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <dt-bindings/pinctrl/mt65xx.h>

#include "pinctrl-mtk-common.h"
#include "pinctrl-mtk-mt6589.h"

/*
 * E2, E4, E8, E16: Drive
 * SR: ???
 * DM: Dummy
 * MSB <-> LSB
 */
static const struct mtk_drv_group_desc mt6589_drv_grp[] = {
	/* grp 0: SR E8 E4 E2: 2/4/6/8/10/12/14/16mA */
	MTK_DRV_GRP(2, 16, 0, 2, 2),
	/* grp 1: SR E8 E4 DM: 4/8/12/16mA */
	MTK_DRV_GRP(4, 16, 1, 2, 4),
	/* grp 2: SR E4 E2 DM: 2/4/6/8mA */
	MTK_DRV_GRP(2, 8, 1, 2, 2),
	/* grp 3: SR E16 E8 E4 4/8/12/16/20/24/28/32mA */
	MTK_DRV_GRP(4, 32, 0, 2, 4),
};


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

static const struct mtk_pin_drv_grp mt6589_pin_drv[] = {
	/* MSDC0_DAT 7 to 4 */
	MTK_PIN_DRV_GRP(0, DRV_CON0, 0, 0),
	MTK_PIN_DRV_GRP(1, DRV_CON0, 0, 0),
	MTK_PIN_DRV_GRP(2, DRV_CON0, 0, 0),
	MTK_PIN_DRV_GRP(3, DRV_CON0, 0, 0),

	/* MSDC0_RSTB */
	MTK_PIN_DRV_GRP(4, DRV_CON0, 8, 1),

	/* MSDC0_CMD */
	MTK_PIN_DRV_GRP(5, DRV_CON0, 4, 0),

	/* MSDC0_CLK */
	MTK_PIN_DRV_GRP(6, DRV_CON12, 12, 0),

	/* MSDC0_DAT 3 to 0 */
	MTK_PIN_DRV_GRP(7, DRV_CON0, 0, 0),
	MTK_PIN_DRV_GRP(8, DRV_CON0, 0, 0),
	MTK_PIN_DRV_GRP(9, DRV_CON0, 0, 0),
	MTK_PIN_DRV_GRP(10, DRV_CON0, 0, 0),

	/* NFI */
	MTK_PIN_DRV_GRP(11, DRV_CON0, 12, 1),
	MTK_PIN_DRV_GRP(12, DRV_CON0, 12, 1),
	MTK_PIN_DRV_GRP(13, DRV_CON0, 12, 1),
	MTK_PIN_DRV_GRP(14, DRV_CON0, 12, 1),
	MTK_PIN_DRV_GRP(15, DRV_CON0, 12, 1),
	MTK_PIN_DRV_GRP(16, DRV_CON0, 12, 1),
	MTK_PIN_DRV_GRP(17, DRV_CON0, 12, 1),

	/* NLD 0 to 15 */
	MTK_PIN_DRV_GRP(18, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(19, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(20, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(21, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(22, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(23, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(24, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(25, DRV_CON0, 16, 1),
	MTK_PIN_DRV_GRP(26, DRV_CON0, 20, 1),
	MTK_PIN_DRV_GRP(27, DRV_CON0, 20, 1),
	MTK_PIN_DRV_GRP(28, DRV_CON0, 20, 1),
	MTK_PIN_DRV_GRP(29, DRV_CON0, 20, 1),
	MTK_PIN_DRV_GRP(30, DRV_CON0, 20, 1),
	MTK_PIN_DRV_GRP(31, DRV_CON0, 20, 1),
	MTK_PIN_DRV_GRP(32, DRV_CON0, 20, 1),
	MTK_PIN_DRV_GRP(33, DRV_CON0, 20, 1),

	/* EINT 0 to 4 */
	MTK_PIN_DRV_GRP(34, DRV_CON0, 24, 2),
	MTK_PIN_DRV_GRP(35, DRV_CON0, 28, 2),
	MTK_PIN_DRV_GRP(36, DRV_CON1, 0, 2),
	MTK_PIN_DRV_GRP(37, DRV_CON1, 4, 2),
	MTK_PIN_DRV_GRP(38, DRV_CON1, 8, 2),

	/* SPI0 */
	MTK_PIN_DRV_GRP(39, DRV_CON1, 12, 1),
	MTK_PIN_DRV_GRP(40, DRV_CON1, 12, 1),
	MTK_PIN_DRV_GRP(41, DRV_CON1, 12, 1),
	MTK_PIN_DRV_GRP(42, DRV_CON1, 12, 1),
	MTK_PIN_DRV_GRP(43, DRV_CON1, 12, 1),

	/* SIM */
	MTK_PIN_DRV_GRP(44, DRV_CON1, 16, 1),
	MTK_PIN_DRV_GRP(45, DRV_CON1, 16, 1),
	MTK_PIN_DRV_GRP(46, DRV_CON1, 16, 1),
	MTK_PIN_DRV_GRP(47, DRV_CON1, 16, 1),
	MTK_PIN_DRV_GRP(48, DRV_CON1, 16, 1),
	MTK_PIN_DRV_GRP(49, DRV_CON1, 16, 1),

	/* ADC */
	MTK_PIN_DRV_GRP(50, DRV_CON1, 20, 1),
	MTK_PIN_DRV_GRP(51, DRV_CON1, 20, 1),
	MTK_PIN_DRV_GRP(52, DRV_CON1, 20, 1),

	/* DAC */
	MTK_PIN_DRV_GRP(53, DRV_CON1, 24, 1),
	MTK_PIN_DRV_GRP(54, DRV_CON1, 24, 1),
	MTK_PIN_DRV_GRP(55, DRV_CON1, 24, 1),

	/* RTC32K_CK */
	/*
	MTK_PIN_DRV_GRP(56, , , 1), // no drive?
	*/

	/* IDDIG */
	MTK_PIN_DRV_GRP(57, DRV_CON1, 28, 2),

	/* WATCHDOG */
	MTK_PIN_DRV_GRP(58, DRV_CON2, 0, 1),

	/* SRCLKENA */
	MTK_PIN_DRV_GRP(59, DRV_CON2, 4, 1),

	/* SRCVOLTEN */
	MTK_PIN_DRV_GRP(60, DRV_CON2, 8, 1),

	/* JTAG */
	MTK_PIN_DRV_GRP(61, DRV_CON3, 0, 1),
	MTK_PIN_DRV_GRP(62, DRV_CON3, 0, 1),
	MTK_PIN_DRV_GRP(63, DRV_CON3, 0, 1),
	MTK_PIN_DRV_GRP(64, DRV_CON3, 0, 1),
	MTK_PIN_DRV_GRP(65, DRV_CON3, 0, 1),
	MTK_PIN_DRV_GRP(66, DRV_CON3, 0, 1),

	/* UR2 */
	MTK_PIN_DRV_GRP(69, DRV_CON3, 4, 1),
	MTK_PIN_DRV_GRP(70, DRV_CON3, 8, 1),
	MTK_PIN_DRV_GRP(71, DRV_CON3, 12, 1),
	MTK_PIN_DRV_GRP(72, DRV_CON3, 16, 1),

	/* PWM 1 to 4 */
	MTK_PIN_DRV_GRP(73, DRV_CON3, 20, 1),
	MTK_PIN_DRV_GRP(74, DRV_CON3, 24, 1),
	MTK_PIN_DRV_GRP(75, DRV_CON3, 28, 1),
	MTK_PIN_DRV_GRP(76, DRV_CON4, 0, 1),

	/* UR1 */
	MTK_PIN_DRV_GRP(77, DRV_CON4, 4, 1),
	MTK_PIN_DRV_GRP(78, DRV_CON4, 8, 1),
	MTK_PIN_DRV_GRP(79, DRV_CON4, 12, 1),
	MTK_PIN_DRV_GRP(80, DRV_CON4, 16, 1),

	/* UR4 */
	MTK_PIN_DRV_GRP(81, DRV_CON4, 20, 1),
	MTK_PIN_DRV_GRP(82, DRV_CON4, 24, 1),

	/* BPI1B */
	MTK_PIN_DRV_GRP(83, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(84, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(85, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(86, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(87, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(88, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(89, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(90, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(91, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(92, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(93, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(94, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(95, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(96, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(97, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(98, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(99, DRV_CON5, 12, 1),

	/* VM 1, 0 */
	MTK_PIN_DRV_GRP(100, DRV_CON5, 12, 1),
	MTK_PIN_DRV_GRP(101, DRV_CON5, 12, 1),

	/* BSI 1 */
	MTK_PIN_DRV_GRP(102, DRV_CON5, 16, 1),
	MTK_PIN_DRV_GRP(103, DRV_CON5, 16, 1),
	MTK_PIN_DRV_GRP(104, DRV_CON5, 16, 1),

	/* TXBPI1 */
	MTK_PIN_DRV_GRP(105, DRV_CON5, 20, 1),

	/* EXT_CLK_EN */
	MTK_PIN_DRV_GRP(106, DRV_CON4, 28, 1),

	/* SRCLKENA2 */
	MTK_PIN_DRV_GRP(107, DRV_CON5, 0, 1),

	/* BSI1A */
	MTK_PIN_DRV_GRP(108, DRV_CON5, 4, 1),
	MTK_PIN_DRV_GRP(109, DRV_CON5, 4, 1),
	MTK_PIN_DRV_GRP(110, DRV_CON5, 4, 1),
	MTK_PIN_DRV_GRP(111, DRV_CON5, 4, 1),
	MTK_PIN_DRV_GRP(112, DRV_CON5, 4, 1),

	/* BSI1C */
	MTK_PIN_DRV_GRP(113, DRV_CON5, 8, 1),
	MTK_PIN_DRV_GRP(114, DRV_CON5, 8, 1),

	/* EINT10_AUXIN2, EINT11_AUXIN3, EINT16_AUXIN3 */
	MTK_PIN_DRV_GRP(115, DRV_CON6, 0, 1),
	MTK_PIN_DRV_GRP(116, DRV_CON6, 4, 1),
	MTK_PIN_DRV_GRP(117, DRV_CON6, 8, 1),

	/* I2S */
	MTK_PIN_DRV_GRP(120, DRV_CON6, 12, 1),
	MTK_PIN_DRV_GRP(121, DRV_CON6, 12, 1),
	MTK_PIN_DRV_GRP(122, DRV_CON6, 12, 1),
	MTK_PIN_DRV_GRP(123, DRV_CON6, 12, 1),

	/* EINT 5 to 9 */
	MTK_PIN_DRV_GRP(124, DRV_CON6, 16, 2),
	MTK_PIN_DRV_GRP(125, DRV_CON6, 20, 2),
	MTK_PIN_DRV_GRP(126, DRV_CON6, 24, 2),
	MTK_PIN_DRV_GRP(127, DRV_CON6, 28, 2),
	MTK_PIN_DRV_GRP(128, DRV_CON7, 0, 2),

	/* DISP_PWM */
	MTK_PIN_DRV_GRP(129, DRV_CON7, 28, 1),

	/* LPTE/MSDC4_DAT0, LRSTB/MSDC4_DAT1 */
	MTK_PIN_DRV_GRP(130, DRV_CON8, 20, 0),
	MTK_PIN_DRV_GRP(131, DRV_CON8, 20, 0),

	/* LPCE1B, LPCE0B */
	MTK_PIN_DRV_GRP(132, DRV_CON8, 28, 1),
	MTK_PIN_DRV_GRP(133, DRV_CON9, 0, 1),

	/* SPI1 / MSDC4 */
	MTK_PIN_DRV_GRP(134, DRV_CON8, 20, 0),
	MTK_PIN_DRV_GRP(135, DRV_CON8, 20, 0),
	MTK_PIN_DRV_GRP(136, DRV_CON8, 20, 0),
	MTK_PIN_DRV_GRP(137, DRV_CON8, 20, 0),

	/* LCD / MSDC4 */
	MTK_PIN_DRV_GRP(138, DRV_CON8, 20, 0),
	MTK_PIN_DRV_GRP(139, DRV_CON8, 0, 0),
	MTK_PIN_DRV_GRP(140, DRV_CON8, 20, 0),
	MTK_PIN_DRV_GRP(141, DRV_CON7, 16, 0),
	MTK_PIN_DRV_GRP(142, DRV_CON7, 20, 1),

	/* DPI */
	MTK_PIN_DRV_GRP(143, DRV_CON9, 8, 1),
	MTK_PIN_DRV_GRP(144, DRV_CON9, 8, 1),
	MTK_PIN_DRV_GRP(145, DRV_CON9, 8, 1),
	MTK_PIN_DRV_GRP(146, DRV_CON9, 8, 1),
	MTK_PIN_DRV_GRP(147, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(148, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(149, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(150, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(151, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(152, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(153, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(154, DRV_CON9, 12, 1),
	MTK_PIN_DRV_GRP(155, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(156, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(157, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(158, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(159, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(160, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(161, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(162, DRV_CON9, 16, 1),
	MTK_PIN_DRV_GRP(163, DRV_CON9, 20, 1),
	MTK_PIN_DRV_GRP(164, DRV_CON9, 20, 1),
	MTK_PIN_DRV_GRP(165, DRV_CON9, 20, 1),
	MTK_PIN_DRV_GRP(166, DRV_CON9, 20, 1),
	MTK_PIN_DRV_GRP(167, DRV_CON9, 20, 1),
	MTK_PIN_DRV_GRP(168, DRV_CON9, 20, 1),
	MTK_PIN_DRV_GRP(169, DRV_CON9, 20, 1),
	MTK_PIN_DRV_GRP(170, DRV_CON9, 20, 1),

	/* MSDC1_INSI, MSDC2_INSI */
	MTK_PIN_DRV_GRP(171, DRV_CON9, 24, 1),
	MTK_PIN_DRV_GRP(172, DRV_CON10, 0, 1),

	/* MSDC2 */
	MTK_PIN_DRV_GRP(173, DRV_CON10, 4, 1),
	MTK_PIN_DRV_GRP(174, DRV_CON10, 8, 3),
	MTK_PIN_DRV_GRP(175, DRV_CON10, 8, 3),
	MTK_PIN_DRV_GRP(176, DRV_CON10, 12, 3),
	MTK_PIN_DRV_GRP(177, DRV_CON12, 20, 3),
	MTK_PIN_DRV_GRP(178, DRV_CON10, 8, 3),
	MTK_PIN_DRV_GRP(179, DRV_CON10, 8, 3),

	/* MSDC1 */
	MTK_PIN_DRV_GRP(180, DRV_CON10, 20, 3),
	MTK_PIN_DRV_GRP(181, DRV_CON10, 20, 3),
	MTK_PIN_DRV_GRP(182, DRV_CON10, 16, 1),
	MTK_PIN_DRV_GRP(183, DRV_CON10, 24, 3),
	MTK_PIN_DRV_GRP(184, DRV_CON12, 16, 3),
	MTK_PIN_DRV_GRP(185, DRV_CON10, 20, 3),
	MTK_PIN_DRV_GRP(186, DRV_CON10, 20, 3),

	/* CMPCLK, CMMCLK, CMRST, CMPDN, CMFLASH */
	MTK_PIN_DRV_GRP(209, DRV_CON11, 0, 1),
	MTK_PIN_DRV_GRP(210, DRV_CON11, 4, 1),
	MTK_PIN_DRV_GRP(211, DRV_CON11, 8, 1),
	MTK_PIN_DRV_GRP(212, DRV_CON11, 12, 1),
	MTK_PIN_DRV_GRP(213, DRV_CON11, 16, 1),

	/* SRCLKENAI */
	MTK_PIN_DRV_GRP(218, DRV_CON11, 20, 1),

	/* UR3 */
	MTK_PIN_DRV_GRP(219, DRV_CON11, 24, 1),
	MTK_PIN_DRV_GRP(220, DRV_CON11, 28, 1),

	/* PCM0 */
	MTK_PIN_DRV_GRP(221, DRV_CON12, 0, 2),
	MTK_PIN_DRV_GRP(222, DRV_CON12, 0, 2),
	MTK_PIN_DRV_GRP(223, DRV_CON12, 0, 2),
	MTK_PIN_DRV_GRP(224, DRV_CON12, 0, 2),
	MTK_PIN_DRV_GRP(225, DRV_CON12, 0, 2),

	/* MSDC3 */
	MTK_PIN_DRV_GRP(226, DRV_CON12, 4, 0),
	MTK_PIN_DRV_GRP(227, DRV_CON12, 4, 0),
	MTK_PIN_DRV_GRP(228, DRV_CON12, 8, 0),
	MTK_PIN_DRV_GRP(229, DRV_CON12, 24, 0),
	MTK_PIN_DRV_GRP(230, DRV_CON12, 4, 0),
	MTK_PIN_DRV_GRP(231, DRV_CON12, 4, 0),
};

#define MT6589_SIM_MODE_PER_REG	3
#define MT6589_SIM_MODE_BITS	4

/* based on mtk_pmx_set_mode (pinctrl-mtk-common.c)*/
static void mt6589_pinmux_set(struct regmap *reg, unsigned int pin, unsigned int mode)
{
	unsigned int pin2, reg_addr, val;
	unsigned char bit;
	unsigned int mask = (1L << MT6589_SIM_MODE_BITS) - 1;
	if (pin < 44 || pin > 49) return;

	pin2 = pin - 44;
	if (pin2 <= 2) reg_addr = 0x0980;
	else reg_addr = 0x09a0;

	mode &= mask;
	bit = pin2 % MT6589_SIM_MODE_PER_REG;
	mask <<= (MT6589_SIM_MODE_BITS * bit);
	val = (mode << (MT6589_SIM_MODE_BITS * bit));
	regmap_update_bits(reg, reg_addr, mask, val);
}

static int mt6589_pull_set(struct regmap *regmap,
		const struct mtk_pinctrl_devdata *devdata,
		unsigned int pin, bool isup, unsigned int arg)
{
	unsigned int pin2, reg_addr, bit_en, bit_sel;
	unsigned int mask, val;
	bool enable = (arg != MTK_PUPD_SET_R1R0_00);

	if (pin < 44 || pin > 49)
		return -EINVAL;

	pin2 = pin - 44;
	if (pin2 <= 2)
		reg_addr = 0x0990;
	else
		reg_addr = 0x09b0;

	bit_en = (pin2 % 3) + 4;
	bit_sel = (pin2 % 3) + 8;

	mask = BIT(bit_en) | BIT(bit_sel);
	val = (enable ? BIT(bit_en) : 0) | (isup ? BIT(bit_sel) : 0);

	regmap_update_bits(regmap, reg_addr, mask, val);

	return 0;
}

/* TODO: MSDC_R0, BIAS */
static const struct mtk_pinctrl_devdata mt6589_pinctrl_data = {
	.pins = mtk_pins_mt6589,
	.npins = ARRAY_SIZE(mtk_pins_mt6589),
	.grp_desc = mt6589_drv_grp,
	.n_grp_cls = ARRAY_SIZE(mt6589_drv_grp),
	.pin_drv_grp = mt6589_pin_drv,
	.n_pin_drv_grps = ARRAY_SIZE(mt6589_pin_drv),
	.drv_multibase = true,
	.spec_pull_set = mt6589_pull_set,
	.dir_offset = 0x0000,
	.ies_offset = 0x0100,
	.ies_multibase = true,
	.pullen_offset = 0x0200,
	.pullen_multibase = true,
	.smt_offset = 0x0300,
	.smt_multibase = true,
	.pullsel_offset = 0x0400,
	.pullsel_multibase = true,
	.dout_offset = 0x0800,
	.din_offset = 0x0a00,
	.pinmux_offset = 0x0c00,
	.spec_pinmux_set = mt6589_pinmux_set,
	.type1_start = 114,
	.type1_end = 169 + 1,
	.port_shf = 4,
	.port_mask = 0xf,
	.port_align = 4,
	.mode_mask = 0xf,
	.mode_per_reg = 5,
	.mode_shf = 4,
	.eint_hw = {
		.port_mask = 7,
		.ports     = 6,
		.ap_num    = 192,
		.db_cnt    = 16,
		.db_time   = debounce_time_mt6795,
	},
};

static const struct of_device_id mt6589_pctrl_match[] = {
	{ .compatible = "mediatek,mt6589-pinctrl", .data = &mt6589_pinctrl_data },
	{},
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
