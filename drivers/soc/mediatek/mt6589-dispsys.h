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
 * Fixed datapath links need no crossbar register and have no routing-table
 * entry.
 *
 * Blocks with no driver
 * ---------------------
 * The crossbar below describes more blocks than the KMS driver uses. The
 * B8000 display path is OVL0 -> BLS -> RDMA0 -> DSI0, discovered through OF
 * graph. The DISP0 blocks not on that path are listed below with the reasons
 * each is unused.
 *
 *  block  base      IRQ  clock gates          reset       why there is no driver
 *  ROT    0x14001000 161 ROT_ENGINE/SMI       DISP_ROT_RST no rotated panel mount on this board
 *  SCL    0x14002000 162 CLK_DISP0_SCL        DISP_SCL_RST panel is native 1280x800; no scaling
 *  WDMA0  0x14004000 164 WDMA0_ENGINE/SMI     DISP_WDMA0_RST no consumer on this path
 *  WDMA1  0x14005000 165 WDMA1_ENGINE/SMI     DISP_WDMA1_RST consumer is panel readback via DBI
 *  DBI    0x1400c000 171 DBI_ENGINE/SMI/OUT   -           panel is DSI video mode; no DBI node
 *  DPI0   0x1400e000 173 CLK_DISP1_DPI0        -           no parallel-RGB output on this board
 *  DPI1   0x1400f000 174 CLK_DISP1_DPI1        -           no parallel-RGB output on this board
 *  CMDQ   0x14012000 176 CMDQ_ENGINE/SMI      -           ISA-incompatible, see below
 *
 * CMDQ deserves its own note, because "there is no driver" is the wrong reason
 * and it is the kind of wrong that makes the work look cheap.
 *
 * The mainline GCE mailbox driver DOES exist upstream and is in this tree:
 * drivers/mailbox/mtk-cmdq-mailbox.c, byte-identical to upstream.  MT6589 is
 * simply not in its cmdq_of_ids[] table.  So this is not a missing driver; it
 * is a missing SoC entry, which sounds like a few lines of data and is not.
 *
 * MT6589's DISP_CMDQ is ISA- and submission-model-incompatible with it:
 *
 *  - MT6589 GCE has eight opcodes (data sheet table 39-1, p. 1466): 0x1 Read,
 *    0x2 Move, 0x4 Write, 0x8 Poll, 0x10 Jump, 0x20 WFE, 0x40 EOC, 0x80 Lock.
 *    The driver defines CMDQ_CODE_READ_S = 0x80 - which on this SoC is a
 *    thread-group Lock, so every PA-mode read would execute a lock - plus
 *    WRITE_S 0x90, WRITE_S_MASK 0x91 and LOGIC 0xa0, which do not exist here
 *    at all.  mtk-cmdq-helper.c emits them, so the no-client-reg PA-mode
 *    fallback every display block depends on cannot be expressed.
 *  - There is no mailbox, no task slots and no priority: submission is
 *    per-thread CMDQ_THRn_PC / END_ADDR / GROUP / EN, and scheduling is
 *    round-robin by time slot.  0x48, where the driver writes GCE_GCTL, is
 *    not a register on this part.  The CMDQ resets are DISP_SW_RST_B bits 12
 *    and 13, which the in-tree MT6589_DISP_CMDQ_DISPCK_RST / SMICK_RST
 *    already name correctly; MT6589_M4U_PORT_CMDQ is a LARB2 port id and is
 *    not a reset at all.
 *  - Worst of all, cmdq_init() at mtk-cmdq-mailbox.c:221-222 loops
 *    `for (i = 0; i <= CMDQ_MAX_EVENT; i++)` with CMDQ_MAX_EVENT = 0x3ff and
 *    writes each value to offset 0x68, which the data sheet names Command
 *    Queue Thread Group 3 Mutex, whose OWNER and LOCK fields are read-only.
 *    Merely probing would scribble 1024 values over a read-only register.  (The
 *    highest real event on this SoC is 34.)
 *
 * So do NOT add a mediatek,mt6589-gce line to cmdq_of_ids[]: a wrong entry is
 * worse than none, because probe alone is destructive here.  Doing it properly
 * is roughly 600-700 lines of new logic - an MT6589-specific mailbox provider
 * plus a cmdq_pkt_* shim restricted to the five valid opcodes - not the 130
 * lines of table data it first appears to be.
 *
 * None of that is urgent: the display works today.  Every display block falls
 * back to direct register writes through mtk_ddp_write(),
 * mtk_ddp_write_relaxed() and mtk_ddp_write_mask() in mtk_ddp_comp.c, whose
 * runtime discriminator is whether a cmdq_pkt was supplied rather than any
 * Kconfig.  CMDQ is an ordering and performance optimisation, never a
 * correctness fix, so leaving it off costs throughput and nothing else.
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
 *  - COLOR and TDSHP are not on the selected B8000 OF-graph path. SCL, WDMA,
 *    DBI and DPI are likewise not selected for this panel. Their register-field
 *    definitions remain above, while the C route table lists the supported
 *    connections and OF graph selects the active component sequence.
 *
 * DISP_MISC (0x060) holds DPI/DBI output controls. The routing table uses
 * DPI0_MODE_EN for the RDMA1 -> DPI0 alternative path.
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

