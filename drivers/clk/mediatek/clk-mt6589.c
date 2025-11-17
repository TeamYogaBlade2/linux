// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>


/* TOPRGU - topckgen? */
/*
 * FIXME: are the regs correct?
 *	> #define CLK_CFG_0           (TOPRGU_BASE + 0x0140)
 *	but many SoCs use 0x0040 as CLK_CFG_0
 * FIXME: ops and flags
 * FIXME: MUX_AUDINTBUS siblings
 */

static const struct mtk_fixed_factor top_divs[] = {
	FACTOR(CLK_TOP_SYSPLL, "syspll_ck", "mainpll", 1, 2),
	FACTOR(CLK_TOP_SYSPLL_D2, "syspll_d2", "syspll_ck", 1, 2),
	FACTOR(CLK_TOP_SYSPLL_D3, "syspll_d3", "syspll_ck", 1, 3),
	FACTOR(CLK_TOP_SYSPLL_D4, "syspll_d4", "syspll_ck", 1, 4),
	FACTOR(CLK_TOP_SYSPLL_D6, "syspll_d6", "syspll_ck", 1, 6),
	FACTOR(CLK_TOP_UNIVPLL_D2, "univpll_d2", "univpll", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL_D3, "univpll_d3", "univpll", 1, 3),
	FACTOR(CLK_TOP_UNIVPLL_D5, "univpll_d5", "univpll", 1, 5),
	FACTOR(CLK_TOP_UNIVPLL1_D2, "univpll1_d2", "univpll_d2", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D4, "univpll1_d4", "univpll_d2", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D2, "univpll2_d2", "univpll_d3", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL2_D4, "univpll2_d4", "univpll_d3", 1, 4),
	FACTOR(CLK_TOP_MMPLL_D3, "mmpll_d3", "mmpll", 1, 3),
	FACTOR(CLK_TOP_MMPLL_D4, "mmpll_d4", "mmpll", 1, 4),
	FACTOR(CLK_TOP_MMPLL_D5, "mmpll_d5", "mmpll", 1, 5),
	FACTOR(CLK_TOP_MMPLL_D6, "mmpll_d6", "mmpll", 1, 6),
};

static struct mtk_composite top_muxes[] = {
	// /* CLK_CFG_0 */
	// MUX_GATE(CLK_TOP_MUX_MFG, "mfg_sel", mfg_parents,
	// 	0x0140, 16, 3, 23),
	// MUX_GATE(CLK_TOP_MUX_IRDA, "irda_sel", irda_parents,
	// 	0x0140, 24, 2, 31),
	//
	// /* CLK_CFG_1 */
	// MUX_GATE(CLK_TOP_MUX_CAM, "cam_sel", cam_parents,
	// 	0x0144, 0, 4, 7),
	// MUX_GATE(CLK_TOP_MUX_AUDINTBUS, "audintbus_sel", audintbus_parents,
	// 	0x0144, 8, 2, 15),
	// MUX_GATE(CLK_TOP_MUX_JPG, "jpg_sel", jpg_parents,
	// 	0x0144, 16, 3, 23),
	// MUX_GATE(CLK_TOP_MUX_DISP, "disp_sel", disp_parents,
	// 	0x0144, 24, 3, 31),
	//
	// /* CLK_CFG_2 */
	// MUX_GATE(CLK_TOP_MUX_MSDC1, "msdc1_sel", msdc1_parents,
	// 	0x0148, 0, 3, 7),
	// MUX_GATE(CLK_TOP_MUX_MSDC2, "msdc2_sel", msdc2_parents,
	// 	0x0148, 8, 3, 15),
	// MUX_GATE(CLK_TOP_MUX_MSDC3, "msdc3_sel", msdc3_parents,
	// 	0x0148, 16, 3, 23),
	// MUX_GATE(CLK_TOP_MUX_MSDC4, "msdc4_sel", msdc4_parents,
	// 	0x0148, 24, 3, 31),
	//
	// /* CLK_CFG_3 */
	// MUX_GATE(CLK_TOP_MUX_USB20, "usb20_sel", usb20_parents,
	// 	0x014C, 0, 2, 7),
	//
	// /* CLK_CFG_4 */
	// MUX_GATE(CLK_TOP_MUX_HYD, "hyd_sel", hyd_parents,
	// 	0x0150, 0, 3, 7),
	// MUX_GATE(CLK_TOP_MUX_VENC, "venc_sel", venc_parents,
	// 	0x0150, 8, 3, 15),
	// MUX_GATE(CLK_TOP_MUX_SPI, "spi_sel", spi_parents,
	// 	0x0150, 16, 3, 23),
	// MUX_GATE(CLK_TOP_MUX_UART, "uart_sel", uart_parents,
	// 	0x0150, 24, 2, 31),
	//
	// /* CLK_CFG_6 */
	// MUX_GATE(CLK_TOP_MUX_CAMTG, "camtg_sel", camtg_parents,
	// 	0x0158, 8, 3, 15),
	// /*
	// MUX_GATE(CLK_TOP_MUX_FD, "fd_sel", fd_parents,
	// 	0x0158, 16, 3, 23),
	// */
	// MUX_GATE(CLK_TOP_MUX_AUDIO, "audio_sel", audio_parents,
	// 	0x0158, 24, 2, 31),
	//
	// /* CLK_CFG_7 */
	// MUX_GATE(CLK_TOP_MUX_VDEC, "vdec_sel", vdec_parents,
	// 	0x015C, 8, 4, 15),
	// MUX_GATE(CLK_TOP_MUX_DPILVDS, "dpilvds_sel", dpilvds_parents,
	// 	0x015C, 24, 3, 31),
	//
	// /* CLK_CFG_8 */
	// MUX_GATE(CLK_TOP_MUX_PMICSPI, "pmicspi_sel", pmicspi_parents,
	// 	0x0164, 0, 3, 7),
	// MUX_GATE(CLK_TOP_MUX_MSDC0, "msdc0_sel", msdc0_parents,
	// 	0x0164, 8, 3, 15),
	// MUX_GATE(CLK_TOP_MUX_SMI_MFG_AS, "smi_mfg_as_sel", smi_mfg_as_parents,
	// 	0x0164, 16, 2, 23),
};

