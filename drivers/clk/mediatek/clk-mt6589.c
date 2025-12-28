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

static DEFINE_SPINLOCK(mt6589_clk_lock);

static const struct mtk_fixed_clk top_fixed_clks[] = {
	FIXED_CLK(CLK_TOP_CLK_NULL, "clk_null", NULL, 0),
};

static const struct mtk_fixed_factor top_divs[] = {
	FACTOR(CLK_TOP_CLKPH_MCK, "clkph_mck", "clk_null", 1, 1),

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
	"f_f26m_ck", // TODO:
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
	"lvdspll_ck", // TODO:
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
	"hf_fsmi_ck", // TODO:
	"hf_fmfg_ck", // TODO:
	"hf_fhyd_ck", // TODO:
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
	// .divider_clks = top_adj_divs,
	// .num_divider_clks = ARRAY_SIZE(top_adj_divs),
	.clk_lock = &mt6589_clk_lock,
};


/* INFRACFG */

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
	// GATE_INFRA(CLK_INFRA_DBGCLK, "infra_dbgclk", "", 0),
	// GATE_INFRA(CLK_INFRA_SMI, "infra_smi", "", 1),
	// GATE_INFRA(CLK_INFRA_SPI0, "infra_spi0", "", 2),
	GATE_INFRA(CLK_INFRA_AUDIO, "infra_audio", "audintbus_sel", 5),
	// GATE_INFRA(CLK_INFRA_CEC, "infra_cec", "", 6),
	// GATE_INFRA(CLK_INFRA_MFGAXI, "infra_mfgaxi", "", 7),
	// GATE_INFRA(CLK_INFRA_M4U, "infra_m4u", "", 8),
	// GATE_INFRA(CLK_INFRA_MD1MCUAXI, "infra_md1mcuaxi", "", 9),
	// GATE_INFRA(CLK_INFRA_MD1HWMIXAXI, "infra_md1hwmixaxi", "", 10),
	// GATE_INFRA(CLK_INFRA_MD1AHB, "infra_md1ahb", "", 11),
	// GATE_INFRA(CLK_INFRA_MD2MCUAXI, "infra_md2mcuaxi", "", 12),
	// GATE_INFRA(CLK_INFRA_MD2HWMIXAXI, "infra_md2hwmixaxi", "", 13),
	// GATE_INFRA(CLK_INFRA_MD2AHB, "infra_md2ahb", "", 14),
	// GATE_INFRA(CLK_INFRA_CPUM, "infra_cpum", "", 15),
	// GATE_INFRA(CLK_INFRA_KP, "infra_kp", "", 16),
	// GATE_INFRA(CLK_INFRA_CCIF0, "infra_ccif0", "", 20),
	// GATE_INFRA(CLK_INFRA_CCIF1, "infra_ccif1", "", 21),
	// GATE_INFRA(CLK_INFRA_PMICSPI, "infra_pmicspi", "", 22),
	// GATE_INFRA(CLK_INFRA_PMICWRAP, "infra_pmicwrap", "", 23),
};

static const struct mtk_clk_desc infra_desc = {
	.clks = infra_clks,
	.num_clks = ARRAY_SIZE(infra_clks),
};


/* PERICFG */

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
	GATE_PERI0(CLK_PERI0_USB0, "peri_usb0", "usb20_sel", 10),
	GATE_PERI0(CLK_PERI0_USB1, "peri_usb1", "usb20_sel", 11),
	// GATE_PERI0(CLK_PERI0_APDMA, "peri_apdma", "", 12),
	GATE_PERI0(CLK_PERI0_MSDC0, "peri_msdc0", "msdc0_sel", 13),
	GATE_PERI0(CLK_PERI0_MSDC1, "peri_msdc1", "msdc1_sel", 14),
	GATE_PERI0(CLK_PERI0_MSDC2, "peri_msdc2", "msdc2_sel", 15),
	GATE_PERI0(CLK_PERI0_MSDC3, "peri_msdc3", "msdc3_sel", 16),
	GATE_PERI0(CLK_PERI0_MSDC4, "peri_msdc4", "msdc4_sel", 17),
	// GATE_PERI0(CLK_PERI0_APHIF, "peri_aphif", "", 18),
	// GATE_PERI0(CLK_PERI0_MDHIF, "peri_mdhif", "", 19),
	// GATE_PERI0(CLK_PERI0_NLI, "peri_nli", "", 20),
	GATE_PERI0(CLK_PERI0_IRDA, "peri_irda", "irda_sel", 21),
	GATE_PERI0(CLK_PERI0_UART0, "peri_uart0", "uart_sel", 22), // FIXME: Are UART clocks divided two times?
	GATE_PERI0(CLK_PERI0_UART1, "peri_uart1", "uart_sel", 23),
	GATE_PERI0(CLK_PERI0_UART2, "peri_uart2", "uart_sel", 24),
	GATE_PERI0(CLK_PERI0_UART3, "peri_uart3", "uart_sel", 25),
	// GATE_PERI0(CLK_PERI0_I2C0, "peri_i2c0", "", 26),
	// GATE_PERI0(CLK_PERI0_I2C1, "peri_i2c1", "", 27),
	// GATE_PERI0(CLK_PERI0_I2C2, "peri_i2c2", "", 28),
	// GATE_PERI0(CLK_PERI0_I2C3, "peri_i2c3", "", 29),
	// GATE_PERI0(CLK_PERI0_I2C4, "peri_i2c4", "", 30),
	// GATE_PERI0(CLK_PERI0_I2C5, "peri_i2c5", "", 31),
	//
	// GATE_PERI1(CLK_PERI1_I2C6, "peri_i2c6", "", 0),
	// GATE_PERI1(CLK_PERI1_WRAP, "peri_wrap", "", 1),
	// GATE_PERI1(CLK_PERI1_AUXADC, "peri_auxadc", "", 2),
	GATE_PERI1(CLK_PERI1_SPI1, "peri_spi1", "spi_sel", 3),
	// GATE_PERI1(CLK_PERI1_FHCTL, "peri_fhctl", "", 4),
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
