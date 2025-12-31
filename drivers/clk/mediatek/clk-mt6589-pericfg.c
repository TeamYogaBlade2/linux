// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

#define PERI_PDN0_SET	0x0008
#define PERI_PDN0_CLR	0x0010
#define PERI_PDN0_STA	0x0018
#define PERI_PDN1_SET	0x000c
#define PERI_PDN1_CLR	0x0014
#define PERI_PDN1_STA	0x001c

static const struct mtk_gate_regs peri0_cg_regs = {
	.set_ofs = PERI_PDN0_SET,
	.clr_ofs = PERI_PDN0_CLR,
	.sta_ofs = PERI_PDN0_STA,
};

static const struct mtk_gate_regs peri1_cg_regs = {
	.set_ofs = PERI_PDN1_SET,
	.clr_ofs = PERI_PDN1_CLR,
	.sta_ofs = PERI_PDN1_STA,
};

#define GATE_PERI0(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &peri0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_PERI1(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &peri1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate peri_clks[] = {
	GATE_PERI0(CLK_PERI0_NFI, "peri_nfi", "axi_sel", 0), /* auxadc, maybe */
	GATE_PERI0(CLK_PERI0_THERM, "peri_therm", "axi_sel", 1), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM1, "peri_pwm1", "axi_sel", 2), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM2, "peri_pwm2", "axi_sel", 3), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM3, "peri_pwm3", "axi_sel", 4), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM4, "peri_pwm4", "axi_sel", 5), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM5, "pwri_pwm5", "axi_sel", 6), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM6, "pwri_pwm6", "axi_sel", 7), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM7, "peri_pwm7", "axi_sel", 8), /* maybe */
	GATE_PERI0(CLK_PERI0_PWM, "peri_pwm", "axi_sel", 9), /* maybe */
	GATE_PERI0(CLK_PERI0_USB0, "peri_usb0", "usb20_sel", 10),
	GATE_PERI0(CLK_PERI0_USB1, "peri_usb1", "usb20_sel", 11),
	GATE_PERI0(CLK_PERI0_APDMA, "peri_apdma", "axi_sel", 12), /* maybe */
	GATE_PERI0(CLK_PERI0_MSDC0, "peri_msdc0", "msdc0_sel", 13),
	GATE_PERI0(CLK_PERI0_MSDC1, "peri_msdc1", "msdc1_sel", 14),
	GATE_PERI0(CLK_PERI0_MSDC2, "peri_msdc2", "msdc2_sel", 15),
	GATE_PERI0(CLK_PERI0_MSDC3, "peri_msdc3", "msdc3_sel", 16),
	GATE_PERI0(CLK_PERI0_MSDC4, "peri_msdc4", "msdc4_sel", 17),
	GATE_PERI0(CLK_PERI0_APHIF, "peri_aphif", "axi_sel", 18), /* maybe */
	GATE_PERI0(CLK_PERI0_MDHIF, "peri_mdhif", "axi_sel", 19), /* maybe */
	GATE_PERI0(CLK_PERI0_NLI, "peri_nli", "axi_sel", 20), /* maybe */
	GATE_PERI0(CLK_PERI0_IRDA, "peri_irda", "irda_sel", 21),
	GATE_PERI0(CLK_PERI0_UART0, "peri_uart0", "peri_uart0_sel", 22),
	GATE_PERI0(CLK_PERI0_UART1, "peri_uart1", "peri_uart1_sel", 23),
	GATE_PERI0(CLK_PERI0_UART2, "peri_uart2", "peri_uart2_sel", 24),
	GATE_PERI0(CLK_PERI0_UART3, "peri_uart3", "peri_uart3_sel", 25),
	GATE_PERI0(CLK_PERI0_I2C0, "peri_i2c0", "axi_sel", 26), /* maybe */
	GATE_PERI0(CLK_PERI0_I2C1, "peri_i2c1", "axi_sel", 27), /* maybe */
	GATE_PERI0(CLK_PERI0_I2C2, "peri_i2c2", "axi_sel", 28), /* maybe */
	GATE_PERI0(CLK_PERI0_I2C3, "peri_i2c3", "axi_sel", 29), /* maybe */
	GATE_PERI0(CLK_PERI0_I2C4, "peri_i2c4", "axi_sel", 30), /* maybe */
	GATE_PERI0(CLK_PERI0_I2C5, "peri_i2c5", "axi_sel", 31), /* maybe */

	GATE_PERI1(CLK_PERI1_I2C6, "peri_i2c6", "axi_sel", 0), /* maybe */
	GATE_PERI1(CLK_PERI1_PWRAP, "peri_pwrap", "axi_sel", 1), /* maybe */
	GATE_PERI1(CLK_PERI1_AUXADC, "peri_auxadc", "axi_sel", 2), /* maybe */
	GATE_PERI1(CLK_PERI1_SPI1, "peri_spi1", "spi_sel", 3),
	GATE_PERI1(CLK_PERI1_FHCTL, "peri_fhctl", "clk26m", 4), /* maybe */
};

static const char * const uart_mux_parents[] = {
	"clk26m", /* 26 MHz */
	"uart_sel", /* or univpll2_d8, 52 MHz */
};

static const struct mtk_composite peri_muxs[] = {
	MUX(CLK_PERI_MUX_UART0, "peri_uart0_sel", uart_mux_parents,
		0x040c, 0, 1),
	MUX(CLK_PERI_MUX_UART1, "peri_uart1_sel", uart_mux_parents,
		0x040c, 1, 1),
	MUX(CLK_PERI_MUX_UART2, "peri_uart2_sel", uart_mux_parents,
		0x040c, 2, 1),
	MUX(CLK_PERI_MUX_UART3, "peri_uart3_sel", uart_mux_parents,
		0x040c, 3, 1),
};

static u16 pericfg_rst_ofs[] = { 0x0, 0x4, };

static const struct mtk_clk_rst_desc peri_clk_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = pericfg_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(pericfg_rst_ofs),
};

static const struct mtk_clk_desc peri_desc = {
	.clks = peri_clks,
	.num_clks = ARRAY_SIZE(peri_clks),
	.composite_clks = peri_muxes,
	.num_composite_clks = ARRAY_SIZE(peri_muxes),
	.rst_desc = &peri_clk_rst_desc,
};


static const struct of_device_id of_match_clk_mt6589_pericfg[] = {
	{ .compatible = "mediatek,mt6589-pericfg", .data = &peri_desc, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_pericfg);

static struct platform_driver clk_mt6589_pericfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-pericfg",
		.of_match_table = of_match_clk_mt6589_pericfg,
	},
};
module_platform_driver(clk_mt6589_pericfg_drv);

MODULE_DESCRIPTION("MediaTek MT6589 pericfg clocks driver");
MODULE_LICENSE("GPL");
