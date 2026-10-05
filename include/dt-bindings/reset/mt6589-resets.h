/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef _DT_BINDINGS_RESET_CONTROLLER_MT6589
#define _DT_BINDINGS_RESET_CONTROLLER_MT6589

/*
 * The value of every ID below is the bit position of that reset inside the
 * register its provider maps, because every provider on this SoC is a
 * clk-mtk / mmsys reset controller: drivers/clk/mediatek/reset.c indexes
 * rst_bank_ofs[id / 32] and masks BIT(id % 32).  An ID is therefore not a
 * free-standing enumeration and cannot be renumbered - see the note at the
 * bottom of this header.
 *
 * Citations are to the MT6589 TD-HSPA data sheet, whose printed page number
 * equals its PDF page number.
 */

/*
 * PERI - provider: pericfg (0x10003000), "mediatek,mt6589-pericfg",
 * registered by drivers/clk/mediatek/clk-mt6589-pericfg.c with two banks
 * (PERI_GLOBALCON_RST0 at +0x0000, PERI_GLOBALCON_RST1 at +0x0004), so IDs
 * 0..63 are valid.
 */
#define MT6589_PERI_UART0_SW_RST	0
#define MT6589_PERI_UART1_SW_RST	1
#define MT6589_PERI_UART2_SW_RST	2
#define MT6589_PERI_UART3_SW_RST	3
#define MT6589_PERI_IRDA_SW_RST		4
#define MT6589_PERI_PTP_RST		5
#define MT6589_PERI_AP_HIF_SW_RST	6
#define MT6589_PERI_MD_HIF_SW_RST	8
#define MT6589_PERI_NLI_SW_RST		9
#define MT6589_PERI_AUXADC_SW_RST	10
#define MT6589_PERI_DMA_SW_RS_RST	11
#define MT6589_PERI_NFI_SW_RST_RST	14
#define MT6589_PERI_PWM_SW_RST		15
#define MT6589_PERI_THERM_SW_RST	16
#define MT6589_PERI_MSDC0_SW_RST	17
#define MT6589_PERI_MSDC1_SW_RST	18
#define MT6589_PERI_MSDC2_SW_RST	19
#define MT6589_PERI_MSDC3_SW_RST	20
#define MT6589_PERI_I2C0_SW_RST		22
#define MT6589_PERI_I2C1_SW_RST		23
#define MT6589_PERI_I2C2_SW_RST		24
#define MT6589_PERI_I2C3_SW_RST		25
#define MT6589_PERI_I2C4_SW_RST		26
#define MT6589_PERI_I2C5_SW_RST		27
#define MT6589_PERI_I2C6_SW_RST		28
#define MT6589_PERI_USB_SW_RST		29
/*
 * Bit 1 of PERI_GLOBALCON_RST1, data sheet p. 470.  The register calls the
 * bit SPI0_SW_RST while its mnemonic and its "Resets SPI1 software"
 * description say SPI1; aquaris-5 puts the SPI controller on SPI0 (the
 * preloader pwr path resets bit 1 of RST1 for PWRAP_SPICTL, see below), so
 * the bit is the SPI0 controller's.
 */
#define MT6589_PERI_SPI0_SW_RST		33
/*
 * Bit 2 of PERI_GLOBALCON_RST1, used by the vendor to reset the PMIC wrapper
 * bridge.  The data sheet p. 470 leaves bits 31:2 of that register
 * undeclared, so this ID has no data sheet bit to cite; the evidence is
 * aquaris-5 mediatek/platform/mt6589/preloader/src/drivers/mt_pmic_wrap_init.c:733
 * and :738, which reset the bridge together with PWRAP_SPICTL by setting and
 * clearing PERI_GLOBALCON_RST1 bit 2.
 */
#define MT6589_PERI_PWRAP_BRIDGE_SW_RST 34

/*
 * INFRA - provider: infracfg (0x10001000), "mediatek,mt6589-infracfg",
 * registered by drivers/clk/mediatek/clk-mt6589-infracfg.c with two banks
 * (INFRA_RST0 at +0x0030, INFRA_RST1 at +0x0034), so IDs 0..63 are valid.
 * The two banks are far apart in the ID space: RST0 holds bits 0..8 and RST1
 * holds bits 0..4, which is why the RST1 entries below start at 32.
 */
#define MT6589_INFRA_EMI_REG_RST	0
#define MT6589_INFRA_DRAMC0_AO_RST	1
#define MT6589_INFRA_CCIF0_RST		2
#define MT6589_INFRA_AP_CIRP_EINT_RST	3
#define MT6589_INFRA_APXGPT_RST		4
#define MT6589_INFRA_SCPSYS_RST		5
#define MT6589_INFRA_CCIF1_RST		6
#define MT6589_INFRA_PMIC_WRAP_RST	7
#define MT6589_INFRA_KP_RST		8
#define MT6589_INFRA_EMI_RST		32
#define MT6589_INFRA_DRAMC0_RST		34
#define MT6589_INFRA_SMI_RST		35
#define MT6589_INFRA_M4U_RST		36

