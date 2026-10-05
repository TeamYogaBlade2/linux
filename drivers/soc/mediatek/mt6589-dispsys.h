/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef __SOC_MEDIATEK_MT6589_DISPSYS_H
#define __SOC_MEDIATEK_MT6589_DISPSYS_H

#include "mtk-mmsys.h"

/*
 * MT6589 DISPSYS_CONFIG
 *
 * The register block is described in the MT6589 datasheet, chapter 38
 * "Display Subsystem Configuration". DISPSYS_CONFIG is based at 0x14000000.
 *
 * Registers 0x020..0x05c are the display crossbar controls. MOUT_EN fields
 * enable output ports and *_SEL fields select the input consumed by a block.
 * Fixed datapath links do not need entries in the routing table below.
 *
 * Blocks with no driver
 * ---------------------
 * The crossbar below describes more blocks than the KMS driver drives. The
 * main path is OVL0 -> BLS -> RDMA0 -> DSI0 (mtk_drm_drv.c), and these are
 * the DISP0 blocks that have no driver, with the reason for each. A grep for
 * e.g. "mt6589-rotator" returns nothing, so without this note the next person
 * has to re-derive the whole table from the datasheet.
 *
 *  block  base      IRQ  clock gates          reset       why there is no driver
 *  ROT    0x14001000 161 ROT_ENGINE/SMI       DISP_ROT_RST no rotated panel mount on this board
 *  SCL    0x14002000 162 CLK_DISP0_SCL        DISP_SCL_RST panel is native 1280x800; no scaling
 *  WDMA0  0x14004000 164 WDMA0_ENGINE/SMI     DISP_WDMA0_RST no consumer on this path
 *  WDMA1  0x14005000 165 WDMA1_ENGINE/SMI     DISP_WDMA1_RST consumer is panel readback via DBI
 *  DBI    0x1400c000 171 DBI_ENGINE/SMI/OUT   -           panel is DSI video mode; no DBI node
 *  DPI0   0x1400e000 173 CLK_DISP1_DPI0        -           no parallel-RGB output on this board
 *  DPI1   0x1400f000 174 CLK_DISP1_DPI1        -           no parallel-RGB output on this board
 *  CMDQ   0x14012000 176 CMDQ_ENGINE/SMI      -           no command-queue driver exists upstream
 *
 * Two of these deserve the detail, because the routing table does carry entries
 * for them and that is misleading on its own:
 *
 *  - WDMA is the write-only output leg, used downstream for panel readback
 *    (screencap, WDMA1) and for an SCL-to-memory leg marked "FIXME: for hdmi
 *    temp" upstream. Both need something this board does not have: panel
 *    readback needs the DBI path, which a DSI video-mode panel does not use,
 *    and the memory leg needs HDMI. The registers are a clean datasheet match
 *    (chapter 41), so this is a missing driver rather than uncertain hardware.
 *    mtk_ddp_comp.c reserves DDP_COMPONENT_WDMA0/1 with a NULL funcs pointer;
 *    every accessor in mtk_ddp_comp.h guards on !comp->funcs, so a component
 *    that ends up on a path degrades to a no-op rather than faulting.
 *
 *  - COLOR and TDSHP have drivers (mtk_disp_color.c, mtk_disp_tdshp.c) and DT
 *    nodes, but are deliberately not on the MT6589 path; see mtk_drm_drv.c.
 *    The reason is not simply that the vendor omits them. The BSP's own path
 *    setup for DISP_MODULE_DSI, DISP_MODULE_DSI_VDO and DISP_MODULE_DSI_CMD
 *    routes OVL -> COLOR -> BLS with COLOR_SEL=1 and BLS_SEL=1
 *    (aquaris-5 ddp_path.c:850-858), whereas the LK bootloader uses a
 *    different topology with no COLOR at all (OVL_MOUT_EN=0x2 straight to BLS).
 *    The vendor trees disagree, the LK topology is the one this driver follows,
 *    and COLOR has never been exercised on hardware here. Note that putting
 *    COLOR0 into the CRTC component array without also adding a (COLOR0, BLS)
 *    route silently disables the display: mtk_mmsys_ddp_connect() only wires
 *    ADJACENT pairs and returns void, so a missing route becomes a no-op
 *    rather than an error. Both halves have to change together.
 *
 * DISP_MISC (0x060) holds the DPI0/1/2 and DBI C/IO select bits. It is defined
 * here and written by nothing, because only DPI and DBI would use it.
 */

