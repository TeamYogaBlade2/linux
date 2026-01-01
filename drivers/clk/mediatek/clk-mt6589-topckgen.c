// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: akku <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mt6589-clk.h>

static DEFINE_SPINLOCK(mt6589_clk_lock);

static const struct mtk_fixed_clk top_fixed_clks[] = {
	FIXED_CLK(CLK_TOP_CLK_NULL, "clk_null", NULL, 0),
};

static const struct mtk_fixed_factor top_divs[] = {
	FACTOR(CLK_TOP_CLKPH_MCK, "clkph_mck", "clk_null", 1, 1),
	FACTOR(CLK_TOP_CPUM_TCK_IN, "cpum_tck_in", "clk_null", 1, 1),

	FACTOR(CLK_TOP_SYSPLL, "syspll_ck", "mainpll", 1, 2), // mainpll_806m
	FACTOR(CLK_TOP_MAINPLL_D3, "mainpll_d3", "mainpll", 1, 3), // mainpll_537p3m
	FACTOR(CLK_TOP_MAINPLL_D5, "mainpll_d5", "mainpll", 1, 5), // mainpll_322p4m
	FACTOR(CLK_TOP_MAINPLL_D7, "mainpll_d7", "mainpll", 1, 7), // mainpll_230p3m

	FACTOR(CLK_TOP_SYSPLL_D2, "syspll_d2", "syspll_ck", 1, 2),
	FACTOR(CLK_TOP_SYSPLL_D3, "syspll_d3", "syspll_ck", 1, 3),
	FACTOR(CLK_TOP_SYSPLL_D3P5, "syspll_d3p5", "syspll_ck", 2, 7),
	FACTOR(CLK_TOP_SYSPLL_D4, "syspll_d4", "syspll_ck", 1, 4),
	FACTOR(CLK_TOP_SYSPLL_D5, "syspll_d5", "syspll_ck", 1, 5),
	FACTOR(CLK_TOP_SYSPLL_D6, "syspll_d6", "syspll_ck", 1, 6),
	FACTOR(CLK_TOP_SYSPLL_D8, "syspll_d8", "syspll_ck", 1, 8),
	FACTOR(CLK_TOP_SYSPLL_D10, "syspll_d10", "syspll_ck", 1, 10),
	FACTOR(CLK_TOP_SYSPLL_D16, "syspll_d16", "syspll_ck", 1, 16),
	FACTOR(CLK_TOP_SYSPLL_D24, "syspll_d24", "syspll_ck", 1, 24),

	FACTOR(CLK_TOP_UNIVPLL_D2, "univpll_d2", "univpll", 1, 2), // univpll_624m
	FACTOR(CLK_TOP_UNIVPLL_D3, "univpll_d3", "univpll", 1, 3), // univpll_416m
	FACTOR(CLK_TOP_UNIVPLL_D5, "univpll_d5", "univpll", 1, 5), // univpll_249p6m
	FACTOR(CLK_TOP_UNIVPLL_D7, "univpll_d7", "univpll", 1, 7), // univpll_178p3m
	FACTOR(CLK_TOP_UNIVPLL_D10, "univpll_d10", "univpll", 1, 10),
	FACTOR(CLK_TOP_UNIVPLL_D26, "univpll_d26", "univpll", 1, 26), // univpll_48m

	FACTOR(CLK_TOP_UNIVPLL1_D2, "univpll1_d2", "univpll_d2", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL1_D4, "univpll1_d4", "univpll_d2", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL1_D6, "univpll1_d6", "univpll_d2", 1, 6),
	FACTOR(CLK_TOP_UNIVPLL1_D8, "univpll1_d8", "univpll_d2", 1, 8),
	FACTOR(CLK_TOP_UNIVPLL1_D10, "univpll1_d10", "univpll_d10", 1, 10),

	FACTOR(CLK_TOP_UNIVPLL2_D2, "univpll2_d2", "univpll_d3", 1, 2),
	FACTOR(CLK_TOP_UNIVPLL2_D4, "univpll2_d4", "univpll_d3", 1, 4),
	FACTOR(CLK_TOP_UNIVPLL2_D6, "univpll2_d6", "univpll_d3", 1, 6),
	FACTOR(CLK_TOP_UNIVPLL2_D8, "univpll2_d8", "univpll_d3", 1, 8), // unconfirmed

	FACTOR(CLK_TOP_MMPLL_D3, "mmpll_d3", "mmpll", 1, 3),
	FACTOR(CLK_TOP_MMPLL_D4, "mmpll_d4", "mmpll", 1, 4),
	FACTOR(CLK_TOP_MMPLL_D5, "mmpll_d5", "mmpll", 1, 5),
	FACTOR(CLK_TOP_MMPLL_D6, "mmpll_d6", "mmpll", 1, 6),
	FACTOR(CLK_TOP_MMPLL_D7, "mmpll_d7", "mmpll", 1, 7),

	FACTOR(CLK_TOP_LVDSPLL, "lvdspll_ck", "lvdspll", 1, 1),
	FACTOR(CLK_TOP_LVDSPLL_D2, "lvdspll_d2", "lvdspll", 1, 2), // lvdspll_180m
	FACTOR(CLK_TOP_LVDSPLL_D4, "lvdspll_d4", "lvdspll", 1, 4),
	FACTOR(CLK_TOP_LVDSPLL_D8, "lvdspll_d8", "lvdspll", 1, 8),

	FACTOR(CLK_TOP_LVDSTX_CLKDIG_CT, "lvdstx_clkdig_cts", "lvdspll", 1, 1),

	FACTOR(CLK_TOP_TVHDMI_H, "tvhdmi_h_ck", "tvdpll", 1, 1),

	FACTOR(CLK_TOP_HDMITX_CLKDIG_D2, "hdmitx_clkdig_d2", "hdmitx_clkdig_cts", 1, 2),
	FACTOR(CLK_TOP_HDMITX_CLKDIG_D3, "hdmitx_clkdig_d3", "hdmitx_clkdig_cts", 1, 3),

	FACTOR(CLK_TOP_TVHDMI_D2, "tvhdmi_d2", "tvhdmi_h_ck", 1, 2),
	FACTOR(CLK_TOP_TVHDMI_D4, "tvhdmi_d4", "tvhdmi_h_ck", 1, 4),

	FACTOR(CLK_TOP_MEMPLL_MCK_D4, "mempll_mck_d4", "clkph_mck", 1, 4),

	FACTOR(CLK_TOP_AD_ISP_208M_CK, "ad_isp_208m_ck", "isppll", 1, 8), // ?

	FACTOR(CLK_TOP_AD_MSDC_H208M_CK, "ad_msdc_h208m_ck", "msdcpll", 1, 8), // ?
};

