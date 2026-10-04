// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 CSI-2 receiver (seninf / csi2) driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * The MT6589 image subsystem puts two independent CSI-2 receivers in one
 * register window at 0x1500_8000, one for each camera port:
 *
 *   seninf_top  +0x0000  parallel sensor clock gates, shared N3D reset
 *   csi2        +0x0100  receiver 1 at +0x100, receiver 2 at +0x180
 *   SCAM        +0x0200  the sensor-CAM adaptors that follow each receiver
 *
 * This driver owns the receivers and the top-level clock gating.  The SCAM
 * blocks are not implemented here; see README.md.
 *
 * Register map transcribed from the MT6589 data sheet, chapter "MIPI RX
 * Configuration Module".
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/phy/phy.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "mtk-csi2-rx.h"

struct mtk_csi2_rx {
	struct device *dev;
	void __iomem *regs;

	/*
	 * Port index into the duplicated csi2 registers.  Port 0 is
	 * SENINF1_CSI2_*, port 1 is SENINF2_CSI2_*.
	 */
	unsigned int port;

	/*
	 * The D-PHY that sits below this receiver.  Obtained from the DT
	 * "phys" property, exactly as the MT6589 DSI does with its TX PHY,
	 * so the two are consistent.  Initialised on power_on() and
	 * released on power_off(), because the PHY is what makes the
	 * receiver's lanes able to receive anything at all.
	 */
	struct phy *phy;

	struct clk *seninf_clk;
	struct clk *seninf_csi2_clk;
	struct clk *seninf_tg_clk;
	struct reset_control *seninf_rst;

struct v4l2_subdev sd;
struct mutex lock;

	/*
	 * The media pads, indexed as CSI2_PAD_SINK / CSI2_PAD_SRC below.  The
	 * receiver is a pure bridge: one sink for the sensor's MIPI stream and
	 * one source towards SCAM.  Flags are filled in in probe, before
	 * media_entity_pads_init() assigns each pad its index from its
	 * position in this array.
	 */
	struct media_pad pads[CSI2_PAD_NUM];

	/*
	 * The format last accepted through set_fmt().  s_stream() runs with
	 * only the subdev, not the subdev state, so the negotiated geometry
	 * is cached here for it to program from.
	 */
	u32 width;
	u32 height;
	u32 code;

	/*
	 * How many MIPI CSI-2 data lanes this receiver is programmed for, and
	 * the CSI2_CTRL[3:1] D-Lane enable word derived from it.  Both come
	 * from the DT, never from a register readback; see the long comment on
	 * mtk_csi2_rx_parse_lanes() for why there is no register to ask.
	 */
	u32 num_data_lanes;
	u32 dlane_bits;
};

static inline struct mtk_csi2_rx *sd_to_csi2rx(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mtk_csi2_rx, sd);
}

/* Offset of this port's csi2 register block, relative to the window. */
static inline u32 csi2_ofs(const struct mtk_csi2_rx *priv, u32 reg)
{
	return 0x100 + priv->port * CSI2_PORT_STRIDE + reg;
}

static inline void csi2_write(struct mtk_csi2_rx *priv, u32 reg, u32 val)
{
	writel(val, priv->regs + csi2_ofs(priv, reg));
}

static inline u32 csi2_read(struct mtk_csi2_rx *priv, u32 reg)
{
	return readl(priv->regs + csi2_ofs(priv, reg));
}

static inline void csi2_update_bits(struct mtk_csi2_rx *priv, u32 reg,
				    u32 mask, u32 val)
{
	u32 tmp = csi2_read(priv, reg);

	writel((tmp & ~mask) | (val & mask), priv->regs + csi2_ofs(priv, reg));
}

/* ------------------------------------------------------------------ */
/* Lane configuration                                                  */
/* ------------------------------------------------------------------ */

/*
 * Map a lane count onto the CSI2_CTRL D-Lane enable bits.
 *
 * DLANE{1,2,3}_EN are one bit per data lane at CSI2_CTRL[3:1] ("Enables CSI2 N
 * data lane", draft/ds/mipi.txt:2925-2927), so N lanes means the low N of those
 * three bits:
 *
 *	1 lane -> DLANE1_EN                        == BIT(1)
 *	2 lanes-> DLANE2_EN | DLANE1_EN             == BIT(2) | BIT(1)
 *	3 lanes-> DLANE3_EN | DLANE2_EN | DLANE1_EN == BIT(3) | BIT(2) | BIT(1)
 *
 * which is literally ((1 << lanes) - 1) << 1 -- the same expression the vendor
 * uses at seninf_drv.cpp:1287.  It is written out lane by lane rather than as
 * that shift so that each bit in the result can be read against its documented
 * name; a count outside 1..CSI2_MAX_DATA_LANES is rejected here rather than
 * silently masking down to something else (0, which would enable no data lane
 * at all, or 4+, which this register cannot express).
 */
static u32 mtk_csi2_rx_lane_bits(unsigned int lanes)
{
	switch (lanes) {
	case 1:
		return CSI2_DLANE1_EN;
	case 2:
		return CSI2_DLANE2_EN | CSI2_DLANE1_EN;
	case 3:
		return CSI2_DLANE3_EN | CSI2_DLANE2_EN | CSI2_DLANE1_EN;
	default:
		return 0;
	}
}

