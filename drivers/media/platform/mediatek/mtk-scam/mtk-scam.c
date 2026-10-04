// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 sensor-CAM (SCAM) adaptor
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * The SCAM blocks sit between each CSI-2 receiver and the CAM/ISP.  They
 * take the unpacked byte stream plus the hsync/vsync the receiver produces,
 * convert it to the ISP's line-based handshake, and report frame and line
 * interrupts to the ISP.
 *
 * Register map from the MT6589 data sheet, module "SCAM", base 0x15008200,
 * with a 0x80 stride between the two instances:
 *
 *   +0x00 CFG    configuration (reset 0x10000400)
 *   +0x04 CON    reset and enable
 *   +0x0c INT    interrupt status
 *   +0x10 SIZE   frame size
 *   +0x20 CFG2   second configuration
 *   +0x30 INFO0  information
 *   +0x34 INFO1  information
 *   +0x40 STA    status counters
 *
 * This driver is PARTIAL.  The register offsets and the CFG bit fields are
 * transcribed from the data sheet, and the enable/reset/size/frame-
 * interrupt path is implemented.  The parts that need to be finished are
 * called out explicitly below:
 *
 *   1. There is no vendor SCAM driver for the MIPI camera path in the
 *      reference tree and no way to test, so the data-type and pixel-format
 *      handling is not written: CFG.WARN_MASK, CFG.CSD_NUM, CFG.DBG_MD and
 *      all of CFG2 have documented bit positions but no known-good
 *      programming sequence.  (CFG and CON *are* fully documented and are
 *      used below; see the comments on those defines.)
 *   2. The media graph is wired as far as the blocks that exist: this
 *      subdev has a real sink and source pad (see SCAM_PAD_* in mtk-scam.h)
 *      and initialises them in probe, so the receiver -> SCAM -> CAM links
 *      resolve.  What is still missing is the frame notifier and any
 *      propagation of a format negotiated upstream; the CAM/ISP side
 *      consumes the stream without doing anything with it yet.  See
 *      README.md.
 *   3. This tree's media API is a reduced fork, so the usual
 *      v4l2_mbus_csi2_capability negotiation is unavailable; the frame
 *      format is taken from the sensor through set_pad() instead.  The
 *      CSI-2 receiver upstream (mtk-csi2-rx.c) negotiates the code and
 *      passes the geometry down to here through this driver's own state.
 *      The pad ops do implement enum_mbus_code()/enum_frame_size() and
 *      init_state(), so userspace can enumerate the (single) code and the
 *      geometry range rather than being told -ENOIOCTLCMD.
 *
 * As with the D-PHY and the receiver, treat this as untested scaffolding:
 * it will probe and it will enable, and it will not yet produce frames.
 */

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <media/media-entity.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "mtk-scam.h"

/* Register offsets, relative to the start of this port's SCAM block. */
#define SCAM_CFG				0x00
#define SCAM_CON				0x04
#define SCAM_INT				0x0c
#define SCAM_SIZE				0x10
#define SCAM_CFG2				0x20
#define SCAM_INFO0				0x30
#define SCAM_INFO1				0x34
#define SCAM_STA				0x40

/*
 * Distance between SCAM1 (0x15008200) and SCAM2 (0x15008280).  Informational
 * only: each port has its own DT node and its own mapping, so the driver never
 * computes an offset with it (see scam_ofs()).
 */
#define SCAM_PORT_STRIDE			0x80

/*
 * SCAM_CFG (0x15008200), reset value 0x10000400.
 *
 * Every position below is taken from the MT6589 data sheet's SCAM1_CFG bit
 * table (chapter "MIPI RX Configuration Module", page 2268 of 2519;
 * draft/ds/mipi.txt:6727-6747 for the two bit rows and 6748-6810 for the
 * field descriptions) and independently confirmed against the vendor tree's
 * REG_SCAM1_CFG bitfield union,
 *
 *	mediatek/platform/mt6589/hardware/mtkcam/core/drv/imgsensor/
 *		seninf_reg.h:847-873
 *
 * whose LSB-first field list gives the same positions for every named bit:
 * INTEN0..6 = bits 0..6, Cycle = bits 10:8, Clock_inverse = bit 12,
 * Continuous_mode = bit 17, Debug_mode = bit 20, CSD_NUM = bits 25:24,
 * Warning_mask = bit 28.  The datasheet and the vendor agree on every field;
 * there is no conflict to reconcile here.
 *
 * The numbering is full of gaps, and getting one wrong moves a field onto a
 * neighbour or onto nothing at all.  Notably CYC is bits 10:8 (a 3-bit
 * field, not a single bit) and DBG_MD is bit 20, not bit 23 or 24.
 */
#define SCAM_CFG_WARN_MASK			BIT(28)
#define SCAM_CFG_CSD_NUM			GENMASK(25, 24)
#define SCAM_CFG_DBG_MD				BIT(20)
#define SCAM_CFG_CONT				BIT(17)
#define SCAM_CFG_CLK_INV			BIT(12)
#define SCAM_CFG_CYC				GENMASK(10, 8)
#define SCAM_CFG_INTEN				GENMASK(6, 0)

/*
 * Documented SCAM_CFG reset value: 0x10000400.
 *
 * It decomposes as exactly two documented fields, and nothing else:
 *
 *	0x10000400 = 0x10000000 | 0x00000400
 *	           = BIT(28)     | (4 << 8)
 *	           = WARN_MASK=1 | CYC=4 (bits 10:8)
 *
 * Cross-checked against the data sheet's per-bit reset row for SCAM1_CFG
 * (draft/ds/mipi.txt:6745 and 6752): bit 28 WARN_MASK resets to 1, bit 12
 * CLK_INV to 0, bits 10:8 CYC to 100b, and every other named bit to 0.
 * So the reset value is not a magic number to be preserved by accident -- it
 * *is* "WARN_MASK=1, CYC=4", and those are the two bits this driver must not
 * lose.
 *
 * The vendor driver programs the same CYC explicitly rather than relying on
 * reset: seninf_drv.cpp:1374, SENINF_WRITE_BITS(pSeninf, SCAM1_CFG, Cycle,
 * 4), with the single call site sensor_hal.cpp:1144 passing
 * setTg1Serial(clk_inv, 320, 240, conti_mode=1, csd_num=0).  That is
 * independent confirmation that CYC=4 is the intended operating point.
 */