static const char * axi_parents[] = {
	"clk26m",
	"syspll_d3",
	"syspll_d4",
	"syspll_d6",
	"univpll_d5",
	"univpll2_d2",
	"syspll_d3p5",
};

static const char * smi_parents[] = {
	"clk26m",
	"syspll_d3",
	"syspll_d8",
	"univpll_d5",
	"univpll1_d6",
	"mmpll_d4",
	"mmpll_d5",
	"mmpll_d6",
};

static const char * const mfg_parents[] = {
	"univpll1_d4",
	"mmpll_d6",
	"syspll_d2",
	"syspll_d3",
	"univpll1_d2",
	"mmpll_d3",
	"mmpll_d4",
	"mmpll_d5",
};

static const char * const irda_parents[] = {
	"clk26m",
	"univpll2_d8",
	"univpll1_d6",
};

static const char * const cam_parents[] = {
	"clk26m",
	"syspll_d3",
	"syspll_d3p5",
	"syspll_d4",
	"syspll_d6",
	"syspll_d8",
	"ad_isp_208m_ck",
	"univpll_d5",
	"univpll2_d2",
	"univpll_d7",
	"univpll1_d4",
};

static const char * const audintbus_parents[] = {
	"clk26m",
	"syspll_d6",
	"univpll_d10",
};

static const char * const jpg_parents[] = {
	"clk26m",
	"syspll_d5",
	"syspll_d4",
	"univpll2_d2",
	"univpll_d7",
};

static const char * const disp_parents[] = {
	"clk26m",
	"syspll_d3p5",
	"syspll_d3",
	"univpll2_d2",
	"univpll_d5",
};

static const char * const msdc1_parents[] = {
	"clk26m",
	"syspll_d6",
	"syspll_d5",
	"univpll1_d4",
	"unicpll2_d4",
	"ad_msdc_h208m_ck",
};

static const char * const msdc2_parents[] = {
	"clk26m",
	"syspll_d6",
	"syspll_d5",
	"univpll1_d4",
	"unicpll2_d4",
	"ad_msdc_h208m_ck",
};

static const char * const msdc3_parents[] = {
	"clk26m",
	"syspll_d6",
	"syspll_d5",
	"univpll1_d4",
	"unicpll2_d4",
	"ad_msdc_h208m_ck",
};

