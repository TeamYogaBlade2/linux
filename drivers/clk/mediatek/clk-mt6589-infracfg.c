// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

#define TOP_CKMUXSEL	0x0

static const char * const infra_mux1_parents[] = {
	"clk26m",
	"armpll",
	"mainpll",
	"mmpll", /* MMPLL/2 */
};

static const struct mtk_composite cpu_muxes[] = {
	MUX(CLK_INFRA_MUX1, "infra_mux1_sel", infra_mux1_parents, TOP_CKMUXSEL, 2, 2),
};

#define INFRA_PDN_SET	0x0040
#define INFRA_PDN_CLR	0x0044
#define INFRA_PDN_STA	0x0048

static const struct mtk_gate_regs infra_cg_regs = {
	.set_ofs = INFRA_PDN_SET,
	.clr_ofs = INFRA_PDN_CLR,
	.sta_ofs = INFRA_PDN_STA,
};

#define GATE_INFRA(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &infra_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate infra_clks[] = {
	GATE_INFRA(CLK_INFRA_DBGCLK, "infra_dbgclk", "axi_sel", 0), /* maybe */
	GATE_INFRA(CLK_INFRA_SMI, "infra_smi", "smi_sel", 1), /* maybe */
	GATE_INFRA(CLK_INFRA_SPI0, "infra_spi0", "spi_sel", 2), /* maybe, is it infra_mfg_bus? */
	GATE_INFRA(CLK_INFRA_AUDIO, "infra_audio", "audintbus_sel", 5),
	GATE_INFRA(CLK_INFRA_DEVAPC, "infra_devapc", "axi_sel", 6), /* is it infra cec (setclr_inv)? maybe correct parent */
	GATE_INFRA(CLK_INFRA_MFGAXI, "infra_mfgaxi", "axi_sel", 7), /* maybe */
	GATE_INFRA(CLK_INFRA_M4U, "infra_m4u", "mem_sel", 8), /* or axi_sel, maybe */
	GATE_INFRA(CLK_INFRA_MD1MCUAXI, "infra_md1mcuaxi", "axi_sel", 9), /* maybe */
	GATE_INFRA(CLK_INFRA_MD1HWMIXAXI, "infra_md1hwmixaxi", "axi_sel", 10), /* maybe */
	GATE_INFRA(CLK_INFRA_MD1AHB, "infra_md1ahb", "axi_sel", 11), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2MCUAXI, "infra_md2mcuaxi", "axi_sel", 12), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2HWMIXAXI, "infra_md2hwmixaxi", "axi_sel", 13), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2AHB, "infra_md2ahb", "axi_sel", 14), /* maybe */
	GATE_INFRA(CLK_INFRA_CPUM, "infra_cpum", "cpum_tck_in", 15), /* from MT8135 */
	GATE_INFRA(CLK_INFRA_KP, "infra_kp", "axi_sel", 16), /* maybe */
	GATE_INFRA(CLK_INFRA_CCIF0, "infra_ccif0", "axi_sel", 20), /* maybe */
	GATE_INFRA(CLK_INFRA_CCIF1, "infra_ccif1", "axi_sel", 21), /* maybe */
	GATE_INFRA(CLK_INFRA_PMICSPI, "infra_pmicspi", "pmicspi_sel", 22), /* maybe */
	GATE_INFRA(CLK_INFRA_PMICWRAP, "infra_pmicwrap", "axi_sel", 23), /* maybe */
};

static u16 infrasys_rst_ofs[] = { 0x30, 0x34, };

static const struct mtk_clk_rst_desc infra_clk_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = infrasys_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(infrasys_rst_ofs),
};

static const struct mtk_clk_desc infra_desc = {
	.clks = infra_clks,
	.num_clks = ARRAY_SIZE(infra_clks),
	.cpumuxes = cpu_muxes,
	.num_cpumuxes = ARRAY_SIZE(cpu_muxes),
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