/*
 * Work out how many data lanes this receiver is programmed for.
 *
 * The lane count is a property of the sensor and the D-PHY between it and this
 * block, so the only honest source is the sensor-facing endpoint of the media
 * graph: the "data-lanes" array on this subdev's own sink endpoint, parsed the
 * standard way with v4l2_fwnode_endpoint_parse() so the count is exactly the
 * one the media framework would use for that endpoint.
 *
 * This used to be read out of CSI2_DBG & 0xE and written back into
 * CSI2_CTRL & 0xE.  That was meaningless twice over:
 *
 *   - CSI2_DBG is the debug register.  Its bits 31:22 are the
 *     LN{3,2,1,0,C}_{HSRXDB,LPRXDB}_EN "for test only" overrides
 *     (draft/ds/mipi.txt:3350-3386), and nothing at bits 3:1 is a lane
 *     enable, so the read produced a number with no relationship to the lanes
 *     in use;
 *   - when it happened to read 0, the code left the D-Lane enables cleared
 *     entirely and still set CSI2_EN, i.e. it enabled a receiver with no
 *     data lane enabled, which cannot receive anything.  The lane enables
 *     are not optional on this block: CSI2_EN gates the receiver and
 *     DLANE*_EN are what let the data lanes through it.
 *
 * Neither the data sheet nor the vendor offers a register holding the
 * configured lane count, so there is nothing to read back: it has to come from
 * the DT.
 *
 * There is currently no such endpoint in this tree.  The sensor (A5142) and the
 * MIPI D-PHY node were both removed -- commit b048dc62423c "media: i2c: drop
 * the A5142 sensor driver" took out the A5142's ports node and with it the
 * receiver's sink endpoint, because the DT describes the sensor side of the
 * link and there is nothing left to point it at (see the note on the receiver
 * node in mt6589-lenovo-blade-camera.dtsi).  The removed A5142 was itself
 * documented as a "5 megapixel RAW sensor, 2-lane MIPI CSI-2" (a5142.c header
 * comment), so two lanes is what this board's camera drove.
 *
 * So: parse the endpoint when there is one and use what it says; when there is
 * not, fall back to that documented two-lane figure, loudly, rather than to a
 * debug-register read that means nothing or to enabling no lanes at all.  The
 * fallback is a warning rather than an error on purpose: this is the one piece
 * of the configuration that genuinely cannot be derived on this board, and
 * failing probe over it would take the receiver, SCAM and CAM -- all of which
 * probe independently of the sensor -- out of the media graph, making the
 * graph harder to inspect and not more correct.  When a sensor or D-PHY node
 * comes back, the endpoint parse below picks up the real count unchanged.
 */
static u32 mtk_csi2_rx_lane_count(struct device *dev)
{
	struct device_node *ep __free(device_node) = NULL;
	struct v4l2_fwnode_endpoint v4l2_ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned int lanes;

	/*
	 * port@0 is this subdev's sink pad under the core's default 1:1 pad
	 * mapping (v4l2_subdev_get_fwnode_pad_1_to_1()), which is the same
	 * mapping CSI2_PAD_SINK == 0 already assumes.
	 */
	ep = of_graph_get_endpoint_by_regs(dev->of_node, 0, 0);
	if (ep) {
		if (!v4l2_fwnode_endpoint_parse(of_fwnode_handle(ep), &v4l2_ep) &&
		    v4l2_ep.bus_type == V4L2_MBUS_CSI2_DPHY) {
			lanes = v4l2_ep.bus.mipi_csi2.num_data_lanes;
			if (lanes && lanes <= CSI2_MAX_DATA_LANES)
				return lanes;

			dev_warn(dev,
				 "sink endpoint declares %u data lanes, which CSI2_CTRL cannot enable (1..%u)\n",
				 lanes, CSI2_MAX_DATA_LANES);
		} else {
			dev_warn(dev,
				 "sink endpoint is not a usable MIPI CSI-2 D-PHY endpoint\n");
		}
	}

	/*
	 * No usable endpoint: the sensor and D-PHY nodes are absent from this
	 * tree, so nothing in the DT states a lane count.  Fall back to the
	 * removed A5142's documented two-lane interface, which is what this
	 * board's rear camera drove, and say so rather than deciding silently.
	 */
	lanes = 2;
	dev_warn(dev,
		 "no data-lanes in the DT (no sensor or D-PHY node on this board); assuming %u data lanes\n",
		 lanes);

	return lanes;
}

/*
 * Read the lane count and precompute the register word for it, refusing any
 * count this block cannot enable.
 */