/*
 * TOPRGU - DEFINED BUT NOT USABLE ON MT6589, kept for other boards.
 *
 * These are bits 0..11 of WDT_SWSYSRST (0x10000018, data sheet p. 408) in
 * TOPRGU at 0x10000000.  No DT node may reference them:
 *
 *  - TOPRGU has no reset controller.  The node that covers its base address
 *    is wdt (watchdog@10000000) and drivers/clk/mediatek/clk-mt6589-topckgen.c
 *    registers no rst_desc for topckgen either, so a "&topckgen ..." or
 *    "&wdt ..." reference would not resolve.
 *  - Even if a provider were added, WDT_SWSYSRST is write protected: writes
 *    only take effect with 0x88 in bits 31:24 (data sheet p. 408, and
 *    aquaris-5 mediatek/platform/mt6589/preloader/src/drivers/mt_pmic_wrap_init.c:735
 *    writes 0x88000000 | ...).  The generic controller in
 *    drivers/clk/mediatek/reset.c does a plain regmap_update_bits() and
 *    would silently drop every reset.
 *  - Data sheet p. 405 section 6.2 states that infrasys and apmixedsys are
 *    excluded from software controllable reset in any case, so
 *    MT6589_TOPRGU_INFRA_RST has no effect to begin with.
 *
 * Whole groups among these are additionally dead on this board: the MD,
 * MD_LITE, VDEC, VENC and IMG families have no AP-side driver here, and
 * DDRPHY_RST would also reset MEMPLL, which must not happen while the memory
 * controller is running.
 */
#define MT6589_TOPRGU_INFRA_RST		0
#define MT6589_TOPRGU_DISP_RST		1
#define MT6589_TOPRGU_MFG_RST		2
#define MT6589_TOPRGU_VENC_RST		3
#define MT6589_TOPRGU_VDEC_RST		4
#define MT6589_TOPRGU_IMG_RST		5
#define MT6589_TOPRGU_DDRPHY_RST	6
#define MT6589_TOPRGU_MD_RST		7
#define MT6589_TOPRGU_INFRA_AO_RST	8
#define MT6589_TOPRGU_MD_LITE_RST	9
#define MT6589_TOPRGU_APMIXED_RST	10
#define MT6589_TOPRGU_PWRAP_SPICTL_RST	11

#define MT6589_TOPRGU_RST_NUM		12

/*
 * MFG - provider: mfgsys (0x10206000), "mediatek,mt6589-mfgsys",
 * registered by drivers/clk/mediatek/clk-mt6589-mfg.c against MFG_RESET
 * (0x1020600c, data sheet p. 2433).
 *
 * Bit 1 is documented as "Reserved" in the data sheet, so MT6589_MFG_AXI_RESET
 * is only as good as the register layout; bit 0 is the real G3D reset and the
 * only one any MT6589 code touches (nothing in aquaris-5 writes MFG_RESET).
 */
#define MT6589_MFG_G3D_RESET		0
#define MT6589_MFG_AXI_RESET		1

/*
 * DISP - provider: dispsys (0x14000000), "mediatek,mt6589-dispsys",
 * registered by drivers/soc/mediatek/mtk-mmsys.c against DISP_SW_RST_B
 * (0x14000140, data sheet p. 1458).
 *
 * Note the polarity: in this register 0 resets and 1 releases, which is the
 * opposite of every other register described here.  mtk_mmsys_reset_update()
 * accounts for that, so an ID from this group behaves normally from DT.
 */
#define MT6589_DISP_SMI_LARB2_RST	0
#define MT6589_DISP_ROT_RST		1
#define MT6589_DISP_SCL_RST		2
#define MT6589_DISP_OVL_RST		3
#define MT6589_DISP_COLOR_RST		4
#define MT6589_DISP_TDSHP_RST		5
#define MT6589_DISP_BLS_RST		6
#define MT6589_DISP_WDMA0_RST		7
#define MT6589_DISP_WDMA1_RST		8
#define MT6589_DISP_RDMA0_RST		9
#define MT6589_DISP_RDMA1_RST		10
#define MT6589_DISP_GAMMA_RST		11
#define MT6589_DISP_CMDQ_DISPCK_RST	12
#define MT6589_DISP_CMDQ_SMICK_RST	13
#define MT6589_DISP_G2D_RST		14
#define MT6589_DISP_DBI_DISPCK_RST	15
#define MT6589_DISP_DBI_SMICK_RST	16
#define MT6589_DISP_DBI_INTERFACECK_RST	17
#define MT6589_DISP_DSI_RST		18
#define MT6589_DISP_DPI0_RST		19
#define MT6589_DISP_DPI1_RST		20

/*
 * No reset exists for the MIPI TX config block (mipi_tx0 at 0x10012000), so
 * it has no ID in this header.  Its register list (data sheet p. 2280,
 * section 56.3) has no software reset register at all - the PLL and the lane
 * LDOs are brought up through DSI_PLL_TOP and the DSI_CON/DSI_BG_CON power
 * bits - and aquaris-5's DSI driver
 * (mediatek/platform/mt6589/kernel/drivers/video/dsi_drv.c) resets nothing
 * before reprogramming the PLL, only powering the block down again on the way
 * out.  Do not add a "resets" property to that node on the strength of the
 * DSI block's own MT6589_DISP_DSI_RST, which covers the DSI controller and
 * not the PHY.
 */

/*
 * Because an ID is a bit index, renumbering one silently re-targets the reset
 * at every node that already uses it.  Retire unused IDs by commenting them
 * out with the reason instead, and add new ones only from the register
 * layout above.
 */
#endif  /* _DT_BINDINGS_RESET_CONTROLLER_MT6589 */