#define SCAM_CFG_RESET				0x10000400

/*
 * SCAM_CFG value this driver programs in mtk_scam_start_stream(), spelled as
 * a build-up from the documented reset state rather than as a mask against
 * it.
 *
 * This expression used to read
 *
 *	(SCAM_CFG_RESET & ~SCAM_CFG_PRESERVE) | SCAM_CFG_CONT
 *
 * which is arithmetically self-defeating: SCAM_CFG_PRESERVE was
 * WARN_MASK|CLK_INV|CYC == 0x10001700, a *superset* of everything
 * SCAM_CFG_RESET sets, so SCAM_CFG_RESET & ~SCAM_CFG_PRESERVE == 0 and the
 * block was actually programmed with CONT alone (0x00020000).  The comment
 * claimed the mask was "keeping the enables", but it kept nothing -- it
 * discarded the reset configuration, WARN_MASK=1 and CYC=4 both, while
 * looking like it was protecting them.
 *
 * Building up from the reset value cannot fail that way: OR-ing more bits
 * into a nonzero base can only add bits, never clear the base.  Each field
 * below is a named constant, so what the driver sets is readable without
 * doing hex arithmetic, and a field added here cannot silently collide with
 * one already in SCAM_CFG_RESET the way a complement-and-mask could.
 *
 * What this driver keeps from reset, and the two fields it adds on top:
 *
 *   WARN_MASK - left at its reset value of 1 (inherited from SCAM_CFG_RESET,
 *	           not added below): "warnings before the 1st frame start
 *	           packet will not be recorded", i.e. suppress startup
 *	           warnings.  That is the documented reset behaviour and this
 *	           driver has no evidence to change it.
 *   CYC=4     - likewise inherited; see the reset decomposition above.
 *   INTEN0..6 - enabled here.  Out of reset they are all 0, which would
 *	           leave SCAM_INT raising no interrupt at all, while this
 *	           driver clears SCAM_INT on every stream start as if the
 *	           interrupts were live.  Enabling the whole documented set
 *	           (frame end, CRC error, wrong sync header / packet ID /
 *	           line ID / data ID / size) is what makes that clearing
 *	           meaningful and gives the ISP-visible block its diagnostics.
 *	           Per-interrupt routing to a real ISP IRQ is not written
 *	           yet, so there is nothing finer-grained to select here.
 *   CONT=1    - continuous mode.  SCAM's job here is to run for as long as
 *	           streaming is on, and there is no stop-frame mechanism in
 *	           this driver, so single-run mode would capture one frame and
 *	           then sit idle.  The vendor agrees for its own SCAM use:
 *	           sensor_hal.cpp:1144 passes conti_mode=1.
 *   CLK_INV   - left at 0 from the reset value.  It inverts the latch for
 *	           the sensor's serial clock and is a property of the sensor
 *	           wiring, not of this block; the vendor takes it from
 *	           sensorInfo[0].SensorClockPolarity.  Nothing in this tree
 *	           carries that information, so the reset value stands and a
 *	           sensor needing the other polarity needs a DT property.
 *
 * Result: 0x10000400 | 0x0000007f | 0x00020000 == 0x1002047f.  Note that
 * INTEN is GENMASK(6, 0), i.e. bits 6..0 only: it contributes 0x7f and does
 * not reach bit 8, which is the low bit of CYC.  OR-ing in the interrupt
 * enables therefore cannot disturb CYC, which is checked below rather than
 * assumed.
 */
#define SCAM_CFG_INTEN_ALL			SCAM_CFG_INTEN

/*
 * CYC = 4 as a plain constant.  FIELD_PREP() would say it more obviously, but
 * it expands to a statement expression and so cannot be used in a file-scope
 * static_assert(); the shift and mask are spelled out to keep the assertions
 * below constant expressions.
 */
#define SCAM_CFG_CYC_4				(4 << 8)

#define SCAM_CFG_START				(SCAM_CFG_RESET |		\
						 SCAM_CFG_INTEN_ALL |	\
						 SCAM_CFG_CONT)

/*
 * Build-time proof of the decomposition recorded above: SCAM_CFG_RESET is
 * exactly WARN_MASK=1 | CYC=4 and nothing else.  Exact equality (not a subset
 * test) is deliberate -- it also fails if the reset value ever grows a third
 * set bit, which would mean this driver is programming a configuration other
 * than the documented one.  If anyone edits SCAM_CFG_RESET or moves WARN_MASK
 * or CYC, this stops the build instead of letting the driver quietly program a
 * CYC the vendor never validated.
 */
static_assert(SCAM_CFG_RESET == (SCAM_CFG_WARN_MASK | SCAM_CFG_CYC_4));

/*
 * The other documented CFG fields really are clear out of reset, which is what
 * makes them this driver's to choose: CONT and CLK_INV are 0, CSD_NUM and
 * DBG_MD are 0.  Checked separately rather than folded into the equality above
 * because GENMASK() expands to arithmetic that cannot appear inside the
 * constant expression above on this compiler.
 */
static_assert((SCAM_CFG_RESET &
	       (SCAM_CFG_CONT | SCAM_CFG_CLK_INV | SCAM_CFG_CSD_NUM |
		SCAM_CFG_DBG_MD | SCAM_CFG_INTEN)) == 0);

/*
 * The value mtk_scam_start_stream() writes must never lose the reset bits.
 * This is the exact defect that was fixed above: the old mask arithmetic
 * evaluated to a word with WARN_MASK and CYC cleared.  Keep the assertion so a
 * future edit that reintroduces complement-and-mask arithmetic fails loudly.
 */
static_assert((SCAM_CFG_START & SCAM_CFG_RESET) == SCAM_CFG_RESET);
static_assert((SCAM_CFG_START & (SCAM_CFG_WARN_MASK | SCAM_CFG_CYC)) ==
	      (SCAM_CFG_WARN_MASK | SCAM_CFG_CYC_4));
