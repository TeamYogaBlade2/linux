/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef __SOC_MEDIATEK_MT6589_DISPSYS_H
#define __SOC_MEDIATEK_MT6589_DISPSYS_H

#include "mtk-mmsys.h"

/*
 * MT6589 display routing: the DISPSYS_CONFIG block.
 *
 * All offsets below are relative to the dispsys syscon node
 * (arch/arm/boot/dts/mediatek/mt6589.dtsi, "syscon@14000000"), so they land
 * at 0x14000000 + offset.  That base is DISPSYS_BASE in the vendor trees
 * (lk/include/platform/mt_reg_base.h:163, IO_PHYS + 0x04000000), which is
 * the same value the vendor DISP_REG_CONFIG_* macros add these offsets to
 * (kernel/drivers/dispsys/ddp_reg.h:111-126).
 *
 * The register names and offsets 0x020..0x05c below are corroborated by the
 * data sheet chapter "38.3 Register Definition, Module name: DISPSYS_CONFIG"
 * (draft/ds/ovl.txt:192527-192598, addresses 0x14000020..0x1400005C).  The
 * MT6589-Datasheet.txt extract in the tree root is the G2D chapter only and
 * says nothing about DISPSYS, so it is not a usable source here.
 *
 *
 * TOPOLOGIES
 * ==========
 *
 * The MT6589 routing fabric is a set of crossbar switches, not a fixed
 * pipeline.  Each processing engine has a "multiple output enable" (MOUT_EN)
 * register deciding which downstream engines it drives, and each consumer
 * has an "input selection" (*_SEL) register deciding which upstream engine
 * drives it.  A path exists only when both halves agree.  That is why the
 * same two engines can be chained in two different orders.
 *
 * The vendor ships two different trees and they build DIFFERENT chains.  This
 * is the single most important fact in this file, and it is why the driver
 * programs one of them rather than "the vendor path".
 *
 * 1. LK bootloader  (lk/ddp_path.c:225-252, disp_path_config())
 *
 *      OVL0 --[OVL_MOUT_EN=0x2]--> BLS --[direct link]--> RDMA0 --> DSI0
 *
 *    The overlay's multiple-output register selects bit 1 ("Output to BLS",
 *    data sheet draft/ds/ovl.txt:192783) and BLS's input select is 0 ("From
 *    overlay", draft/ds/ovl.txt:192276).  COLOR is not in the chain at all:
 *    OVL_MOUT_EN is written with 0x2, so bit 2 (Output to COLOR) stays 0 and
 *    COLOR_MOUT_EN is never touched.  Three engines deep from OVL to panel.
 *
 * 2. Vendor kernel BSP  (kernel/drivers/dispsys/ddp_path.c:854-936)
 *
 *      OVL0 --[OVL_MOUT_EN=0x4]--> COLOR --[COLOR_MOUT_EN=0x8]--> BLS
 *           --[direct link]--> RDMA0 --> DSI0
 *
 *    Here the overlay's output goes to bit 2 ("Output to COLOR") instead,
 *    COLOR drives bit 3 of its own MOUT_EN ("Output to BLS",
 *    draft/ds/ovl.txt:192825), and both input selects change with it:
 *    COLOR_SEL = 1 ("From overlay after alpha blending",
 *    draft/ds/ovl.txt:193199) and BLS_SEL = 1 ("From color engine",
 *    draft/ds/ovl.txt:193282).  Four engines deep.
 *
 *    The BSP also carries a longer variant, present but commented out in the
 *    source at ddp_path.c:860-867, 887-894 and 913-920:
 *
 *      OVL_PQ_MOUT --[TDSHP_MOUT_EN=0x10]--> TDSHP --[OVL direct link]--> OVL
 *      OVL_MOUT --[OVL_MOUT_EN=0x4]--> COLOR --[COLOR_MOUT_EN=0x8]--> BLS
 *
 *    with TDSHP_SEL = 0 ("From overlay before alpha blending").  That is the
 *    path 2D sharpness is designed for; documentation only here.
 *
 *
 * WHICH ONE THIS DRIVER PROGRAMS, AND WHY
 * =======================================
 *
 * This driver programs topology 1, OVL0 -> BLS -> RDMA0.  Three reasons:
 *
 *  - It is the chain the LK bootloader hands over with a live DSI link on
 *    this panel.  A path proven to drive this hardware from cold boot is the
 *    one to keep.
 *  - COLOR is kept in the path component list so its registers stay clocked
 *    and configured, but routing the signal through it is what previously
 *    stalled the overlay: OVL_RUN read back clear with the overlay's FME_UND
 *    set and the RDMA EOF aborts latched.  That is the signature of an
 *    upstream engine feeding a consumer that is not in the path and so never
 *    gets drained.  Not programming COLOR_MOUT_EN at all keeps that from
 *    happening, since OVL_MOUT_EN bit 2 is never set.
 *  - mtk_mmsys_ddp_connect() applies every matching route unconditionally at
 *    probe and again on every connect.  It has no notion of which engines the
 *    caller has actually clocked, so a route that points into an engine nobody
 *    enabled is worse than no route at all.
 *
 * Topology 2 is therefore documented in full below - all of its register
 * values are defined and commented - but deliberately NOT enabled as
 * MMSYS_ROUTE entries.  Turning it on would additionally require COLOR to be
 * on the path's component list ahead of BLS and to have its MOUT_RST and
 * clocking sorted out first; it is a change to the path description, not a
 * register value.
 *
 *
 * ROUTING REGISTER SUMMARY
 * ========================
 *
 * Widths and encodings are from the data sheet (draft/ds/ovl.txt, the
 * "DISP_*" bit tables at 192692-193350).  All of these registers reset to 0,
 * which matters: at reset every engine's outputs are disabled, so nothing
 * downstream is fed and nothing drains until a route is written.
 *
 *  Off  Register                Field(s)  Encoding
 *  ---  ----------------------  --------  ---------------------------------
 *  0x020 DISP_SCL_MOUT_EN       [3:0]     0:WDMA0  1:OVL  2:COLOR  3:TDSHP
 *  0x024 DISP_OVL_MOUT_EN       [3:0]     0:WDMA1  1:BLS   2:COLOR  3:TDSHP
 *  0x028 DISP_COLOR_MOUT_EN     [4:0]     0:OVL_PQ 1:TDSHP 2:WDMA0 3:BLS
 *                                         4:OVL direct link
 *  0x02c DISP_TDSHP_MOUT_EN     [4:0]     0:OVL_PQ 1:COLOR 2:WDMA0 3:BLS
 *                                         4:OVL direct link
 *  0x030 DISP_MOUT_RST          [3:0]     0:SCL 1:OVL 2:COLOR 3:TDSHP
 *  0x034 DISP_RDMA0_OUT_SEL     [1:0]     0:DSI0  1:DBI   2:DPI0
 *  0x038 DISP_RDMA1_OUT_SEL     [1:0]     0:DBI   1:DPI0  2:DPI1
 *  0x03c DISP_OVL_PQ_OUT_SEL    [0]       0:COLOR  1:TDSHP
 *  0x040 DISP_WDMA0_SEL         [1:0]     0:SCL   1:COLOR 2:TDSHP
 *  0x044 DISP_OVL_SEL           [1:0]     0:SCL   1:COLOR 2:TDSHP
 *  0x048 DISP_OVL_PQ_IN_SEL     [0]       0:COLOR  1:TDSHP
 *  0x04c DISP_COLOR_SEL        [1:0]     0:OVL before blend  1:OVL after
 *                                                   2:TDSHP  3:SCL
 *  0x050 DISP_TDSHP_SEL        [1:0]     0:OVL before blend  1:OVL after
 *                                                   2:COLOR  3:SCL
 *  0x054 DISP_BLS_SEL          [1:0]     0:OVL   1:COLOR 2:TDSHP
 *  0x058 DISP_DBI_SEL          [0]       0:RDMA0  1:RDMA1
 *  0x05c DISP_DPI0_SEL         [0]       0:RDMA0  1:RDMA1
 *
 * 0x030 MOUT_RST, 0x03c OVL_PQ_OUT_SEL and 0x044 OVL_SEL are written by no
 * vendor path code in either tree, but they are documented above and given
 * defines so the whole block is described rather than half of it.
 *
 *
 * WHAT IS LIVE AND WHAT IS DOCUMENTATION ONLY
 * ===========================================
 *
 * Live - written by mt6589_dispsys_routing_table[]:
 *   OVL_MOUT_EN[1] BLS, OVL_MOUT_EN[0] WDMA1, BLS_SEL = 0, COLOR_SEL = 1,
 *   RDMA0_OUT_SEL, RDMA1_OUT_SEL, DPI0_SEL = 1, SCL_MOUT_EN[0].
 *
 * Defined but never written - no route touches them:
 *   SCL_MOUT_EN[1:3], OVL_MOUT_EN[2:3], COLOR_MOUT_EN, TDSHP_MOUT_EN,
 *   WDMA0_SEL, OVL_SEL, OVL_PQ_IN_SEL, TDSHP_SEL, BLS_SEL = 1,
 *   BLS_SEL[1], DBI_SEL.
 *
 * Note the asymmetry that trips everyone up: the *_SEL registers are 2-bit
 * fields in hardware, but the only encodings any of these engines ever uses
 * are 0 and 1, so the meaningful write mask is a single bit for every one of
 * them except COLOR_SEL - where the BSP's "1" is likewise just bit 0.  The
 * full field width is named *_WIDTH above; the routing table writes
 * single-bit masks so no reserved encoding is ever disturbed.
 */