static const char * const msdc4_parents[] = {
	"clk26m",
	"syspll_d6",
	"syspll_d5",
	"univpll1_d4",
	"unicpll2_d4",
	"ad_msdc_h208m_ck",
};

static const char * const usb20_parents[] = {
	"clk26m",
	"univpll2_d6",
	"univpll1_d10",
};

static const char * const hyd_parents[] = {
	"univpll1_d4",
	"mmpll_d6",
	"syspll_d2",
	"syspll_d3",
	"univpll1_d2",
	"mmpll_d3",
	"mmpll_d4",
	"mmpll_d5",
};

static const char * const venc_parents[] = {
	"clk26m",
	"syspll_d3",
	"syspll_d8",
	"univpll_d5",
	"univpll1_d6",
	"mmpll_d4",
	"mmpll_d5",
	"mmpll_d6",
};

static const char * const spi_parents[] = {
	"clk26m",
	"syspll_d6",
	"syspll_d8",
	"syspll_d10",
	"univpll1_d6",
	"univpll1_d8",
};

static const char * const uart_parents[] = {
	"clk26m",
	"univpll2_d8",
};

static const char * const mem_parents[] = {
	"clk26m",
	"clkph_mck",
	"clk_null",
	"clk_null",
};

static const char * const camtg_parents[] = {
	"clk26m",
	"univpll_d26",
	"univpll1_d6",
	"syspll_d16",
	"syspll_d8",
	"ad_isp_208m_ck",
};

static const char * const fd_parents[] = {
	"clk26m",
	"syspll_d6",
	"syspll_d8",
	"univpll_d10",
	"univpll2_d4",
};

static const char * const audio_parents[] = {
	"clk26m",
	"syspll_d24",
};

static const char * const fix_parents[] = {
	"rtc_clk",
	"clk26m", /* f_f26m_ck" */
	"univpll_d5",
	"univpll_d7",
	"univpll1_d2",
	"univpll1_d4",
	"univpll1_d6",
	"univpll1_d8",
};

static const char * const vdec_parents[] = {
	"clk26m",
	"syspll_d3p5",
	"syspll_d4",
	"syspll_d5",
	"syspll_d6",
	"syspll_d8",
	"univpll2_d2",
	"univpll_d7",
	"univpll_d10",
	"univpll2_d4",
};

static const char * const dpilvds_parents[] = {
	"clk26m",
	"lvdspll_ck",
	"lvdspll_d2",
	"lvdspll_d4",
	"lvdspll_d8",
};

static const char * const pmicspi_parents[] = {
	"clk26m",
	"univpll2_d6",
	"syspll_d8",
	"syspll_d10",
	"univpll1_d10",
	"mempll_mck_d4",
	"univpll_d26",
	"syspll_d24",
};

static const char * const msdc0_parents[] = {
	"clk26m",
	"syspll_d6",
	"syspll_d5",
	"univpll1_d4",
	"unicpll2_d4",
	"ad_msdc_h208m_ck",
};

static const char * const smi_mfg_as_parents[] = {
	"clk26m",
	"smi_sel",
	"mfg_sel",
	"hyd_sel",
};

static struct mtk_composite top_muxes[] = {
	/* CLK_CFG_0 */
	MUX(CLK_TOP_MUX_AXI, "axi_sel", axi_parents,
		0x0140, 0, 3),
	MUX_GATE(CLK_TOP_MUX_SMI, "smi_sel", smi_parents,
		0x0140, 8, 3, 15),
	MUX_GATE(CLK_TOP_MUX_MFG, "mfg_sel", mfg_parents,
		0x0140, 16, 3, 23),
	MUX_GATE(CLK_TOP_MUX_IRDA, "irda_sel", irda_parents,
		0x0140, 24, 2, 31),

	/* CLK_CFG_1 */
	MUX_GATE(CLK_TOP_MUX_CAM, "cam_sel", cam_parents,
		0x0144, 0, 4, 7),
	MUX_GATE(CLK_TOP_MUX_AUDINTBUS, "audintbus_sel", audintbus_parents,
		0x0144, 8, 2, 15),
	MUX_GATE(CLK_TOP_MUX_JPG, "jpg_sel", jpg_parents,
		0x0144, 16, 3, 23),
	MUX_GATE(CLK_TOP_MUX_DISP, "disp_sel", disp_parents,
		0x0144, 24, 3, 31),

