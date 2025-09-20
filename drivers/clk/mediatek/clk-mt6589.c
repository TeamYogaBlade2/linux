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

static struct mtk_composite top_muxes[] = {
	/* CLK_CFG_0 */
	/* CLK_CFG_1 */
	/* CLK_CFG_2 */
	/* CLK_CFG_3 */
	/* CLK_CFG_4 */
	/* CLK_CFG_5 */
	/* CLK_CFG_6 */
	/* CLK_CFG_7 */
	/* CLK_MISC_CFG_2 */
	/* CLK_CFG_8 */
};

static const struct mtk_clk_desc topck_desc = {
	.composite_clks = top_muxes,
	.num_composite_clks = ARRAY_SIZE(top_muxes),
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
	// GATE_INFRA(CLK_INFRA_AUDIO, "", "", 0),
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
	// GATE_PERI0(CLK_PERI0_USB0, "peri_usb0", "", 10),
	// GATE_PERI0(CLK_PERI0_USB1, "peri_usb1", "", 11),
	// GATE_PERI0(CLK_PERI0_APDMA, "peri_apdma", "", 12),
	// GATE_PERI0(CLK_PERI0_MSDC0, "peri_msdc0", "", 13),
	// GATE_PERI0(CLK_PERI0_MSDC1, "peri_msdc1", "", 14),
	// GATE_PERI0(CLK_PERI0_MSDC2, "peri_msdc2", "", 15),
	// GATE_PERI0(CLK_PERI0_MSDC3, "peri_msdc3", "", 16),
	// GATE_PERI0(CLK_PERI0_MSDC4, "peri_msdc4", "", 17),
	// GATE_PERI0(CLK_PERI0_APHIF, "peri_aphif", "", 18),
	// GATE_PERI0(CLK_PERI0_MDHIF, "peri_mdhif", "", 19),
	// GATE_PERI0(CLK_PERI0_NLI, "peri_nli", "", 20),
	// GATE_PERI0(CLK_PERI0_IRDA, "peri_irda", "", 21),
	// GATE_PERI0(CLK_PERI0_UART0, "peri_uart0", "", 22),
	// GATE_PERI0(CLK_PERI0_UART1, "peri_uart1", "", 23),
	// GATE_PERI0(CLK_PERI0_UART2, "peri_uart2", "", 24),
	// GATE_PERI0(CLK_PERI0_UART3, "peri_uart3", "", 25),
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
	// GATE_PERI1(CLK_PERI1_SPI1, "peri_spi1", "", 35),
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
