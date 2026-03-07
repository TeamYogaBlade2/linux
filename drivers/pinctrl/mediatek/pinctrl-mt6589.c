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

/* for 2nd base */
#define PINS_FIELD1(_s_pin, _e_pin, _s_addr, _x_addrs, _s_bit, _x_bits)	\
	PIN_FIELD_CALC(_s_pin, _e_pin, 1, _s_addr, _x_addrs, _s_bit,	\
		       _x_bits, 32, 1)

/* all R0 are in one register */
#define PIN_FIELD_R0(_bit, _pin) \
	PIN_FIELD_CALC(_pin, _pin, 0, 0x04f0, 0x0, _bit, 1, 32, 1)

/*
 * MT6589's Drive/Slew Rate register
 *
 * E2, E4, E8, E16: Drive
 * SR: Slew Rate
 * DM: Dummy
 * MSB <-> LSB
 *
 * DRV_GRP1: SR  E8  E4  DM  : 4/8/12/16mA
 * DRV_GRP3: SR  E4  E2  DM  : 2/4/6/8mA
 * DRV_GRP4: SR  E8  E4  E2  : 2/4/6/8/10/12/14/16mA
 * DRV_GRP5: SR  E16 E8  E4  : 4/8/12/16/20/24/28/32mA
 */

#define PIN_FIELD_SR(_pin, _offset, _bit, _base) \
	PIN_FIELD_CALC(_pin, _pin, _base, _offset, 0x0, _bit, 1, 32, 1)

#define PINS_FIELD_SR(_pin_s, _pin_e, _offset, _bit, _base) \
	PIN_FIELD_CALC(_pin_s, _pin_e, _base, _offset, 0x0, _bit, 1, 32, 1)

#define PIN_FIELD_DRV(_pin, _offset, _bit, _base) \
	PIN_FIELD_CALC(_pin, _pin, _base, _offset, 0x0, _bit, 3, 32, 1)

#define PINS_FIELD_DRV(_pin_s, _pin_e, _offset, _bit, _base) \
	PIN_FIELD_CALC(_pin_s, _pin_e, _base, _offset, 0x0, _bit, 3, 32, 1)

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

