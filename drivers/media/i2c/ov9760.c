// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OV9760 1.6 Mpixel RAW10 sensor, one MIPI CSI-2 lane.
 *
 * Register settings from the MediaTek driver in the GPL bq/aquaris-5HD
 * kernel tree (mediatek/custom/common/kernel/imgsensor/ov9760_mipi_raw,
 * OV9760MIPI_Set720P), which the Lenovo B8000-H stock kernel also carries:
 * 24 MHz MCLK, 60 MHz pixel clock, 600 Mbit/s on the lane, HTS 1736,
 * VTS 1148 (30 frames/s).  The 1280x720 window is a centred crop of that
 * array, the only mode: libcamera's software ISP scales it for smaller
 * sizes. A 640x480 subsampled mode was tried and dropped: the vertical
 * skip repeated one Bayer row, the horizontal one blanked every other row,
 * and binning mixed the two, so colours came out wrong (bluish in calls).  No test pattern: the MTK driver has none and neither
 * the SMIA register 0x0601 nor the OmniVision 0x5e00/0x5040/0x503d give one.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define OV9760_MCLK			24000000
#define OV9760_LINK_FREQ		300000000ULL	/* 600 Mbit/s DDR */
#define OV9760_PIXEL_RATE		60000000
#define OV9760_DATA_LANES		1

#define OV9760_REG_CHIP_ID		CCI_REG16(0x0000)
#define OV9760_CHIP_ID			0x9760
#define OV9760_REG_MODE_SELECT		CCI_REG8(0x0100)
#define OV9760_REG_ORIENTATION		CCI_REG8(0x0101)
#define OV9760_REG_SOFTWARE_RESET	CCI_REG8(0x0103)
#define OV9760_REG_VTS			CCI_REG16(0x0340)
#define OV9760_REG_HTS			CCI_REG16(0x0342)
#define OV9760_REG_EXPOSURE		CCI_REG24(0x3500)	/* lines << 4 */
#define OV9760_REG_GAIN			CCI_REG16(0x350a)	/* 16 = 1x */

#define OV9760_HTS			1736
#define OV9760_VTS_DEF			1148
#define OV9760_VTS_MAX			0x7fff
#define OV9760_EXPOSURE_MIN		1
#define OV9760_EXPOSURE_MARGIN		4
#define OV9760_GAIN_MIN			16
#define OV9760_GAIN_MAX			0x3d0	/* the MTK driver's clamp */
#define OV9760_GAIN_DEF			16

#define OV9760_MBUS_CODE		MEDIA_BUS_FMT_SBGGR10_1X10

static const char * const ov9760_supply_names[] = {
	"avdd",		/* VCAMA 2.8 V */
	"dovdd",	/* VGP2 1.8 V */
};

static const s64 ov9760_link_freqs[] = { OV9760_LINK_FREQ };