/*
 * DISPSYS software reset.  The register is the active-low SW_RST_B form
 * shared with the other MediaTek MMSYS blocks: a 0 bit holds its component
 * in reset and a 1 releases it, and the register resets to all ones
 * (data sheet draft/ds/ovl.txt:195002, DISP_SW_RST_B reset FFFFFFFF).  That
 * is the same polarity mtk_mmsys_reset_update() drives by default, so no
 * per-SoC override is needed.  Bit assignments are in
 * include/dt-bindings/reset/mt6589-resets.h.
 */
#define MT6589_DISP_SW_RST_B			0x140
#define MT6589_DISP_NUM_RESETS			21

/*
 * DISPSYS_CONFIG routing register offsets, in ascending address order.
 *
 * Names match the data sheet register names and the vendor DISP_REG_CONFIG_*
 * names one-for-one; the offsets match both (draft/ds/ovl.txt:192527-192598,
 * ddp_reg.h:111-126).
 */
#define MT6589_DISP_SCL_MOUT_EN		0x020		/* scaler outputs	 */
#define MT6589_DISP_OVL_MOUT_EN		0x024		/* overlay outputs	 */
#define MT6589_DISP_COLOR_MOUT_EN	0x028		/* color engine outputs */
#define MT6589_DISP_TDSHP_MOUT_EN	0x02c		/* 2D sharpness outputs */
#define MT6589_DISP_MOUT_RST		0x030		/* per-engine MOUT reset */
#define MT6589_DISP_RDMA0_OUT_SEL	0x034		/* RDMA0 output pick	 */
#define MT6589_DISP_RDMA1_OUT_SEL	0x038		/* RDMA1 output pick	 */
#define MT6589_DISP_OVL_PQ_OUT_SEL	0x03c		/* OVL PQ output pick	 */
#define MT6589_DISP_WDMA0_SEL		0x040		/* WDMA0 input pick	 */
#define MT6589_DISP_OVL_SEL		0x044		/* OVL input pick	 */
#define MT6589_DISP_OVL_PQ_IN_SEL	0x048		/* OVL PQ input pick	 */
#define MT6589_DISP_COLOR_SEL		0x04c		/* color engine in pick	 */
#define MT6589_DISP_TDSHP_SEL		0x050		/* 2D sharpness in pick	 */
#define MT6589_DISP_BLS_SEL		0x054		/* BLS input pick	 */
#define MT6589_DISP_DBI_SEL		0x058		/* DBI input pick	 */
#define MT6589_DISP_DPI0_SEL		0x05c		/* DPI0 input pick	 */

