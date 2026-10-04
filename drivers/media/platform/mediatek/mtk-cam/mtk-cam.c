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
 * transcribed into mtk-cam.h.  What is implemented here is the software
 * reset handshake, the CAM block enable, and the start/stop strobes, because
 * those are the parts with documented field positions and a vendor
 * reference sequence.
 *
 * What does not exist, and why
 * ----------------------------
 * The image processing itself - the CPIPE stages, the pixel-rate meters,
 * the memory sequencer, the interrupt controllers - is a large register
 * space with no upstream MediaTek ISP driver to port (the closest is the
 * mt8167 ISP, a different architecture with different register names) and
 * no way to test.  Writing a plausible-looking ISP that cannot be
 * validated would be worse than writing none, so this driver deliberately
 * stops at the control plane and says so.
 *
 * Specifically, be clear about what probing this driver does and does not
 * mean:
 *
 *   - This subdev has ONE PAD, a sink, so it IS the downstream endpoint of
 *     the media graph: csi2-rx -> scam -> cam resolves end to end, and
 *     media-ctl can list the chain and the pads on it.
 *   - There is NO DMA and NO video node.  Nothing allocates buffers, so no
 *     frame is ever delivered to userspace, and the stream that arrives on
 *     the sink pad goes nowhere.  The pad describes what the block is wired
 *     to, not what it does with the picture.
 *   - There are NO pad operations (no get_fmt/set_fmt/enum_framesizes) and no
 *     internal_ops.  CAM does not negotiate or expose a format, so a graph
 *     walk that asks it for one gets -ENOIOCTLCMD.  That is expected for a
 *     block with no pipeline behind it; when the CPIPE stages exist, the
 *     format handling has to be written to go with them.
 *   - Only the CAM block enable is set; none of the CPIPE stage enables are
 *     programmed, because the pipeline that would use them is absent.
 *
 * So "the CAM driver probed" means only that a register window mapped and
 * reset cleanly, and that the graph endpoint exists.  It does NOT mean the
 * camera pipeline works.  The register map is the recoverable part and is
 * recorded here for whoever writes the rest.
 *
 * Treat this as documentation that happens to compile.  Loading it will not
 * produce a picture.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <media/media-entity.h>
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
 * Bounded wait for the software-reset status flag.
 *
 * SW_RST_ST is documented as "0: DMA is busy, 1: DMA is idle.  HW reset can
 * be done", so after triggering we poll it until the block reports it is
 * idle.  The register's reset value is 0x00000002, i.e. SW_RST_ST already
 * reads 1 out of reset, so a plain blocking poll could in principle spin
 * forever on a block that never asserts it.  Bound it.
 */
#define CAM_SW_RST_TIMEOUT_US	100000	/* 100 ms */
#define CAM_SW_RST_POLL_US	10

/*
 * Perform the documented CAM software-reset handshake.
 *
 * This follows the vendor sequence, which the data sheet describes in prose
 * and which two independent vendor implementations agree on:
 *
 *   camera_isp.c:1144-1161   1 -> poll bit1 -> 0x5 -> 0x4 -> 0
 *   gdma_drv_6589_ctl.c:40   1 -> poll bit1 ->      0x4 -> 0
 *   isp_function.h:131-139   [0]=1, wait [1]==1, [2]=1, delay, [0]=0, [2]=0
 *
 * 1. assert  SW_RST_Trig (write-only bit 0 = 1)
 * 2. poll    SW_RST_ST (read-only bit 1) until it reads 1, bounded
 * 3. assert  HW_RST (bit 2) alongside the still-asserted trigger, then
 *    leave HW_RST asserted on its own
 * 4. clear everything (0)
 *
 * Steps 3 and 4 are the "async HW reset.  Resets all CAM modules, except for
 * register-setting modules" part of the sequence.  Never leaving the trigger
 * asserted matters: it holds the block in reset.
 *
 * The order of steps 2 and 3 is the point of the timeout check.  SW_RST_ST is
 * documented as "0: DMA is busy, 1: DMA is idle.  HW reset can be done", so
 * if the poll expires the DMA is still running and the block says so.  The
 * previous code performed the whole HW_RST sequence first and only inspected
 * the timeout afterwards, i.e. it yanked a hardware reset out from under a
 * DMA engine that had just reported itself busy, and then returned
 * -ETIMEDOUT as if nothing had happened.  On timeout nothing is written at
 * all: the trigger is left as it is and the caller is told the reset was not
 * confirmed.
 *
 * Note the vendor poll loop in camera_isp.c:1157 is itself buggy -- it reads
 * "while ((!Reg) & ISP_REG_SW_CTL_SW_RST_STATUS)", which applies & before !
 * and so always exits immediately.  The copy in gdma_drv_6589_ctl.c:53 is the
 * correct one and is what the loop below follows, with the unbounded vendor
 * spin replaced by a timeout.
 *
 * TODO(unverified): the handshake is the documented and vendor-confirmed one,
 * but it has not been run against hardware.  The poll deadline is a guess.
 */
