// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Akari Tsuyukusa <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6589-clk.h>

#define TOP_CKMUXSEL	0x0000
#define TOP_CKDIV1	0x0008

/*
 * TOP_CKDIV1 (armdiv1) encodes the ARM PLL divider as a fraction.  The
 * field is 5 bits wide: bits [4:3] pick the denominator (0b01 -> 4,
 * 0b10 -> 5, 0b11 -> 6) and bits [2:0] the numerator minus one, so
 * e.g. 0x0a is 2/4, 0x12 is 3/5 and 0x1d is 1/6.  The downstream CPU
 * DVFS driver switches through 0x0a (2/4) while reprogramming ARMPLL
 * and back to 0x00 (= 4/4, i.e. no division) afterwards.
 */
/*
 * Only the integer divisions can be expressed through the common
 * divider ops; the fractional encodings (e.g. 3/4) are left out --
 * the CPU DVFS path only ever uses 4/4 (= bypass) and 2/4 (= /2)
 * anyway.
 */
static const struct clk_div_table mt6589_armdiv1_table[] = {
	{ .val = 0x08, .div = 1 },	/* 4/4 */
	{ .val = 0x0a, .div = 2 },	/* 2/4 */
	{ .val = 0x18, .div = 1 },	/* 6/6 */
	{ .val = 0x1b, .div = 2 },	/* 3/6 */
	{ .val = 0x13, .div = 2 },	/* 2/5 */
	{ }
};
#define INFRA_RST0	0x0030
#define INFRA_RST1	0x0034
#define INFRA_PDN_SET	0x0040
#define INFRA_PDN_CLR	0x0044
#define INFRA_PDN_STA	0x0048

static const char * const infra_mux1_parents[] = {
	"clk26m",
	"armpll",
	"mainpll",
	"mmpll_d2",
};

static const struct mtk_composite cpu_muxes[] = {
	MUX(CLK_INFRA_MUX1, "infra_mux1_sel", infra_mux1_parents, TOP_CKMUXSEL, 2, 2),
};

static const struct mtk_gate_regs infra_cg_regs = {
	.set_ofs = INFRA_PDN_SET,
	.clr_ofs = INFRA_PDN_CLR,
	.sta_ofs = INFRA_PDN_STA,
};

#define GATE_INFRA(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &infra_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate infra_clks[] = {
	GATE_INFRA(CLK_INFRA_DBGCLK, "infra_dbgclk", "axi_sel", 0), /* mt8135 */
	GATE_INFRA(CLK_INFRA_SMI, "infra_smi", "smi_sel", 1), /* mt8135 */
	GATE_INFRA(CLK_INFRA_SPI0, "infra_spi0", "spi_sel", 2), /* maybe, is it infra_mfg_bus? */
	GATE_INFRA(CLK_INFRA_AUDIO, "infra_audio", "audintbus_sel", 5),
	GATE_MTK(CLK_INFRA_CEC, "infra_cec", "axi_sel", &infra_cg_regs, 6, &mtk_clk_gate_ops_setclr_inv), /* or devapc */
	GATE_INFRA(CLK_INFRA_MFGAXI, "infra_mfgaxi", "axi_sel", 7), /* mt8135 */
	GATE_INFRA(CLK_INFRA_M4U, "infra_m4u", "mem_sel", 8), /* mt8135 */
	GATE_INFRA(CLK_INFRA_MD1MCUAXI, "infra_md1mcuaxi", "axi_sel", 9), /* maybe */
	GATE_INFRA(CLK_INFRA_MD1HWMIXAXI, "infra_md1hwmixaxi", "axi_sel", 10), /* maybe */
	GATE_INFRA(CLK_INFRA_MD1AHB, "infra_md1ahb", "axi_sel", 11), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2MCUAXI, "infra_md2mcuaxi", "axi_sel", 12), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2HWMIXAXI, "infra_md2hwmixaxi", "axi_sel", 13), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2AHB, "infra_md2ahb", "axi_sel", 14), /* maybe */
	GATE_INFRA(CLK_INFRA_CPUM, "infra_cpum", "cpum_tck_in", 15), /* mt8135 */
	GATE_INFRA(CLK_INFRA_KP, "infra_kp", "axi_sel", 16), /* mt8135 */
	GATE_INFRA(CLK_INFRA_CCIF0, "infra_ccif0", "axi_sel", 20), /* mt8135 */
	GATE_INFRA(CLK_INFRA_CCIF1, "infra_ccif1", "axi_sel", 21), /* mt8135 */
	GATE_INFRA(CLK_INFRA_PMICSPI, "infra_pmicspi", "pmicspi_sel", 22), /* mt8135 */
	GATE_INFRA(CLK_INFRA_PMICWRAP, "infra_pmicwrap", "axi_sel", 23), /* mt8135 */
};

static u16 infrasys_rst_ofs[] = { INFRA_RST0, INFRA_RST1 };

static const struct mtk_clk_rst_desc infra_clk_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = infrasys_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(infrasys_rst_ofs),
};

/*
 * The CPU DVFS path divides ARMPLL through this field while
 * reprogramming the PLL; exposing it as a clock lets cpufreq switch
 * through it instead of poking the register directly.
 */
static const struct mtk_clk_divider infra_dividers[] = {
	{
		.id = CLK_INFRA_ARMDIV1,
		.name = "armdiv1",
		.parent_name = "infra_mux1_sel",
		.div_reg = TOP_CKDIV1,
		.div_shift = 0,
		.div_width = 5,
		.clk_div_table = mt6589_armdiv1_table,
	},
};

static const struct mtk_clk_desc infra_desc = {
	.clks = infra_clks,
	.num_clks = ARRAY_SIZE(infra_clks),
	.cpumuxes = cpu_muxes,
	.num_cpumuxes = ARRAY_SIZE(cpu_muxes),
	.divider_clks = infra_dividers,
	.num_divider_clks = ARRAY_SIZE(infra_dividers),
	.rst_desc = &infra_clk_rst_desc,
};

static const struct of_device_id of_match_clk_mt6589_infracfg[] = {
	{ .compatible = "mediatek,mt6589-infracfg", .data = &infra_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_infracfg);

static struct platform_driver clk_mt6589_infracfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-infracfg",
		.of_match_table = of_match_clk_mt6589_infracfg,
	},
};
module_platform_driver(clk_mt6589_infracfg_drv);

MODULE_DESCRIPTION("MediaTek MT6589 infracfg clocks driver");
MODULE_LICENSE("GPL");