/* OV9760MIPI_Set720P of the MTK driver, without the window. */
static const struct cci_reg_sequence ov9760_init_regs[] = {
	{ CCI_REG8(0x3660), 0x80 }, { CCI_REG8(0x3680), 0xf4 },
	{ CCI_REG8(0x3002), 0x80 }, { CCI_REG8(0x3012), 0x08 },
	{ CCI_REG8(0x3014), 0x04 }, { CCI_REG8(0x3022), 0x02 },
	{ CCI_REG8(0x3023), 0x0f }, { CCI_REG8(0x3080), 0x00 },
	{ CCI_REG8(0x3090), 0x02 }, { CCI_REG8(0x3091), 0x28 },
	{ CCI_REG8(0x3092), 0x02 }, { CCI_REG8(0x3093), 0x02 },
	{ CCI_REG8(0x3094), 0x00 }, { CCI_REG8(0x3095), 0x00 },
	{ CCI_REG8(0x3096), 0x01 }, { CCI_REG8(0x3097), 0x00 },
	{ CCI_REG8(0x3098), 0x04 }, { CCI_REG8(0x3099), 0x14 },
	{ CCI_REG8(0x309a), 0x03 }, { CCI_REG8(0x309c), 0x00 },
	{ CCI_REG8(0x309d), 0x00 }, { CCI_REG8(0x309e), 0x01 },
	{ CCI_REG8(0x309f), 0x00 }, { CCI_REG8(0x30a2), 0x01 },
	{ CCI_REG8(0x30b0), 0x0a }, { CCI_REG8(0x30b3), 0x32 },
	{ CCI_REG8(0x30b4), 0x02 }, { CCI_REG8(0x30b5), 0x00 },
	{ CCI_REG8(0x3503), 0x27 }, { CCI_REG8(0x3509), 0x10 },
	{ CCI_REG8(0x3600), 0x7c }, { CCI_REG8(0x3621), 0xb8 },
	{ CCI_REG8(0x3622), 0x23 }, { CCI_REG8(0x3631), 0xe2 },
	{ CCI_REG8(0x3634), 0x03 }, { CCI_REG8(0x3662), 0x14 },
	{ CCI_REG8(0x366b), 0x03 }, { CCI_REG8(0x3682), 0x82 },
	{ CCI_REG8(0x3705), 0x20 }, { CCI_REG8(0x3708), 0x64 },
	{ CCI_REG8(0x371b), 0x60 }, { CCI_REG8(0x3732), 0x40 },
	{ CCI_REG8(0x3745), 0x00 }, { CCI_REG8(0x3746), 0x18 },
	{ CCI_REG8(0x3780), 0x2a }, { CCI_REG8(0x3781), 0x8c },
	{ CCI_REG8(0x378f), 0xf5 }, { CCI_REG8(0x3823), 0x37 },
	{ CCI_REG8(0x383d), 0x88 }, { CCI_REG8(0x4000), 0x23 },
	{ CCI_REG8(0x4001), 0x04 }, { CCI_REG8(0x4002), 0x45 },
	{ CCI_REG8(0x4004), 0x08 }, { CCI_REG8(0x4005), 0x40 },
	{ CCI_REG8(0x4006), 0x40 }, { CCI_REG8(0x4009), 0x40 },
	{ CCI_REG8(0x404f), 0x8f }, { CCI_REG8(0x4058), 0x44 },
	{ CCI_REG8(0x4101), 0x32 }, { CCI_REG8(0x4102), 0xa4 },
	{ CCI_REG8(0x4520), 0xb0 }, { CCI_REG8(0x4580), 0x08 },
	{ CCI_REG8(0x4582), 0x00 }, { CCI_REG8(0x4307), 0x30 },
	{ CCI_REG8(0x4605), 0x00 }, { CCI_REG8(0x4608), 0x02 },
	{ CCI_REG8(0x4609), 0x00 }, { CCI_REG8(0x4801), 0x0f },
	{ CCI_REG8(0x4819), 0xb6 }, { CCI_REG8(0x4837), 0x21 },
	{ CCI_REG8(0x4906), 0xff }, { CCI_REG8(0x4d00), 0x04 },
	{ CCI_REG8(0x4d01), 0x4b }, { CCI_REG8(0x4d02), 0xfe },
	{ CCI_REG8(0x4d03), 0x09 }, { CCI_REG8(0x4d04), 0x1e },
	{ CCI_REG8(0x4d05), 0xb7 }, { CCI_REG8(0x4800), 0x44 },
	{ OV9760_REG_HTS, OV9760_HTS },
};

/*
 * The MTK full-size window is x 8..1503, y 2..1125 with output 1472x1104
 * and offsets 4, 4: the read window is the output plus 24 columns and 20
 * lines. The modes keep that margin around a centred crop.
 */