static int mtk_cam_sw_reset(struct mtk_cam *cam)
{
	unsigned int timeout;

	lockdep_assert_held(&cam->lock);

	cam_write(cam, CAM_CTL_SW_CTL, CAM_SW_CTL_SW_RST_TRIG);

	for (timeout = 0; timeout < CAM_SW_RST_TIMEOUT_US;
	     timeout += CAM_SW_RST_POLL_US) {
		if (cam_read(cam, CAM_CTL_SW_CTL) & CAM_SW_CTL_SW_RST_ST)
			break;

		udelay(CAM_SW_RST_POLL_US);
	}

	/*
	 * Decide before touching anything else.  Reaching here means the block
	 * never reported itself idle, so the DMA may still be mid-transfer and
	 * HW_RST would land in the middle of it.
	 */
	if (timeout == CAM_SW_RST_TIMEOUT_US) {
		dev_warn(cam->dev,
			 "CAM_SW_RST_ST still read 0 after %u us; not asserting HW_RST, reset not confirmed\n",
			 CAM_SW_RST_TIMEOUT_US);
		return -ETIMEDOUT;
	}

	/* DMA is idle: it is now safe to reset the block. */
	cam_write(cam, CAM_CTL_SW_CTL,
		  CAM_SW_CTL_SW_RST_TRIG | CAM_SW_CTL_HW_RST);
	cam_write(cam, CAM_CTL_SW_CTL, CAM_SW_CTL_HW_RST);
	cam_write(cam, CAM_CTL_SW_CTL, 0);

	return 0;
}

/*
 * Bring the control plane up or down.
 *
 * The documented order is: reset the block, enable the sub-modules, then
 * set the start bit.  On the way down the reverse.
 */
static int mtk_cam_start(struct mtk_cam *cam)
{
	int ret;

	lockdep_assert_held(&cam->lock);

	ret = mtk_cam_sw_reset(cam);
	if (ret)
		return ret;

	/*
	 * Enable the CAM block itself.  EN1 is a read/write shadow, but the
	 * data sheet drives it through write-one-to-set and
	 * write-one-to-clear companion registers (all bits type WO), so the
	 * shadow is never written directly.
	 *
	 * The remaining EN1 bits are the individual CPIPE stages.  They are
	 * deliberately left alone: turning them on without programming the
	 * pipeline would run blocks against unconfigured registers, and the
	 * pipeline is not implemented here anyway.
	 */
	cam_write(cam, CAM_CTL_EN1_SET, CAM_CTL_EN1_CAM_EN);

	/* Start. */
	cam_write(cam, CAM_CTL_START, CAM_CTL_START_PASS2B_START |
		  CAM_CTL_START_PASS2_START);

	cam->streaming = true;

	return 0;
}