static const struct mtk_clk_desc topck_desc = {
	// .clks = top_clks,
	// .num_clks = ARRAY_SIZE(top_clks),
	// .fixed_clks = top_fixed_clks,
	// .num_fixed_clks = ARRAY_SIZE(top_fixed_clks),
	.factor_clks = top_divs,
	.num_factor_clks = ARRAY_SIZE(top_divs),
	.composite_clks = top_muxes,
	.num_composite_clks = ARRAY_SIZE(top_muxes),
	// .divider_clks = top_adj_divs,
	// .num_divider_clks = ARRAY_SIZE(top_adj_divs),
	// .clk_lock = &mt6589_clk_lock,
};


/* INFRACFG */

#define GATE_INFRA(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &infra_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate_regs infra_cg_regs = {
	.set_ofs = 0x0040,
	.clr_ofs = 0x0044,
	.sta_ofs = 0x0048,
};

static const struct mtk_gate infra_clks[] = {
	// GATE_INFRA(CLK_INFRA_DBGCLK, "", "", 0),
	// GATE_INFRA(CLK_INFRA_SMI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_SPI0, "", "", 0),
	// GATE_INFRA(CLK_INFRA_AUDIO, "", "audintbus_sel", 0),
	// GATE_INFRA(CLK_INFRA_CEC, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MFGAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_M4U, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD1MCUAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD1HWMIXAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD1AHB, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD2MCUAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD2HWMIXAXI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_MD2AHB, "", "", 0),
	// GATE_INFRA(CLK_INFRA_CPUM, "", "", 0),
	// GATE_INFRA(CLK_INFRA_KP, "", "", 0),
	// GATE_INFRA(CLK_INFRA_CCIF0, "", "", 0),
	// GATE_INFRA(CLK_INFRA_CCIF1, "", "", 0),
	// GATE_INFRA(CLK_INFRA_PMICSPI, "", "", 0),
	// GATE_INFRA(CLK_INFRA_PMICWRAP, "", "", 0),
};

static const struct mtk_clk_desc infra_desc = {
	.clks = infra_clks,
	.num_clks = ARRAY_SIZE(infra_clks),
};


/* PERICFG */

static const struct mtk_gate_regs peri0_cg_regs = {
	.set_ofs = 0x0008,
	.clr_ofs = 0x0010,
	.sta_ofs = 0x0018,
};

static const struct mtk_gate_regs peri1_cg_regs = {
	.set_ofs = 0x000c,
	.clr_ofs = 0x0014,
	.sta_ofs = 0x001c,
};

#define GATE_PERI0(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &peri0_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