/* DISPSYS_CONFIG routing registers. */
#define MT6589_DISP_SCL_MOUT_EN        0x020
#define MT6589_DISP_OVL_MOUT_EN        0x024
#define MT6589_DISP_COLOR_MOUT_EN      0x028
#define MT6589_DISP_TDSHP_MOUT_EN      0x02c
#define MT6589_DISP_MOUT_RST           0x030
#define MT6589_DISP_RDMA0_OUT_SEL      0x034
#define MT6589_DISP_RDMA1_OUT_SEL      0x038
#define MT6589_DISP_OVL_PQ_OUT_SEL     0x03c
#define MT6589_DISP_WDMA0_SEL          0x040
#define MT6589_DISP_OVL_SEL            0x044
#define MT6589_DISP_OVL_PQ_IN_SEL      0x048
#define MT6589_DISP_COLOR_SEL          0x04c
#define MT6589_DISP_TDSHP_SEL          0x050
#define MT6589_DISP_BLS_SEL            0x054
#define MT6589_DISP_DBI_SEL            0x058
#define MT6589_DISP_DPI0_SEL           0x05c
#define MT6589_DISP_MISC               0x060

/* DISPSYS software reset. */
#define MT6589_DISP_SW_RST_B           0x140
#define MT6589_DISP_NUM_RESETS         21

/* DISP_SCL_MOUT_EN: bits [3:0]. */
#define MT6589_SCL_MOUT_EN_MASK        GENMASK(3, 0)
#define MT6589_SCL_MOUT_EN_WDMA0       BIT(0)
#define MT6589_SCL_MOUT_EN_OVL         BIT(1)
#define MT6589_SCL_MOUT_EN_COLOR       BIT(2)
#define MT6589_SCL_MOUT_EN_TDSHP       BIT(3)

/* DISP_OVL_MOUT_EN: bits [3:0]. */
#define MT6589_OVL_MOUT_EN_MASK        GENMASK(3, 0)
#define MT6589_OVL_MOUT_EN_WDMA1       BIT(0)
#define MT6589_OVL_MOUT_EN_BLS         BIT(1)
#define MT6589_OVL_MOUT_EN_COLOR       BIT(2)
#define MT6589_OVL_MOUT_EN_TDSHP       BIT(3)

/* DISP_COLOR_MOUT_EN: bits [4:0]. */
#define MT6589_COLOR_MOUT_EN_MASK      GENMASK(4, 0)
#define MT6589_COLOR_MOUT_EN_OVL_PQ    BIT(0)
#define MT6589_COLOR_MOUT_EN_TDSHP     BIT(1)
#define MT6589_COLOR_MOUT_EN_WDMA0     BIT(2)
#define MT6589_COLOR_MOUT_EN_BLS       BIT(3)
#define MT6589_COLOR_MOUT_EN_OVL       BIT(4)

/* DISP_TDSHP_MOUT_EN: bits [4:0]. */
#define MT6589_TDSHP_MOUT_EN_MASK      GENMASK(4, 0)
#define MT6589_TDSHP_MOUT_EN_OVL_PQ    BIT(0)
#define MT6589_TDSHP_MOUT_EN_COLOR     BIT(1)
#define MT6589_TDSHP_MOUT_EN_WDMA0     BIT(2)
#define MT6589_TDSHP_MOUT_EN_BLS       BIT(3)
#define MT6589_TDSHP_MOUT_EN_OVL       BIT(4)