/*
 * DISP_SCL_MOUT_EN, bits [3:0], data sheet draft/ds/ovl.txt:192740-192750.
 * A 1 enables that output port; 0 disables it.  Bit 0 is the only one any
 * path here uses (the MDP SCL -> WDMA0 route).
 */
#define MT6589_SCL_MOUT_EN_WIDTH		GENMASK(3, 0)
#define MT6589_SCL_MOUT_EN_WDMA0		BIT(0)
#define MT6589_SCL_MOUT_EN_OVL			BIT(1)
#define MT6589_SCL_MOUT_EN_COLOR		BIT(2)
#define MT6589_SCL_MOUT_EN_TDSHP		BIT(3)
#define MT6589_SCL_MOUT_EN_WDMA0_MASK		BIT(0)

/*
 * DISP_OVL_MOUT_EN, bits [3:0], data sheet draft/ds/ovl.txt:192781-192790.
 *
 * This is the register that decides the topology.  The bootloader writes
 * 0x2 (bit 1, BLS) and never sets bit 2; the vendor kernel BSP writes 0x4
 * (bit 2, COLOR) and relies on COLOR to reach BLS.  Because both bits can be
 * set at once, OVL can genuinely fan out to BLS and COLOR simultaneously -
 * bit 1 plus bit 2 is a valid hardware state even though neither vendor
 * tree uses it.
 */
