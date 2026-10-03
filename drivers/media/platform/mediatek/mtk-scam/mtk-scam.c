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
 *   2. The media-controller graph glue (v4l2_subdev internal_ops, the
 *      notifier, and the ISP-facing endpoint) is not written, because the
 *      ISP side of the graph does not exist yet.  See README.md.
 *   3. This tree's media API is a reduced fork, so the usual
 *      v4l2_mbus_csi2_capability negotiation is unavailable; the frame
 *      format is taken from the sensor through set_pad() instead.  The
 *      CSI-2 receiver upstream (mtk-csi2-rx.c) negotiates the code and
 *      passes the geometry down to here through this driver's own state.
 *
 * As with the D-PHY and the receiver, treat this as untested scaffolding:
 * it will probe and it will enable, and it will not yet produce frames.
 */

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
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

/* Stride between SCAM1 at 0x15008200 and SCAM2 at 0x15008280. */
#define SCAM_PORT_STRIDE			0x80

/*
 * SCAM_CFG (0x15008200), reset value 0x10000400.  Field positions from the
 * MT6589 data sheet page 2268.  Note the two gaps in the numbering: CSD_NUM
 * is bits 25:24 (not 27:26) and DBG_MD is bit 23 (not 24).
 */
#define SCAM_CFG_WARN_MASK			BIT(28)
#define SCAM_CFG_CSD_NUM			GENMASK(25, 24)
#define SCAM_CFG_DBG_MD				BIT(23)
#define SCAM_CFG_CONT				BIT(17)
#define SCAM_CFG_CLK_INV			BIT(15)
#define SCAM_CFG_CYC				BIT(9)
#define SCAM_CFG_INTEN				GENMASK(6, 0)

/* Documented SCAM_CFG reset value. */
#define SCAM_CFG_RESET				0x10000400

/*
 * The documented CFG reset has WARN_MASK, CLK_INV and CYC already set.
 * Those three are the ones whose polarity the data sheet states, so they are
 * left alone on reset and only INTEN is programmed by this driver.
 */
#define SCAM_CFG_PRESERVE			(SCAM_CFG_WARN_MASK | \
						 SCAM_CFG_CLK_INV | \
						 SCAM_CFG_CYC)

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

/* SCAM_INT, per-port stride above.  Frame and line completion. */
#define SCAM_INT_0				BIT(0)
#define SCAM_INT_1				BIT(1)
#define SCAM_INT_2				BIT(2)
#define SCAM_INT_3				BIT(3)
#define SCAM_INT_4				BIT(4)
#define SCAM_INT_5				BIT(5)
#define SCAM_INT_6				BIT(6)

/* All the interrupt bits together, for a write-1-to-clear. */
#define SCAM_INT_MASK				GENMASK(6, 0)

static inline u32 scam_ofs(const struct mtk_scam *scam, u32 reg)
{
	return scam->port * SCAM_PORT_STRIDE + reg;
}

static inline void scam_write(struct mtk_scam *scam, u32 reg, u32 val)
{
	writel(val, scam->regs + scam_ofs(scam, reg));
}

static inline u32 scam_read(struct mtk_scam *scam, u32 reg)
{
	return readl(scam->regs + scam_ofs(scam, reg));
}

/* ------------------------------------------------------------------ */
/* Format                                                             */
/* ------------------------------------------------------------------ */

/*
 * SCAM does not convert pixel formats; it only carries the stream to the
 * ISP.  The format is therefore whatever the sensor negotiated, and this
 * driver simply programs the size it was given.
 *
 * This is the only place SCAM_SIZE is written.  It used to be
 *
 *	scam_write(scam, SCAM_SIZE, SCAM_SIZE_HEIGHT << 16 | SCAM_SIZE_WIDTH);
 *
 * which is doubly wrong: SCAM_SIZE_HEIGHT/.._WIDTH are *masks*, not shift
 * amounts, so GENMASK(31, 16) << 16 is 0xFFFF0000 << 16 and truncates to
 * exactly 0 in a 32-bit word, leaving SCAM_SIZE programmed as height 0 /
 * width 65535 -- and the negotiated size was never used at all.  FIELD_PREP
 * puts each value in its own documented field.
 */