static int mtk_csi2_rx_parse_lanes(struct mtk_csi2_rx *priv)
{
	u32 bits;

	priv->num_data_lanes = mtk_csi2_rx_lane_count(priv->dev);

	bits = mtk_csi2_rx_lane_bits(priv->num_data_lanes);
	if (!bits)
		return dev_err_probe(priv->dev, -EINVAL,
				     "%u data lanes cannot be enabled: CSI2_CTRL has DLANE1_EN..DLANE3_EN only\n",
				     priv->num_data_lanes);

	priv->dlane_bits = bits;

	dev_info(priv->dev, "%u data lane(s), DLANE enables 0x%x\n",
		 priv->num_data_lanes, bits);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Pads                                                                */
/* ------------------------------------------------------------------ */

/*
 * The receiver is a pure bridge.  It unpacks MIPI packets, checks them and
 * hands the bytes to SCAM, which hands them to the ISP; there is no pixel
 * format conversion anywhere in this path, and the receiver has no way to
 * retag one.  So the format on its source pad is always whatever the
 * sensor negotiated on the sink pad.
 *
 * CSI2_CTRL.DATA_FLOW (bits 18:17) is a receive-side selector -- 0: data
 * packet, 1: generic long packet, 2: all data packet (data sheet page 2261)
 * -- and is not a pixel format.  Nothing in this block re-interprets the
 * payload, so MEDIA_BUS_FMT_FIXED would be a lie: it would make the sensor's
 * SBGGR10 look like it had become something opaque.
 */
/*
 * The pad ids (CSI2_PAD_SINK / CSI2_PAD_SRC) live in mtk-csi2-rx.h, because
 * the pads[] array in struct mtk_csi2_rx is sized with CSI2_PAD_NUM.
 */

/* ------------------------------------------------------------------ */
/* Data format handling                                               */
/* ------------------------------------------------------------------ */

/*
 * The mbus codes this receiver can carry.
 *
 * The A5142 in this tree drives MEDIA_BUS_FMT_SBGGR10_1X10 (a5142.c), and
 * the MT6589 CSI-2 receiver is a packet-level block: CSI2_INTSTA carries
 * "wrong data ID" / "wrong packet ID" faults rather than any format
 * negotiation, and the data type in the incoming short packets is passed
 * through untouched to SCAM and on to the ISP.  The receiver therefore
 * transports whatever RAW Bayer order the sensor picks, and all of them are
 * RAW10 here -- the ISP's RAW10 path.
 *
 * Corroboration: the only vendor code that programs this block does so for a
 * MT6589 camera, seninf_drv.cpp:1354-1369, and the vendor HAL exposes no
 * media-bus codes at all (there is no MEDIA_BUS_FMT table anywhere under
 * mediatek/), so there is no vendor enumeration to contradict this.
 *
 * MEDIA_BUS_FMT_SBGGR10_1X10 must be present and first: it is what the
 * in-tree A5142 advertises, and it is what mtk-scam.c defaults to.
 */
static const u32 mtk_csi2_rx_codes[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

/*
 * Real default used only when the state has nothing better to offer.  It is
 * the A5142's preview mode (a5142_modes[A5142_MODE_PREVIEW]), not an
 * invented size.
 */
#define CSI2_RX_DEFAULT_WIDTH	1280
#define CSI2_RX_DEFAULT_HEIGHT	960

static int mtk_csi2_rx_init_state(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state)
{
	struct mtk_csi2_rx *priv = sd_to_csi2rx(sd);
	struct v4l2_mbus_framefmt *fmt;
	unsigned int pad;

	/*
	 * Seed both pads.  The core has already allocated state->pads (it
	 * does that in __v4l2_subdev_state_alloc() because media_entity_pads_init()
	 * left entity.num_pads non-zero, and it zeroes the array), and the two
	 * ends of a bridge always agree, so one seed and a copy is enough.
	 *
	 * The bound is CSI2_PAD_NUM rather than a literal 2 so that adding a pad
	 * to the array cannot silently leave this loop short of it, and
	 * v4l2_subdev_state_get_format() bounds-checks the index anyway, so an
	 * over-long loop fails here rather than reading past the array.
	 */
	for (pad = 0; pad < CSI2_PAD_NUM; pad++) {
		fmt = v4l2_subdev_state_get_format(state, pad);
		if (!fmt)
			return -EINVAL;

		fmt->width = CSI2_RX_DEFAULT_WIDTH;
		fmt->height = CSI2_RX_DEFAULT_HEIGHT;
		fmt->code = mtk_csi2_rx_codes[0];
		fmt->field = V4L2_FIELD_NONE;
		fmt->colorspace = V4L2_COLORSPACE_SMPTE170M;
		fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
		fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	}

	/*
	 * Remember the negotiated size; s_stream() has no other source.  Seeding
	 * the code too keeps the whole cache defined, so a stream started
	 * without any ACTIVE S_FMT programs from this documented default rather
	 * than from a partly-uninitialised value.
	 */
	priv->width = CSI2_RX_DEFAULT_WIDTH;
	priv->height = CSI2_RX_DEFAULT_HEIGHT;
	priv->code = mtk_csi2_rx_codes[0];

	return 0;
}

/* Clamp a proposed format to something this receiver can actually carry. */
static void mtk_csi2_rx_prepare_fmt(struct v4l2_mbus_framefmt *fmt)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_csi2_rx_codes); i++)
		if (mtk_csi2_rx_codes[i] == fmt->code)
			break;

	if (i == ARRAY_SIZE(mtk_csi2_rx_codes))
		fmt->code = mtk_csi2_rx_codes[0];

	/* The 12-bit SCAM_SIZE fields downstream cap the geometry. */
	if (!fmt->width || fmt->width > SCAM_SIZE_MAX)
		fmt->width = CSI2_RX_DEFAULT_WIDTH;
	if (!fmt->height || fmt->height > SCAM_SIZE_MAX)
		fmt->height = CSI2_RX_DEFAULT_HEIGHT;

	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SMPTE170M;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int mtk_csi2_rx_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	/*
	 * The source pad is not an independent choice: it must be able to
	 * pass through whatever the sink negotiated, so it reports the sink's
	 * current code rather than the full table.
	 */
	if (code->pad == CSI2_PAD_SRC) {
		struct v4l2_mbus_framefmt *sink;

		if (code->index)
			return -EINVAL;

		sink = v4l2_subdev_state_get_format(state, CSI2_PAD_SINK);
		if (!sink)
			return -EINVAL;

		code->code = sink->code;

		return 0;
	}

	if (code->pad != CSI2_PAD_SINK)
		return -EINVAL;

	if (code->index >= ARRAY_SIZE(mtk_csi2_rx_codes))
		return -EINVAL;

	code->code = mtk_csi2_rx_codes[code->index];

	return 0;
}

static int mtk_csi2_rx_enum_frame_size(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_frame_size_enum *fse)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_csi2_rx_codes); i++)
		if (mtk_csi2_rx_codes[i] == fse->code)
			break;

	if (i == ARRAY_SIZE(mtk_csi2_rx_codes))
		return -EINVAL;

	/*
	 * This bridge passes the sensor's frame through untouched, so it must
	 * not advertise a narrower window than the sensor can produce --
	 * otherwise the sensor would be clamped to fit.  The upper bound is
	 * the 12-bit SCAM_SIZE limit downstream, not a made-up figure.
	 */
	fse->min_width = 1;
	fse->max_width = SCAM_SIZE_MAX;
	fse->min_height = 1;
	fse->max_height = SCAM_SIZE_MAX;

	return 0;
}

static int mtk_csi2_rx_get_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *fmt)
{
	struct mtk_csi2_rx *priv = sd_to_csi2rx(sd);
	struct v4l2_mbus_framefmt *sink;

	lockdep_assert_held(&priv->lock);

	if (fmt->pad != CSI2_PAD_SINK && fmt->pad != CSI2_PAD_SRC)
		return -EINVAL;

	sink = v4l2_subdev_state_get_format(state, CSI2_PAD_SINK);
	if (!sink)
		return -EINVAL;

	/*
	 * Report the sink's format on both pads: the receiver does not change
	 * it.  Note that fmt->which is left exactly as the caller set it --
	 * V4L2_SUBDEV_FORMAT_ACTIVE and V4L2_SUBDEV_FORMAT_TRY mean different
	 * things to the V4L2 core and this driver must not paper over that.
	 * This used to hardcode 1280x800 / MEDIA_BUS_FMT_FIXED and force
	 * which = TRY, which both lost the negotiated size and told the core
	 * that ACTIVE queries were hypothetical.
	 */
	fmt->format = *sink;

	return 0;
}

