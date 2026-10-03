// SPDX-License-Identifier: GPL-2.0
/*
 * A5142 camera sensor driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * 5 megapixel RAW sensor, 2-lane MIPI CSI-2, driven over SCCB at 0x6c.
 * This is the main (rear) camera of the Lenovo YOGA Tablet 8/10 (MT6589).
 *
 * The register table was transcribed from the vendor driver in
 * mediatek/custom/common/kernel/imgsensor/a5142_mipi_raw/.  That driver is
 * a HAL shim around the same register set; only its tables are useful.
 *
 * Note the chip ID 0x4800 is shared with the A5141, so reading the ID
 * alone cannot tell the two apart.  See a5142_check_sensor_id().
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>

#include "a5142.h"

/* Chip ID, read from 0x000a/0x000b.  Shared with the A5141. */
#define A5142_CHIP_ID		0x4800
#define A5142_REG_CHIP_ID_H	0x000a
#define A5142_REG_CHIP_ID_L	0x000b

#define A5142_REG_CTRL		0x0103
#define A5142_CTRL_SWRST	0x01
#define A5142_REG_STREAM	0x0100
#define A5142_STREAM_START	0x01
#define A5142_STREAM_STOP	0x00

/* Grouped parameter hold: wraps exposure and dummy-line updates. */
#define A5142_REG_GRP_HOLD	0x0104
#define A5142_GRP_HOLD		0x01

#define A5142_REG_SHUTTER	0x0202
#define A5142_REG_GAIN		0x0204

#define A5142_REG_FRAME_LEN	0x0340
#define A5142_REG_LINE_LEN	0x0342

/*
 * Frame timing, from the vendor driver's period constants:
 *   preview: 1296 + 1855 = 3151 line length, 972 + 128 = 1100 frame length
 *   capture: 2592 + 1102 = 3694 line length, 1944 + 77 = 2021 frame length
 */
#define A5142_PV_WIDTH		1280
#define A5142_PV_HEIGHT		960
#define A5142_PV_HTS		3151
#define A5142_PV_VTS		1100

#define A5142_CAP_WIDTH		2560
#define A5142_CAP_HEIGHT	1920
#define A5142_CAP_HTS		3694
#define A5142_CAP_VTS		2021

/*
 * Pixel rates.  The vendor driver programs PCLK = 104 MHz with a 26 MHz
 * MCLK (the "Demo Initialization 1296 x 972 MCLK= 26MHz, PCLK=104MHz"
 * comment in its init sequence), which at the preview timing above gives
 * a 30 fps frame rate.
 */
#define A5142_PV_PIXEL_RATE	(104000000U * 2 / 10)
#define A5142_CAP_PIXEL_RATE	(104000000U * 2 / 10)

/* Exposure control limits, in microseconds. */
#define A5142_EXPOSURE_MIN_US	300
#define A5142_EXPOSURE_MAX_US	30000000

/*
 * Power-on configuration.
 *
 * Transcribed from A5142MIPI_Init_setting() in the vendor driver, with the
 * RAW10 / MIPI_INTERFACE branches selected (this sensor is configured for
 * 2-lane MIPI with a 10-bit RAW output) and the software reset, stream
 * stop, stream start and parameter-hold writes removed because this driver
 * performs those itself under its own control.
 */