static_assert(SCAM_CFG_START == 0x1002047f);

/*
 * SCAM_CON is documented as "reset and enable" (MT6589 data sheet page 2269,
 * SCAM1_CON, reset value 0x00000000) and its two fields are:
 *
 *   16  RST  RW  Reset.  Writing "1" stops SCAM immediately and keeps it in
 *               reset.  Write 0 to return to the normal state.  The
 *               software reset does NOT reset all register settings.
 *    0  ENA  RW  Enable.  Writing "1" starts SCAM.  The data sheet is
 *               explicit about the order: "Be sure to trigger SCAM first
 *               before triggering the image sensor and to clear RST before
 *               setting ENA = 1."
 *
 * So the earlier assumption of SOFTRST at bit 0 and EN at bit 1 was wrong --
 * EN is bit 0 and RST is bit 16.
 *
 * Both positions are confirmed by the vendor REG_SCAM1_CON bitfield union,
 * seninf_reg.h:880-890 (Enable:1 then Reset:1 after 15 reserved bits, i.e.
 * bit 0 and bit 16).
 */
#define SCAM_CON_RST				BIT(16)
#define SCAM_CON_ENA				BIT(0)

/*
 * SCAM_SIZE (0x15008210 for port 0): frame width and height in pixels.
 *
 * The data sheet (page 2269, SCAM1_SIZE) gives the fields as 27:16 HEIGHT
 * and 11:0 WIDTH -- 12 bits each, not a 16/16 split, with the two gaps at
 * 31:28 and 15:12 unnamed.
 *
 * The vendor tree agrees and pins down the ordering: seninf_reg.h:907-917
 * declares
 *
 *	FIELD WIDTH  : 12;
 *	FIELD rsv_12 : 4;
 *	FIELD HEIGHT : 12;
 *	FIELD rsv_28 : 4;
 *
 * and seninf_drv.cpp:1354-1369 drives those fields via
 * SENINF_WRITE_BITS(pSeninf, SCAM1_SIZE, WIDTH, width) followed by
 * SENINF_WRITE_BITS(pSeninf, SCAM1_SIZE, HEIGHT, height), with the single
 * call site sensor_hal.cpp:1144 passing (clk_inv, width, height, ...) as
 * (..., 320, 240, ...) -- landscape, so width is the low field.
 *
 * (That vendor path is the analog-TV serial input, not the CSI-2 camera
 * path, but SCAM1_SIZE is one register and both agree.)
 *
 * Both fields are 12 bits, so width and height are each limited to 0..4095.
 */
#define SCAM_SIZE_HEIGHT			GENMASK(27, 16)
#define SCAM_SIZE_WIDTH				GENMASK(11, 0)
#define SCAM_SIZE_MAX				4095

/*
 * SCAM_INT, per-port stride above.  Frame and line completion.
 *
 * Audited against the data sheet (page 2269, SCAM1_INT): the seven named bits
 * INT0..INT6 are bits 0..6, all type "RC" (read-cleared / write-1-to-clear),
 * and bits 31:7 are unnamed.  The vendor REG_SCAM1_INT bitfield union,
 * seninf_reg.h:892-903, is the same seven bits at 0..6.  The individual
 * SCAM_INT_0..SCAM_INT_6 defines below are therefore correct as they stand.
 */
#define SCAM_INT_0				BIT(0)
#define SCAM_INT_1				BIT(1)
#define SCAM_INT_2				BIT(2)
#define SCAM_INT_3				BIT(3)
#define SCAM_INT_4				BIT(4)
#define SCAM_INT_5				BIT(5)
#define SCAM_INT_6				BIT(6)

/* All the interrupt bits together, for a write-1-to-clear. */
#define SCAM_INT_MASK				GENMASK(6, 0)

/*
 * Register window size covered by one DT reg entry.
 *
 * SCAM1_STA is the last documented register of the port block and it is a full
 * 32-bit register, so the block needs 0x40 + 0x04 = 0x44 bytes, and the next
 * port starts 0x80 bytes later.  SCAM_SIZE_STA is therefore also the largest
 * byte offset this driver may touch; probe rejects a port whose block would
 * not fit in the mapping.
 */
#define SCAM_SIZE_STA				0x40
#define SCAM_BLOCK_SIZE			(SCAM_SIZE_STA + sizeof(u32))

/*
 * Offset of a register inside the window mapped from the "reg" DT property.
 *
 * The mapping is exactly one port's SCAM block -- SCAM1 at 0x15008200 or
 * SCAM2 at 0x15008280, one node per port -- so a register offset needs no
 * port scaling at all.  This used to be
 *
 *	return scam->port * SCAM_PORT_STRIDE + reg;
 *
 * which silently relocated every access: the DT claimed the whole
 * 0x15008000/0x900 receiver aperture (the same window the CSI-2 receiver node
 * already owns), so port 0's CFG was computed as 0x15008000, which is
 * SENINF_TOP_CTRL, and the whole driver was reading and writing the
 * receiver's and the top block's registers.
 *
 * Bounds are asserted rather than assumed: SCAM_STA is the highest documented
 * register, so nothing may reach past SCAM_BLOCK_SIZE.
 */
static inline u32 scam_ofs(u32 reg)
{
	if (WARN_ON_ONCE(reg + sizeof(u32) > SCAM_BLOCK_SIZE))
		return 0;

	return reg;
}

static inline void scam_write(struct mtk_scam *scam, u32 reg, u32 val)
{
	writel(val, scam->regs + scam_ofs(reg));
}

static inline u32 scam_read(struct mtk_scam *scam, u32 reg)
{
	return readl(scam->regs + scam_ofs(reg));
}

/* ------------------------------------------------------------------ */
/* Format                                                             */
/* ------------------------------------------------------------------ */

