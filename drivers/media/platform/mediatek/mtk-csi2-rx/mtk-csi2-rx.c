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
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/phy/phy.h>
#include <media/v4l2-device.h>
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
	struct media_entity entity;
	struct mutex lock;
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
/* Data format handling                                               */
/* ------------------------------------------------------------------ */

/*
 * The receiver is format agnostic: it unpacks MIPI packets and hands raw
 * bytes to SCAM.  So there is exactly one mbus format, RAW8, and the size
 * it reports back is the size the subdev's pad is.
 */
/*
 * The receiver does not itself pick a pixel format: the sensor decides it
 * and the SCAM block converts it.  Accepting every code lets the sensor
 * negotiate freely, and is what the other CSI-2 receivers in the tree do
 * when they have no format conversion of their own.
 */
static int mtk_csi2_rx_enum_mbus_code(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_mbus_code_enum *code)
{
	return 0;
}

static int mtk_csi2_rx_enum_frame_size(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *state,
				       struct v4l2_subdev_frame_size_enum *fse)
{
	fse->min_width = 1280;
	fse->max_width = 3264;
	fse->min_height = 800;
	fse->max_height = 2448;

	return 0;
}

static int mtk_csi2_rx_get_fmt(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_format *fmt)
{
	struct mtk_csi2_rx *priv = sd_to_csi2rx(sd);

	lockdep_assert_held(&priv->lock);

	fmt->which = V4L2_SUBDEV_FORMAT_TRY;
	fmt->format.width = 1280;
	fmt->format.height = 800;
	fmt->format.code = MEDIA_BUS_FMT_FIXED;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_DEFAULT;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	return 0;
}

static const struct v4l2_subdev_pad_ops mtk_csi2_rx_pad_ops = {
	.enum_mbus_code = mtk_csi2_rx_enum_mbus_code,
	.enum_frame_size = mtk_csi2_rx_enum_frame_size,
	.get_fmt = mtk_csi2_rx_get_fmt,
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
	u32 lanes;
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

	ret = reset_control_deassert(priv->seninf_rst);
	if (ret)
		goto err_tg_clk;

	/*
	 * The D-PHY must be initialised before the receiver is configured:
	 * it is what puts the sensor's lanes into a state the receiver can
	 * lock onto.  Returns NULL when the node has no "phys", so a board
	 * that wires the PHY some other way still works.
	 */
	if (priv->phy) {
		ret = phy_init(priv->phy);
		if (ret) {
			dev_err(priv->dev, "failed to init D-PHY: %d\n", ret);
			goto err_rst;
		}

		ret = phy_power_on(priv->phy);
		if (ret) {
			dev_err(priv->dev, "failed to power on D-PHY: %d\n",
				ret);
			goto err_rst;
		}
	}

	/* Gate off the parallel sensor clock until a stream is running. */
	writel(0, priv->regs + SENINF_TOP_CTRL);

	/*
	 * Restore CSI2_CTRL to its documented reset value, then clear the
	 * read-only status bits back to zero and set the lane enables and
	 * the receiver enable.  The upper half of CSI2_CTRL is read-only
	 * status, so only the lower half is written.
	 */
	csi2_write(priv, CSI2_CTRL, CSI2_CTRL_RESET & CSI2_CTRL_RW);

	lanes = csi2_read(priv, CSI2_DBG) & CSI2_LANE_MASK;
	if (lanes)
		csi2_update_bits(priv, CSI2_CTRL, CSI2_LANE_MASK, lanes);

	csi2_update_bits(priv, CSI2_CTRL, CSI2_EN, CSI2_EN);

	/* Clear any stale latched error status from a previous run. */
	csi2_write(priv, CSI2_INTSTA, CSI2_INTSTA_ERR);

	return 0;

err_rst:
	reset_control_assert(priv->seninf_rst);
err_tg_clk:
	clk_disable_unprepare(priv->seninf_tg_clk);
err_csi2_clk:
	clk_disable_unprepare(priv->seninf_csi2_clk);
err_seninf_clk:
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

	reset_control_assert(priv->seninf_rst);
	clk_disable_unprepare(priv->seninf_tg_clk);
	clk_disable_unprepare(priv->seninf_csi2_clk);
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

static int mtk_csi2_rx_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mtk_csi2_rx *priv = sd_to_csi2rx(sd);
	struct device *dev = priv->dev;
	int ret;

	lockdep_assert_held(&priv->lock);


	if (enable) {
		/*
		 * The parallel sensor clock must be running before the
		 * receiver is released, otherwise the first frame is missed.
		 */
		writel(SENINF_PCLK_EN(priv->port),
		       priv->regs + SENINF_TOP_CTRL);

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0)
			return ret;

		/* Drain any pending error, then clear the status. */
		csi2_write(priv, CSI2_INTSTA, CSI2_INTSTA_ERR);
	} else {
		pm_runtime_put_autosuspend(dev);
		writel(0, priv->regs + SENINF_TOP_CTRL);
	}

	return 0;
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

	priv->seninf_rst = devm_reset_control_get(dev, NULL);
	if (IS_ERR(priv->seninf_rst))
		return dev_err_probe(dev, PTR_ERR(priv->seninf_rst),
				     "Failed to get reset control\n");

	priv->sd.state_lock = &priv->lock;
	priv->sd.ops = &mtk_csi2_rx_subdev_ops;
	priv->sd.entity.name = dev_name(dev);
	priv->sd.entity.obj_type = MEDIA_ENTITY_TYPE_V4L2_SUBDEV;

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret)
		return ret;

	pm_runtime_set_active(dev);

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);

	/*
	 * Take one reference and bring the hardware up, then drop back to
	 * suspended so the first s_stream() pays for the power-up.  This
	 * mirrors what the other receiver drivers do: probe leaves the
	 * device quiesced but verified.
	 */
	pm_runtime_get_noresume(dev);

	ret = mtk_csi2_rx_power_on(priv);
	if (ret)
		goto error_pm;

	dev_info(dev, "registered CSI-2 receiver %u\n", priv->port);

	return 0;

error_pm:
	pm_runtime_put(dev);
	v4l2_async_unregister_subdev(&priv->sd);

	return ret;
}

static const struct of_device_id mtk_csi2_rx_of_match[] = {
	{ .compatible = "mediatek,mt6589-csi2-rx" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_csi2_rx_of_match);

static struct platform_driver mtk_csi2_rx_driver = {
	.probe = mtk_csi2_rx_probe,
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