#define MT6589_OVL_MOUT_EN_WIDTH		GENMASK(3, 0)
#define MT6589_OVL_MOUT_EN_WDMA1		BIT(0)
#define MT6589_OVL_MOUT_EN_BLS			BIT(1)
#define MT6589_OVL_MOUT_EN_COLOR		BIT(2)
#define MT6589_OVL_MOUT_EN_TDSHP		BIT(3)
#define MT6589_OVL_MOUT_EN_WDMA1_MASK		BIT(0)
#define MT6589_OVL_MOUT_EN_BLS_MASK		BIT(1)
#define MT6589_OVL_MOUT_EN_COLOR_MASK		BIT(2)

/*
 * DISP_COLOR_MOUT_EN, bits [4:0], data sheet draft/ds/ovl.txt:192821-192831.
 * Bit 3 is "Output to BLS" and is what the vendor kernel BSP writes as 0x8
 * to build topology 2.  Not programmed here: with OVL_MOUT_EN[2] clear,
 * COLOR has no input, and enabling an output from an engine with no input is
 * the case that stalled the overlay.
 *
 * Bit 4 is "Output to OVL direct link input (OVL_SEL)", the consumer
 * referenced by DISP_OVL_SEL.
 */
#define MT6589_COLOR_MOUT_EN_WIDTH		GENMASK(4, 0)
#define MT6589_COLOR_MOUT_EN_OVL_PQ		BIT(0)
#define MT6589_COLOR_MOUT_EN_TDSHP		BIT(1)
#define MT6589_COLOR_MOUT_EN_WDMA0		BIT(2)
#define MT6589_COLOR_MOUT_EN_BLS		BIT(3)
#define MT6589_COLOR_MOUT_EN_OVL_DIRECT		BIT(4)
#define MT6589_COLOR_MOUT_EN_BLS_MASK		BIT(3)

/*
 * DISP_TDSHP_MOUT_EN, bits [4:0], data sheet draft/ds/ovl.txt:192882-192892.
 * Same port list and same width as COLOR_MOUT_EN - the two engines are
 * interchangeable siblings on the fabric.
 *
 * The vendor kernel BSP writes 0x10 (bit 4, OVL direct link) only inside its
 * commented-out longer chain at ddp_path.c:862, never in the chain it
 * actually builds.  So in live vendor code this register is never written at
 * all: 2D sharpness is clocked and reset but not routed.
 */