/*
 * SCAM does not convert pixel formats; it only carries the stream to the
 * ISP.  The format is therefore whatever the sensor negotiated, and this
 * driver simply programs the size it was given.
 *
 * There is exactly one bus code in play.  The A5142 sensor in this graph
 * produces SBGGR10_1X10 (a5142.c:180 and a5142_enum_frame_size(), which
 * rejects any other code), and SCAM parses the CSD without altering the
 * payload, so the code is carried through untouched.  It is worth naming in
 * one place, because it is otherwise easy to "improve" by inventing a table
 * of formats this block cannot convert.
 */
#define SCAM_MBUS_CODE				MEDIA_BUS_FMT_SBGGR10_1X10

/*
 * Geometry limits.  SCAM_SIZE_MAX is already defined above from the width of
 * the register fields; the minimum is 1 pixel in each direction, because a
 * zero width or height is not a frame and programming it would tell the block
 * to expect nothing at all.
 *
 * The maximum is deliberately not narrowed further.  SCAM is a bridge, and a
 * bridge must not advertise a smaller window than the sensor can produce or
 * the sensor would be clamped to fit; the real ceiling is what the sensor
 * negotiates, bounded only by the 12-bit register fields.
 */
#define SCAM_SIZE_MIN				1

/*
 * Fill in the fields SCAM never varies, leaving geometry and code alone.  Every
 * path that hands a format back to the caller goes through this, so these four
 * cannot drift apart between the getter and the setter.
 */
static void mtk_scam_complete_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SMPTE170M;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

/*
 * Is this geometry encodable in SCAM_SIZE?
 *
 * Out-of-range values must not reach the register: both fields are 12 bits, so
 * e.g. 4160x960 would silently become 64x960 and the block would be programmed
 * for a different picture than the pipeline negotiated.  Checking before the
 * write is the whole point.
 */
static bool mtk_scam_size_valid(const struct v4l2_mbus_framefmt *fmt)
{
	if (fmt->width < SCAM_SIZE_MIN || fmt->width > SCAM_SIZE_MAX)
		return false;
	if (fmt->height < SCAM_SIZE_MIN || fmt->height > SCAM_SIZE_MAX)
		return false;

	return true;
}

/*
 * Return the format this driver is currently using.
 *
 * This is a getter and has to behave like one: whatever the caller put in
 * format->format is overwritten with the driver's current state and the call
 * succeeds.  It must never validate the caller's proposal, because
 * VIDIOC_SUBDEV_G_FMT is a *query*, and the core calls it with whatever the
 * caller had -- including an all-zero format, which is a normal way to ask
 * "what do you do?".  Rejecting that would fail a read-only query for no
 * reason at all.
 *
 * The pad ops used to be this function and the setter at once -- .get_fmt and
 * .set_fmt both pointed at a single mtk_scam_set_pad() -- which inverted the
 * meaning of G_FMT twice over: a query could return -EINVAL because the
 * caller's buffer was zeroed, and a successful "query" would program a
 * hardware register as a side effect.
 *
 * The answer comes from the cached active state for an ACTIVE query and from
 * the state object for a TRY one; see the comment on which below.  scam->size
 * is what was last programmed into SCAM_SIZE, so for the ACTIVE case it is the
 * truth about the hardware; the subdev state is seeded from the same source in
 * mtk_scam_init_state() and kept in step by mtk_scam_set_pad().  Reading the
 * cache also means an ACTIVE G_FMT cannot fail for want of a state object: the
 * core passes state == NULL for an ACTIVE query when the driver is not
 * media-controller-managed, and this driver does not depend on one.
 *
 * format->which is honoured rather than papered over: the two mean different
 * things to the core.  TRY means "tell me what you would propose", ACTIVE
 * means "tell me what is running", and reporting the same cached geometry for
 * both is what used to stop a userspace negotiation loop from converging.
 * format->pad is left exactly as the caller set it.
 */
static int mtk_scam_get_pad(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct mtk_scam *scam = to_mtk_scam(sd);

	lockdep_assert_held(&scam->lock);

	if (format->pad > SCAM_PAD_SRC)
		return -EINVAL;

	/*
	 * A TRY query is about what this driver would propose, so the answer
	 * has to come from the state object -- that is where
	 * mtk_scam_set_pad() put a TRY format.  Returning the active cache for
	 * it meant a TRY S_FMT(1920x1080) followed by a TRY G_FMT on the same
	 * state still reported the pre-negotiation 1280x960: the proposal was
	 * accepted into the state and then ignored on the way back out, so a
	 * userspace negotiation loop could never converge on what it had just
	 * proposed.
	 *
	 * state == NULL is checked rather than dereferenced: check_state() in
	 * v4l2-subdev.c already returns -EINVAL for a TRY with no state, but a
	 * direct call from another in-tree driver must not become a NULL
	 * dereference either.  Such a call falls through to the active cache
	 * below, which is the honest answer when there is nothing else to read.
	 */
	if (format->which == V4L2_SUBDEV_FORMAT_TRY && state) {
		struct v4l2_mbus_framefmt *state_fmt;

		/*
		 * SCAM is a bridge and does not change the frame, so both pads
		 * hold the same one.  The pad that was asked about is read
		 * rather than assumed, so a caller that somehow got the two out
		 * of step sees the real answer instead of a substituted one.
		 */
		state_fmt = v4l2_subdev_state_get_format(state, format->pad);
		if (!state_fmt)
			return -EINVAL;

		format->format = *state_fmt;
		mtk_scam_complete_fmt(&format->format);

		return 0;
	}

	/*
	 * ACTIVE, or a TRY with no state to read: report the cached active
	 * geometry.  That is what was last programmed into SCAM_SIZE, so it is
	 * the truth about the hardware, and it means the two pads cannot
	 * disagree with each other or with SCAM_SIZE.
	 */
	format->format.width = scam->size.width;
	format->format.height = scam->size.height;
	format->format.code = scam->size.code;
	mtk_scam_complete_fmt(&format->format);

	return 0;
}

