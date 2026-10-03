/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek CSI-2 receiver (seninf / csi2) driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * Register definitions transcribed from the MT6589 data sheet, chapter
 * "MIPI RX Configuration Module" (seninf_top / seninf / csi2 / SCAM).
 */

#ifndef _MTK_CSI2_RX_H
#define _MTK_CSI2_RX_H

#include <linux/bits.h>

/*
 * The MT6589 image subsystem lays the sensor front end out as three
 * register blocks at 0x1500_8000:
 *
 *   seninf_top  +0x0000   per-port parallel clock gates and N3D reset
 *   csi2        +0x0100   the two CSI-2 receivers themselves
 *   SCAM        +0x0200   the two sensor-CAM adaptors (SCAM1 / SCAM2)
 *
 * Every register block is duplicated per sensor port, so the driver works
 * with a "port index" (0 or 1) and derives the offsets from it.
 */

#define SENINF_MAX_PORTS			2

/* Port index inside a register name, e.g. SENINF1_CSI2_CTRL is port 0. */
#define SENINF_PORT(id)				((id) - 1)

/* ---------------------------------------------------------------------
 * seninf_top: 0x1500_8000
 * ------------------------------------------------------------------ */

#define SENINF_TOP_CTRL				0x00

/*
 * 31     SENINF_TOP_DBG_SEL    0: seninf 1 csi2, 1: seninf 2 csi2
 * 16     SENINF_TOP_N3D_SW_RST  N3D software reset, active high
 * 11:10  SENINF{1,2}_PCLK_EN   parallel sensor clock enable (0: gated)
 *  9     SENINF2_PCLK_SEL       parallel sensor clock select (0: pclk, 1: mclk)
 *  8     SENINF1_PCLK_SEL       parallel sensor clock select (0: pclk, 1: mclk)
 */
#define SENINF_TOP_DBG_SEL			BIT(31)
#define SENINF_TOP_N3D_SW_RST			BIT(16)
#define SENINF_PCLK_EN(port)			BIT(11 - (port))
#define SENINF_PCLK_SEL(port)			BIT(9 - (port) + 1)

/*
 * SENINF_TOP_DBG_SEL selects which receiver a debug write applies to.  It
 * is a global, not per-port, so the driver only touches it on the shared
 * register and leaves it alone afterwards.
 */
#define SENINF_TOP_DBG_SEL_PORT(port)		((port) << 31)

/* ---------------------------------------------------------------------
 * csi2: 0x1500_8100, per-port stride 0x80
 * ------------------------------------------------------------------ */

#define CSI2_CTRL				0x00
#define CSI2_DELAY				0x04
#define CSI2_INTEN				0x08
#define CSI2_INTSTA				0x0c
#define CSI2_ECCDBG				0x10
#define CSI2_CRCDBG				0x14
#define CSI2_DBG				0x18
#define CSI2_VER				0x1c
#define CSI2_SHORT_INFO				0x20
#define CSI2_LNFSM				0x24
#define CSI2_LNMUX				0x28
#define CSI2_HSYNC_CNT				0x2c
#define CSI2_CAL				0x30
#define CSI2_DS					0x34
#define CSI2_VS					0x38
#define CSI2_BIST				0x3c

/* Stride between SENINF1_CSI2_* and SENINF2_CSI2_*. */
#define CSI2_PORT_STRIDE			0x80