#define MT6589_TDSHP_MOUT_EN_WIDTH		GENMASK(4, 0)
#define MT6589_TDSHP_MOUT_EN_OVL_PQ		BIT(0)
#define MT6589_TDSHP_MOUT_EN_COLOR		BIT(1)
#define MT6589_TDSHP_MOUT_EN_WDMA0		BIT(2)
#define MT6589_TDSHP_MOUT_EN_BLS		BIT(3)
#define MT6589_TDSHP_MOUT_EN_OVL_DIRECT		BIT(4)
#define MT6589_TDSHP_MOUT_EN_OVL_DIRECT_MASK	BIT(4)

/*
 * DISP_MOUT_RST, bits [3:0], data sheet draft/ds/ovl.txt:192928-192936.
 * One resets the corresponding engine's multiple-output state; a 1 resets,
 * so the reset value 0 means "nothing held in MOUT reset".  No vendor path
 * code touches it and neither does this driver - it exists to break a stuck
 * fan-out, which is not a configuration any path needs at probe.  Defined for
 * completeness of the block, not because it is programmed.
 */
#define MT6589_MOUT_RST_WIDTH			GENMASK(3, 0)
#define MT6589_MOUT_RST_SCL			BIT(0)
#define MT6589_MOUT_RST_OVL			BIT(1)
#define MT6589_MOUT_RST_COLOR			BIT(2)
#define MT6589_MOUT_RST_TDSHP			BIT(3)

/*
 * DISP_RDMA0_OUT_SEL, bits [1:0], data sheet draft/ds/ovl.txt:192963-192967.
 * Encodings 0 = DSI, 1 = DBI, 2 = DPI0, others reserved.  The data sheet says
 * "DSI" and this block has a single DSI output, so 0 is DSI0.
 */
#define MT6589_RDMA0_SOUT_WIDTH			GENMASK(1, 0)
#define MT6589_RDMA0_SOUT_DSI0			0x0
#define MT6589_RDMA0_SOUT_DBI			0x1
#define MT6589_RDMA0_SOUT_DPI0			0x2
#define MT6589_RDMA0_SOUT_MASK			GENMASK(1, 0)

/*
 * DISP_RDMA1_OUT_SEL, bits [1:0], data sheet draft/ds/ovl.txt:193011-193020.
 *
 * Encodings 0 = DBI, 1 = DPI0, 2 = DPI1, others reserved.
 *
 * This is the one field where the data sheet and the vendor source have to
 * be read together.  The data sheet's description line says "Selects RDMA 0
 * output" and labels encoding 0 "Output to DBI" - the same sentence appears
 * under both OUT_SEL registers and only the 1 and 2 encodings differ.  What
 * makes 0 = DBI credible rather than a copy-paste slip is that DISP_DBI_SEL
 * is a two-way switch between RDMA0 and RDMA1
 * (draft/ds/ovl.txt:193309-193316): RDMA1 reaches DBI only through this
 * field, and a zero-reset register that routed nowhere would make the reset
 * state dead.  RDMA1 has no DSI output on this block at all, so "0 = DSI0",
 * which was the open question, is not an available encoding.
 *
 * No vendor path code ever writes 0 here; both callers use 1 or 2
 * (ddp_path.c:875 for DPI0, :903 for DPI1).  The define exists for
 * completeness, not because anything programs it.
 */
#define MT6589_RDMA1_SOUT_WIDTH			GENMASK(1, 0)
#define MT6589_RDMA1_SOUT_DBI			0x0
#define MT6589_RDMA1_SOUT_DPI0			0x1
#define MT6589_RDMA1_SOUT_DPI1			0x2
#define MT6589_RDMA1_SOUT_MASK			GENMASK(1, 0)

/*
 * DISP_OVL_PQ_OUT_SEL, bit [0], data sheet draft/ds/ovl.txt:193053-193056.
 * The "pixel quality" overlay output, i.e. the feed that exists to be
 * sharpened: 0 = from color engine, 1 = from 2D sharpness.  Only meaningful
 * when OVL_PQ_IN_SEL selects TDSHP.  Not written by any vendor path or by
 * this driver.
 */