static const struct cci_reg_sequence ov9760_720p_regs[] = {
	{ CCI_REG16(0x0344), 104 },	/* x_addr_start */
	{ CCI_REG16(0x0346), 194 },	/* y_addr_start */
	{ CCI_REG16(0x0348), 1407 },	/* x_addr_end */
	{ CCI_REG16(0x034a), 933 },	/* y_addr_end */
	{ CCI_REG16(0x034c), 1280 },	/* x_output_size */
	{ CCI_REG16(0x034e), 720 },	/* y_output_size */
	{ CCI_REG8(0x3811), 0x04 },	/* x_offset */
	{ CCI_REG8(0x3813), 0x04 },	/* y_offset */
	{ CCI_REG8(0x0381), 0x01 },	/* x_even_inc */
	{ CCI_REG8(0x0383), 0x01 },	/* x_odd_inc */
	{ CCI_REG8(0x0385), 0x01 },	/* y_even_inc */
	{ CCI_REG8(0x0387), 0x01 },	/* y_odd_inc */
	{ CCI_REG8(0x3820), 0x00 },
	{ CCI_REG8(0x3821), 0x00 },
};

/*
 * The array as the MTK driver reads it in full: 1472x1104 output. The mode
 * crops it centred.
 */
#define OV9760_ARRAY_WIDTH		1472
#define OV9760_ARRAY_HEIGHT		1104

struct ov9760_mode {
	u32 width;
	u32 height;
	struct v4l2_rect crop;
	u32 vts_def;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const struct ov9760_mode ov9760_modes[] = {
	{
		.width = 1280, .height = 720, .vts_def = OV9760_VTS_DEF,
		.crop = { 96, 192, 1280, 720 },
		.regs = ov9760_720p_regs, .num_regs = ARRAY_SIZE(ov9760_720p_regs),
	},
};

struct ov9760 {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *mclk;
	struct gpio_desc *reset;
	struct gpio_desc *powerdown;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ov9760_supply_names)];

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;

	const struct ov9760_mode *mode;
};

static inline struct ov9760 *to_ov9760(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov9760, sd);
}