/* DISP_MOUT_RST: bits [3:0]. */
#define MT6589_MOUT_RST_MASK           GENMASK(3, 0)
#define MT6589_MOUT_RST_SCL            BIT(0)
#define MT6589_MOUT_RST_OVL            BIT(1)
#define MT6589_MOUT_RST_COLOR          BIT(2)
#define MT6589_MOUT_RST_TDSHP          BIT(3)

/* DISP_RDMA0_OUT_SEL: 0 = DSI, 1 = DBI, 2 = DPI0. */
#define MT6589_RDMA0_OUT_SEL_MASK      GENMASK(1, 0)
#define MT6589_RDMA0_OUT_SEL_DSI       0x0
#define MT6589_RDMA0_OUT_SEL_DBI       0x1
#define MT6589_RDMA0_OUT_SEL_DPI0      0x2

/* DISP_RDMA1_OUT_SEL: 0 = DBI, 1 = DPI0, 2 = DPI1. */
#define MT6589_RDMA1_OUT_SEL_MASK      GENMASK(1, 0)
#define MT6589_RDMA1_OUT_SEL_DBI       0x0
#define MT6589_RDMA1_OUT_SEL_DPI0      0x1
#define MT6589_RDMA1_OUT_SEL_DPI1      0x2

/* DISP_OVL_PQ_OUT_SEL: 0 = COLOR, 1 = TDSHP. */
#define MT6589_OVL_PQ_OUT_SEL_MASK     BIT(0)
#define MT6589_OVL_PQ_OUT_SEL_COLOR    0x0
#define MT6589_OVL_PQ_OUT_SEL_TDSHP    0x1

/* DISP_WDMA0_SEL: 0 = SCL, 1 = COLOR, 2 = TDSHP. */
#define MT6589_WDMA0_SEL_MASK          GENMASK(1, 0)
#define MT6589_WDMA0_SEL_SCL           0x0
#define MT6589_WDMA0_SEL_COLOR         0x1
#define MT6589_WDMA0_SEL_TDSHP         0x2

/* DISP_OVL_SEL: 0 = SCL, 1 = COLOR, 2 = TDSHP. */
#define MT6589_OVL_SEL_MASK            GENMASK(1, 0)
#define MT6589_OVL_SEL_SCL             0x0
#define MT6589_OVL_SEL_COLOR           0x1
#define MT6589_OVL_SEL_TDSHP           0x2

/* DISP_OVL_PQ_IN_SEL: 0 = COLOR, 1 = TDSHP. */
#define MT6589_OVL_PQ_IN_SEL_MASK      BIT(0)
#define MT6589_OVL_PQ_IN_SEL_COLOR     0x0
#define MT6589_OVL_PQ_IN_SEL_TDSHP     0x1

/* DISP_COLOR_SEL: 0 = OVL pre-blend, 1 = OVL post-blend, 2 = TDSHP, 3 = SCL. */
#define MT6589_COLOR_SEL_MASK          GENMASK(1, 0)
#define MT6589_COLOR_SEL_OVL_PRE       0x0
#define MT6589_COLOR_SEL_OVL_POST      0x1
#define MT6589_COLOR_SEL_TDSHP         0x2
#define MT6589_COLOR_SEL_SCL           0x3

/* DISP_TDSHP_SEL: 0 = OVL pre-blend, 1 = OVL post-blend, 2 = COLOR, 3 = SCL. */
#define MT6589_TDSHP_SEL_MASK          GENMASK(1, 0)
#define MT6589_TDSHP_SEL_OVL_PRE       0x0
#define MT6589_TDSHP_SEL_OVL_POST      0x1
#define MT6589_TDSHP_SEL_COLOR         0x2
#define MT6589_TDSHP_SEL_SCL           0x3

/* DISP_BLS_SEL: 0 = OVL, 1 = COLOR, 2 = TDSHP. */
#define MT6589_BLS_SEL_MASK            GENMASK(1, 0)
#define MT6589_BLS_SEL_OVL             0x0
#define MT6589_BLS_SEL_COLOR           0x1
#define MT6589_BLS_SEL_TDSHP           0x2