#define GATE_PERI1(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &peri1_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate peri_clks[] = {
	// GATE_PERI0(CLK_PERI0_NFI, "peri_nfi", "", 0),
	// GATE_PERI0(CLK_PERI0_THERM, "peri_therm", "", 1),
	// GATE_PERI0(CLK_PERI0_PWM1, "peri_pwm1", "", 2),
	// GATE_PERI0(CLK_PERI0_PWM2, "peri_pwm2", "", 3),
	// GATE_PERI0(CLK_PERI0_PWM3, "peri_pwm3", "", 4),
	// GATE_PERI0(CLK_PERI0_PWM4, "peri_pwm4", "", 5),
	// GATE_PERI0(CLK_PERI0_PWM5, "pwri_pwm5", "", 6),
	// GATE_PERI0(CLK_PERI0_PWM6, "pwri_pwm6", "", 7),
	// GATE_PERI0(CLK_PERI0_PWM7, "peri_pwm7", "", 8),
	// GATE_PERI0(CLK_PERI0_PWM, "peri_pwm", "", 9),
	// GATE_PERI0(CLK_PERI0_USB0, "peri_usb0", "usb20_sel", 10),
	// GATE_PERI0(CLK_PERI0_USB1, "peri_usb1", "usb20_sel", 11),
	// GATE_PERI0(CLK_PERI0_APDMA, "peri_apdma", "", 12),
	// GATE_PERI0(CLK_PERI0_MSDC0, "peri_msdc0", "msdc0_sel", 13),
	// GATE_PERI0(CLK_PERI0_MSDC1, "peri_msdc1", "msdc1_sel", 14),
	// GATE_PERI0(CLK_PERI0_MSDC2, "peri_msdc2", "msdc2_sel", 15),
	// GATE_PERI0(CLK_PERI0_MSDC3, "peri_msdc3", "msdc3_sel", 16),
	// GATE_PERI0(CLK_PERI0_MSDC4, "peri_msdc4", "msdc4_sel", 17),
	// GATE_PERI0(CLK_PERI0_APHIF, "peri_aphif", "", 18),
	// GATE_PERI0(CLK_PERI0_MDHIF, "peri_mdhif", "", 19),
	// GATE_PERI0(CLK_PERI0_NLI, "peri_nli", "", 20),
	// GATE_PERI0(CLK_PERI0_IRDA, "peri_irda", "irda_sel", 21),
	// GATE_PERI0(CLK_PERI0_UART0, "peri_uart0", "uart_sel", 22),
	// GATE_PERI0(CLK_PERI0_UART1, "peri_uart1", "uart_sel", 23),
	// GATE_PERI0(CLK_PERI0_UART2, "peri_uart2", "uart_sel", 24),
	// GATE_PERI0(CLK_PERI0_UART3, "peri_uart3", "uart_sel", 25),
	// GATE_PERI0(CLK_PERI0_I2C0, "peri_i2c0", "", 26),
	// GATE_PERI0(CLK_PERI0_I2C1, "peri_i2c1", "", 27),
	// GATE_PERI0(CLK_PERI0_I2C2, "peri_i2c2", "", 28),
	// GATE_PERI0(CLK_PERI0_I2C3, "peri_i2c3", "", 29),
	// GATE_PERI0(CLK_PERI0_I2C4, "peri_i2c4", "", 30),
	// GATE_PERI0(CLK_PERI0_I2C5, "peri_i2c5", "", 31),
	//
	// GATE_PERI1(CLK_PERI1_I2C6, "peri_i2c6", "", 32), // shift must be fixed
	// GATE_PERI1(CLK_PERI1_WRAP, "peri_wrap", "", 33),
	// GATE_PERI1(CLK_PERI1_AUXADC, "peri_auxadc", "", 34),
	// GATE_PERI1(CLK_PERI1_SPI1, "peri_spi1", "spi_sel", 35),
	// GATE_PERI1(CLK_PERI1_FHCTL, "peri_fhctl", "", 36),
};

static const struct mtk_clk_desc peri_desc = {
	.clks = peri_clks,
	.num_clks = ARRAY_SIZE(peri_clks),
};


static const struct of_device_id of_match_clk_mt6589[] = {
	{ .compatible = "mediatek,mt6589-topckgen", .data = &topck_desc },
	{ .compatible = "mediatek,mt6589-infracfg", .data = &infra_desc },
	{ .compatible = "mediatek,mt6589-pericfg", .data = &peri_desc, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589);

static struct platform_driver clk_mt6589_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589",
		.of_match_table = of_match_clk_mt6589,
	},
};
module_platform_driver(clk_mt6589_drv);

MODULE_DESCRIPTION("MediaTek MT6589 main clocks driver");
MODULE_LICENSE("GPL");