static const struct mtk_mmsys_routes mt6589_dispsys_routing_table[] = {
	/*
	 * LIVE - main path step 1: OVL output -> BLS.
	 *
	 * The overlay goes straight to BLS, not via COLOR.  The bootloader
	 * does exactly this (lk/ddp_path.c:229, OVL_MOUT_EN = 0x2 and
	 * BLS_SEL = 0, commented "ovl_mout output to bls" and "bls_sel
	 * from overlay"), and that is a path known to drive this panel.
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
		    MT6589_OVL_MOUT_EN_BLS, MT6589_OVL_MOUT_EN_BLS),
	MMSYS_ROUTE(OVL0, BLS,
		    MT6589_DISP_BLS_SEL,
		    MT6589_BLS_SEL_MASK, MT6589_BLS_SEL_OVL),

	/*
	 * LIVE but off the signal path: COLOR's input select.
	 *
	 * COLOR is not on the B8000 OF-graph path. This selector entry is kept
	 * for reference; it does not make COLOR part of the active route.
	 * A COLOR path needs its own output mux and an adjacent graph edge.
	 * COLOR_SEL = 1 is the vendor BSP's topology-2 value ("from overlay
	 * after alpha blending", kernel ddp_path.c:857); it is written here
	 * only so the register does not keep a stale bootloader value.  It
	 * has no effect while OVL_MOUT_EN[2] is 0, because that bit - not
	 * this register - is what gives COLOR an input.
	 */
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6589_DISP_COLOR_SEL,
		    MT6589_COLOR_SEL_MASK, MT6589_COLOR_SEL_OVL_POST),

	/*
	 * LIVE - main path step 3: RDMA0 output -> DSI0 / DBI / DPI0.
	 *   BLS feeds RDMA0 via direct link (no SEL register needed).
	 *   RDMA0_OUT_SEL steers RDMA0's output to the target interface.
	 *
	 * DSI0 is the reset default (val=0x0), but write it explicitly
	 * anyway.  The bootloader hands the panel a live DSI link, so the
	 * register arrives holding whatever the previous kernel left in it,
	 * and mtk_mmsys_ddp_disconnect() can clear it on the way down.
	 * Assuming a reset value is what left this path unprogrammed.
	 *
	 * The register is shared, so exactly one of these three survives a
	 * given connect: writing the DBI or DPI0 encoding necessarily
	 * deselects DSI0.  They are listed so the encodings exist and are
	 * checked by the MMSYS_ROUTE build-time assertions; only the DSI0
	 * one is reached on this board, because DBI and DPI0 are not clocked
	 * here.
	 */

	/* RDMA0 -> DSI0 (main LCD path) */
	MMSYS_ROUTE(RDMA0, DSI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_OUT_SEL_MASK, MT6589_RDMA0_OUT_SEL_DSI),

	/* RDMA0 -> DBI */
	MMSYS_ROUTE(RDMA0, DBI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_OUT_SEL_MASK, MT6589_RDMA0_OUT_SEL_DBI),

	/* RDMA0 -> DPI0 (OVL-sourced) */
	MMSYS_ROUTE(RDMA0, DPI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_OUT_SEL_MASK, MT6589_RDMA0_OUT_SEL_DPI0),

	/*
	 * LIVE - memory-out path: OVL -> WDMA1.
	 *   Allows screen-capture concurrently with the main LCD path.
	 *   Shares OVL_MOUT_EN with the BLS route above; the two bits are
	 *   independent, so both can be set and this is a real fan-out, not
	 *   an either/or.  Vendor lk/ddp_path.c:250 writes 0x1 for the
	 *   WDMA1 destination.
	 */
	MMSYS_ROUTE(OVL0, WDMA1,
		    MT6589_DISP_OVL_MOUT_EN,
		    MT6589_OVL_MOUT_EN_WDMA1, MT6589_OVL_MOUT_EN_WDMA1),

	/*
	 * LIVE - direct RDMA1 paths: bypass OVL/COLOR/BLS entirely.
	 *   Used for external display (e.g. HDMI via a bridge chip).
	 *
	 * RDMA1 -> DPI0 needs both halves of the switch: RDMA1_OUT_SEL = 1
	 * picks DPI0 as the target, and DPI0_SEL = 1 makes RDMA1 - not RDMA0 -
	 * the engine actually driving it.  Both match the vendor BSP
	 * (kernel ddp_path.c:875-876).
	 */

	/* RDMA1 -> DPI0 */
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_OUT_SEL_MASK, MT6589_RDMA1_OUT_SEL_DPI0),
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_DPI0_SEL,
		    MT6589_DPI0_SEL_MASK, MT6589_DPI0_SEL_RDMA1),

	/* RDMA1 -> DPI1 */
	MMSYS_ROUTE(RDMA1,  DPI1,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_OUT_SEL_MASK, MT6589_RDMA1_OUT_SEL_DPI1),

	/*
	 * LIVE - MDP path: SCL -> WDMA0.
	 *   Used for MDP (Media Data Path) scaling + write-back.
	 *   ROT feeds SCL; SCL_MOUT_EN routes the output to WDMA0.
	 *   WDMA0_SEL = 0 (from scaler) is the reset value and is not
	 *   written; see the WDMA0_SEL comment above.  The vendor BSP does
	 *   write it explicitly (kernel ddp_path.c:930).
	 */
	MMSYS_ROUTE(SCL, WDMA0,
		    MT6589_DISP_SCL_MOUT_EN,
		    MT6589_SCL_MOUT_EN_WDMA0, MT6589_SCL_MOUT_EN_WDMA0),

	/*
	 * DOCUMENTATION ONLY - the vendor kernel BSP's topology 2, spelled
	 * out as the two extra MMSYS_ROUTE entries it would need:
	 *
	 *   OVL0 -> COLOR0, MT6589_DISP_OVL_MOUT_EN,
	 *           MT6589_OVL_MOUT_EN_COLOR_MASK, MT6589_OVL_MOUT_EN_COLOR,
	 *   COLOR0 -> BLS, MT6589_DISP_COLOR_MOUT_EN,
	 *             MT6589_COLOR_MOUT_EN_BLS_MASK, MT6589_COLOR_MOUT_EN_BLS,
	 *
	 * together with BLS_SEL = MT6589_BLS_SEL_COLOR instead of the
	 * MT6589_BLS_SEL_OVL the table uses above.  All three values are
	 * defined above and all three are what kernel ddp_path.c:854-858
	 * writes.
	 *
	 * These are NOT enabled, deliberately:
	 *  - OVL_MOUT_EN[2] into an engine that is not on the path is the
	 *    configuration that stalled the overlay (OVL_RUN clear, FME_UND,
	 *    RDMA EOF aborts).
	 *  - mtk_mmsys_ddp_connect() has no notion of which engines are
	 *    clocked, so the route would fire regardless.
	 *  - the vendor BSP pairs these writes with a path component list
	 *    that includes COLOR ahead of BLS; enabling the registers without
	 *    that is a half-applied topology.
	 *
	 * DOCUMENTATION ONLY - the BSP's commented-out longer chain with 2D
	 * sharpness, which needs three more entries:
	 *
	 *   TDSHP_MOUT_EN = MT6589_TDSHP_MOUT_EN_OVL_DIRECT (bit 4) to feed
	 *     the overlay's direct-link input,
	 *   OVL_SEL = MT6589_OVL_SEL_TDSHP to pick 2D sharpness as the
	 *     overlay's input,
	 *   TDSHP_SEL = MT6589_TDSHP_SEL_OVL_PRE_BLEND to take the pre-blend
	 *     overlay tap,
	 *
	 * after which the COLOR entries above complete it.  Vendor source
	 * ddp_path.c:860-867, commented out there as well.
	 *
	 * DOCUMENTATION ONLY - RDMA1 -> DBI, the one encoding of RDMA1_OUT_SEL
	 * no vendor path builds.  It would need RDMA1_OUT_SEL =
	 * MT6589_RDMA1_SOUT_DBI and, because DISP_DBI_SEL switches DBI
	 * between the two RDMAs, DBI_SEL = MT6589_DBI_SEL_RDMA1.  See the
	 * RDMA1_OUT_SEL comment for why 0 = DBI is the data sheet reading
	 * that was adopted.
	 */
};

#endif /* __SOC_MEDIATEK_MT6589_DISPSYS_H */