static int ov9760_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov9760 *ov = container_of(ctrl->handler, struct ov9760, ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 max = ov->mode->height + ctrl->val - OV9760_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(ov->exposure, ov->exposure->minimum, max,
					 ov->exposure->step,
					 min_t(s64, ov->exposure->default_value, max));
	}

	/* Registers are written only while the sensor is powered. */
	if (!pm_runtime_get_if_in_use(ov->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(ov->regmap, OV9760_REG_EXPOSURE, ctrl->val << 4, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(ov->regmap, OV9760_REG_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_VBLANK:
		cci_write(ov->regmap, OV9760_REG_VTS,
			  ov->mode->height + ctrl->val, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(ov->dev);
	return ret;
}

static const struct v4l2_ctrl_ops ov9760_ctrl_ops = {
	.s_ctrl = ov9760_set_ctrl,
};

static int ov9760_init_controls(struct ov9760 *ov)
{
	struct v4l2_ctrl_handler *hdl = &ov->ctrls;
	const struct ov9760_mode *mode = ov->mode;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl *ctrl;
	int ret;

	v4l2_ctrl_handler_init(hdl, 10);

	ctrl = v4l2_ctrl_new_int_menu(hdl, &ov9760_ctrl_ops, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(ov9760_link_freqs) - 1, 0,
				      ov9760_link_freqs);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(hdl, &ov9760_ctrl_ops, V4L2_CID_PIXEL_RATE,
			  OV9760_PIXEL_RATE, OV9760_PIXEL_RATE, 1,
			  OV9760_PIXEL_RATE);

	ov->hblank = v4l2_ctrl_new_std(hdl, &ov9760_ctrl_ops, V4L2_CID_HBLANK,
				       OV9760_HTS - mode->width,
				       OV9760_HTS - mode->width, 1,
				       OV9760_HTS - mode->width);
	if (ov->hblank)
		ov->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ov->vblank = v4l2_ctrl_new_std(hdl, &ov9760_ctrl_ops, V4L2_CID_VBLANK,
				       mode->vts_def - mode->height,
				       OV9760_VTS_MAX - mode->height, 1,
				       mode->vts_def - mode->height);

	ov->exposure = v4l2_ctrl_new_std(hdl, &ov9760_ctrl_ops, V4L2_CID_EXPOSURE,
					 OV9760_EXPOSURE_MIN,
					 mode->vts_def - OV9760_EXPOSURE_MARGIN, 1,
					 mode->vts_def - OV9760_EXPOSURE_MARGIN);

	v4l2_ctrl_new_std(hdl, &ov9760_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  OV9760_GAIN_MIN, OV9760_GAIN_MAX, 1, OV9760_GAIN_DEF);

	ret = v4l2_fwnode_device_parse(ov->dev, &props);
	if (ret)
		goto err;
	v4l2_ctrl_new_fwnode_properties(hdl, &ov9760_ctrl_ops, &props);

	if (hdl->error) {
		ret = hdl->error;
		goto err;
	}
	ov->sd.ctrl_handler = hdl;
	return 0;
err:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static void ov9760_fill_format(const struct ov9760_mode *mode,
			       struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = OV9760_MBUS_CODE;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int ov9760_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct ov9760 *ov = to_ov9760(sd);
	int ret;

	ret = pm_runtime_resume_and_get(ov->dev);
	if (ret)
		return ret;

	ret = cci_write(ov->regmap, OV9760_REG_SOFTWARE_RESET, 0x01, NULL);
	if (ret)
		goto err;
	usleep_range(10000, 11000);

	cci_multi_reg_write(ov->regmap, ov9760_init_regs,
			    ARRAY_SIZE(ov9760_init_regs), &ret);
	cci_multi_reg_write(ov->regmap, ov->mode->regs, ov->mode->num_regs, &ret);
	if (ret)
		goto err;

	ret = __v4l2_ctrl_handler_setup(&ov->ctrls);
	if (ret)
		goto err;

	ret = cci_write(ov->regmap, OV9760_REG_MODE_SELECT, 0x01, NULL);
	if (ret)
		goto err;
	return 0;
err:
	dev_err(ov->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put(ov->dev);
	return ret;
}

static int ov9760_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct ov9760 *ov = to_ov9760(sd);

	cci_write(ov->regmap, OV9760_REG_MODE_SELECT, 0x00, NULL);
	pm_runtime_put(ov->dev);
	return 0;
}

static int ov9760_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct ov9760 *ov = to_ov9760(sd);
	const struct ov9760_mode *mode;
	int ret;

	mode = v4l2_find_nearest_size(ov9760_modes, ARRAY_SIZE(ov9760_modes),
				      width, height, fmt->format.width,
				      fmt->format.height);
	ov9760_fill_format(mode, &fmt->format);
	*v4l2_subdev_state_get_format(state, fmt->pad) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;
	if (v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	ov->mode = mode;
	ret = __v4l2_ctrl_modify_range(ov->vblank, mode->vts_def - mode->height,
				       OV9760_VTS_MAX - mode->height, 1,
				       mode->vts_def - mode->height);
	if (ret)
		return ret;
	ret = __v4l2_ctrl_s_ctrl(ov->vblank, mode->vts_def - mode->height);
	if (ret)
		return ret;
	/* ctrls.lock is the state lock and is held here: no v4l2_ctrl_find(). */
	return __v4l2_ctrl_modify_range(ov->hblank,
					OV9760_HTS - mode->width,
					OV9760_HTS - mode->width, 1,
					OV9760_HTS - mode->width);
}

static int ov9760_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = OV9760_MBUS_CODE;
	return 0;
}

static int ov9760_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(ov9760_modes) ||
	    fse->code != OV9760_MBUS_CODE)
		return -EINVAL;
	fse->min_width = fse->max_width = ov9760_modes[fse->index].width;
	fse->min_height = fse->max_height = ov9760_modes[fse->index].height;
	return 0;
}

static int ov9760_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	const struct v4l2_mbus_framefmt *fmt;
	unsigned int i;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		fmt = v4l2_subdev_state_get_format(state, 0);
		for (i = 0; i < ARRAY_SIZE(ov9760_modes); i++)
			if (ov9760_modes[i].width == fmt->width &&
			    ov9760_modes[i].height == fmt->height)
				break;
		if (i == ARRAY_SIZE(ov9760_modes))
			return -EINVAL;
		sel->r = ov9760_modes[i].crop;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = OV9760_ARRAY_WIDTH;
		sel->r.height = OV9760_ARRAY_HEIGHT;
		return 0;
	}
	return -EINVAL;
}