static int mtk_cam_stop(struct mtk_cam *cam)
{
	lockdep_assert_held(&cam->lock);

	cam->streaming = false;

	cam_write(cam, CAM_CTL_START, 0);

	/*
	 * Clear the enable.  These are write-one-to-clear registers (see the
	 * header): the bit to clear is written as a 1, not a 0.
	 */
	cam_write(cam, CAM_CTL_EN1_CLR, CAM_CTL_EN1_CAM_EN);
	cam_write(cam, CAM_CTL_EN2_CLR, 0);

	return 0;
}

/*
 * Streaming entry point.
 *
 * The lock is taken here, not assumed.  call_s_stream() in v4l2-subdev.c calls
 * the driver callback directly, with no state object and so no lock of any
 * kind, so the lockdep_assert_held() in mtk_cam_sw_reset() -- which is what
 * protects the reset handshake and the enable writes below -- used to assert a
 * mutex nobody held.  Taking cam->lock here is what makes that assert true, and
 * it also serialises the CAM control strobes against any other path that
 * reaches them.
 *
 * A plain mutex is right: mtk_cam_sw_reset() busy-waits with udelay() over up
 * to 100 ms, so this path both sleeps and must not be in atomic context.
 */
static int mtk_cam_s_stream(struct v4l2_subdev *sd, int on)
{
	struct mtk_cam *cam = to_mtk_cam(sd);
	int ret = 0;

	mutex_lock(&cam->lock);

	if (on == cam->streaming)
		goto out_unlock;

	if (on)
		ret = mtk_cam_start(cam);
	else
		ret = mtk_cam_stop(cam);

out_unlock:
	mutex_unlock(&cam->lock);
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
	 *
	 * There is no internal_ops here: CAM has no pad_ops and no format to
	 * seed, so there is no subdev state for init_state() to populate.  That
	 * line is spelled out only because the other two drivers in this chain
	 * do set one; v4l2_subdev_init() itself does not touch internal_ops,
	 * and the devm allocation already zeroed it.
	 */
	v4l2_subdev_init(&cam->sd, &mtk_cam_subdev_ops);
	strscpy(cam->sd.name, dev_name(dev), sizeof(cam->sd.name));

	/*
	 * CAM has no pad_ops, so no pad operation runs under this lock; the
	 * assert in mtk_cam_sw_reset() is nonetheless about the mutex taken by
	 * mtk_cam_s_stream() below, which is this same one.  v4l2_subdev_init()
	 * does not touch state_lock, so this is still the driver's to set.
	 */
	cam->sd.state_lock = &cam->lock;

	/*
	 * CAM is the ISP: it is where the sensor's frames are meant to be
	 * processed, so MEDIA_ENT_F_PROC_VIDEO_ISP is the honest description,
	 * and it is the same value the other in-tree ISP subdevs use
	 * (mali-c55-isp.c:584).
	 */
	cam->sd.entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;

	/*
	 * Create the sink pad.  The DT's cam_in is port@0, which resolves to
	 * pad index 0, so this single pad is the one SCAM's source endpoint
	 * links to.
	 *
	 * There is deliberately no source pad and no pad_ops: this block has
	 * no DMA and no video node, so it consumes nothing and produces
	 * nothing.  A source pad here would advertise a link out of CAM to
	 * something that does not exist.  See the header note.
	 */
	cam->pads[CAM_PAD_SINK].flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&cam->sd.entity, CAM_PAD_NUM, cam->pads);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init entity pads\n");

	ret = v4l2_subdev_init_finalize(&cam->sd);
	if (ret)
		return dev_err_probe(dev, ret, "subdev init error\n");

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
		goto err_subdev_cleanup;

	dev_warn(dev,
		 "registering a survey-level CAM/ISP scaffold; no image pipeline is implemented\n");

	return 0;

err_subdev_cleanup:
	/*
	 * Release the active state allocated by v4l2_subdev_init_finalize().
	 * The pads live in the devm allocation and go with it.  Every error
	 * return above the pads_init() call can return directly.
	 */
	v4l2_subdev_cleanup(&cam->sd);
	return ret;
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