#define MT6589_OVL_PQ_OUT_SEL_WIDTH		GENMASK(0, 0)
#define MT6589_OVL_PQ_OUT_SEL_COLOR		0x0
#define MT6589_OVL_PQ_OUT_SEL_TDSHP		0x1

/*
 * DISP_WDMA0_SEL, bits [1:0], data sheet draft/ds/ovl.txt:193085-193088.
 * Encodings 0 = from scaler, 1 = from color engine, 2 = from 2D sharpness,
 * others reserved.
 *
 * The vendor kernel BSP writes 0 for the SCL -> WDMA0 MDP route
 * (ddp_path.c:930, "0 for SCL").  That write is not needed here: the register
 * resets to 0, and DISP_SCL_MOUT_EN[0] - which the routing table does write -
 * is what actually connects SCL to WDMA0.  Left at reset, WDMA0's input is
 * already the scaler.
 */
#define MT6589_WDMA0_SEL_WIDTH			GENMASK(1, 0)
#define MT6589_WDMA0_SEL_SCL			0x0
#define MT6589_WDMA0_SEL_COLOR			0x1
#define MT6589_WDMA0_SEL_TDSHP			0x2
#define MT6589_WDMA0_SEL_MASK			GENMASK(1, 0)

/*
 * DISP_OVL_SEL, bits [1:0], data sheet draft/ds/ovl.txt:193114-193141.
 * Encodings 0 = from scaler, 1 = from color engine, 2 = from 2D sharpness.
 * The overlay's own input; its consumer side is OVL_MOUT_EN[3] and
 * TDSHP_MOUT_EN[4] / COLOR_MOUT_EN[4] ("OVL direct link input").
 *
 * Not written by any vendor path or by this driver.  At reset it reads 0,
 * i.e. the overlay's input is the scaler; the paths programmed below all
 * start at the overlay, so they do not select an input for it.
 */
#define MT6589_OVL_SEL_WIDTH			GENMASK(1, 0)
#define MT6589_OVL_SEL_SCL			0x0
#define MT6589_OVL_SEL_COLOR			0x1
#define MT6589_OVL_SEL_TDSHP			0x2
#define MT6589_OVL_SEL_MASK			GENMASK(1, 0)

/*
 * DISP_OVL_PQ_IN_SEL, bit [0], data sheet draft/ds/ovl.txt:193168-193171.
 * Encodings 0 = from color engine, 1 = from 2D sharpness.  Selects which
 * engine drives the overlay's pixel-quality input.  Only meaningful together
 * with OVL_PQ_OUT_SEL; not written by any vendor path or by this driver.
 *
 * An earlier revision of this file documented 0 as "from overlay" and 1 as
 * "from 2D sharpness".  The overlay feed is on the COLOR side of that switch,
 * per the data sheet's own "from color engine" wording and the OVL_PQ_MOUT
 * naming used in the COLOR_SEL and TDSHP_SEL tables; "from overlay" was
 * wrong and is corrected here.
 */
#define MT6589_OVL_PQ_IN_SEL_WIDTH		GENMASK(0, 0)
#define MT6589_OVL_PQ_IN_SEL_COLOR		0x0
#define MT6589_OVL_PQ_IN_SEL_TDSHP		0x1
#define MT6589_OVL_PQ_IN_SEL_MASK		GENMASK(0, 0)

/*
 * DISP_COLOR_SEL, bits [1:0], data sheet draft/ds/ovl.txt:193198-193201.
 * Encodings 0 = from overlay before alpha blending (OVL_PQ_MOUT),
 * 1 = from overlay after alpha blending (OVL_MOUT), 2 = from 2D sharpness,
 * 3 = from scaler.  All four are defined; the vendor BSP uses 1.
 *
 * The routing table writes COLOR_SEL = 1 even though COLOR is off the signal
 * path.  That is harmless and intentional: OVL_MOUT_EN[2] is what gates
 * COLOR's input, and this keeps the register from holding a stale bootloader
 * value.  It is also exactly the value topology 2 needs, so enabling that
 * chain later is a one-entry change here plus a change to the path component
 * list.
 */
