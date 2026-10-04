/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef __SOC_MEDIATEK_MT6589_DISPSYS_H
#define __SOC_MEDIATEK_MT6589_DISPSYS_H

#include "mtk-mmsys.h"

/*
 * MT6589 display pipeline overview:
 *
 * Main path (OVL → LCD):
 *   OVL → COLOR → BLS → RDMA0 → DSI0
 *   OVL → COLOR → BLS → RDMA0 → DBI
 *   OVL → COLOR → BLS → RDMA0 → DPI0
 *
 * Memory-out path (concurrent with main path):
 *   OVL → WDMA1
 *
 * Direct RDMA1 paths (bypass OVL/COLOR/BLS):
 *   RDMA1 → DPI0
 *   RDMA1 → DPI1
 *
 * MDP path:
 *   SCL → WDMA0
 *
 * Routing register bit/value summary (from vendor disp_path_config_()):
 *
 *   OVL_MOUT_EN   GENMASK(2,0)  bit[0]=WDMA1, bit[2]=COLOR
 *   COLOR_MOUT_EN GENMASK(3,0)  bit[3]=BLS
 *   COLOR_SEL     0x1           0x1=from OVL
 *   BLS_SEL       0x1           0x1=from COLOR
 *   RDMA0_OUT_SEL 0x3           0x0=DSI0, 0x1=DBI, 0x2=DPI0
 *   RDMA1_OUT_SEL 0x3           0x1=DPI0, 0x2=DPI1
 *   DPI0_SEL      0x1           0x0=RDMA0, 0x1=RDMA1
 *   DBI_SEL       0x1           0x0=RDMA0
 *   SCL_MOUT_EN   BIT(0)        bit[0]=WDMA0
 *   WDMA0_SEL     0x1           0x0=SCL
 */

/*
 * DISPSYS software reset.  The register is the active-low SW_RST_B form
 * shared with the other MediaTek MMSYS blocks: a 0 bit holds its component
 * in reset and a 1 releases it, and the register resets to all ones.  That
 * is the same polarity mtk_mmsys_reset_update() drives by default, so no
 * per-SoC override is needed.  Bit assignments are in
 * include/dt-bindings/reset/mt6589-resets.h.
 */
#define MT6589_DISP_SW_RST_B			0x140
#define MT6589_DISP_NUM_RESETS			21

#define MT6589_DISP_SCL_MOUT_EN		0x020
#define MT6589_DISP_OVL_MOUT_EN		0x024
#define MT6589_DISP_COLOR_MOUT_EN	0x028
#define MT6589_DISP_RDMA0_OUT_SEL	0x034
#define MT6589_DISP_RDMA1_OUT_SEL 	0x038
#define MT6589_DISP_WDMA0_SEL_IN	0x040
#define MT6589_DISP_COLOR_SEL_IN	0x04c
#define MT6589_DISP_BLS_SEL_IN		0x054
#define MT6589_DISP_DPI0_SEL_IN		0x05c
#define MT6589_DISP_DBI_SEL_IN		0x058

/* OVL_MOUT_EN */
#define MT6589_OVL_MOUT_EN_WDMA1	BIT(0)
#define MT6589_OVL_MOUT_EN_COLOR	BIT(2)
#define MT6589_OVL_MOUT_EN_WDMA1_MASK	BIT(0)
#define MT6589_OVL_MOUT_EN_COLOR_MASK	BIT(2)
/* DISP_OVL_MOUT_EN bit 1 = "Output to BLS" (data sheet). */
#define MT6589_OVL_MOUT_EN_BLS		BIT(1)
#define MT6589_OVL_MOUT_EN_BLS_MASK	BIT(1)

/* COLOR_MOUT_EN */
#define MT6589_COLOR_MOUT_EN_BLS	BIT(3)
#define MT6589_COLOR_MOUT_EN_MASK	BIT(3)

/* COLOR_SEL_IN */
#define MT6589_COLOR_SEL_IN_OVL		0x1
#define MT6589_COLOR_SEL_IN_MASK	0x1

/* BLS_SEL_IN */
#define MT6589_BLS_SEL_IN_COLOR		0x1
/* DISP_BLS_SEL: 0 = "From overlay", 1 = "From color engine" (data sheet). */
#define MT6589_BLS_SEL_IN_OVL		0x0
#define MT6589_BLS_SEL_IN_MASK		0x1

/* RDMA0_SOUT_SEL */
#define MT6589_RDMA0_SOUT_DSI0		0x0
#define MT6589_RDMA0_SOUT_DBI		0x1
#define MT6589_RDMA0_SOUT_DPI0		0x2
#define MT6589_RDMA0_SOUT_MASK		0x3

/* RDMA1_SOUT_SEL */
#define MT6589_RDMA1_SOUT_DPI0		0x1
#define MT6589_RDMA1_SOUT_DPI1		0x2
#define MT6589_RDMA1_SOUT_MASK		0x3

/* DPI0_SEL_IN */
#define MT6589_DPI0_SEL_IN_RDMA1	0x1
#define MT6589_DPI0_SEL_IN_MASK		0x1