/*
 * Apply a proposed format.
 *
 * TRY and ACTIVE are different operations and are kept apart here:
 *
 *   V4L2_SUBDEV_FORMAT_TRY     a proposal.  Nothing has been accepted by the
 *                              pipeline, so the receiver must not touch the
 *                              hardware and must not adopt the geometry as
 *                              its current one.  It is validated and stored in
 *                              the caller's TRY state only.
 *   V4L2_SUBDEV_FORMAT_ACTIVE  the pipeline has accepted it.  The geometry is
 *                              cached for s_stream() to program from.
 *
 * The state object is always updated, because that is the whole point of the
 * state: it is what a later get_fmt() on the same state reports back.
 *
 * The cache below is NOT updated for a TRY format, and that is the one part of
 * this function that has to be right.  priv->width/height/code is what
 * mtk_csi2_rx_start_stream() reads to program the block, and the core calls
 * s_stream() with the subdev only -- there is no state object in that path --
 * so the cache is the streaming path's entire view of the negotiated format.
 * Updating it unconditionally meant a TRY S_FMT followed by no ACTIVE S_FMT
 * (a perfectly ordinary negotiation sequence: probe with TRY, then stream
 * only if something upstream accepts) left the hardware programmed for a
 * geometry the pipeline never accepted.  init_state() seeds the cache, so an
 * ACTIVE-less stream still starts from the documented default rather than
 * from uninitialised memory.
 */
static int mtk_csi2_rx_set_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *fmt)
{
	struct mtk_csi2_rx *priv = sd_to_csi2rx(sd);
	struct v4l2_mbus_framefmt *sink, *src;
	bool try_fmt = fmt->which == V4L2_SUBDEV_FORMAT_TRY;

	lockdep_assert_held(&priv->lock);

	if (fmt->pad != CSI2_PAD_SINK && fmt->pad != CSI2_PAD_SRC)
		return -EINVAL;

	sink = v4l2_subdev_state_get_format(state, CSI2_PAD_SINK);
	src = v4l2_subdev_state_get_format(state, CSI2_PAD_SRC);
	if (!sink || !src)
		return -EINVAL;

	/*
	 * The source pad always mirrors the sink; there is nothing to
	 * negotiate separately.  Ask the state what the caller wanted rather
	 * than writing the incoming struct through unchanged.
	 */
	mtk_csi2_rx_prepare_fmt(&fmt->format);

	*sink = fmt->format;
	*src = *sink;

	/*
	 * Cache it, but only for an accepted format: s_stream() has no access
	 * to the subdev state, and the block's own size registers are
	 * programmed from here.  A TRY format is confined to the state object
	 * above.
	 */
	if (!try_fmt) {
		priv->width = sink->width;
		priv->height = sink->height;
		priv->code = sink->code;
	}

	fmt->format = *sink;

	return 0;
}

static const struct v4l2_subdev_internal_ops mtk_csi2_rx_internal_ops = {
	.init_state = mtk_csi2_rx_init_state,
};

static const struct v4l2_subdev_pad_ops mtk_csi2_rx_pad_ops = {
	.enum_mbus_code = mtk_csi2_rx_enum_mbus_code,
	.enum_frame_size = mtk_csi2_rx_enum_frame_size,
	.get_fmt = mtk_csi2_rx_get_fmt,
	.set_fmt = mtk_csi2_rx_set_fmt,
};

/* ------------------------------------------------------------------ */
/* Runtime power management                                           */
/* ------------------------------------------------------------------ */

/*
 * The reset bit and the lane enables are self-clearing in effect: after a
 * reset the receiver must be re-programmed, so power_on() rebuilds the
 * whole configuration rather than trying to save and restore it.
 */