#define MT6589_COLOR_SEL_WIDTH			GENMASK(1, 0)
#define MT6589_COLOR_SEL_OVL_PRE_BLEND		0x0	/* from OVL_PQ_MOUT */
#define MT6589_COLOR_SEL_OVL_POST_BLEND		0x1	/* from OVL_MOUT    */
#define MT6589_COLOR_SEL_TDSHP			0x2
#define MT6589_COLOR_SEL_SCL			0x3
/*
 * The write mask is the whole two-bit field, not just bit 0.  The field is
 * documented [1:0] with four encodings, so writing only bit 0 leaves the
 * upper bit at whatever the bootloader left there - and 2 and 3 select the
 * 2D sharpness engine and the scaler, neither of which is in this path.  A
 * one-bit mask would therefore let a stale upper bit steer the colour engine
 * away from the overlay while still appearing to program it correctly.
 */
#define MT6589_COLOR_SEL_MASK			MT6589_COLOR_SEL_WIDTH

/*
 * DISP_TDSHP_SEL, bits [1:0], data sheet draft/ds/ovl.txt:193220-193228.
 * Same four encodings as COLOR_SEL: 0 = from overlay before alpha blending,
 * 1 = from overlay after alpha blending, 2 = from color engine, 3 = from
 * scaler.  The two engines sit at different points in the chain, so the order
 * of the upper half differs - COLOR_SEL 2 is sharpness, TDSHP_SEL 2 is color -
 * even though both share the same width.
 *
 * The vendor kernel BSP writes 0 ("tdshp_sel from overlay before blending")
 * only in its commented-out longer chain (ddp_path.c:865, :892, :918).  Not
 * written here.
 *
 * An earlier revision of this file treated this as a 1-bit field.  The data
 * sheet bit table says [1:0] and lists four encodings, so it is 2 bits.
 */
#define MT6589_TDSHP_SEL_WIDTH			GENMASK(1, 0)
#define MT6589_TDSHP_SEL_OVL_PRE_BLEND		0x0	/* from OVL_PQ_MOUT */
#define MT6589_TDSHP_SEL_OVL_POST_BLEND		0x1	/* from OVL_MOUT    */
#define MT6589_TDSHP_SEL_COLOR			0x2
#define MT6589_TDSHP_SEL_SCL			0x3
#define MT6589_TDSHP_SEL_MASK			GENMASK(0, 0)

/*
 * DISP_BLS_SEL, bits [1:0], data sheet draft/ds/ovl.txt:193276-193284.
 * Encodings 0 = from overlay, 1 = from color engine, 2 = from 2D sharpness,
 * others reserved.
 *
 * 0 is the live value - topology 1, overlay straight into BLS.  The BSP's 1
 * is topology 2.  Encoding 2 is documented here but has no vendor path code
 * behind it.
 */
#define MT6589_BLS_SEL_WIDTH			GENMASK(1, 0)
#define MT6589_BLS_SEL_OVL			0x0
#define MT6589_BLS_SEL_COLOR			0x1
#define MT6589_BLS_SEL_TDSHP			0x2
/*
 * Whole two-bit field, not just bit 0: the data sheet encodes 0 = overlay,
 * 1 = colour engine, 2 = 2D sharpness (draft/ds/ovl.txt:193276-193284).  A
 * one-bit write mask would leave a stale bit 1 selecting the colour engine
 * when only the overlay is wanted.
 */
#define MT6589_BLS_SEL_MASK			MT6589_BLS_SEL_WIDTH

/*
 * DISP_DBI_SEL, bit [0], data sheet draft/ds/ovl.txt:193309-193316.
 * Encodings 0 = from RDMA0, 1 = from RDMA1.
 *
 * The data sheet calls 1 "From RDMA1", not "From 2D sharpness"; an earlier
 * revision of this file had the latter, which was wrong.  Both vendor trees
 * write 0 (lk/ddp_path.c:246, kernel ddp_path.c:924).
 *
 * Not in the routing table.  The live panel path is DSI0 and DBI is not
 * clocked, so there is nothing to route.
 */