/* DISP_DBI_SEL: 0 = RDMA0, 1 = RDMA1. */
#define MT6589_DBI_SEL_MASK            BIT(0)
#define MT6589_DBI_SEL_RDMA0           0x0
#define MT6589_DBI_SEL_RDMA1           0x1

/* DISP_DPI0_SEL: 0 = RDMA0, 1 = RDMA1. */
#define MT6589_DPI0_SEL_MASK           BIT(0)
#define MT6589_DPI0_SEL_RDMA0          0x0
#define MT6589_DPI0_SEL_RDMA1          0x1

/* DISP_MISC: DPI/DBI external IO mode controls. */
#define MT6589_DISP_MISC_DPI0_MODE_EN  BIT(0)
#define MT6589_DISP_MISC_DPI0_I2X_EN   BIT(1)
#define MT6589_DISP_MISC_DPI0_EDGE_SEL BIT(2)
#define MT6589_DISP_MISC_DPI0_IO_SEL   BIT(3)
#define MT6589_DISP_MISC_DPI1_MODE_EN  BIT(4)
#define MT6589_DISP_MISC_DPI1_I2X_EN   BIT(5)
#define MT6589_DISP_MISC_DPI1_EDGE_SEL BIT(6)
#define MT6589_DISP_MISC_DPI1_IO_EN    BIT(7)
#define MT6589_DISP_MISC_DPI2_MODE_EN  BIT(8)
#define MT6589_DISP_MISC_DPI2_I2X_EN   BIT(9)
#define MT6589_DISP_MISC_DPI2_EDGE_SEL BIT(10)
#define MT6589_DISP_MISC_DPI2_IO_EN    BIT(11)
#define MT6589_DISP_MISC_DBI_C_IO_SEL  BIT(12)

/*
 * Routes used by the MT6589 display paths. When a connection needs both an
 * output-port enable and an input selector, both registers are represented by
 * separate entries with the same (from, to) pair.
 *
 * The MT6589 datasheet path-debug registers show fixed links that therefore
 * need no route entry (DISP_PATH_DEBUG0 p. 1446): bit 00 "rot -> scl" and bit
 * 07 "bls -> rdma0"; plus DISP_PATH_DEBUG1 bit 02 "rdma1 -> gamma".
 */