static int ov9760_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	ov9760_fill_format(&ov9760_modes[0], v4l2_subdev_state_get_format(state, 0));
	return 0;
}

static const struct v4l2_subdev_video_ops ov9760_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops ov9760_pad_ops = {
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ov9760_set_fmt,
	.enum_mbus_code = ov9760_enum_mbus_code,
	.enum_frame_size = ov9760_enum_frame_size,
	.get_selection = ov9760_get_selection,
	.enable_streams = ov9760_enable_streams,
	.disable_streams = ov9760_disable_streams,
};

static const struct v4l2_subdev_ops ov9760_subdev_ops = {
	.video = &ov9760_video_ops,
	.pad = &ov9760_pad_ops,
};

static const struct v4l2_subdev_internal_ops ov9760_internal_ops = {
	.init_state = ov9760_init_state,
};

static const struct media_entity_operations ov9760_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/*
 * Stock kdCISModulePowerOn order: reset and power-down asserted, VCAMA,
 * VGP2, 1 ms, 3 ms, reset released, 1 ms, power-down released.  MCLK runs
 * before the rails come up. VGP5, which the stock code also names, is the
 * touch panel's rail and stays out of this driver.
 */
static int ov9760_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov9760 *ov = to_ov9760(sd);
	int ret;

	ret = clk_prepare_enable(ov->mclk);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(ov->powerdown, 1);
	gpiod_set_value_cansleep(ov->reset, 1);

	ret = regulator_bulk_enable(ARRAY_SIZE(ov->supplies), ov->supplies);
	if (ret) {
		clk_disable_unprepare(ov->mclk);
		return ret;
	}
	/* Stock: 1 ms + 3 ms + delays between the steps, about 7-8 ms. */
	usleep_range(7000, 8000);
	gpiod_set_value_cansleep(ov->reset, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ov->powerdown, 0);
	usleep_range(1000, 2000);
	return 0;
}

static int ov9760_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov9760 *ov = to_ov9760(sd);

	gpiod_set_value_cansleep(ov->reset, 1);
	gpiod_set_value_cansleep(ov->powerdown, 1);
	regulator_bulk_disable(ARRAY_SIZE(ov->supplies), ov->supplies);
	clk_disable_unprepare(ov->mclk);
	return 0;
}

static int ov9760_identify(struct ov9760 *ov)
{
	u64 id;
	int ret;

	ret = cci_read(ov->regmap, OV9760_REG_CHIP_ID, &id, NULL);
	if (ret)
		return dev_err_probe(ov->dev, ret, "cannot read the chip ID\n");
	if (id != OV9760_CHIP_ID)
		return dev_err_probe(ov->dev, -ENXIO, "chip ID %04llx, not %04x\n",
				     id, OV9760_CHIP_ID);
	return 0;
}

static int ov9760_check_endpoint(struct ov9760 *ov)
{
	struct v4l2_fwnode_endpoint bus_cfg = { .bus_type = V4L2_MBUS_CSI2_DPHY };
	struct fwnode_handle *ep;
	unsigned long bitmap;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(ov->dev), NULL);
	if (!ep)
		return dev_err_probe(ov->dev, -EINVAL, "no endpoint\n");
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(ov->dev, ret, "cannot parse the endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != OV9760_DATA_LANES) {
		ret = dev_err_probe(ov->dev, -EINVAL, "%u data lanes, not 1\n",
				    bus_cfg.bus.mipi_csi2.num_data_lanes);
		goto out;
	}
	ret = v4l2_link_freq_to_bitmap(ov->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       ov9760_link_freqs,
				       ARRAY_SIZE(ov9760_link_freqs), &bitmap);