/* SCL_MOUT_EN */
#define MT6589_SCL_MOUT_EN_WDMA0	BIT(0)
#define MT6589_SCL_MOUT_EN_MASK		BIT(0)

static const struct mtk_mmsys_routes mt6589_dispsys_routing_table[] = {
	/*
	 * Main path step 1: OVL output -> BLS.
	 *
	 * The overlay goes straight to BLS, not via COLOR.  The bootloader
	 * does exactly this (lk/ddp_path.c: for DISP_MODULE_DSI it writes
	 * OVL_MOUT_EN = 0x2 and BLS_SEL = 0, commented "ovl_mout output to
	 * bls" and "bls_sel from overlay"), and that is a path known to
	 * drive this panel.
	 *
	 * The data sheet agrees on both encodings:
	 *   DISP_OVL_MOUT_EN bit 1 = "Output to BLS", bit 2 = "Output to
	 *   COLOR" - so the bootloader value 0x2 is bit 1, BLS.
	 *   DISP_BLS_SEL 0 = "From overlay", 1 = "From color engine" - so
	 *   the bootloader value 0 is the overlay.
	 *
	 * Routing through COLOR instead left OVL_RUN clear and the OVL
	 * reporting FME_UND with the RDMA EOF aborts set: the overlay was
	 * feeding an engine that was not in the path, so it never drained.
	 */
	MMSYS_ROUTE(OVL0, BLS,
		    MT6589_DISP_OVL_MOUT_EN,
		    MT6589_OVL_MOUT_EN_BLS_MASK, MT6589_OVL_MOUT_EN_BLS),
	MMSYS_ROUTE(OVL0, BLS,
		    MT6589_DISP_BLS_SEL_IN,
		    MT6589_BLS_SEL_IN_MASK, MT6589_BLS_SEL_IN_OVL),

	/*
	 * COLOR is kept in the path component list so its registers are still
	 * clocked and configured, but it is not on the signal route.
	 */
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6589_DISP_COLOR_SEL_IN,
		    MT6589_COLOR_SEL_IN_MASK, MT6589_COLOR_SEL_IN_OVL),

	/*
	 * Main path step 3: RDMA0 output → DSI0 / DBI / DPI0
	 *   BLS feeds RDMA0 via direct-link (no SEL register needed).
	 *   RDMA0_SOUT_SEL steers RDMA0's output to the target interface.
	 *
	 * DSI0 is the reset default (val=0x0), but write it explicitly
	 * anyway.  The bootloader hands the panel a live DSI link, so the
	 * register arrives holding whatever the previous kernel left in it,
	 * and mtk_mmsys_ddp_disconnect() can clear it on the way down.
	 * Assuming a reset value is what left this path unprogrammed.
	 */

	/* RDMA0 → DSI0 (main LCD path) */
	MMSYS_ROUTE(RDMA0, DSI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_SOUT_MASK, MT6589_RDMA0_SOUT_DSI0),

	/* RDMA0 → DBI */
	MMSYS_ROUTE(RDMA0, DBI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_SOUT_MASK, MT6589_RDMA0_SOUT_DBI),

	/* RDMA0 → DPI0 (OVL-sourced) */
	MMSYS_ROUTE(RDMA0, DPI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_SOUT_MASK, MT6589_RDMA0_SOUT_DPI0),

	/*
	 * Memory-out path: OVL → WDMA1
	 *   Allows screen-capture concurrently with the main LCD path.
	 *   Enabled by setting bit[0] of OVL_MOUT_EN alongside bit[2].
	 */
	MMSYS_ROUTE(OVL0, WDMA1,
		    MT6589_DISP_OVL_MOUT_EN,
		    MT6589_OVL_MOUT_EN_WDMA1_MASK, MT6589_OVL_MOUT_EN_WDMA1),

	/*
	 * Direct RDMA1 paths: bypass OVL/COLOR/BLS entirely.
	 *   Used for external display (e.g. HDMI via bridge chip).
	 */

	/* RDMA1 → DPI0 */
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_SOUT_MASK, MT6589_RDMA1_SOUT_DPI0),
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_DPI0_SEL_IN,
		    MT6589_DPI0_SEL_IN_MASK, MT6589_DPI0_SEL_IN_RDMA1),

	/* RDMA1 → DPI1 */
	MMSYS_ROUTE(RDMA1,  DPI1,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_SOUT_MASK, MT6589_RDMA1_SOUT_DPI1),

	/*
	 * MDP path: SCL → WDMA0
	 *   Used for MDP (Media Data Path) scaling + write-back.
	 *   ROT feeds SCL; SCL_MOUT_EN routes the output to WDMA0.
	 *   WDMA0_SEL_IN defaults to SCL (val=0x0); no SEL entry needed.
	 */
	MMSYS_ROUTE(SCL, WDMA0,
		    MT6589_DISP_SCL_MOUT_EN,
		    MT6589_SCL_MOUT_EN_MASK, MT6589_SCL_MOUT_EN_WDMA0),
};

#endif /* __SOC_MEDIATEK_MT6589_DISPSYS_H */