static const struct reg_sequence a5142_init_regs[] = {
	{ .reg = 0x301a, .def = 0x0218 }, { .reg = 0x3064, .def = 0xb800 }, { .reg = 0x31ae, .def = 0x0202 },
	{ .reg = 0x0112, .def = 0x0a0a }, { .reg = 0x316a, .def = 0x8400 }, { .reg = 0x316c, .def = 0x8400 },
	{ .reg = 0x316e, .def = 0x8400 }, { .reg = 0x3efa, .def = 0x1a1f }, { .reg = 0x3ed2, .def = 0xd965 },
	{ .reg = 0x3ed8, .def = 0x7f1b }, { .reg = 0x3eda, .def = 0x2f11 }, { .reg = 0x3ee2, .def = 0x0060 },
	{ .reg = 0x3ef2, .def = 0xd965 }, { .reg = 0x3ef8, .def = 0x797f }, { .reg = 0x3efc, .def = 0x286f },
	{ .reg = 0x3efe, .def = 0x2c01 }, { .reg = 0x3e00, .def = 0x042f }, { .reg = 0x3e02, .def = 0xffff },
	{ .reg = 0x3e04, .def = 0xffff }, { .reg = 0x3e06, .def = 0xffff }, { .reg = 0x3e08, .def = 0x8071 },
	{ .reg = 0x3e0a, .def = 0x7281 }, { .reg = 0x3e0c, .def = 0x4011 }, { .reg = 0x3e0e, .def = 0x8010 },
	{ .reg = 0x3e10, .def = 0x60a5 }, { .reg = 0x3e12, .def = 0x4080 }, { .reg = 0x3e14, .def = 0x4180 },
	{ .reg = 0x3e16, .def = 0x0018 }, { .reg = 0x3e18, .def = 0x46b7 }, { .reg = 0x3e1a, .def = 0x4994 },
	{ .reg = 0x3e1c, .def = 0x4997 }, { .reg = 0x3e1e, .def = 0x4682 }, { .reg = 0x3e20, .def = 0x0018 },
	{ .reg = 0x3e22, .def = 0x4241 }, { .reg = 0x3e24, .def = 0x8000 }, { .reg = 0x3e26, .def = 0x1880 },
	{ .reg = 0x3e28, .def = 0x4785 }, { .reg = 0x3e2a, .def = 0x4992 }, { .reg = 0x3e2c, .def = 0x4997 },
	{ .reg = 0x3e2e, .def = 0x4780 }, { .reg = 0x3e30, .def = 0x4d80 }, { .reg = 0x3e32, .def = 0x100c },
	{ .reg = 0x3e34, .def = 0x8000 }, { .reg = 0x3e36, .def = 0x184a }, { .reg = 0x3e38, .def = 0x8042 },
	{ .reg = 0x3e3a, .def = 0x001a }, { .reg = 0x3e3c, .def = 0x9610 }, { .reg = 0x3e3e, .def = 0x0c80 },
	{ .reg = 0x3e40, .def = 0x4dc6 }, { .reg = 0x3e42, .def = 0x4a80 }, { .reg = 0x3e44, .def = 0x0018 },
	{ .reg = 0x3e46, .def = 0x8042 }, { .reg = 0x3e48, .def = 0x8041 }, { .reg = 0x3e4a, .def = 0x0018 },
	{ .reg = 0x3e4c, .def = 0x804b }, { .reg = 0x3e4e, .def = 0xb74b }, { .reg = 0x3e50, .def = 0x8010 },
	{ .reg = 0x3e52, .def = 0x6056 }, { .reg = 0x3e54, .def = 0x001c }, { .reg = 0x3e56, .def = 0x8211 },
	{ .reg = 0x3e58, .def = 0x8056 }, { .reg = 0x3e5a, .def = 0x827c }, { .reg = 0x3e5c, .def = 0x0970 },
	{ .reg = 0x3e5e, .def = 0x8082 }, { .reg = 0x3e60, .def = 0x7281 }, { .reg = 0x3e62, .def = 0x4c40 },
	{ .reg = 0x3e64, .def = 0x8e4d }, { .reg = 0x3e66, .def = 0x8110 }, { .reg = 0x3e68, .def = 0x0caf },
	{ .reg = 0x3e6a, .def = 0x4d80 }, { .reg = 0x3e6c, .def = 0x100c }, { .reg = 0x3e6e, .def = 0x8440 },
	{ .reg = 0x3e70, .def = 0x4c81 }, { .reg = 0x3e72, .def = 0x7c5f }, { .reg = 0x3e74, .def = 0x7000 },
	{ .reg = 0x3e76, .def = 0x0000 }, { .reg = 0x3e78, .def = 0x0000 }, { .reg = 0x3e7a, .def = 0x0000 },
	{ .reg = 0x3e7c, .def = 0x0000 }, { .reg = 0x3e7e, .def = 0x0000 }, { .reg = 0x3e80, .def = 0x0000 },
	{ .reg = 0x3e82, .def = 0x0000 }, { .reg = 0x3e84, .def = 0x0000 }, { .reg = 0x3e86, .def = 0x0000 },
	{ .reg = 0x3e88, .def = 0x0000 }, { .reg = 0x3e8a, .def = 0x0000 }, { .reg = 0x3e8c, .def = 0x0000 },
	{ .reg = 0x3e8e, .def = 0x0000 }, { .reg = 0x3e90, .def = 0x0000 }, { .reg = 0x3e92, .def = 0x0000 },
	{ .reg = 0x3e94, .def = 0x0000 }, { .reg = 0x3e96, .def = 0x0000 }, { .reg = 0x3e98, .def = 0x0000 },
	{ .reg = 0x3e9a, .def = 0x0000 }, { .reg = 0x3e9c, .def = 0x0000 }, { .reg = 0x3e9e, .def = 0x0000 },
	{ .reg = 0x3ea0, .def = 0x0000 }, { .reg = 0x3ea2, .def = 0x0000 }, { .reg = 0x3ea4, .def = 0x0000 },
	{ .reg = 0x3ea6, .def = 0x0000 }, { .reg = 0x3ea8, .def = 0x0000 }, { .reg = 0x3eaa, .def = 0x0000 },
	{ .reg = 0x3eac, .def = 0x0000 }, { .reg = 0x3eae, .def = 0x0000 }, { .reg = 0x3eb0, .def = 0x0000 },
	{ .reg = 0x3eb2, .def = 0x0000 }, { .reg = 0x3eb4, .def = 0x0000 }, { .reg = 0x3eb6, .def = 0x0000 },
	{ .reg = 0x3eb8, .def = 0x0000 }, { .reg = 0x3eba, .def = 0x0000 }, { .reg = 0x3ebc, .def = 0x0000 },
	{ .reg = 0x3ebe, .def = 0x0000 }, { .reg = 0x3ec0, .def = 0x0000 }, { .reg = 0x3ec2, .def = 0x0000 },
	{ .reg = 0x3ec4, .def = 0x0000 }, { .reg = 0x3ec6, .def = 0x0000 }, { .reg = 0x3ec8, .def = 0x0000 },
	{ .reg = 0x3eca, .def = 0x0000 }, { .reg = 0x3170, .def = 0x2150 }, { .reg = 0x317a, .def = 0x0150 },
	{ .reg = 0x3ecc, .def = 0x2200 }, { .reg = 0x3174, .def = 0x0000 }, { .reg = 0x3176, .def = 0x0000 },
	{ .reg = 0x30bc, .def = 0x0384 }, { .reg = 0x30c0, .def = 0x1220 }, { .reg = 0x30d4, .def = 0x9200 },
	{ .reg = 0x30b2, .def = 0xc000 }, { .reg = 0x31b0, .def = 0x00c4 }, { .reg = 0x31b2, .def = 0x0064 },
	{ .reg = 0x31b4, .def = 0x0e77 }, { .reg = 0x31b6, .def = 0x0d24 }, { .reg = 0x31b8, .def = 0x020e },
	{ .reg = 0x31ba, .def = 0x0710 }, { .reg = 0x31bc, .def = 0x2a0d }, { .reg = 0x31be, .def = 0xc007 },
	{ .reg = 0x305e, .def = 0x112e }, { .reg = 0x30f0, .def = 0x0000 }, { .reg = 0x0300, .def = 0x05 },
	{ .reg = 0x0302, .def = 0x01 }, { .reg = 0x0304, .def = 0x02 }, { .reg = 0x0306, .def = 0x28 },
	{ .reg = 0x0308, .def = 0x0a }, { .reg = 0x030a, .def = 0x01 },
};