	/* CLK_CFG_2 */
	MUX_GATE(CLK_TOP_MUX_MSDC1, "msdc1_sel", msdc1_parents,
		0x0148, 0, 3, 7),
	MUX_GATE(CLK_TOP_MUX_MSDC2, "msdc2_sel", msdc2_parents,
		0x0148, 8, 3, 15),
	MUX_GATE(CLK_TOP_MUX_MSDC3, "msdc3_sel", msdc3_parents,
		0x0148, 16, 3, 23),
	MUX_GATE(CLK_TOP_MUX_MSDC4, "msdc4_sel", msdc4_parents,
		0x0148, 24, 3, 31),

	/* CLK_CFG_3 */
	MUX_GATE(CLK_TOP_MUX_USB20, "usb20_sel", usb20_parents,
		0x014c, 0, 2, 7),

	/* CLK_CFG_4 */
	MUX_GATE(CLK_TOP_MUX_HYD, "hyd_sel", hyd_parents,
		0x0150, 0, 3, 7),
	MUX_GATE(CLK_TOP_MUX_VENC, "venc_sel", venc_parents,
		0x0150, 8, 3, 15),
	MUX_GATE(CLK_TOP_MUX_SPI, "spi_sel", spi_parents,
		0x0150, 16, 3, 23),
	MUX_GATE(CLK_TOP_MUX_UART, "uart_sel", uart_parents,
		0x0150, 24, 2, 31),

	/* CLK_CFG_6 */
	MUX_GATE(CLK_TOP_MUX_MEM, "mem_sel", mem_parents,
		0x0158, 0, 2, 7),
	MUX_GATE(CLK_TOP_MUX_CAMTG, "camtg_sel", camtg_parents,
		0x0158, 8, 3, 15),
	MUX_GATE(CLK_TOP_MUX_FD, "fd_sel", fd_parents,
		0x0158, 16, 3, 23),
	MUX_GATE(CLK_TOP_MUX_AUDIO, "audio_sel", audio_parents,
		0x0158, 24, 2, 31),

	/* CLK_CFG_7 */
	MUX_GATE(CLK_TOP_MUX_FIX, "fix_sel", fix_parents,
		0x015c, 0, 3, 7),
	MUX_GATE(CLK_TOP_MUX_VDEC, "vdec_sel", vdec_parents,
		0x015c, 8, 4, 15),
	MUX_GATE(CLK_TOP_MUX_DPILVDS, "dpilvds_sel", dpilvds_parents,
		0x015c, 24, 3, 31),

	/* CLK_CFG_8 */
	MUX_GATE(CLK_TOP_MUX_PMICSPI, "pmicspi_sel", pmicspi_parents,
		0x0164, 0, 3, 7),
	MUX_GATE(CLK_TOP_MUX_MSDC0, "msdc0_sel", msdc0_parents,
		0x0164, 8, 3, 15),
	MUX_GATE(CLK_TOP_MUX_SMI_MFG_AS, "smi_mfg_as_sel", smi_mfg_as_parents,
		0x0164, 16, 2, 23),
};

#define TOPCK_PDN_SET	0x0170
#define TOPCK_PDN_CLR	0x0174
#define TOPCK_PDN_STA	0x0178

static const struct mtk_gate_regs topck_cg_regs = {
	.set_ofs = TOPCK_PDN_SET,
	.clr_ofs = TOPCK_PDN_CLR,
	.sta_ofs = TOPCK_PDN_STA,
};

#define GATE_TOPCK(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &topck_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate top_clks[] = {
	GATE_TOPCK(CLK_TOPCK_PMICSPI, "topck_pmicspi", "pmicspi_sel", 0),
};

static const struct mtk_clk_desc topck_desc = {
	.clks = top_clks,
	.num_clks = ARRAY_SIZE(top_clks),
	.fixed_clks = top_fixed_clks,
	.num_fixed_clks = ARRAY_SIZE(top_fixed_clks),
	.factor_clks = top_divs,
	.num_factor_clks = ARRAY_SIZE(top_divs),
	.composite_clks = top_muxes,
	.num_composite_clks = ARRAY_SIZE(top_muxes),
	.clk_lock = &mt6589_clk_lock,
};

static const struct of_device_id of_match_clk_mt6589_topckgen[] = {
	{ .compatible = "mediatek,mt6589-topckgen", .data = &topck_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_topckgen);

static struct platform_driver clk_mt6589_topckgen_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-topckgen",
		.of_match_table = of_match_clk_mt6589_topckgen,
	},
};
module_platform_driver(clk_mt6589_topckgen_drv);

MODULE_DESCRIPTION("MediaTek MT6589 topckgen driver");
MODULE_LICENSE("GPL");