/*
 * Apply a proposed format.
 *
 * TRY and ACTIVE are different operations and are kept apart here:
 *
 *   V4L2_SUBDEV_FORMAT_TRY     a proposal.  Nothing has been accepted by the
 *                              pipeline, so the driver must not touch the
 *                              hardware and must not adopt the geometry as
 *                              its current one.  It is validated and stored
 *                              in the caller's TRY state only.
 *   V4L2_SUBDEV_FORMAT_ACTIVE  the pipeline has accepted it.  The geometry is
 *                              cached and SCAM_SIZE is programmed.
 *
 * The TRY path used to write SCAM_SIZE unconditionally, because get_fmt and
 * set_fmt were the same function and nothing branched on format->which before
 * the register write.  A TRY format is only a suggestion -- the v4l2-subdev
 * documentation is explicit that it must not modify any device state -- so
 * writing a hardware register from it meant a userspace format negotiation
 * loop, which probes TRY formats freely before committing anything, was
 * silently reprogramming the frame size of a block that might have been
 * streaming at the time.
 *
 * Validation policy, deliberately chosen and applied identically to TRY and
 * ACTIVE: a geometry outside the encodable range is rejected with -EINVAL
 * rather than clamped.
 *
 * Clamping is the more common subdev convention and would have been the
 * easier choice here, but it is wrong for this block.  The clamp target would
 * be SCAM_SIZE_MAX = 4095, a size no sensor in this graph produces; reporting
 * that back as the accepted format would leave get_fmt() and get_selection()
 * describing a 4095-line frame that was never requested and that the block was
 * never told to expect.  -EINVAL instead tells the caller its proposal is
 * unsupportable, which is true, and leaves the driver's idea of the format
 * unchanged and consistent with the hardware.  The range is a hard register
 * limit rather than a preference, so silently reshaping the caller's frame is
 * never the right answer.
 *
 * A zero width or height is rejected on the same grounds: it is not a frame.
 * That is also exactly what a G_FMT query legitimately carries, which is why
 * the getter above must never run this check.
 */
static int mtk_scam_set_pad(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct mtk_scam *scam = to_mtk_scam(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct v4l2_mbus_framefmt *state_fmt;
	unsigned int pad;
	bool try_fmt = format->which == V4L2_SUBDEV_FORMAT_TRY;

	lockdep_assert_held(&scam->lock);

	if (format->pad > SCAM_PAD_SRC)
		return -EINVAL;

	if (!mtk_scam_size_valid(fmt))
		return -EINVAL;

	/*
	 * SCAM does not change the bus code, so there is nothing to negotiate:
	 * whatever the caller asked for, the only code that crosses this block
	 * is the sensor's.  An unsupported code is normalised rather than
	 * rejected, which is the usual subdev behaviour and the more forgiving
	 * one -- a negotiation loop that asks S_FMT with a zeroed code field,
	 * which is exactly what the getter may hand back, must still be able to
	 * converge.  Rejecting here would have been a stricter change than this
	 * block's single-format behaviour warrants.
	 */
	if (fmt->code != SCAM_MBUS_CODE)
		fmt->code = SCAM_MBUS_CODE;

	mtk_scam_complete_fmt(fmt);

	/*
	 * This is a bridge: the two ends of it always carry the same frame.
	 * Writing both pads keeps a later link validation from comparing the
	 * proposal against a different geometry on the other pad.
	 */
	if (try_fmt) {
		/*
		 * A TRY format lives in the caller's state object and nowhere
		 * else.  It is deliberately not copied into scam->size: the
		 * pipeline has not accepted it, so it must not become the
		 * driver's idea of the running geometry, or get_fmt() and
		 * get_selection() would start reporting a frame the block was
		 * never programmed for.
		 *
		 * The core guarantees state is non-NULL for TRY (check_state()
		 * in v4l2-subdev.c returns -EINVAL otherwise), but the checks
		 * below are kept so a direct call from another in-tree driver
		 * cannot turn into a NULL dereference.
		 */
		for (pad = SCAM_PAD_SINK; pad <= SCAM_PAD_SRC; pad++) {
			state_fmt = v4l2_subdev_state_get_format(state, pad);
			if (!state_fmt)
				return -EINVAL;

			*state_fmt = *fmt;
		}

		return 0;
	}

	/*
 * ACTIVE: the pipeline accepted this, so program it.  This is the only place
 * SCAM_SIZE is written.  It used to be
 *
 *	scam_write(scam, SCAM_SIZE, SCAM_SIZE_HEIGHT << 16 | SCAM_SIZE_WIDTH);
 *
 * which is doubly wrong: SCAM_SIZE_HEIGHT/.._WIDTH are *masks*, not shift
 * amounts, so GENMASK(27, 16) << 16 truncates to exactly 0 in a 32-bit word,
 * leaving SCAM_SIZE programmed as height 0 / width 65535.  FIELD_PREP puts
 * each value in its own documented field.
 *
 * The size programmed is the negotiated one, not scam->size.  It used to come
 * from scam->size, which nothing ever updated, so only the probe-time
 * 1280x960 default ever reached the block.
 */
	scam_write(scam, SCAM_SIZE,
		   FIELD_PREP(SCAM_SIZE_HEIGHT, fmt->height) |
		   FIELD_PREP(SCAM_SIZE_WIDTH, fmt->width));

	/*
	 * Cache the geometry so get_fmt() and get_selection() report what was
	 * actually programmed rather than the probe-time default.
	 */
	scam->size.width = fmt->width;
	scam->size.height = fmt->height;
	scam->size.code = fmt->code;

	/*
	 * Keep the active state object in step too, when there is one.  The
	 * getter and the streaming path read scam->size, so this is not
	 * load-bearing, but a caller inspecting the state directly should not
	 * see a stale format.
	 */
	for (pad = SCAM_PAD_SINK; pad <= SCAM_PAD_SRC; pad++) {
		state_fmt = v4l2_subdev_state_get_format(state, pad);
		if (state_fmt)
			*state_fmt = *fmt;
	}

	return 0;
}

/*
 * Seed the pads, and with them the cached geometry, so the subdev reports a
 * real format from the first VIDIOC_SUBDEV_G_FMT instead of an uninitialised
 * (zero) one.
 */
static int mtk_scam_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state)
{
	struct mtk_scam *scam = to_mtk_scam(sd);
	struct v4l2_mbus_framefmt *fmt;
	unsigned int pad;

	lockdep_assert_held(&scam->lock);

	/*
	 * Seed every pad in pads[], which is exactly SCAM_PAD_SINK..SCAM_PAD_SRC
	 * inclusive.  The bound is written against the enum, not as a literal
	 * count, so it cannot quietly stop covering the last pad if the pad
	 * array grows.  v4l2_subdev_state_get_format() also range-checks the
	 * index against entity.num_pads, so an over-long loop would fail here
	 * rather than read past the array.
	 *
	 * This state exists at all because media_entity_pads_init() ran before
	 * v4l2_subdev_init_finalize() in probe: __v4l2_subdev_state_alloc()
	 * only allocates state->pads when entity.num_pads is non-zero.
	 */
	for (pad = 0; pad < SCAM_PAD_NUM; pad++) {
		fmt = v4l2_subdev_state_get_format(state, pad);
		if (!fmt)
			return -EINVAL;

		fmt->width = SCAM_DEFAULT_WIDTH;
		fmt->height = SCAM_DEFAULT_HEIGHT;
		fmt->code = SCAM_MBUS_CODE;
		mtk_scam_complete_fmt(fmt);
	}

	scam->size.width = SCAM_DEFAULT_WIDTH;
	scam->size.height = SCAM_DEFAULT_HEIGHT;
	scam->size.code = SCAM_MBUS_CODE;

	return 0;
}