static const struct mtk_mmsys_routes mt6589_dispsys_routing_table[] = {
	/* SCL -> WDMA0 */
	MMSYS_ROUTE(SCL, WDMA0,
		    MT6589_DISP_SCL_MOUT_EN,
		    MT6589_SCL_MOUT_EN_WDMA0, MT6589_SCL_MOUT_EN_WDMA0),
	MMSYS_ROUTE(SCL, WDMA0,
		    MT6589_DISP_WDMA0_SEL,
		    MT6589_WDMA0_SEL_MASK, MT6589_WDMA0_SEL_SCL),

	/* SCL -> OVL */
	MMSYS_ROUTE(SCL, OVL0,
		    MT6589_DISP_SCL_MOUT_EN,
		    MT6589_SCL_MOUT_EN_OVL, MT6589_SCL_MOUT_EN_OVL),
	MMSYS_ROUTE(SCL, OVL0,
		    MT6589_DISP_OVL_SEL,
		    MT6589_OVL_SEL_MASK, MT6589_OVL_SEL_SCL),

	/* SCL -> COLOR */
	MMSYS_ROUTE(SCL, COLOR0,
		    MT6589_DISP_SCL_MOUT_EN,
		    MT6589_SCL_MOUT_EN_COLOR, MT6589_SCL_MOUT_EN_COLOR),
	MMSYS_ROUTE(SCL, COLOR0,
		    MT6589_DISP_COLOR_SEL,
		    MT6589_COLOR_SEL_MASK, MT6589_COLOR_SEL_SCL),

	/* SCL -> TDSHP */
	MMSYS_ROUTE(SCL, TDSHP,
		    MT6589_DISP_SCL_MOUT_EN,
		    MT6589_SCL_MOUT_EN_TDSHP, MT6589_SCL_MOUT_EN_TDSHP),
	MMSYS_ROUTE(SCL, TDSHP,
		    MT6589_DISP_TDSHP_SEL,
		    MT6589_TDSHP_SEL_MASK, MT6589_TDSHP_SEL_SCL),

	/* OVL -> WDMA1 */
	MMSYS_ROUTE(OVL0, WDMA1,
		    MT6589_DISP_OVL_MOUT_EN,
		    MT6589_OVL_MOUT_EN_WDMA1, MT6589_OVL_MOUT_EN_WDMA1),

	/* OVL -> BLS */
	MMSYS_ROUTE(OVL0, BLS,
		    MT6589_DISP_OVL_MOUT_EN,
		    MT6589_OVL_MOUT_EN_BLS, MT6589_OVL_MOUT_EN_BLS),
	MMSYS_ROUTE(OVL0, BLS,
		    MT6589_DISP_BLS_SEL,
		    MT6589_BLS_SEL_MASK, MT6589_BLS_SEL_OVL),

	/* OVL -> COLOR */
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6589_DISP_OVL_MOUT_EN,
		    MT6589_OVL_MOUT_EN_COLOR, MT6589_OVL_MOUT_EN_COLOR),
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6589_DISP_COLOR_SEL,
		    MT6589_COLOR_SEL_MASK, MT6589_COLOR_SEL_OVL_POST),

	/* OVL -> TDSHP */
	MMSYS_ROUTE(OVL0, TDSHP,
		    MT6589_DISP_OVL_MOUT_EN,
		    MT6589_OVL_MOUT_EN_TDSHP, MT6589_OVL_MOUT_EN_TDSHP),
	MMSYS_ROUTE(OVL0, TDSHP,
		    MT6589_DISP_TDSHP_SEL,
		    MT6589_TDSHP_SEL_MASK, MT6589_TDSHP_SEL_OVL_POST),

	/* COLOR -> WDMA0 */
	MMSYS_ROUTE(COLOR0, WDMA0,
		    MT6589_DISP_COLOR_MOUT_EN,
		    MT6589_COLOR_MOUT_EN_WDMA0, MT6589_COLOR_MOUT_EN_WDMA0),
	MMSYS_ROUTE(COLOR0, WDMA0,
		    MT6589_DISP_WDMA0_SEL,
		    MT6589_WDMA0_SEL_MASK, MT6589_WDMA0_SEL_COLOR),

	/* COLOR -> OVL direct input */
	MMSYS_ROUTE(COLOR0, OVL0,
		    MT6589_DISP_COLOR_MOUT_EN,
		    MT6589_COLOR_MOUT_EN_OVL, MT6589_COLOR_MOUT_EN_OVL),
	MMSYS_ROUTE(COLOR0, OVL0,
		    MT6589_DISP_OVL_SEL,
		    MT6589_OVL_SEL_MASK, MT6589_OVL_SEL_COLOR),

	/* COLOR -> TDSHP */
	MMSYS_ROUTE(COLOR0, TDSHP,
		    MT6589_DISP_COLOR_MOUT_EN,
		    MT6589_COLOR_MOUT_EN_TDSHP, MT6589_COLOR_MOUT_EN_TDSHP),
	MMSYS_ROUTE(COLOR0, TDSHP,
		    MT6589_DISP_TDSHP_SEL,
		    MT6589_TDSHP_SEL_MASK, MT6589_TDSHP_SEL_COLOR),

	/* COLOR -> BLS */
	MMSYS_ROUTE(COLOR0, BLS,
		    MT6589_DISP_COLOR_MOUT_EN,
		    MT6589_COLOR_MOUT_EN_BLS, MT6589_COLOR_MOUT_EN_BLS),
	MMSYS_ROUTE(COLOR0, BLS,
		    MT6589_DISP_BLS_SEL,
		    MT6589_BLS_SEL_MASK, MT6589_BLS_SEL_COLOR),

	/* TDSHP -> WDMA0 */
	MMSYS_ROUTE(TDSHP, WDMA0,
		    MT6589_DISP_TDSHP_MOUT_EN,
		    MT6589_TDSHP_MOUT_EN_WDMA0, MT6589_TDSHP_MOUT_EN_WDMA0),
	MMSYS_ROUTE(TDSHP, WDMA0,
		    MT6589_DISP_WDMA0_SEL,
		    MT6589_WDMA0_SEL_MASK, MT6589_WDMA0_SEL_TDSHP),

	/* TDSHP -> COLOR */
	MMSYS_ROUTE(TDSHP, COLOR0,
		    MT6589_DISP_TDSHP_MOUT_EN,
		    MT6589_TDSHP_MOUT_EN_COLOR, MT6589_TDSHP_MOUT_EN_COLOR),
	MMSYS_ROUTE(TDSHP, COLOR0,
		    MT6589_DISP_COLOR_SEL,
		    MT6589_COLOR_SEL_MASK, MT6589_COLOR_SEL_TDSHP),

	/* TDSHP -> OVL direct input */
	MMSYS_ROUTE(TDSHP, OVL0,
		    MT6589_DISP_TDSHP_MOUT_EN,
		    MT6589_TDSHP_MOUT_EN_OVL, MT6589_TDSHP_MOUT_EN_OVL),
	MMSYS_ROUTE(TDSHP, OVL0,
		    MT6589_DISP_OVL_SEL,
		    MT6589_OVL_SEL_MASK, MT6589_OVL_SEL_TDSHP),

	/* TDSHP -> BLS */
	MMSYS_ROUTE(TDSHP, BLS,
		    MT6589_DISP_TDSHP_MOUT_EN,
		    MT6589_TDSHP_MOUT_EN_BLS, MT6589_TDSHP_MOUT_EN_BLS),
	MMSYS_ROUTE(TDSHP, BLS,
		    MT6589_DISP_BLS_SEL,
		    MT6589_BLS_SEL_MASK, MT6589_BLS_SEL_TDSHP),

	/* RDMA0 -> DSI */
	MMSYS_ROUTE(RDMA0, DSI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_OUT_SEL_MASK, MT6589_RDMA0_OUT_SEL_DSI),

	/* RDMA0 -> DBI */
	MMSYS_ROUTE(RDMA0, DBI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_OUT_SEL_MASK, MT6589_RDMA0_OUT_SEL_DBI),
	MMSYS_ROUTE(RDMA0, DBI0,
		    MT6589_DISP_DBI_SEL,
		    MT6589_DBI_SEL_MASK, MT6589_DBI_SEL_RDMA0),

	/* RDMA0 -> DPI0 */
	MMSYS_ROUTE(RDMA0, DPI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_OUT_SEL_MASK, MT6589_RDMA0_OUT_SEL_DPI0),
	MMSYS_ROUTE(RDMA0, DPI0,
		    MT6589_DISP_DPI0_SEL,
		    MT6589_DPI0_SEL_MASK, MT6589_DPI0_SEL_RDMA0),

	/* RDMA1 -> DBI */
	MMSYS_ROUTE(RDMA1, DBI0,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_OUT_SEL_MASK, MT6589_RDMA1_OUT_SEL_DBI),
	MMSYS_ROUTE(RDMA1, DBI0,
		    MT6589_DISP_DBI_SEL,
		    MT6589_DBI_SEL_MASK, MT6589_DBI_SEL_RDMA1),

	/* RDMA1 -> DPI0 */
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_OUT_SEL_MASK, MT6589_RDMA1_OUT_SEL_DPI0),
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_DPI0_SEL,
		    MT6589_DPI0_SEL_MASK, MT6589_DPI0_SEL_RDMA1),
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_MISC,
		    MT6589_DISP_MISC_DPI0_MODE_EN,
		    MT6589_DISP_MISC_DPI0_MODE_EN),

	/* RDMA1 -> DPI1 */
	MMSYS_ROUTE(RDMA1, DPI1,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_OUT_SEL_MASK, MT6589_RDMA1_OUT_SEL_DPI1),
};

#endif /* __SOC_MEDIATEK_MT6589_DISPSYS_H */
