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

/*
 * Media pad ids for the receiver subdev.
 *
 * The order is load-bearing in two places at once, so it is fixed here and
 * not re-derived anywhere:
 *
 *   - the DT, whose receiver source port is port@1 (seninf_out, which
 *     points at SCAM's sink).  A port's index is its pad index, resolved by
 *     the core's default v4l2_subdev_get_fwnode_pad_1_to_1();
 *   - mtk_csi2_rx->pads, because media_entity_pads_init() gives each pad the
 *     index it has in that array, and every pad_ops handler in mtk-csi2-rx.c
 *     then indexes by pad number.
 *
 * The sink pad exists in the driver because the block does receive from a
 * sensor, but there is no sensor or D-PHY node in the DT for it to point at
 * yet, so it has no peer in the graph.  That is the same honest gap as the
 * missing "phys" property: the pad describes what the hardware does, the DT
 * does not yet describe a peer for it.  When the D-PHY node comes back, add
 * a receiver port@0 whose endpoint points at the PHY's source endpoint.
 */
enum {
	CSI2_PAD_SINK = 0,	/* from the sensor, via the D-PHY */
	CSI2_PAD_SRC,		/* to the SCAM adaptor */
	CSI2_PAD_NUM,
};

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
 * The D-Lane enables are the three one-hot bits DLANE3_EN / DLANE2_EN /
 * DLANE1_EN at CSI2_CTRL[3:1], and CSI2_EN is at [0].  The data sheet (SENINF1_
 * CSI2_CTRL field table, draft/ds/mipi.txt:2925-2928, repeated for SENINF2 at
 * 4223-4226) gives their descriptions verbatim as "Enables CSI2 3 data lane",
 * "Enables CSI2 2 data lane", "Enables CSI2 1 data lane" and "Enables CSI2".
 * The vendor tree agrees with the bit order: seninf_reg.h:382-385 declares
 * CSI2_EN followed by DLANE1_EN, DLANE2_EN, DLANE3_EN as one-bit fields from
 * bit 0 up, and seninf_drv.cpp:1287 programs them as
 *
 *	(((1 << dlane_num) - 1) << 1) | (csi2_en << 0)
 *
 * i.e. lane N is "the low N of the DLANE bits, shifted up past CSI2_EN", which
 * is exactly the mapping spelled out in mtk-csi2_rx_lane_bits() below.
 *
 * There are only three data-lane bits, so this receiver cannot represent more
 * than three lanes; a four-lane configuration is not expressible on it.
 */
#define CSI2_LANE_MASK				(CSI2_DLANE1_EN | \
						 CSI2_DLANE2_EN | \
						 CSI2_DLANE3_EN)

/* The most data lanes CSI2_CTRL can enable: DLANE3_EN is the top one. */
#define CSI2_MAX_DATA_LANES			3

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

/* ---------------------------------------------------------------------
 * SCAM: 0x1500_8200, per-port stride 0x80
 * ------------------------------------------------------------------ */

/*
 * SCAM_SIZE carries the frame geometry in two 12-bit fields, HEIGHT at
 * 27:16 and WIDTH at 11:0 (data sheet page 2269; matching the vendor
 * REG_SCAM1_SIZE bitfield in seninf_reg.h:907-917).  The receiver sits
 * directly upstream of that register, so this is the widest frame it can
 * hand on.
 */
#define SCAM_SIZE_HEIGHT			GENMASK(27, 16)
#define SCAM_SIZE_WIDTH				GENMASK(11, 0)
#define SCAM_SIZE_MAX				4095

#endif /* _MTK_CSI2_RX_H */