static int mtk_csi2_rx_power_on(struct mtk_csi2_rx *priv)
{
	int ret;

	/* Each of these may be absent; see the note at the clk_get calls. */
	if (priv->seninf_clk) {
		ret = clk_prepare_enable(priv->seninf_clk);
		if (ret)
			return ret;
	}

	if (priv->seninf_csi2_clk) {
		ret = clk_prepare_enable(priv->seninf_csi2_clk);
		if (ret)
			goto err_seninf_clk;
	}

	if (priv->seninf_tg_clk) {
		ret = clk_prepare_enable(priv->seninf_tg_clk);
		if (ret)
			goto err_csi2_clk;
	}

	/*
	 * Zero the return value first.  Every clock above returned early on its
	 * own error, so on the path that reaches here the last assignment was
	 * the tg_grp enable's success (or there was no tg_grp clock at all, in
	 * which case ret still holds whatever the csi2 enable left).  Without
	 * this, a board with no tg_grp clock would return that stale 0-or-error
	 * from power_on() even on the path that goes on to succeed, and a board
	 * whose last clock succeeded would return its 0 -- which happens to be
	 * right, but only by accident.  Reset it so the value returned below
	 * always belongs to the code that actually produced it.
	 */
	ret = 0;

	/*
	 * The reset is optional: see the long note on the
	 * devm_reset_control_get_optional() call in probe.  Where it is absent
	 * the block is simply brought up without one.
	 */
	if (priv->seninf_rst) {
		ret = reset_control_deassert(priv->seninf_rst);
		if (ret)
			goto err_tg_clk;
	}

	/*
	 * Power the D-PHY up.  It has to be on before the receiver is
	 * configured: it is what puts the sensor's lanes into a state the
	 * receiver can lock onto.
	 *
	 * Note this is only the *power* half of the PHY lifecycle.  phy_init()
	 * is deliberately NOT here: it is a lifetime operation, paired with
	 * phy_exit() at remove, and this function runs again on every resume.
	 * See the note on the phy_init() call in probe.
	 *
	 * priv->phy is NULL when the node has no "phys", so a board that wires
	 * the PHY some other way still works.
	 */
	if (priv->phy) {
		ret = phy_power_on(priv->phy);
		if (ret) {
			dev_err(priv->dev, "failed to power on D-PHY: %d\n",
				ret);
			goto err_deassert_rst;
		}
	}

	/* Gate off the parallel sensor clock until a stream is running. */
	writel(0, priv->regs + SENINF_TOP_CTRL);

	/*
	 * Restore CSI2_CTRL to its documented reset value, then clear the
	 * read-only status bits back to zero, set the D-Lane enables for this
	 * receiver's configured lane count, and only then set CSI2_EN.  The
	 * upper half of CSI2_CTRL is read-only status, so only the lower half
	 * is written.
	 *
	 * The order matters: the lanes are enabled before the receiver that
	 * uses them, and both come from priv->dlane_bits, which was resolved
	 * from the DT in probe.  The documented reset value 0x2d80 has
	 * LP11_RST_EN (bit 7) and SYNC_RST_EN (bit 8) set and every DLANE*_EN
	 * (bits 3:1) and CSI2_EN (bit 0) clear, so the lane and receiver bits
	 * are set by this driver rather than inherited from reset.
	 *
	 * This used to derive the lane word from a CSI2_DBG readback instead,
	 * which produced a meaningless value and, at zero, left the receiver
	 * enabled with no data lane enabled at all.  See the note on
	 * mtk_csi2_rx_lane_bits() for the bit positions and why there is
	 * nothing here that can be read back.
	 */
	csi2_write(priv, CSI2_CTRL, CSI2_CTRL_RESET & CSI2_CTRL_RW);

	csi2_update_bits(priv, CSI2_CTRL, CSI2_LANE_MASK, priv->dlane_bits);

	csi2_update_bits(priv, CSI2_CTRL, CSI2_EN, CSI2_EN);

	/* Clear any stale latched error status from a previous run. */
	csi2_write(priv, CSI2_INTSTA, CSI2_INTSTA_ERR);

	return 0;

err_deassert_rst:
	if (priv->seninf_rst)
		reset_control_assert(priv->seninf_rst);
err_tg_clk:
	if (priv->seninf_tg_clk)
		clk_disable_unprepare(priv->seninf_tg_clk);
err_csi2_clk:
	if (priv->seninf_csi2_clk)
		clk_disable_unprepare(priv->seninf_csi2_clk);
err_seninf_clk:
	if (priv->seninf_clk)
		clk_disable_unprepare(priv->seninf_clk);
	return ret;
}
static void mtk_csi2_rx_power_off(struct mtk_csi2_rx *priv)
{
	/*
	 * Put the receiver back into its hardware reset state and re-assert
	 * the top-level reset, then drop the clocks in reverse order.
	 */
	csi2_update_bits(priv, CSI2_CTRL, CSI2_EN, 0);

	if (priv->phy)
		phy_power_off(priv->phy);

	if (priv->seninf_rst)
		reset_control_assert(priv->seninf_rst);
	if (priv->seninf_tg_clk)
		clk_disable_unprepare(priv->seninf_tg_clk);
	if (priv->seninf_csi2_clk)
		clk_disable_unprepare(priv->seninf_csi2_clk);
	if (priv->seninf_clk)
		clk_disable_unprepare(priv->seninf_clk);
}

static int mtk_csi2_rx_runtime_suspend(struct device *dev)
{
	struct mtk_csi2_rx *priv = dev_get_drvdata(dev);

	mtk_csi2_rx_power_off(priv);

	return 0;
}

static int mtk_csi2_rx_runtime_resume(struct device *dev)
{
	struct mtk_csi2_rx *priv = dev_get_drvdata(dev);

	return mtk_csi2_rx_power_on(priv);
}

static const struct dev_pm_ops mtk_csi2_rx_pm_ops = {
	.runtime_suspend = mtk_csi2_rx_runtime_suspend,
	.runtime_resume = mtk_csi2_rx_runtime_resume,
	};

/* ------------------------------------------------------------------ */
/* Stream / control                                                  */
/* ------------------------------------------------------------------ */

static int mtk_csi2_rx_start_stream(struct mtk_csi2_rx *priv)
{
	int ret;

	lockdep_assert_held(&priv->lock);

	/*
	 * Take the runtime-PM reference FIRST.  Everything below touches
	 * registers, and this block's registers only exist once
	 * runtime_resume() has run (it enables the clocks, releases the
	 * reset and initialises the D-PHY).  This used to write
	 * SENINF_TOP_CTRL before pm_runtime_resume_and_get(), i.e. a write to
	 * a powered-down block.
	 */
	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret)
		return ret;

	/*
	 * The parallel sensor clock must be running before the receiver is
	 * released, otherwise the first frame is missed.  mtk_csi2_rx_power_on()
	 * gated it off, so re-open it here.
	 */
	writel(SENINF_PCLK_EN(priv->port), priv->regs + SENINF_TOP_CTRL);

	/* Drain any pending error, then clear the status. */
	csi2_write(priv, CSI2_INTSTA, CSI2_INTSTA_ERR);

	return 0;
}

static void mtk_csi2_rx_stop_stream(struct mtk_csi2_rx *priv)
{
	lockdep_assert_held(&priv->lock);

	/*
	 * Tear the stream down while the block is still powered: gate the
	 * parallel sensor clock, then release the PM reference.  runtime_suspend()
	 * disables the receiver and asserts the reset, so doing it in this
	 * order means the gate-off happens inside the resume window instead
	 * of after the clocks have already gone away.
	 */
	writel(0, priv->regs + SENINF_TOP_CTRL);

	pm_runtime_put_autosuspend(priv->dev);
}

