// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 CAM/ISP top-level (control) driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * THIS DRIVER IS A SURVEY-LEVEL SCAFFOLD, NOT A WORKING ISP.
 *
 * What exists
 * -----------
 * The MT6589 CAM block is the ISP.  Its control registers are at 0x15004000
 * and are documented in the data sheet; the ones this driver uses are
 * transcribed into mtk-cam.h.  What is implemented here is the start/stop
 * sequencing and the sub-module enable bits, because those are the parts
 * with documented field positions and a self-evident order.
 *
 * What does not exist, and why
 * ----------------------------
 * The image processing itself - the CPIPE stages, the pixel-rate meters,
 * the memory sequencer, the interrupt controllers - is a large register
 * space with no vendor reference driver in the available tree and no way to
 * test.  There is no upstream MediaTek ISP driver to port from either: the
 * closest is the mt8167 ISP, which is a different architecture with
 * different register names.
 *
 * Writing a plausible-looking ISP that cannot be validated would be worse
 * than writing none, so this driver deliberately stops at the control
 * plane and says so.  The register map is the recoverable part and it is
 * recorded here for whoever writes the rest.
 *
 * Treat this as documentation that happens to compile.  Loading it will not
 * produce a picture.
 */

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <media/v4l2-subdev.h>

#include "mtk-cam.h"

static inline void cam_write(struct mtk_cam *cam, u32 reg, u32 val)
{
	writel(val, cam->regs + reg);
}

static inline u32 cam_read(struct mtk_cam *cam, u32 reg)
{
	return readl(cam->regs + reg);
}

/*
 * Bring the control plane up or down.
 *
 * The documented order is: assert the module resets, release them, enable
 * the sub-modules, then set the start bit.  On the way down the reverse.
 *
 * TODO(unverified): the data sheet gives the bit positions but no
 * programming sequence, so the ordering here is inferred from the register
 * names.  It has not been run against hardware.
 */
static int mtk_cam_start(struct mtk_cam *cam)
{
	lockdep_assert_held(&cam->lock);

	/*
	 * Reset, then release.  CAM_SW_CTL_ALL writes every bit, which is
	 * only correct if every bit is a reset; that has not been
	 * confirmed, so the release half is what actually matters.
	 */
	cam_write(cam, CAM_CTL_SW_CTL, CAM_SW_CTL_ALL);
	cam_write(cam, CAM_CTL_SW_CTL, 0);

	/*
	 * Enable the sub-modules.  EN1/EN2 are write-only shadows with
	 * separate set/clear registers, so they are driven that way and
	 * never written directly.
	 *
	 * TODO: the individual sub-module bit positions are not yet
	 * recovered from the data sheet; see the header.  Until they are,
	 * nothing is enabled and the ISP cannot run, which is why this
	 * driver does not claim to be functional.
	 */
	// cam_write(cam, CAM_CTL_EN1_SET, CAM_CTL_EN1_MEM_IN);

	/* Start. */
	cam_write(cam, CAM_CTL_START, CAM_CTL_START_PASS2B_START |
		  CAM_CTL_START_PASS2_STARTC);

	cam->streaming = true;

	return 0;
}

static int mtk_cam_stop(struct mtk_cam *cam)
{
	lockdep_assert_held(&cam->lock);

	cam->streaming = false;

	cam_write(cam, CAM_CTL_START, 0);
	cam_write(cam, CAM_CTL_EN1_CLR, CAM_SW_CTL_ALL);
	cam_write(cam, CAM_CTL_EN2_CLR, CAM_SW_CTL_ALL);

	return 0;
}

static int mtk_cam_s_stream(struct v4l2_subdev *sd, int on)
{
	struct mtk_cam *cam = to_mtk_cam(sd);
	int ret = 0;

	lockdep_assert_held(&cam->lock);

	if (on == cam->streaming)
		goto out_unlock;

	if (on)
		ret = mtk_cam_start(cam);
	else
		ret = mtk_cam_stop(cam);

out_unlock:
	return ret;
}

static const struct v4l2_subdev_video_ops mtk_cam_video_ops = {
	.s_stream = mtk_cam_s_stream,
};

static const struct v4l2_subdev_ops mtk_cam_subdev_ops = {
	.video = &mtk_cam_video_ops,
};

static int mtk_cam_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_cam *cam;
	int ret;

	cam = devm_kzalloc(dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->dev = dev;
	dev_set_drvdata(dev, cam);
	mutex_init(&cam->lock);

	cam->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cam->regs))
		return PTR_ERR(cam->regs);

	cam->sd.state_lock = &cam->lock;
	cam->sd.ops = &mtk_cam_subdev_ops;

	/*
	 * CAM_CTL_START resets to 0.  Checking it catches a wrong reg
	 * property at probe rather than at first frame.
	 */
	ret = cam_read(cam, CAM_CTL_START);
	if (ret)
		dev_warn(dev,
			 "CAM_CTL_START reads 0x%08x, expected 0 on reset; check the reg property\n",
			 ret);

	ret = v4l2_async_register_subdev(&cam->sd);
	if (ret)
		return ret;

	dev_warn(dev,
		 "registering a survey-level CAM/ISP scaffold; no image pipeline is implemented\n");

	return 0;
}

static const struct of_device_id mtk_cam_of_match[] = {
	{ .compatible = "mediatek,mt6589-cam" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_cam_of_match);

static struct platform_driver mtk_cam_driver = {
	.probe = mtk_cam_probe,
	.driver = {
		.name = "mtk-cam",
		.of_match_table = mtk_cam_of_match,
	},
};
module_platform_driver(mtk_cam_driver);

MODULE_DESCRIPTION("MediaTek MT6589 CAM/ISP control plane (scaffold)");
MODULE_AUTHOR("Lenovo Linux Team");
MODULE_LICENSE("GPL");