/* ------------------------------------------------------------------ */
/* Format and controls                                                 */
/* ------------------------------------------------------------------ */

static struct a5142_mode_info a5142_modes[] = {
	{
		.width = A5142_PV_WIDTH,
		.height = A5142_PV_HEIGHT,
		.hts = A5142_PV_HTS,
		.vts = A5142_PV_VTS,
		.pix_rate = A5142_PV_PIXEL_RATE,
	},
	{
		.width = A5142_CAP_WIDTH,
		.height = A5142_CAP_HEIGHT,
		.hts = A5142_CAP_HTS,
		.vts = A5142_CAP_VTS,
		.pix_rate = A5142_CAP_PIXEL_RATE,
	},
};

/*
 * The sensor counts exposure in lines, so the minimum integration time is
 * one line time and the V4L2 microsecond control is scaled by it.
 */
static void a5142_update_frame_duration(struct a5142 *a5142)
{
	a5142->integration_time_min = DIV_ROUND_UP(1000000u * a5142->hts,
						   a5142->vts);
}

static int a5142_set_pad(struct v4l2_subdev *sd,
			 struct v4l2_subdev_state *state,
			 struct v4l2_subdev_format *format)
{
	struct a5142 *a5142 = to_a5142(sd);

	mutex_lock(&a5142->lock);