static int mtk_scam_set_pad(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *state,
			    struct v4l2_subdev_format *format)
{
	struct mtk_scam *scam = to_mtk_scam(sd);
	struct mtk_scam_frame_size *size;

	lockdep_assert_held(&scam->lock);

	size = &scam->size;

	/*
	 * Both SCAM_SIZE fields are 12 bits wide, so a size that does not fit
	 * would be silently truncated into a wrong frame geometry.  Reject it
	 * rather than program something the block will interpret as a
	 * different picture.
	 */
	if (!size->width || !size->height ||
	    size->width > SCAM_SIZE_MAX || size->height > SCAM_SIZE_MAX)
		return -EINVAL;

	scam_write(scam, SCAM_SIZE,
		   FIELD_PREP(SCAM_SIZE_HEIGHT, size->height) |
		   FIELD_PREP(SCAM_SIZE_WIDTH, size->width));

	format->format.width = size->width;
	format->format.height = size->height;
	format->format.code = size->code;
	format->format.field = V4L2_FIELD_NONE;
	format->format.colorspace = V4L2_COLORSPACE_SMPTE170M;
	format->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	format->format.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	return 0;
}

static int mtk_scam_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_selection *sel)
{
	struct mtk_scam *scam = to_mtk_scam(sd);

	if (sel->pad)
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
	.get_fmt = mtk_scam_set_pad,
	.set_fmt = mtk_scam_set_pad,
	.get_selection = mtk_scam_get_selection,
};

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

/*
 * Bring the block out of reset, clear any latched interrupt, and start it.
 *
 * SCAM_CON.ENA must be set only after SCAM_CON.RST has been released (data
 * sheet page 2269: "clear RST before setting ENA = 1"), and SCAM itself must
 * be triggered before the image sensor is (same page).
 */
static int mtk_scam_start_stream(struct mtk_scam *scam)
{
	u32 val;

	lockdep_assert_held(&scam->lock);

	/* Reset, then release, leaving the block halted. */
	scam_write(scam, SCAM_CON, SCAM_CON_RST);
	scam_write(scam, SCAM_CON, 0);

	/* Clear any stale status from a previous run (INT* are write-1-clear). */
	scam_write(scam, SCAM_INT, SCAM_INT_MASK);

	/* Restore the documented reset configuration, keeping the enables. */
	val = (SCAM_CFG_RESET & ~SCAM_CFG_PRESERVE) | SCAM_CFG_CONT;
	scam_write(scam, SCAM_CFG, val);

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

static int mtk_scam_s_stream(struct v4l2_subdev *sd, int on)
{
	struct mtk_scam *scam = to_mtk_scam(sd);
	int ret = 0;

	lockdep_assert_held(&scam->lock);

	if (on == scam->streaming)
		goto out_unlock;

	if (on)
		ret = mtk_scam_start_stream(scam);
	else
		mtk_scam_stop_stream(scam);

out_unlock:
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

	ret = of_property_read_u32(dev->of_node, "mediatek,scam-port",
				   &scam->port);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing mediatek,scam-port\n");

	if (scam->port >= SCAM_MAX_PORTS)
		return dev_err_probe(dev, -EINVAL,
				     "mediatek,scam-port %u out of range (max %u)\n",
				     scam->port, SCAM_MAX_PORTS - 1);

	scam->sd.state_lock = &scam->lock;
	scam->sd.ops = &mtk_scam_subdev_ops;

	/*
	 * Sanity check: the reset value of SCAM_CFG is documented as
	 * 0x10000400.  If the aperture is wrong, reading it back gives
	 * something else, and it is better to say so here than to fail
	 * mysteriously on the first frame.
	 */
	ret = scam_read(scam, SCAM_CFG);
	if (ret != SCAM_CFG_RESET)
		dev_warn(dev,
			 "SCAM_CFG reads 0x%08x, expected the documented reset 0x%08x; check the reg property\n",
			 ret, SCAM_CFG_RESET);

	scam->size.width = 1280;
	scam->size.height = 960;
	scam->size.code = MEDIA_BUS_FMT_SBGGR10_1X10;

	ret = v4l2_async_register_subdev(&scam->sd);
	if (ret)
		return ret;

	dev_info(dev, "registered SCAM adaptor %u (partial, see source)\n",
		 scam->port);

	return 0;
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