out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int ov9760_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov9760 *ov;
	unsigned int i;
	int ret;

	ov = devm_kzalloc(dev, sizeof(*ov), GFP_KERNEL);
	if (!ov)
		return -ENOMEM;
	ov->dev = dev;
	ov->mode = &ov9760_modes[0];

	v4l2_i2c_subdev_init(&ov->sd, client, &ov9760_subdev_ops);

	ret = ov9760_check_endpoint(ov);
	if (ret)
		return ret;

	ov->mclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(ov->mclk))
		return dev_err_probe(dev, PTR_ERR(ov->mclk), "no MCLK\n");
	ret = clk_set_rate(ov->mclk, OV9760_MCLK);
	if (ret)
		return dev_err_probe(dev, ret, "cannot set MCLK\n");
	if (clk_get_rate(ov->mclk) != OV9760_MCLK)
		return dev_err_probe(dev, -EINVAL, "MCLK %lu Hz, not 24 MHz\n",
				     clk_get_rate(ov->mclk));

	ov->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ov->reset))
		return dev_err_probe(dev, PTR_ERR(ov->reset), "no reset GPIO\n");
	ov->powerdown = devm_gpiod_get(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(ov->powerdown))
		return dev_err_probe(dev, PTR_ERR(ov->powerdown),
				     "no powerdown GPIO\n");
	for (i = 0; i < ARRAY_SIZE(ov->supplies); i++)
		ov->supplies[i].supply = ov9760_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ov->supplies), ov->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "no supplies\n");

	ov->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ov->regmap))
		return PTR_ERR(ov->regmap);

	ret = ov9760_power_on(dev);
	if (ret)
		return dev_err_probe(dev, ret, "cannot power on\n");
	ret = ov9760_identify(ov);
	if (ret)
		goto err_power;

	ret = ov9760_init_controls(ov);
	if (ret)
		goto err_power;

	ov->sd.internal_ops = &ov9760_internal_ops;
	ov->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov->sd.entity.ops = &ov9760_entity_ops;
	ov->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ov->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&ov->sd.entity, 1, &ov->pad);
	if (ret)
		goto err_ctrls;

	ov->sd.state_lock = ov->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&ov->sd);
	if (ret)
		goto err_entity;

	/* On now; runtime PM switches it off once probe is done. */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&ov->sd);
	if (ret)
		goto err_pm;

	pm_runtime_idle(dev);
	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&ov->sd);
err_entity:
	media_entity_cleanup(&ov->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&ov->ctrls);
err_power:
	ov9760_power_off(dev);
	return ret;
}

static void ov9760_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov9760 *ov = to_ov9760(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ov->ctrls);
	pm_runtime_disable(ov->dev);
	if (!pm_runtime_status_suspended(ov->dev)) {
		ov9760_power_off(ov->dev);
		pm_runtime_set_suspended(ov->dev);
	}
}

static DEFINE_RUNTIME_DEV_PM_OPS(ov9760_pm_ops, ov9760_power_off,
				 ov9760_power_on, NULL);

static const struct of_device_id ov9760_of_match[] = {
	{ .compatible = "ovti,ov9760" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov9760_of_match);

static struct i2c_driver ov9760_driver = {
	.driver = {
		.name = "ov9760",
		.pm = pm_ptr(&ov9760_pm_ops),
		.of_match_table = ov9760_of_match,
	},
	.probe = ov9760_probe,
	.remove = ov9760_remove,
};
module_i2c_driver(ov9760_driver);

MODULE_DESCRIPTION("OmniVision OV9760 sensor driver");
MODULE_LICENSE("GPL");