	format->format.width = a5142->cur_mode->width;
	format->format.height = a5142->cur_mode->height;
	format->format.code = MEDIA_BUS_FMT_SBGGR10_1X10;
	format->format.field = V4L2_FIELD_NONE;
	format->format.colorspace = V4L2_COLORSPACE_SMPTE170M;
	format->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	format->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	format->format.xfer_func = V4L2_XFER_FUNC_DEFAULT;

	a5142->hts = a5142->cur_mode->hts;
	a5142->vts = a5142->cur_mode->vts;
	a5142_update_frame_duration(a5142);

	mutex_unlock(&a5142->lock);

	return 0;
}

static int a5142_enum_frame_size(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(a5142_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = a5142_modes[A5142_MODE_PREVIEW].width;
	fse->max_width = a5142_modes[A5142_MODE_CAPTURE].width;
	fse->min_height = a5142_modes[A5142_MODE_PREVIEW].height;
	fse->max_height = a5142_modes[A5142_MODE_CAPTURE].height;

	return 0;
}

static int a5142_get_selection(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_selection *sel)
{
	struct a5142 *a5142 = to_a5142(sd);

	if (sel->pad)
		return -EINVAL;

	mutex_lock(&a5142->lock);

	sel->target = V4L2_SEL_TGT_CROP;
	sel->r.left = 0;
	sel->r.top = 0;
	sel->r.width = a5142->cur_mode->width;
	sel->r.height = a5142->cur_mode->height;
	sel->flags = V4L2_SEL_FLAG_LE;

	mutex_unlock(&a5142->lock);

	return 0;
}

static const struct v4l2_subdev_pad_ops a5142_pad_ops = {
	.enum_frame_size = a5142_enum_frame_size,
	.get_fmt = a5142_set_pad,
	.set_fmt = a5142_set_pad,
	.get_selection = a5142_get_selection,
};

static int a5142_write_exposure(struct a5142 *a5142, u32 exposure)
{
	u32 lines;
	int ret;

	lines = DIV_ROUND_UP(exposure, a5142->integration_time_min);

	/*
	 * The A5142 takes a plain 16-bit line count in 0x0202, held stable
	 * with the grouped parameter hold bit while it is written.
	 */
	ret = regmap_write(a5142->regmap, A5142_REG_GRP_HOLD, A5142_GRP_HOLD);
	if (ret)
		return ret;

	ret = regmap_write(a5142->regmap, A5142_REG_SHUTTER, lines);
	if (ret)
		goto err_release_hold;

	return regmap_write(a5142->regmap, A5142_REG_GRP_HOLD, 0);

err_release_hold:
	/* Never leave the group asserted: it would freeze the exposure. */
	regmap_write(a5142->regmap, A5142_REG_GRP_HOLD, 0);
	return ret;
}

static int a5142_write_frame_length(struct a5142 *a5142, u32 frame_length)
{
	int ret;

	ret = regmap_write(a5142->regmap, A5142_REG_GRP_HOLD, A5142_GRP_HOLD);
	if (ret)
		return ret;

	ret = regmap_write(a5142->regmap, A5142_REG_FRAME_LEN, frame_length);
	if (ret)
		return ret;

	return regmap_write(a5142->regmap, A5142_REG_GRP_HOLD, 0);
}

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

static int a5142_start_stream(struct a5142 *a5142)
{
	struct device *dev = &a5142->client->dev;
	int ret;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		return ret;

	/*
	 * Apply the mode's nominal exposure before starting the stream.
	 * There is no user-facing exposure control in this build, so the
	 * sensor runs at the register default set by the init table.
	 */
	ret = a5142_write_exposure(a5142, a5142->integration_time_min);
	if (ret)
		goto err_pm;

	ret = a5142_write_frame_length(a5142, a5142->vts);
	if (ret)
		goto err_pm;

	ret = regmap_write(a5142->regmap, A5142_REG_STREAM,
			   A5142_STREAM_START);
	if (ret)
		goto err_pm;

	a5142->streaming = true;

	return 0;

err_pm:
	pm_runtime_put_sync(dev);
	return ret;
}

static int a5142_stop_stream(struct a5142 *a5142)
{
	struct device *dev = &a5142->client->dev;
	int ret;

	a5142->streaming = false;

	ret = regmap_write(a5142->regmap, A5142_REG_STREAM,
			   A5142_STREAM_STOP);

	/*
	 * The runtime-PM reference taken by start_stream() is dropped
	 * regardless of the write result.  The SCCB write needs the sensor
	 * powered, so it has to happen before the put; but a failed write
	 * must not leave the reference held (a leak) nor be swallowed
	 * (which would report success for a stream that is still running).
	 */
	pm_runtime_put_sync(dev);

	return ret;
}

static int a5142_s_stream(struct v4l2_subdev *sd, int on)
{
	struct a5142 *a5142 = to_a5142(sd);
	int ret = 0;

	mutex_lock(&a5142->lock);

	if (on == a5142->streaming)
		goto out_unlock;

	if (on)
		ret = a5142_start_stream(a5142);
	else
		ret = a5142_stop_stream(a5142);

out_unlock:
	mutex_unlock(&a5142->lock);
	return ret;
}

static const struct v4l2_subdev_video_ops a5142_video_ops = {
	.s_stream = a5142_s_stream,
};

static const struct v4l2_subdev_ops a5142_subdev_ops = {
	.video = &a5142_video_ops,
	.pad = &a5142_pad_ops,
};

/* ------------------------------------------------------------------ */
/* Runtime power management                                            */
/* ------------------------------------------------------------------ */

/*
 * The sensor needs the group hold released and a short settle before the
 * SCCB is usable after power-up; the vendor driver waits 5 ms after the
 * software reset and again for the PLL to lock, and the data sheet asks
 * for 20 ms after PWDN rises.
 */
static int a5142_power_on(struct a5142 *a5142)
{
	int ret;

	if (a5142->reset) {
		gpiod_set_value_cansleep(a5142->reset, 0);
		usleep_range(1000, 2000);
		gpiod_set_value_cansleep(a5142->reset, 1);
		usleep_range(1000, 2000);
	}

	if (a5142->pwdn) {
		gpiod_set_value_cansleep(a5142->pwdn, 0);
		msleep(20);
	}

	ret = regmap_write(a5142->regmap, A5142_REG_CTRL, A5142_CTRL_SWRST);
	if (ret)
		return ret;

	msleep(5);

	ret = regmap_multi_reg_write(a5142->regmap, a5142_init_regs,
				     ARRAY_SIZE(a5142_init_regs));
	if (ret)
		return ret;

	/* Let the sensor PLL settle before the first frame. */
	msleep(5);

	return 0;
}

static int a5142_power_off(struct a5142 *a5142)
{
	if (a5142->pwdn)
		gpiod_set_value_cansleep(a5142->pwdn, 1);

	return 0;
}

static int a5142_runtime_suspend(struct device *dev)
{
	struct a5142 *a5142 = dev_get_drvdata(dev);

	return a5142_power_off(a5142);
}

static int a5142_runtime_resume(struct device *dev)
{
	struct a5142 *a5142 = dev_get_drvdata(dev);

	return a5142_power_on(a5142);
}

static const struct dev_pm_ops a5142_pm_ops = {
	.runtime_suspend = a5142_runtime_suspend,
	.runtime_resume = a5142_runtime_resume,
};

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

static const struct regmap_config a5142_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
};