/* CSI2_CTRL */
#define CSI2_BIST_CSI2_DATA_OK			BIT(31)
#define CSI2_LANE_FSM_OK			BIT(30)
#define CSI2_HS_FSM_OK				BIT(29)
#define CSI2_BIST_DATA_OK			BIT(28)
#define CSI2_BIST_START				BIT(27)
#define CSI2_BIST_ERROR_COUNT			GENMASK(26, 19)
#define CSI2_DATA_FLOW				GENMASK(18, 17)
#define CSI2_ASYNC_OPTION			BIT(16)
#define CSI2_SYNC_CLR_EXTEND			BIT(15)
#define CSI2_HSRXEN_PFOOT_CLR			BIT(14)
#define CSI2_VSYNC_TYPE				BIT(13)
#define CSI2_SW_RST				BIT(12)
#define CSI2_SCLK4X_SEL				BIT(11)
#define CSI2_SCLK_SEL				BIT(10)
#define CSI2_ESC_EN				BIT(9)
#define CSI2_SYNC_RST_EN			BIT(8)
#define CSI2_LP11_RST_EN			BIT(7)
#define CSI2_CLK_MISS_EN			BIT(6)
#define CSI2_ED_SEL				BIT(5)
#define CSI2_ECC_EN				BIT(4)
#define CSI2_DLANE3_EN				BIT(3)
#define CSI2_DLANE2_EN				BIT(2)
#define CSI2_DLANE1_EN				BIT(1)
#define CSI2_EN					BIT(0)

/* Bits the driver programs; everything else in CSI2_CTRL is read-only. */
#define CSI2_CTRL_RO				GENMASK(31, 16)
#define CSI2_CTRL_RW				GENMASK(15, 0)

/* CSI2_CTRL reset value, from the data sheet: 0x0000_2d80. */
#define CSI2_CTRL_RESET				0x00002d80

/*
 * LANE count -> the three one-hot DLANE{1,2,3}_EN bits.  The data sheet
 * describes the field as "enables CSI2 N data lane", one bit per lane.
 */
#define CSI2_LANE_MASK				(CSI2_DLANE1_EN | \
						 CSI2_DLANE2_EN | \
						 CSI2_DLANE3_EN)

/* CSI2_DELAY */
#define CSI2_LP2HS_DATA_TERM_DELAY		GENMASK(31, 24)
#define CSI2_LP2HS_DATA_SETTLE_DELAY		GENMASK(23, 16)
#define CSI2_LP2HS_CLK_TERM_DELAY		GENMASK(7, 0)

/* CSI2_INTEN */
#define CSI2_VCHANNEL_ID			GENMASK(31, 30)
#define CSI2_DATA_TYPE				GENMASK(29, 24)
#define CSI2_WC_NUMBER				GENMASK(23, 8)
#define CSI2SYNC_NONSYNC_IRQ_EN			BIT(3)
#define CSI2_ECC_CORRECT_IRQ_EN			BIT(2)
#define CSI2_ECC_ERR_IRQ_EN			BIT(1)
#define CSI2_CRC_ERR_IRQ_EN			BIT(0)

/* CSI2_INTSTA (status bits are write-1-to-clear) */
#define CSI2OUT_VSYNC				BIT(21)
#define CSI2OUT_HSYNC				BIT(20)
#define CSI2_SPARE				GENMASK(7, 5)
#define CSI2_IRQ_CLR_SEL			BIT(4)
#define CSI2SYNC_NONSYNC_IRQ			BIT(3)
#define CSI2_ECC_CORRECT_IRQ			BIT(2)
#define CSI2_ECC_ERR_IRQ			BIT(1)
#define CSI2_CRC_ERR_IRQ			BIT(0)

/* Status bits this driver owns; excludes the read-only sync outputs. */
#define CSI2_INTSTA_ERR				GENMASK(3, 0)

/* CSI2_LNMUX: how the four received lanes map onto the two output pairs. */
#define CSI2_DATA_LN1_MUX			GENMASK(5, 4)
#define CSI2_DATA_LN0_MUX			GENMASK(1, 0)

/* CSI2_CAL */
#define CSI2_CAL_CNT_2				GENMASK(31, 24)
#define CSI2_CAL_CNT_1				GENMASK(23, 16)
#define CSI2_CAL_STATE				GENMASK(6, 4)
#define CSI2_CAL_EN				BIT(0)

/* CSI2_VS */
#define CSI2_VS_VPOS				GENMASK(31, 16)
#define CSI2_VS_HSYNC_CNT			GENMASK(15, 0)

#endif /* _MTK_CSI2_RX_H */