static const struct v4l2_subdev_internal_ops mtk_scam_internal_ops = {
	.init_state = mtk_scam_init_state,
};

/*
 * The subdev state is seeded by mtk_scam_init_state(), so a first G_FMT already
 * reports a real format; enum_mbus_code() and enum_frame_size() are what let a
 * userspace negotiation loop *discover* that there is anything to negotiate.
 * Without them the core returns -ENOIOCTLCMD (see v4l2_subdev_call() in
 * v4l2-subdev.c, which turns a missing op into that error), so any tool that
 * walks the graph with --list-subdev-mbus-codes or that builds a format list
 * by enumerating would skip this subdev entirely.  That is the inconsistency
 * worth fixing here: pad ops that hand out formats but cannot enumerate them.
 */
static int mtk_scam_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	/*
 * Exactly one code, on both pads: SCAM parses the CSD but does not convert
 * the payload, so the sink's code is the source's code.  The source pad is
 * not an independent choice and reports the same single entry rather than a
 * table of formats this block cannot produce.
	 */
	if (code->pad > SCAM_PAD_SRC)
		return -EINVAL;

	if (code->index)
		return -EINVAL;

	code->code = SCAM_MBUS_CODE;

	return 0;
}

static int mtk_scam_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad > SCAM_PAD_SRC)
		return -EINVAL;

	if (fse->code != SCAM_MBUS_CODE)
		return -EINVAL;

	if (fse->index)
		return -EINVAL;

	/*
	 * SCAM is a bridge and passes the sensor's frame through untouched, so
	 * it must not advertise a narrower window than the sensor can produce
	 * -- that would clamp the sensor to fit.  The ceiling is the 12-bit
	 * SCAM_SIZE limit, the same bound mtk_scam_set_pad() enforces, and not
	 * a made-up figure.
	 */
	fse->min_width = SCAM_SIZE_MIN;
	fse->max_width = SCAM_SIZE_MAX;
	fse->min_height = SCAM_SIZE_MIN;
	fse->max_height = SCAM_SIZE_MAX;

	return 0;
}

static int mtk_scam_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_selection *sel)
{
	struct mtk_scam *scam = to_mtk_scam(sd);

	/*
	 * The crop rectangle describes what SCAM is told to expect, which is
	 * its input, i.e. the sink pad.  That is the in-tree convention for
	 * selection targets too -- isppreview.c and ispresizer.c both reject
	 * anything that is not their sink pad -- so this accepts SCAM_PAD_SINK
	 * and nothing else.
	 *
	 * Written as the enum rather than as a bare "if (sel->pad)": the bare
	 * form happens to be right only because the sink is pad 0, so it reads
	 * as a test of pad zero when it is really a test of the sink.
	 */
	if (sel->pad != SCAM_PAD_SINK)
		return -EINVAL;

	lockdep_assert_held(&scam->lock);

	sel->target = V4L2_SEL_TGT_CROP;
	sel->r.left = 0;
	sel->r.top = 0;
	sel->r.width = scam->size.width;
	sel->r.height = scam->size.height;
	sel->flags = V4L2_SEL_FLAG_LE;

	return 0;
}

static const struct v4l2_subdev_pad_ops mtk_scam_pad_ops = {
	.enum_mbus_code = mtk_scam_enum_mbus_code,
	.enum_frame_size = mtk_scam_enum_frame_size,
	.get_fmt = mtk_scam_get_pad,
	.set_fmt = mtk_scam_set_pad,
	.get_selection = mtk_scam_get_selection,
};

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

/*
 * Bring the block out of reset, tell it what frame to expect, clear any latched
 * interrupt, and start it.
 *
 * SCAM_CON.ENA must be set only after SCAM_CON.RST has been released (data
 * sheet page 2269: "clear RST before setting ENA = 1"), and SCAM itself must
 * be triggered before the image sensor is (same page).  SCAM_SIZE is written
 * between the reset and the enable, while the block is halted and before
 * anything can capture a line against a stale size.
 */