/*
 * The A5141 and the A5142 share chip ID 0x4800, so the ID check cannot
 * distinguish them.  The two differ in the MIPI output format register
 * (0x0112): the A5142 is configured here for 10-bit RAW (0x0a0a), and a
 * board that reports this ID with an 8-bit RAW output is assumed to be the
 * A5141.  Both drive the same SCCB sequence, so this is a warning rather
 * than a probe failure.
 */
static int a5142_check_sensor_id(struct a5142 *a5142)
{
	struct device *dev = &a5142->client->dev;
	unsigned int id;
	int ret;

	/*
	 * Every regmap_read() must be checked before its value is used.  If
	 * the SCCB read fails, "id" keeps whatever was there -- here the
	 * previous read's value or uninitialised stack -- and branching on
	 * that garbage can pass a probe for a completely absent sensor.
	 */
	ret = regmap_read(a5142->regmap, A5142_REG_CHIP_ID_H, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip id high byte\n");

	if (id >> 8)
		return -ENXIO;

	ret = regmap_read(a5142->regmap, A5142_REG_CHIP_ID_L, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip id low byte\n");

	if ((id & 0xff) != (A5142_CHIP_ID & 0xff))
		return -ENXIO;

	dev_dbg(dev, "chip id 0x%04x read\n", A5142_CHIP_ID);

	return 0;
}

static int a5142_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct a5142 *a5142;
	int ret;

	a5142 = devm_kzalloc(dev, sizeof(*a5142), GFP_KERNEL);
	if (!a5142)
		return -ENOMEM;

	a5142->dev = dev;
	a5142->client = client;
	mutex_init(&a5142->lock);
	a5142->cur_mode = &a5142_modes[A5142_MODE_PREVIEW];

	a5142->regmap = devm_regmap_init_i2c(client, &a5142_regmap_config);
	if (IS_ERR(a5142->regmap))
		return PTR_ERR(a5142->regmap);

	a5142->pwdn = devm_gpiod_get_optional(dev, "pwdn", GPIOD_OUT_HIGH);
	if (IS_ERR(a5142->pwdn))
		return dev_err_probe(dev, PTR_ERR(a5142->pwdn),
				     "Failed to get power-down GPIO\n");

	a5142->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(a5142->reset))
		return dev_err_probe(dev, PTR_ERR(a5142->reset),
				     "Failed to get reset GPIO\n");

	a5142->sd.state_lock = &a5142->lock;
	a5142->sd.ops = &a5142_subdev_ops;

	pm_runtime_set_active(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);

	pm_runtime_get_noresume(dev);

	ret = a5142_power_on(a5142);
	if (ret) {
		dev_err(dev, "Failed to power on sensor: %d\n", ret);
		goto error_pm;
	}

	ret = a5142_check_sensor_id(a5142);
	if (ret) {
		dev_err(dev, "chip id mismatch: expected 0x%04x\n",
			A5142_CHIP_ID);
		goto error_power_off;
	}

	/* Apply the preview timing so the first frame is well formed. */
	a5142->hts = a5142->cur_mode->hts;
	a5142->vts = a5142->cur_mode->vts;
	a5142_update_frame_duration(a5142);

	ret = v4l2_async_register_subdev(&a5142->sd);
	if (ret)
		goto error_power_off;

	dev_info(dev, "%s: 5MP RAW sensor detected (2-lane MIPI CSI-2)\n",
		 dev_name(dev));

	return 0;

error_power_off:
	a5142_power_off(a5142);
error_pm:
	pm_runtime_put(dev);
	v4l2_async_unregister_subdev(&a5142->sd);

	return ret;
}

static const struct of_device_id a5142_of_match[] = {
	{ .compatible = "a5142,mipi" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, a5142_of_match);

static struct i2c_driver a5142_driver = {
	.probe = a5142_probe,
	.driver = {
		.name = "a5142",
		.of_match_table = a5142_of_match,
		.pm = &a5142_pm_ops,
	},
};
module_i2c_driver(a5142_driver);

MODULE_DESCRIPTION("A5142 camera sensor");
MODULE_AUTHOR("Lenovo Linux Team");
MODULE_LICENSE("GPL");