/*
 * Streaming entry point.
 *
 * The lock is taken here, not assumed.  The pad ops run under the state lock
 * through the core's v4l2_subdev_call wrappers, but call_s_stream() in
 * v4l2-subdev.c calls the driver callback directly with no state object and so
 * no lock of any kind; the lockdep_assert_held() further down used to assert a
 * mutex nobody held, which would fire the moment anything called this from a
 * context that had not taken it, and meanwhile left the cached format and the
 * register programming unserialised against a concurrent S_FMT.  Both use
 * priv->lock, so taking it here makes format changes and stream changes
 * mutually exclusive, which is what the assert below claims.
 *
 * A plain mutex is right: this path programs registers and calls
 * pm_runtime_resume_and_get(), so it sleeps and must not be in atomic context.
 */
static int mtk_csi2_rx_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mtk_csi2_rx *priv = sd_to_csi2rx(sd);
	int ret = 0;

	mutex_lock(&priv->lock);

	if (enable)
		ret = mtk_csi2_rx_start_stream(priv);
	else
		mtk_csi2_rx_stop_stream(priv);

	mutex_unlock(&priv->lock);

	return ret;
}

static const struct v4l2_subdev_video_ops mtk_csi2_rx_video_ops = {
	.s_stream = mtk_csi2_rx_s_stream,
};

static const struct v4l2_subdev_ops mtk_csi2_rx_subdev_ops = {
	.video = &mtk_csi2_rx_video_ops,
	.pad = &mtk_csi2_rx_pad_ops,
};

/* ------------------------------------------------------------------ */
/* Probe / registration                                               */
/* ------------------------------------------------------------------ */