static int mtk_scam_start_stream(struct mtk_scam *scam)
{
	struct v4l2_mbus_framefmt fmt;

	lockdep_assert_held(&scam->lock);

	/*
	 * The cached active size, checked rather than trusted.  init_state()
	 * seeds it from SCAM_DEFAULT_* and only an ACTIVE S_FMT replaces it, so
	 * it is normally in range; a zero here would mean the cache was never
	 * seeded at all, which would tell the block to expect a frame of no
	 * pixels.  Both fields are 12 bits, so an out-of-range value would be
	 * silently truncated into a different picture.  Refuse to start rather
	 * than program either.
	 */
	fmt.width = scam->size.width;
	fmt.height = scam->size.height;

	if (!mtk_scam_size_valid(&fmt))
		return dev_err_probe(scam->dev, -EINVAL,
				     "cached size %ux%u is not encodable in SCAM_SIZE\n",
				     fmt.width, fmt.height);

	/* Reset, then release, leaving the block halted. */
	scam_write(scam, SCAM_CON, SCAM_CON_RST);
	scam_write(scam, SCAM_CON, 0);

	/*
	 * Program the frame size from the same cached geometry mtk_scam_set_pad()
	 * programs it from.  This used not to be written at all here: only
	 * set_pad() did it, so if no ACTIVE S_FMT had ever been issued the block
	 * was told to expect whatever SCAM_SIZE happened to hold (its reset
	 * value) while the software believed it was streaming scam->size, and
	 * the two silently disagreed.
	 */
	scam_write(scam, SCAM_SIZE,
		   FIELD_PREP(SCAM_SIZE_HEIGHT, fmt.height) |
		   FIELD_PREP(SCAM_SIZE_WIDTH, fmt.width));

	/* Clear any stale status from a previous run (INT* are write-1-clear). */
	scam_write(scam, SCAM_INT, SCAM_INT_MASK);

	/*
	 * Program the documented reset configuration (WARN_MASK=1, CYC=4) plus
	 * the interrupt enables and continuous mode this driver needs; see the
	 * long comment on SCAM_CFG_START.  This used to evaluate to CONT alone
	 * because the "preserve" mask it subtracted was a superset of the reset
	 * value, so WARN_MASK and CYC were cleared on the way to the block.
	 */
	scam_write(scam, SCAM_CFG, SCAM_CFG_START);

	/* Start the block. */
	scam_write(scam, SCAM_CON, SCAM_CON_ENA);

	scam->streaming = true;

	return 0;
}

static void mtk_scam_stop_stream(struct mtk_scam *scam)
{
	lockdep_assert_held(&scam->lock);

	scam->streaming = false;

	/*
	 * Hold the block in reset rather than just dropping ENA, so it stops
	 * immediately instead of finishing the frame in flight.  SCAM_CON is
	 * a plain RW register with a readable RST bit, so clear ENA and
	 * assert RST in one write.
	 */
	scam_write(scam, SCAM_CON, SCAM_CON_RST);
}

/*
 * Streaming entry point.
 *
 * The lock is taken here, not assumed.  The pad ops run under the state lock
 * through the core's v4l2_subdev_call wrappers, but call_s_stream() in
 * v4l2-subdev.c calls the driver callback directly with no state object and so
 * no lock of any kind; the lockdep_assert_held() further down used to assert a
 * mutex nobody held, which would fire the moment anything called this from a
 * context that had not taken it, and meanwhile left the cached geometry and the
 * register programming unserialised against a concurrent S_FMT.  Both use
 * scam->lock, so taking it here makes format changes and stream changes
 * mutually exclusive, which is what the assert below claims.
 *
 * A plain mutex is right: this path programs registers, so it sleeps and must
 * not be in atomic context.  Nothing else is taken in this path, so the
 * ordering question does not arise.
 */
static int mtk_scam_s_stream(struct v4l2_subdev *sd, int on)
{
	struct mtk_scam *scam = to_mtk_scam(sd);
	int ret = 0;

	mutex_lock(&scam->lock);

	if (on == scam->streaming)
		goto out_unlock;

	if (on)
		ret = mtk_scam_start_stream(scam);
	else
		mtk_scam_stop_stream(scam);

out_unlock:
	mutex_unlock(&scam->lock);
	return ret;
}

static const struct v4l2_subdev_video_ops mtk_scam_video_ops = {
	.s_stream = mtk_scam_s_stream,
};

static const struct v4l2_subdev_ops mtk_scam_subdev_ops = {
	.video = &mtk_scam_video_ops,
	.pad = &mtk_scam_pad_ops,
};

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