#define MT6589_DBI_SEL_WIDTH			GENMASK(0, 0)
#define MT6589_DBI_SEL_RDMA0			0x0
#define MT6589_DBI_SEL_RDMA1			0x1
#define MT6589_DBI_SEL_MASK			GENMASK(0, 0)

/*
 * DISP_DPI0_SEL, bit [0], data sheet draft/ds/ovl.txt:193342-193344.
 * Encodings 0 = from RDMA0, 1 = from RDMA1.  This is the switch that makes
 * DPI0 either a second OVL-fed output or the target of RDMA1's bypass path.
 */
#define MT6589_DPI0_SEL_WIDTH			GENMASK(0, 0)
#define MT6589_DPI0_SEL_RDMA0			0x0
#define MT6589_DPI0_SEL_RDMA1			0x1
#define MT6589_DPI0_SEL_MASK			GENMASK(0, 0)

/*
 * Live routing table.
 *
 * Everything above is documentation.  This is what
 * mt6589_dispsys_ddp_driver_data.routes points at, and
 * mtk_mmsys_ddp_connect() writes every matching entry unconditionally -
 * there is no "only if this engine is enabled" test - so an entry naming an
 * engine that is not clocked is a live route into nothing.
 *
 * Notation: the selects and _MASKs below are single-bit read-modify-write
 * masks.  The *_WIDTH constants above name the full field for anyone who
 * wants the whole crossbar; they are deliberately not what the writes use, so
 * a reserved encoding is never disturbed.
 */
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
		    MT6589_OVL_MOUT_EN_BLS_MASK, MT6589_OVL_MOUT_EN_BLS),
	MMSYS_ROUTE(OVL0, BLS,
		    MT6589_DISP_BLS_SEL,
		    MT6589_BLS_SEL_MASK, MT6589_BLS_SEL_OVL),

	/*
	 * LIVE but off the signal path: COLOR's input select.
	 *
	 * COLOR is kept in the path component list so its registers are still
	 * clocked and configured, but it is not on the signal route.
	 * COLOR_SEL = 1 is the vendor BSP's topology-2 value ("from overlay
	 * after alpha blending", kernel ddp_path.c:857); it is written here
	 * only so the register does not keep a stale bootloader value.  It
	 * has no effect while OVL_MOUT_EN[2] is 0, because that bit - not
	 * this register - is what gives COLOR an input.
	 */
	MMSYS_ROUTE(OVL0, COLOR0,
		    MT6589_DISP_COLOR_SEL,
		    MT6589_COLOR_SEL_MASK, MT6589_COLOR_SEL_OVL_POST_BLEND),

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
		    MT6589_RDMA0_SOUT_MASK, MT6589_RDMA0_SOUT_DSI0),

	/* RDMA0 -> DBI */
	MMSYS_ROUTE(RDMA0, DBI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_SOUT_MASK, MT6589_RDMA0_SOUT_DBI),

	/* RDMA0 -> DPI0 (OVL-sourced) */
	MMSYS_ROUTE(RDMA0, DPI0,
		    MT6589_DISP_RDMA0_OUT_SEL,
		    MT6589_RDMA0_SOUT_MASK, MT6589_RDMA0_SOUT_DPI0),

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
		    MT6589_OVL_MOUT_EN_WDMA1_MASK, MT6589_OVL_MOUT_EN_WDMA1),

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
		    MT6589_RDMA1_SOUT_MASK, MT6589_RDMA1_SOUT_DPI0),
	MMSYS_ROUTE(RDMA1, DPI0,
		    MT6589_DISP_DPI0_SEL,
		    MT6589_DPI0_SEL_MASK, MT6589_DPI0_SEL_RDMA1),

	/* RDMA1 -> DPI1 */
	MMSYS_ROUTE(RDMA1,  DPI1,
		    MT6589_DISP_RDMA1_OUT_SEL,
		    MT6589_RDMA1_SOUT_MASK, MT6589_RDMA1_SOUT_DPI1),

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
		    MT6589_SCL_MOUT_EN_WDMA0_MASK, MT6589_SCL_MOUT_EN_WDMA0),

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