static int mtk_csi2_rx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_csi2_rx *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	dev_set_drvdata(dev, priv);

	mutex_init(&priv->lock);

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return PTR_ERR(priv->regs);

	/*
	 * The DT spells out which receiver this node is.  Port 1 is
	 * SENINF2_*, port 0 is SENINF1_*.
	 */
	ret = of_property_read_u32(dev->of_node, "mediatek,seninf-port",
				   &priv->port);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing or invalid mediatek,seninf-port\n");
	if (priv->port >= SENINF_MAX_PORTS)
		return dev_err_probe(dev, -EINVAL,
				     "mediatek,seninf-port %u out of range (max %u)\n",
				     priv->port, SENINF_MAX_PORTS - 1);

	/*
	 * Resolve the data-lane count and its CSI2_CTRL[3:1] enable word now,
	 * before anything can program the receiver: power_on() runs at the end
	 * of probe and on every resume, and it needs the word to be valid.  A
	 * count this block cannot enable is a hard probe failure rather than a
	 * silently wrong programming; see mtk_csi2_rx_lane_count() for why
	 * there is no register that could be read back instead.
	 */
	ret = mtk_csi2_rx_parse_lanes(priv);
	if (ret)
		return ret;

	/*
	 * These three gates have no entry in the MT6589 clock bindings: the
	 * data sheet documents SENINF at 0x15008000 but does not describe the
	 * CAM_TG_CG register that would hold them, so their bit positions
	 * cannot be derived.  Take them as optional rather than inventing
	 * gate bits - a wrong bit could disable a block something else is
	 * using.  Where the gate is absent the driver simply runs the
	 * receiver off whatever clock state it finds; the ports still
	 * program correctly and the block can be brought up once the real
	 * gate positions are read from hardware.
	 */
	priv->seninf_clk = devm_clk_get_optional(dev, "seninf");
	if (IS_ERR(priv->seninf_clk))
		return dev_err_probe(dev, PTR_ERR(priv->seninf_clk),
				     "Failed to get seninf clock\n");

	priv->seninf_csi2_clk = devm_clk_get_optional(dev, "csi2");
	if (IS_ERR(priv->seninf_csi2_clk))
		return dev_err_probe(dev, PTR_ERR(priv->seninf_csi2_clk),
				     "Failed to get csi2 clock\n");

	priv->seninf_tg_clk = devm_clk_get_optional(dev, "tg_grp");
	if (IS_ERR(priv->seninf_tg_clk))
		return dev_err_probe(dev, PTR_ERR(priv->seninf_tg_clk),
				     "Failed to get tg_grp clock\n");

	/*
	 * Get the D-PHY this receiver sits on top of.  The MT6589 DSI does
	 * the same for its TX PHY (phys = <&mipi_tx0>; phy-names = "dphy"),
	 * so the two are consistent.  A missing "phys" is not fatal: some
	 * boards wire the PHY in a way this driver does not manage.
	 */
	priv->phy = devm_of_phy_get_by_index(dev, dev->of_node, 0);
	if (IS_ERR(priv->phy)) {
		dev_warn(dev,
			 "no D-PHY found, the receiver will be programmed but its lanes will stay idle\n");
		priv->phy = NULL;
	}

	/*
	 * Initialise the PHY once, here, and release it once in .remove.
	 *
	 * phy_init()/phy_exit() are a *lifetime* bracket, not a power
	 * cycle: every in-tree CSI-2 receiver pairs them that way
	 * (dw-mipi-csi2rx.c, rkisp1-csi.c, sun6i-mipi-csi2.c all call
	 * phy_init() from probe/setup and phy_exit() from remove/cleanup).
	 * This used to call phy_init() from mtk_csi2_rx_power_on(), which
	 * runs again on every resume and again on every stream start, and
	 * which had no matching phy_exit() at all.  Each of those calls
	 * re-initialised the PHY without ever releasing it, so the init/exit
	 * lifecycle was unbalanced from the first suspend/resume onwards.
	 * Per-resume power is phy_power_on()/phy_power_off(), which is what
	 * power_on()/power_off() already do correctly.
	 *
	 * Note the PHY is currently absent -- there is no "phys" and no D-PHY
	 * node in the DT, and no in-tree provider for it -- so this is
	 * scaffolding: today every call below is guarded by priv->phy being
	 * non-NULL and does not execute.  It is written so that it is correct
	 * the moment a real PHY provider and DT node exist, rather than leaving
	 * the unbalanced init to be discovered on hardware.
	 */
	if (priv->phy) {
		ret = phy_init(priv->phy);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to init D-PHY\n");
	}

	/*
	 * The sensor-front-end reset is OPTIONAL, deliberately.
	 *
	 * This used to be devm_reset_control_get(), which made the DT's
	 * "resets" property mandatory and therefore made probe fail on the real
	 * target: the camera dtsi has the reset commented out, because the
	 * provider does not exist yet.  Requiring it produced a driver that
	 * could not probe on its own board, which is a worse outcome than
	 * bringing the block up without a reset.
	 *
	 * Why the property is still absent, rather than something to restore:
	 * the data sheet places img_rst in WDT_SWSYSRST at bit 5, and that
	 * register is at 0x10000000 + 0x18 (draft/ds/ovl.txt:55062 and
	 * 55144 -- "img_rst, writes 1 to reset imgsys and its related pad
	 * macro (cam, mipi_rx)").  The vendor tree agrees:
	 * aquaris-5/mediatek/platform/mt6589/kernel/core/include/mach/mt_wdt.h:63
	 *
	 *	#define MTK_WDT_SWSYS_RST_IMG_RST	(0x0020)
	 *
	 * 0x0020 is bit 5 of the same register.  But WDT_SWSYSRST lives in the
	 * WDT/AP_RGU block at 0xF0000000 (or 0x10000000 as mapped by the boot
	 * ROM), NOT in topckgen.  The commented-out line named
	 * "&topckgen MT6589_TOPRGU_IMG_RST", and topckgen in this tree is
	 * syscon@10000100 -- a different node, which declares only
	 * #clock-cells and no #reset-cells at all.  So the name the DTS used
	 * was never going to resolve, and restoring it as written would not
	 * build, let alone name the right reset.
	 *
	 * Guessing an index for topckgen on the strength of a bit position
	 * documented for a different register is exactly the kind of invention
	 * this driver refuses elsewhere (see the note on the seninf clocks
	 * above).  So the contract is made consistent in the direction that
	 * works today: the reset is optional here, the DT omits it, and the
	 * binding says so.
	 *
	 * The honest trade-off: with no reset, whatever state the imgsys/mipi_rx
	 * registers were left in is NOT cleared before this driver programs
	 * them.  On a cold boot the block is already at its documented reset
	 * values and this is harmless.  After a warm reboot, or if another
	 * driver in imgsys ran first and left the receivers enabled, stale
	 * state can survive, and mtk_csi2_rx_power_on() restores the registers
	 * it cares about (CSI2_CTRL, CSI2_INTSTA) but cannot claim the rest are
	 * clean.  Add the property once a reset provider for WDT_SWSYSRST bit 5
	 * exists; this driver needs no change to pick it up.
	 */
	priv->seninf_rst = devm_reset_control_get_optional(dev, NULL);
	if (IS_ERR(priv->seninf_rst)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->seninf_rst),
				    "Failed to get reset control\n");
		goto err_phy_exit;
	}

	if (!priv->seninf_rst)
		dev_warn(dev,
			 "no reset supplied, the receiver will be brought up without clearing imgsys reset state\n");

	priv->sd.internal_ops = &mtk_csi2_rx_internal_ops;

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
	v4l2_subdev_init(&priv->sd, &mtk_csi2_rx_subdev_ops);
	strscpy(priv->sd.name, dev_name(dev), sizeof(priv->sd.name));

	/*
	 * sd.dev is what makes this subdev matchable.  See the note in
	 * mtk-scam.c: v4l2_async_register_subdev() only derives sd->fwnode from
	 * dev_fwnode(sd->dev) when sd->dev is set, so without it the fwnode
	 * stays NULL and v4l2_async_find_match() skips this receiver
	 * entirely.  For this node in particular that would silently cost the
	 * receiver -> SCAM link, since the receiver is the source side of the
	 * only endpoint the graph has wired up on this port.
	 */
	priv->sd.dev = dev;

	/*
	 * The pad ops run under this mutex (v4l2_subdev_init_finalize() makes
	 * the active state use it as its own lock), so every lockdep_assert_held()
	 * in them is about the lock taken here.  v4l2_subdev_init() does not
	 * touch state_lock, so this is still the driver's to set.
	 */
	priv->sd.state_lock = &priv->lock;

	/*
	 * The receiver unpacks packets and repasses them unchanged, so in the
	 * media graph it is an interface bridge, not an ISP or a sensor.  Every
	 * in-tree CSI-2 receiver for the same reason uses this function:
	 * cdns-csi2rx.c, rkisp1-csi.c, dw-mipi-csi2rx.c and sun6i-mipi-csi2.c
	 * all set MEDIA_ENT_F_VID_IF_BRIDGE.
	 */
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;

	/*
	 * Create the pads before v4l2_subdev_init_finalize() below, which is
	 * what allocates the active state: __v4l2_subdev_state_alloc() only
	 * allocates the legacy state->pads array when entity.num_pads is
	 * non-zero, so without these two calls init_state() below would have had
	 * nothing to seed and every v4l2_subdev_state_get_format() in the pad
	 * ops would have returned NULL.
	 *
	 * The array order is the pad index; the DT's seninf_out is port@1,
	 * which matches CSI2_PAD_SRC.
	 */
	priv->pads[CSI2_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	priv->pads[CSI2_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&priv->sd.entity, CSI2_PAD_NUM,
				     priv->pads);
	if (ret) {
		ret = dev_err_probe(dev, ret,
				    "failed to init entity pads\n");
		goto err_phy_exit;
	}

	ret = v4l2_subdev_init_finalize(&priv->sd);
	if (ret) {
		ret = dev_err_probe(dev, ret, "subdev init error\n");
		goto err_phy_exit;
	}

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret)
		goto err_subdev_cleanup;

	pm_runtime_set_active(dev);

	/*
	 * Start the runtime-PM framework.  This is mandatory, not cosmetic:
	 * mtk_csi2_rx_start_stream() takes its reference with
	 * pm_runtime_resume_and_get(), and on a device whose PM state machine
	 * was never enabled that call returns -EAGAIN because the framework is
	 * disabled.  The whole receiver depends on that call to enable the
	 * clocks, release the reset and initialise the D-PHY, so without this
	 * the driver could never start a stream, and the probe-time
	 * pm_runtime_get_noresume()/pm_runtime_put_autosuspend() pair below
	 * would not balance either.
	 *
	 * pm_runtime_set_active() above only marks the device as already on;
	 * pm_runtime_enable() is what makes the state machine live.  The order
	 * matters: set_active() is meaningless once the device is enabled and
	 * before it has been given an initial state.
	 */
	pm_runtime_enable(dev);

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);

	/*
	 * Reference accounting for probe:
	 *
	 *  - pm_runtime_get_noresume() takes one temporary reference purely so
	 *    we can bring the hardware up here and verify it.  It is not a
	 *    "keep it powered" reference: it exists only for the duration of
	 *    this function.
	 *  - mtk_csi2_rx_power_on() brings the receiver up.
	 *  - On success we drop that temporary reference again with
	 *    pm_runtime_put_autosuspend(), which returns the usage counter to
	 *    zero and arms the autosuspend timer, so the receiver really does
	 *    power off shortly after probe and the first s_stream() pays for
	 *    its own power-up via pm_runtime_resume_and_get().  This mirrors
	 *    what the other receiver drivers do: probe leaves the device
	 *    quiesced but verified.
	 *
	 * Leaving the probe reference in place would pin the usage counter at
	 * >= 1 forever, autosuspend could never fire, and the receiver would
	 * stay powered for the whole life of the system.
	 */
	pm_runtime_get_noresume(dev);

	ret = mtk_csi2_rx_power_on(priv);
	if (ret)
		goto error_pm;

	dev_info(dev, "registered CSI-2 receiver %u\n", priv->port);

	/*
	 * Drop the probe-time reference: mark the block as just-used and let
	 * the autosuspend timer take it back down.  pm_runtime_put_autosuspend()
	 * is used (not plain pm_runtime_put()) so the last-access timestamp is
	 * refreshed and the drop routes straight to the autosuspend work, which
	 * is the same release the per-stream stop path uses.
	 */
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;