static int mtk_scam_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_scam *scam;
	struct resource *res;
	int ret;

	scam = devm_kzalloc(dev, sizeof(*scam), GFP_KERNEL);
	if (!scam)
		return -ENOMEM;

	scam->dev = dev;
	dev_set_drvdata(dev, scam);
	mutex_init(&scam->lock);

	scam->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(scam->regs))
		return PTR_ERR(scam->regs);

	/*
	 * The "reg" must cover exactly this port's SCAM block.  A window that
	 * is too small would let every register access run off the end of the
	 * mapping; a window that is large enough to also cover the receiver or
	 * the top clock-gate block is an overlapping claim that cannot be
	 * honoured (two platform devices cannot both reserve one aperture).
	 * SCAM_STA at +0x40 is the last documented register, so 0x44 bytes is
	 * the minimum.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(res))
		return dev_err_probe(dev, PTR_ERR(res),
				     "no reg resource\n");

	if (resource_size(res) < SCAM_BLOCK_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "reg is 0x%zx bytes, need at least 0x%zx for SCAM_STA\n",
				     (size_t)resource_size(res),
				     (size_t)SCAM_BLOCK_SIZE);

	ret = of_property_read_u32(dev->of_node, "mediatek,scam-port",
				   &scam->port);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing mediatek,scam-port\n");

	if (scam->port >= SCAM_MAX_PORTS)
		return dev_err_probe(dev, -EINVAL,
				     "mediatek,scam-port %u out of range (max %u)\n",
				     scam->port, SCAM_MAX_PORTS - 1);

	scam->sd.internal_ops = &mtk_scam_internal_ops;

	/*
	 * v4l2_subdev_init() first, then everything it leaves to the driver.
	 *
	 * It has to come first because it is the only thing that initialises
	 * sd.name, and __v4l2_device_register_subdev() in
	 * drivers/media/v4l2-core/v4l2-device.c rejects any subdev whose name
	 * is empty:
	 *
	 *	if (!v4l2_dev || !sd || sd->v4l2_dev || !sd->name[0])
	 *		return -EINVAL;
	 *
	 * so a subdev registered without it never joins the V4L2 device, and
	 * because that is the async notifier's path the media graph would
	 * never see this node either -- whatever else was fixed in probe.
	 * Assigning the subdev fields by hand, as this used to, left
	 * sd.name all zeroes because none of those fields alias sd.name.
	 *
	 * What v4l2_subdev_init() already does, and so must not be repeated:
	 * it zeroes sd.name, points sd.ops at the ops passed here, sets
	 * sd.v4l2_dev = NULL, resets sd.flags and sd.grp_id, clears
	 * dev_priv/host_priv/privacy_led, initialises sd.list and
	 * sd.async_subdev_endpoint_list, and -- because CONFIG_MEDIA_CONTROLLER
	 * is set -- makes sd.entity.name *point at* sd.name and sets
	 * sd.entity.obj_type = MEDIA_ENTITY_TYPE_V4L2_SUBDEV and
	 * sd.entity.function = MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN.
	 *
	 * Note in particular that entity.name is not a separate copy: it is the
	 * same array as sd.name, so writing dev_name() into entity.name is
	 * exactly what fills sd.name in, which is why this used to appear to
	 * work right up until registration refused it.  Setting sd.name
	 * explicitly below and leaving entity.name alone is therefore both
	 * sufficient and non-duplicative.
	 */
	v4l2_subdev_init(&scam->sd, &mtk_scam_subdev_ops);
	strscpy(scam->sd.name, dev_name(dev), sizeof(scam->sd.name));

	/*
	 * The pad ops run under this mutex (v4l2_subdev_init_finalize() makes
	 * the active state use it as its own lock), so every lockdep_assert_held()
	 * in them is about the lock taken here.  v4l2_subdev_init() does not
	 * touch state_lock, so this is still the driver's to set.
	 */
	scam->sd.state_lock = &scam->lock;

	/*
	 * SCAM sits between the receiver and the CAM/ISP and converts nothing,
	 * so it is the video-interface bridge of this chain.  That is the same
	 * function every in-tree CSI-2 receiver uses for the same reason --
	 * cdns-csi2rx.c, rkisp1-csi.c and dw-mipi-csi2rx.c all set
	 * MEDIA_ENT_F_VID_IF_BRIDGE -- and it is what tools walking the graph
	 * (media-ctl, v4l2-ctl) key their pad-format bookkeeping off.
	 */
	scam->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	/*
	 * Sanity check: SCAM_CFG is documented as resetting to 0x10000400, i.e.
	 * WARN_MASK=1 with CYC=4 and every other named bit 0.  If the aperture
	 * is wrong -- this driver computes no port offset of its own, so the
	 * "reg" property has to point at this port's own block -- then the read
	 * back gives something else, and it is better to say so here than to
	 * fail mysteriously on the first frame.
	 *
	 * The comparison is deliberately against SCAM_CFG_RESET, the *hardware*
	 * reset value, and not against SCAM_CFG_START, the value this driver
	 * later programs.  The two differ (SCAM_CFG_START adds the interrupt
	 * enables and CONT), so comparing the probe-time read against
	 * SCAM_CFG_START would warn on every healthy device and would stop
	 * being able to detect a wrong aperture at all.  This check asks only
	 * "is this the SCAM block I think it is".
	 */
	ret = scam_read(scam, SCAM_CFG);
	if (ret != SCAM_CFG_RESET)
		dev_warn(dev,
			 "SCAM_CFG reads 0x%08x, expected the documented reset 0x%08x; check the reg property\n",
			 ret, SCAM_CFG_RESET);

	/*
	 * scam->size is seeded by mtk_scam_init_state() from the DT pads; do
	 * not hardcode it here as well, or the two can drift apart.
	 */

	/*
	 * Create the pads.  This has to happen before
	 * v4l2_subdev_init_finalize() below, because that is what allocates the
	 * active state, and __v4l2_subdev_state_alloc() only allocates the
	 * legacy state->pads array when sd->entity.num_pads is non-zero:
	 *
	 *	if (!(sd->flags & V4L2_SUBDEV_FL_STREAMS) && sd->entity.num_pads)
	 *		state->pads = ...
	 *
	 * With no pads registered, init_state() below would have had nothing
	 * to seed, v4l2_subdev_state_get_format() would have returned NULL for
	 * every pad, and both mtk_scam_get_pad() and mtk_scam_set_pad() would
	 * have failed on that rather than on anything about the format.
	 *
	 * The order of the pads array is the pad index; the DT names scam_in as
	 * port@0 and scam_out as port@1, which matches SCAM_PAD_SINK and
	 * SCAM_PAD_SRC.
	 */
	scam->pads[SCAM_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	scam->pads[SCAM_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&scam->sd.entity, SCAM_PAD_NUM,
				     scam->pads);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init entity pads\n");

	ret = v4l2_subdev_init_finalize(&scam->sd);
	if (ret)
		return dev_err_probe(dev, ret, "subdev init error\n");

	ret = v4l2_async_register_subdev(&scam->sd);
	if (ret)
		goto err_subdev_cleanup;

	dev_info(dev, "registered SCAM adaptor %u (partial, see source)\n",
		 scam->port);

	return 0;

err_subdev_cleanup:
	/*
	 * Undo the two things the successful path added after the pads were
	 * created: the active state allocated by v4l2_subdev_init_finalize()
	 * and, transitively, nothing else -- the pads live in the devm
	 * allocation, so they go away with it.
	 *
	 * v4l2_subdev_cleanup() is safe to call whether or not
	 * v4l2_async_register_subdev() succeeded, and it is also the only
	 * cleanup needed on any later probe failure, so every error return
	 * above the pads_init() call can just return directly.
	 */
	v4l2_subdev_cleanup(&scam->sd);
	return ret;
}

static const struct of_device_id mtk_scam_of_match[] = {
	{ .compatible = "mediatek,mt6589-scam" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_scam_of_match);

static struct platform_driver mtk_scam_driver = {
	.probe = mtk_scam_probe,
	.driver = {
		.name = "mtk-scam",
		.of_match_table = mtk_scam_of_match,
	},
};
module_platform_driver(mtk_scam_driver);

MODULE_DESCRIPTION("MediaTek MT6589 sensor-CAM adaptor (partial)");
MODULE_AUTHOR("Lenovo Linux Team");
MODULE_LICENSE("GPL");