static const struct mtk_pin_field_calc mt6589_pin_sr_range[] = {
	/* MSDC0_DAT 7 to 4 */
	PINS_FIELD_SR(0, 3, DRV_CON0, 3, 0),

	/* MSDC0_RSTB */
	PIN_FIELD_SR(4, DRV_CON0, 11, 0),

	/* MSDC0_CMD */
	PIN_FIELD_SR(5, DRV_CON0, 7, 0),

	/* MSDC0_CLK */
	PIN_FIELD_SR(6, DRV_CON12, 15, 0),

	/* MSDC0_DAT 3 to 0 */
	PINS_FIELD_SR(7, 10, DRV_CON0, 3, 0),

	/* NFI */
	PINS_FIELD_SR(11, 17, DRV_CON0, 15, 0),

	/* NLD 0 to 15 */
	PINS_FIELD_SR(18, 25, DRV_CON0, 19, 0),
	PINS_FIELD_SR(26, 33, DRV_CON0, 23, 0),

	/* EINT 0 to 4 */
	PIN_FIELD_SR(34, DRV_CON0, 27, 0),
	PIN_FIELD_SR(35, DRV_CON0, 31, 0),
	PIN_FIELD_SR(36, DRV_CON1, 3, 0),
	PIN_FIELD_SR(37, DRV_CON1, 7, 0),
	PIN_FIELD_SR(38, DRV_CON1, 11, 0),

	/* SPI0 */
	PINS_FIELD_SR(39, 43, DRV_CON1, 15, 0),

	/* SIM */
	PINS_FIELD_SR(44, 49, DRV_CON1, 19, 0),

	/* ADC */
	PINS_FIELD_SR(50, 52, DRV_CON1, 23, 0),

	/* DAC */
	PINS_FIELD_SR(53, 55, DRV_CON1, 27, 0),

	/* RTC32K_CK */
	/*
	PIN_FIELD_SR(56, , , 0), // no drive?
	*/

	/* IDDIG */
	PIN_FIELD_SR(57, DRV_CON1, 31, 0),

	/* WATCHDOG */
	PIN_FIELD_SR(58, DRV_CON2, 3, 0),

	/* SRCLKENA */
	PIN_FIELD_SR(59, DRV_CON2, 7, 0),

	/* SRCVOLTEN */
	PIN_FIELD_SR(60, DRV_CON2, 11, 0),

	/* JTAG */
	PINS_FIELD_SR(61, 66, DRV_CON3, 3, 0),

	/* UR2 */
	PIN_FIELD_SR(69, DRV_CON3, 7, 0),
	PIN_FIELD_SR(70, DRV_CON3, 11, 0),
	PIN_FIELD_SR(71, DRV_CON3, 15, 0),
	PIN_FIELD_SR(72, DRV_CON3, 19, 0),

	/* PWM 1 to 4 */
	PIN_FIELD_SR(73, DRV_CON3, 23, 0),
	PIN_FIELD_SR(74, DRV_CON3, 27, 0),
	PIN_FIELD_SR(75, DRV_CON3, 31, 0),
	PIN_FIELD_SR(76, DRV_CON4, 3, 0),

	/* UR1 */
	PIN_FIELD_SR(77, DRV_CON4, 7, 0),
	PIN_FIELD_SR(78, DRV_CON4, 11, 0),
	PIN_FIELD_SR(79, DRV_CON4, 15, 0),
	PIN_FIELD_SR(80, DRV_CON4, 19, 0),

	/* UR4 */
	PIN_FIELD_SR(81, DRV_CON4, 23, 0),
	PIN_FIELD_SR(82, DRV_CON4, 27, 0),

	/* BPI1B */
	PINS_FIELD_SR(83, 99, DRV_CON5, 15, 0),

	/* VM 1, 0 */
	PINS_FIELD_SR(100, 101, DRV_CON5, 15, 0),

	/* BSI 1 */
	PINS_FIELD_SR(102, 104, DRV_CON5, 19, 0),

	/* TXBPI1 */
	PIN_FIELD_SR(105, DRV_CON5, 23, 0),

	/* EXT_CLK_EN */
	PIN_FIELD_SR(106, DRV_CON4, 31, 0),

	/* SRCLKENA2 */
	PIN_FIELD_SR(107, DRV_CON5, 3, 0),

	/* BSI1A */
	PINS_FIELD_SR(108, 112, DRV_CON5, 7, 0),

	/* BSI1C */
	PINS_FIELD_SR(113, 114, DRV_CON5, 11, 0),

	/* EINT10_AUXIN2, EINT11_AUXIN3, EINT16_AUXIN3 */
	PIN_FIELD_SR(115, DRV_CON6, 3, 1),
	PIN_FIELD_SR(116, DRV_CON6, 7, 1),
	PIN_FIELD_SR(117, DRV_CON6, 11, 1),

	/* I2S */
	PINS_FIELD_SR(120, 123, DRV_CON6, 15, 1),

	/* EINT 5 to 9 */
	PIN_FIELD_SR(124, DRV_CON6, 19, 1),
	PIN_FIELD_SR(125, DRV_CON6, 23, 1),
	PIN_FIELD_SR(126, DRV_CON6, 27, 1),
	PIN_FIELD_SR(127, DRV_CON6, 31, 1),
	PIN_FIELD_SR(128, DRV_CON7, 3, 1),

	/* DISP_PWM */
	PIN_FIELD_SR(129, DRV_CON7, 31, 1),

	/* LPTE/MSDC4_DAT0, LRSTB/MSDC4_DAT1 */
	PINS_FIELD_SR(130, 131, DRV_CON8, 23, 1),

	/* LPCE1B, LPCE0B */
	PIN_FIELD_SR(132, DRV_CON8, 31, 1),
	PIN_FIELD_SR(133, DRV_CON9, 3, 1),

	/* SPI1 / MSDC4 */
	PINS_FIELD_SR(134, 137, DRV_CON8, 23, 1),

	/* LCD / MSDC4 */
	PIN_FIELD_SR(138, DRV_CON8, 23, 1),
	PIN_FIELD_SR(139, DRV_CON8, 3, 1),
	PIN_FIELD_SR(140, DRV_CON8, 23, 1),
	PIN_FIELD_SR(141, DRV_CON7, 19, 1),
	PIN_FIELD_SR(142, DRV_CON7, 23, 1),

	/* DPI */
	PINS_FIELD_SR(143, 146, DRV_CON9, 11, 1),
	PINS_FIELD_SR(147, 154, DRV_CON9, 15, 1),
	PINS_FIELD_SR(155, 162, DRV_CON9, 19, 1),
	PINS_FIELD_SR(163, 170, DRV_CON9, 23, 1),

	/* MSDC1_INSI, MSDC2_INSI */
	PIN_FIELD_SR(171, DRV_CON9, 27, 1),
	PIN_FIELD_SR(172, DRV_CON10, 3, 0),

	/* MSDC2 */
	PIN_FIELD_SR(173, DRV_CON10, 7, 0),
	PINS_FIELD_SR(174, 175, DRV_CON10, 11, 0),
	PIN_FIELD_SR(176, DRV_CON10, 15, 0),
	PIN_FIELD_SR(177, DRV_CON12, 23, 0),
	PINS_FIELD_SR(178, 179, DRV_CON10, 11, 0),

	/* MSDC1 */
	PINS_FIELD_SR(180, 181, DRV_CON10, 23, 0),
	PIN_FIELD_SR(182, DRV_CON10, 19, 0),
	PIN_FIELD_SR(183, DRV_CON10, 27, 0),
	PIN_FIELD_SR(184, DRV_CON12, 19, 0),
	PINS_FIELD_SR(185, 186, DRV_CON10, 23, 0),

	/* CMPCLK, CMMCLK, CMRST, CMPDN, CMFLASH */
	PIN_FIELD_SR(209, DRV_CON11, 3, 0),
	PIN_FIELD_SR(210, DRV_CON11, 7, 0),
	PIN_FIELD_SR(211, DRV_CON11, 11, 0),
	PIN_FIELD_SR(212, DRV_CON11, 15, 0),
	PIN_FIELD_SR(213, DRV_CON11, 19, 0),

	/* SRCLKENAI */
	PIN_FIELD_SR(218, DRV_CON11, 23, 0),

	/* UR3 */
	PIN_FIELD_SR(219, DRV_CON11, 27, 0),
	PIN_FIELD_SR(220, DRV_CON11, 31, 0),

	/* PCM0 */
	PINS_FIELD_SR(221, 235, DRV_CON12, 3, 0),

	/* MSDC3 */
	PINS_FIELD_SR(226, 227, DRV_CON12, 7, 0),
	PIN_FIELD_SR(228, DRV_CON12, 11, 0),
	PIN_FIELD_SR(229, DRV_CON12, 27, 0),
	PINS_FIELD_SR(230, 231, DRV_CON12, 7, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_smt_range[] = {
	PIN_FIELD_CALC(0, 113, 0, 0x0300, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(114, 169, 1, 0x0370, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x03a0, 0x10, 10, 1, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_tdsel_range[] = {
	/* MSDC0_RSTB */
	PINS_FIELD(4, 4, 0x0700, 0x0, 0, 4),
	/* MSDC0 */
	PINS_FIELD(, , 0x0700, 0x0, 8, 4),
	/* NFI */
	PINS_FIELD(, , 0x0700, 0x0, 24, 4),
	/* EINT */
	PINS_FIELD(, , 0x0710, 0x0, 0, 4),
	/* SPI */
	PINS_FIELD(, , 0x0710, 0x0, 8, 4),
	/* SIM */
	PINS_FIELD(, , 0x0710, 0x0, 16, 4),
	/* I2S */
	PINS_FIELD(, , 0x0710, 0x0, 24, 4),
	/* GPIO */
	PINS_FIELD(, , 0x0720, 0x0, 0, 4),
	/* JTAG */
	PINS_FIELD(, , 0x0730, 0x0, 0, 4),
	/* UART */
	PINS_FIELD(, , 0x0730, 0x0, 8, 4),
	/* PWM */
	PINS_FIELD(, , 0x0730, 0x0, 24, 4),
	/* BPI */
	PINS_FIELD(, , 0x0740, 0x0, 8, 4),
	/* BSI1A */
	PINS_FIELD(, , 0x0740, 0x0, 24, 4),
	/* BSI1B */
	PINS_FIELD(, , 0x0750, 0x0, 0, 4),
	/* BSI1C */
	PINS_FIELD(, , 0x0750, 0x0, 16, 4),
	/* PCM */
	PINS_FIELD1(, , 0x0760, 0x0, 0, 4),
	/* LCD */
	PINS_FIELD1(, , 0x0760, 0x0, 8, 4),
	/* MSDC4_RSTB */
	PINS_FIELD1(, , 0x0760, 0x0, 16, 4),
	/* MSDC4 */
	PINS_FIELD1(, , 0x0760, 0x0, 24, 4),
	/* DPI */
	PINS_FIELD1(, , 0x0770, 0x0, 8, 4),
	/* MSDC1INSI */
	PINS_FIELD1(, , 0x0770, 0x0, 16, 4),
	/* MSDC2 */
	PINS_FIELD(, , 0x0780, 0x0, 0, 4),
	/* MSDC1 */
	PINS_FIELD(, , 0x0780, 0x0, 16, 4),
	/* CAM */
	PINS_FIELD(, , 0x0790, 0x0, 0, 4),
	/* I2C */
	PINS_FIELD(, , 0x0790, 0x0, 8, 4),
	/* MSDC3 */
	PINS_FIELD(, , 0x0790, 0x0, 16, 4),
};

static const struct mtk_pin_field_calc mt6589_pin_rdsel_range[] = {
	/* MSDC0_RSTB */
	PINS_FIELD(4, 4, 0x0700, 0x0, 4, 2),
	/* MSDC0 */
	PINS_FIELD(, , 0x0700, 0x0, 16, 6),
	/* NFI */
	PINS_FIELD(, , 0x0700, 0x0, 28, 2),
	/* EINT */
	PINS_FIELD(, , 0x0710, 0x0, 4, 2),
	/* SPI */
	PINS_FIELD(, , 0x0710, 0x0, 12, 2),
	/* SIM */
	PINS_FIELD(, , 0x0710, 0x0, 20, 2),
	/* I2S */
	PINS_FIELD(, , 0x0710, 0x0, 28, 2),
	/* GPIO */
	PINS_FIELD(, , 0x0720, 0x0, 4, 2),
	/* JTAG */
	PINS_FIELD(, , 0x0730, 0x0, 4, 2),
	/* UART */
	PINS_FIELD(, , 0x0730, 0x0, 16, 6),
	/* PWM */
	PINS_FIELD(, , 0x0740, 0x0, 0, 6),
	/* BPI */
	PINS_FIELD(, , 0x0740, 0x0, 16, 6),
	/* BSI1A */
	PINS_FIELD(, , 0x0740, 0x0, 28, 2),
	/* BSI1B */
	PINS_FIELD(, , 0x0750, 0x0, 8, 6),
	/* BSI1C */
	PINS_FIELD(, , 0x0750, 0x0, 20, 2),
	/* PCM */
	PINS_FIELD1(, , 0x0760, 0x0, 4, 2),
	/* LCD */
	PINS_FIELD1(, , 0x0760, 0x0, 12, 2),
	/* MSDC4_RSTB */
	PINS_FIELD1(, , 0x0760, 0x0, 20, 2),
	/* MSDC4 */
	PINS_FIELD1(, , 0x0770, 0x0, 0, 6),
	/* DPI */
	PINS_FIELD1(, , 0x0770, 0x0, 12, 2),
	/* MSDC1INSI */
	PINS_FIELD1(, , 0x0770, 0x0, 20, 2),
	/* MSDC2 */
	PINS_FIELD(, , 0x0780, 0x0, 8, 6),
	/* MSDC1 */
	PINS_FIELD(, , 0x0780, 0x0, 24, 6),
	/* CAM */
	PINS_FIELD(, , 0x0790, 0x0, 4, 2),
	/* I2C */
	PINS_FIELD(, , 0x0790, 0x0, 12, 2),
	/* MSDC3 */
	PINS_FIELD(, , 0x0790, 0x0, 24, 6),
};

static const struct mtk_pin_field_calc mt6589_pin_drv_range[] = {
	/* MSDC0_DAT 7 to 4 */
	PINS_FIELD_DRV(0, 3, DRV_CON0, 0, 0),

	/* MSDC0_RSTB */
	PIN_FIELD_DRV(4, DRV_CON0, 8, 0),

	/* MSDC0_CMD */
	PIN_FIELD_DRV(5, DRV_CON0, 4, 0),

	/* MSDC0_CLK */
	PIN_FIELD_DRV(6, DRV_CON12, 12, 0),

	/* MSDC0_DAT 3 to 0 */
	PINS_FIELD_DRV(7, 10, DRV_CON0, 0, 0),

	/* NFI */
	PINS_FIELD_DRV(11, 17, DRV_CON0, 12, 0),

	/* NLD 0 to 15 */
	PINS_FIELD_DRV(18, 25, DRV_CON0, 16, 0),
	PINS_FIELD_DRV(26, 33, DRV_CON0, 20, 0),

	/* EINT 0 to 4 */
	PIN_FIELD_DRV(34, DRV_CON0, 24, 0),
	PIN_FIELD_DRV(35, DRV_CON0, 28, 0),
	PIN_FIELD_DRV(36, DRV_CON1, 0, 0),
	PIN_FIELD_DRV(37, DRV_CON1, 4, 0),
	PIN_FIELD_DRV(38, DRV_CON1, 8, 0),

	/* SPI0 */
	PINS_FIELD_DRV(39, 43, DRV_CON1, 12, 0),

	/* SIM */
	PINS_FIELD_DRV(44, 49, DRV_CON1, 16, 0),

	/* ADC */
	PINS_FIELD_DRV(50, 53, DRV_CON1, 20, 0),

	/* DAC */
	PINS_FIELD_DRV(53, 55, DRV_CON1, 24, 0),

	/* RTC32K_CK */
	/*
	PIN_FIELD_DRV(56, , , 0), // no drive?
	*/

	/* IDDIG */
	PIN_FIELD_DRV(57, DRV_CON1, 28, 0),

	/* WATCHDOG */
	PIN_FIELD_DRV(58, DRV_CON2, 0, 0),

	/* SRCLKENA */
	PIN_FIELD_DRV(59, DRV_CON2, 4, 0),

	/* SRCVOLTEN */
	PIN_FIELD_DRV(60, DRV_CON2, 8, 0),

	/* JTAG */
	PINS_FIELD_DRV(61, 66, DRV_CON3, 0, 0),

	/* UR2 */
	PIN_FIELD_DRV(69, DRV_CON3, 4, 0),
	PIN_FIELD_DRV(70, DRV_CON3, 8, 0),
	PIN_FIELD_DRV(71, DRV_CON3, 12, 0),
	PIN_FIELD_DRV(72, DRV_CON3, 16, 0),

	/* PWM 1 to 4 */
	PIN_FIELD_DRV(73, DRV_CON3, 20, 0),
	PIN_FIELD_DRV(74, DRV_CON3, 24, 0),
	PIN_FIELD_DRV(75, DRV_CON3, 28, 0),
	PIN_FIELD_DRV(76, DRV_CON4, 0, 0),

	/* UR1 */
	PIN_FIELD_DRV(77, DRV_CON4, 4, 0),
	PIN_FIELD_DRV(78, DRV_CON4, 8, 0),
	PIN_FIELD_DRV(79, DRV_CON4, 12, 0),
	PIN_FIELD_DRV(80, DRV_CON4, 16, 0),

	/* UR4 */
	PIN_FIELD_DRV(81, DRV_CON4, 20, 0),
	PIN_FIELD_DRV(82, DRV_CON4, 24, 0),

	/* BPI1B */
	PINS_FIELD_DRV(83, 99, DRV_CON5, 12, 0),

	/* VM 1, 0 */
	PINS_FIELD_DRV(100, 101, DRV_CON5, 12, 0),

	/* BSI 1 */
	PINS_FIELD_DRV(102, 104, DRV_CON5, 16, 0),

	/* TXBPI1 */
	PIN_FIELD_DRV(105, DRV_CON5, 20, 0),

	/* EXT_CLK_EN */
	PIN_FIELD_DRV(106, DRV_CON4, 28, 0),

	/* SRCLKENA2 */
	PIN_FIELD_DRV(107, DRV_CON5, 0, 0),

	/* BSI1A */
	PINS_FIELD_DRV(108, 112, DRV_CON5, 4, 0),

	/* BSI1C */
	PINS_FIELD_DRV(113, 114, DRV_CON5, 8, 0),

	/* EINT10_AUXIN2, EINT11_AUXIN3, EINT16_AUXIN3 */
	PIN_FIELD_DRV(115, DRV_CON6, 0, 1),
	PIN_FIELD_DRV(116, DRV_CON6, 4, 1),
	PIN_FIELD_DRV(117, DRV_CON6, 8, 1),

	/* I2S */
	PINS_FIELD_DRV(120, 123, DRV_CON6, 12, 1),

	/* EINT 5 to 9 */
	PIN_FIELD_DRV(124, DRV_CON6, 16, 1),
	PIN_FIELD_DRV(125, DRV_CON6, 20, 1),
	PIN_FIELD_DRV(126, DRV_CON6, 24, 1),
	PIN_FIELD_DRV(127, DRV_CON6, 28, 1),
	PIN_FIELD_DRV(128, DRV_CON7, 0, 1),

	/* DISP_PWM */
	PIN_FIELD_DRV(129, DRV_CON7, 28, 1),

	/* LPTE/MSDC4_DAT0, LRSTB/MSDC4_DAT1 */
	PINS_FIELD_DRV(130, 131, DRV_CON8, 20, 1),

	/* LPCE1B, LPCE0B */
	PIN_FIELD_DRV(132, DRV_CON8, 28, 1),
	PIN_FIELD_DRV(133, DRV_CON9, 0, 1),

	/* SPI1 / MSDC4 */
	PINS_FIELD_DRV(134, 137, DRV_CON8, 20, 1),

	/* LCD / MSDC4 */
	PIN_FIELD_DRV(138, DRV_CON8, 20, 1),
	PIN_FIELD_DRV(139, DRV_CON8, 0, 1),
	PIN_FIELD_DRV(140, DRV_CON8, 20, 1),
	PIN_FIELD_DRV(141, DRV_CON7, 16, 1),
	PIN_FIELD_DRV(142, DRV_CON7, 20, 1),

	/* DPI */
	PINS_FIELD_DRV(143, 146, DRV_CON9, 8, 1),
	PINS_FIELD_DRV(147, 154, DRV_CON9, 12, 1),
	PINS_FIELD_DRV(155, 162, DRV_CON9, 16, 1),
	PINS_FIELD_DRV(163, 170, DRV_CON9, 20, 1),

	/* MSDC1_INSI, MSDC2_INSI */
	PIN_FIELD_DRV(171, DRV_CON9, 24, 1),
	PIN_FIELD_DRV(172, DRV_CON10, 0, 0),

	/* MSDC2 */
	PIN_FIELD_DRV(173, DRV_CON10, 4, 0),
	PINS_FIELD_DRV(174, 175, DRV_CON10, 8, 0),
	PIN_FIELD_DRV(176, DRV_CON10, 12, 0),
	PIN_FIELD_DRV(177, DRV_CON12, 20, 0),
	PINS_FIELD_DRV(178, 179, DRV_CON10, 8, 0),

	/* MSDC1 */
	PINS_FIELD_DRV(180, 181, DRV_CON10, 20, 0),
	PIN_FIELD_DRV(182, DRV_CON10, 16, 0),
	PIN_FIELD_DRV(183, DRV_CON10, 24, 0),
	PIN_FIELD_DRV(184, DRV_CON12, 16, 0),
	PINS_FIELD_DRV(185, 186, DRV_CON10, 20, 0),

	/* CMPCLK, CMMCLK, CMRST, CMPDN, CMFLASH */
	PIN_FIELD_DRV(209, DRV_CON11, 0, 0),
	PIN_FIELD_DRV(210, DRV_CON11, 4, 0),
	PIN_FIELD_DRV(211, DRV_CON11, 8, 0),
	PIN_FIELD_DRV(212, DRV_CON11, 12, 0),
	PIN_FIELD_DRV(213, DRV_CON11, 16, 0),

	/* SRCLKENAI */
	PIN_FIELD_DRV(218, DRV_CON11, 20, 0),

	/* UR3 */
	PIN_FIELD_DRV(219, DRV_CON11, 24, 0),
	PIN_FIELD_DRV(220, DRV_CON11, 28, 0),

	/* PCM0 */
	PINS_FIELD_DRV(221, 225, DRV_CON12, 0, 0),

	/* MSDC3 */
	PINS_FIELD_DRV(226, 227, DRV_CON12, 4, 0),
	PIN_FIELD_DRV(228, DRV_CON12, 8, 0),
	PIN_FIELD_DRV(229, DRV_CON12, 24, 0),
	PINS_FIELD_DRV(230, 231, DRV_CON12, 4, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_r0_range[] = {
	PIN_FIELD_R0(0, 6),
	PIN_FIELD_R0(1, 5),
	PIN_FIELD_R0(2, 10),
	PIN_FIELD_R0(3, 9),
	PIN_FIELD_R0(4, 8),
	PIN_FIELD_R0(5, 7),
	PIN_FIELD_R0(6, 3),
	PIN_FIELD_R0(7, 2),
	PIN_FIELD_R0(8, 1),
	PIN_FIELD_R0(9, 0),
	PIN_FIELD_R0(10, 229),
	PIN_FIELD_R0(11, 228),
	PIN_FIELD_R0(12, 231),
	PIN_FIELD_R0(13, 230),
	PIN_FIELD_R0(14, 226),
	PIN_FIELD_R0(15, 227),
	PIN_FIELD_R0(16, 139),
	PIN_FIELD_R0(17, 141),
	PIN_FIELD_R0(18, 130),
	PIN_FIELD_R0(19, 131),
	PIN_FIELD_R0(20, 138),
	PIN_FIELD_R0(21, 140),
	PIN_FIELD_R0(22, 137),
	PIN_FIELD_R0(23, 134),
	PIN_FIELD_R0(24, 135),
	PIN_FIELD_R0(25, 136),
};

static const struct mtk_pin_field_calc mt6589_pin_ies_range[] = {
	PIN_FIELD_CALC(0, 113, 0, 0x0100, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(114, 169, 1, 0x0170, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x01a0, 0x10, 10, 1, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_pullen_range[] = {
	PIN_FIELD_CALC(0, 43, 0, 0x0200, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(44, 46, 0, 0x0990, 0x0, 4, 1, 16, 0),
	PIN_FIELD_CALC(47, 49, 0, 0x09b0, 0x0, 4, 1, 16, 0),
	PIN_FIELD_CALC(50, 113, 0, 0x0230, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(114, 169, 1, 0x0270, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x02a0, 0x10, 10, 1, 16, 0),
};

static const struct mtk_pin_field_calc mt6589_pin_pullsel_range[] = {
	PIN_FIELD_CALC(0, 43, 0, 0x0400, 0x10, 0, 1, 16, 0),
	PIN_FIELD_CALC(44, 46, 0, 0x0990, 0x0, 8, 1, 16, 0),
	PIN_FIELD_CALC(47, 49, 0, 0x09b0, 0x0, 8, 1, 16, 0),
	PIN_FIELD_CALC(50, 113, 0, 0x0430, 0x10, 2, 1, 16, 0)
	PIN_FIELD_CALC(114, 169, 1, 0x0470, 0x10, 2, 1, 16, 0),
	PIN_FIELD_CALC(170, 231, 0, 0x04a0, 0x10, 10, 1, 16, 0),
};

static const struct mtk_pin_reg_calc mt6589_reg_cals[PINCTRL_PIN_REG_MAX] = {
	[PINCTRL_PIN_REG_MODE] = MTK_RANGE(mt6589_pin_mode_range),
	[PINCTRL_PIN_REG_DIR] = MTK_RANGE(mt6589_pin_dir_range),
	[PINCTRL_PIN_REG_DI] = MTK_RANGE(mt6589_pin_di_range),
	[PINCTRL_PIN_REG_DO] = MTK_RANGE(mt6589_pin_do_range),
	[PINCTRL_PIN_REG_SR] = MTK_RANGE(mt6589_pin_sr_range),
	[PINCTRL_PIN_REG_SMT] = MTK_RANGE(mt6589_pin_smt_range),
	[PINCTRL_PIN_REG_TDSEL] = MTK_RANGE(mt6589_pin_tdsel_range),
	[PINCTRL_PIN_REG_RDSEL] = MTK_RANGE(mt6589_pin_rdsel_range),
	[PINCTRL_PIN_REG_DRV] = MTK_RANGE(mt6589_pin_drv_range),
	[PINCTRL_PIN_REG_R0] = MTK_RANGE(mt6589_pin_r0_range),
	[PINCTRL_PIN_REG_IES] = MTK_RANGE(mt6589_pin_ies_range),
	[PINCTRL_PIN_REG_PULLEN] = MTK_RANGE(mt6589_pin_pullen_range),
	[PINCTRL_PIN_REG_PULLSEL] = MTK_RANGE(mt6589_pin_pullsel_range),
};

// DINV(no GPIO44~49), BIAS

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
	.ngrps = ARRAY_SIZE(mtk_pins_mt6589),
	.eint_hw = &mt6589_eint_hw,
	.gpio_m = 0,
	.base_names = mt6589_pinctrl_register_base_names,
	.nbase_names = ARRAY_SIZE(mt6589_pinctrl_register_base_names),
//	.pull_type =
/* disable? */
//	.bias_disable_set =
//	.bias_disable_get =
//	.bias_set =
//	.bias_get =
/* maybe */
	.bias_set_combo = mtk_pinconf_bias_set_combo,
	.bias_get_combo = mtk_pinconf_bias_get_combo,
	.drive_set = mtk_pinconf_drive_set_rev1,
	.drive_get = mtk_pinconf_drive_get_rev1,
/* maybe */
	.adv_pull_set = mtk_pinconf_adv_pull_set,
	.adv_pull_get = mtk_pinconf_adv_pull_get,
//	.adv_drive_set =
//	.adv_drive_get =
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