error_pm:
	/*
	 * mtk_csi2_rx_power_on() failed, so nothing is left running: its
	 * err_ labels have already undone the clocks and reset, and the only
	 * step that could have left the D-PHY up (phy_power_on()) is itself
	 * the last fallible one, so reaching here means it never came up.
	 * There is consequently no power_off() to do here.  Drop the
	 * temporary reference, then undo pm_runtime_enable().  Disable before
	 * dropping the reference: pm_runtime_use_autosuspend() is armed above,
	 * so the put taking the count to zero could otherwise fire
	 * mtk_csi2_rx_runtime_suspend() while probe is still unwinding.
	 */
	pm_runtime_disable(dev);
	pm_runtime_put(dev);
	v4l2_async_unregister_subdev(&priv->sd);
	v4l2_subdev_cleanup(&priv->sd);
	goto err_phy_exit;

err_subdev_cleanup:
	/*
	 * Registration failed before any runtime-PM state existed, so there is
	 * no PM reference to drop and no pm_runtime_disable() to undo -- that
	 * machinery is only set up further down.  The active state allocated by
	 * v4l2_subdev_init_finalize() does have to go, and the pads are in the
	 * devm allocation and go with it.
	 */
	v4l2_subdev_cleanup(&priv->sd);

err_phy_exit:
	/*
	 * Close the PHY lifetime opened by the phy_init() above.  Every probe
	 * failure from that point on lands here, so the init/exit pair stays
	 * balanced whether probe fails early (reset control, pads,
	 * init_finalize) or late (async registration, power_on).
	 */
	if (priv->phy)
		phy_exit(priv->phy);

	return ret;
}

static void mtk_csi2_rx_remove(struct platform_device *pdev)
{
	struct mtk_csi2_rx *priv = dev_get_drvdata(&pdev->dev);

	/*
	 * Close the PHY lifetime that probe opened with phy_init().  Without
	 * this the PHY was initialised at probe and never released; see the
	 * long note on the phy_init() call above.
	 *
	 * The receiver must not be streaming at this point, so there is no
	 * power_off() to do here: runtime PM owns the power state, and remove
	 * only runs once the device has been unbound and the last reference
	 * dropped.  The subdev state allocated by v4l2_subdev_init_finalize()
	 * is released here too, since it is not a devm allocation.
	 *
	 * This driver requests no IRQ, so there is no free_irq() to order
	 * against; if one is added, it must be freed BEFORE the phy_exit() and
	 * before runtime PM is disabled below, so that no handler can run
	 * against a powered-down block.
	 */
	if (priv->phy)
		phy_exit(priv->phy);

	v4l2_async_unregister_subdev(&priv->sd);
	v4l2_subdev_cleanup(&priv->sd);

	/*
	 * probe enabled runtime PM (pm_runtime_enable()).  Disable it before
	 * the device disappears so the PM core is not left holding a state
	 * machine for a device that no longer exists; the devm cleanup then runs
	 * with PM off and cannot fire a resume.
	 */
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id mtk_csi2_rx_of_match[] = {
	{ .compatible = "mediatek,mt6589-csi2-rx" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_csi2_rx_of_match);

static struct platform_driver mtk_csi2_rx_driver = {
	.probe = mtk_csi2_rx_probe,
	.remove = mtk_csi2_rx_remove,
	.driver = {
		.name = "mtk-csi2-rx",
		.of_match_table = mtk_csi2_rx_of_match,
		.pm = &mtk_csi2_rx_pm_ops,
	},
};
module_platform_driver(mtk_csi2_rx_driver);

MODULE_DESCRIPTION("MediaTek MT6589 CSI-2 receiver");
MODULE_AUTHOR("Lenovo Linux Team");
MODULE_LICENSE("GPL");
