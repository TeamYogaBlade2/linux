// SPDX-License-Identifier: GPL-2.0-only
/*
 * OmniVision OV9760 1.2 megapixel CMOS image sensor.
 *
 * The OV9760 is configured in the vendor tree as the sub (front) camera of
 * the Lenovo YOGA Tablet 8/10 (MT6589):
 *
 *	CUSTOM_KERNEL_MAIN_IMGSENSOR = a5142_mipi_raw
 *	CUSTOM_KERNEL_SUB_IMGSENSOR  = ov9760_mipi_raw
 *	CUSTOM_KERNEL_SUB_LENS       = dummy_lens
 *
 * (mediatek/config/blade10/ProjectConfig-jb2.mk, MTK_PLATFORM = MT6589).
 * The sub camera has no autofocus actuator, which is why its lens entry is
 * dummy_lens and there is no companion VCM driver.
 *
 * Bus protocol
 * ------------
 * 16-bit register addresses and 8-bit data.  Three-byte write
 * { addr_hi, addr_lo, value } and a two-byte address followed by one data
 * byte for a read (ov9760mipiraw_Sensor.c:100-111).
 *
 * I2C address
 * -----------
 * The vendor probe walks a two-entry list of write addresses, 0x6c then 0x20,
 * and adopts the first one that answers with the right ID
 * (ov9760mipiraw_Sensor.c:199-206, ov9760mipiraw_Sensor.h:167-168).  Which
 * one is actually strapped on this board is NOT determinable from the
 * available sources - see the note in the board DTS.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* System control. */
#define OV9760_REG_SW_RESET		0x0103
#define OV9760_REG_MODE_SELECT		0x0100
#define OV9760_REG_IMAGE_ORIENTATION	0x0101

/* Identification: 0x0000/0x0001 read back as 0x9760. */
#define OV9760_REG_CHIP_ID_HI		0x0000
#define OV9760_REG_CHIP_ID_LO		0x0001
#define OV9760_CHIP_ID			0x9760

/* Timing: only VTS is driven at runtime, for the blanking control. */
#define OV9760_REG_VTS			0x0340

/*
 * Geometry, binning and the odd-increment registers are all fixed by the one
 * mode table below, so they are not given symbolic names: nothing in the
 * runtime paths addresses them.
 */

/* Exposure: 20-bit, split across 0x3500/0x3501/0x3502. */
#define OV9760_REG_EXPOSURE_HH		0x3500
#define OV9760_REG_EXPOSURE_H		0x3501
#define OV9760_REG_EXPOSURE_L		0x3502
/* 0x3503 = 0x27 selects manual gain/exposure (vendor comment on :277). */
#define OV9760_REG_AEC_AGC_MANUAL	0x3503
#define OV9760_AEC_AGC_MANUAL_ON	0x27

/*
 * Gain.  The "real gain mode" bit is 0x3509 = 0x10, set by the mode table at
 * :278, and no runtime path needs to touch it again.
 */
#define OV9760_REG_GAIN_H		0x350A
#define OV9760_REG_GAIN_L		0x350B
/*
 * OV9760MIPI_Gain2reg() computes reg = gain / (BASEGAIN / 16), i.e.
 * gain / 4, and OV9760_Set_Gain() clamps the result to 0x3d0
 * (ov9760mipiraw_Sensor.c:920-941).  So the register is 16.4 fixed point in
 * units of 1/16x and the ceiling corresponds to 61x.
 */
#define OV9760_GAIN_REG_MAX		0x3d0
#define OV9760_GAIN_MIN			0x000	/* 0x -> 0x3d0 == 0x..61x */
#define OV9760_GAIN_MAX			OV9760_GAIN_REG_MAX
#define OV9760_GAIN_DEFAULT		0x010

/* Test pattern: 0x5a80 = 0x84 enables it, 0x00 disables (:1266-1276). */
#define OV9760_REG_TEST_PATTERN		0x5a80
#define OV9760_TEST_PATTERN_ON		0x84

/*
 * Only the full-resolution table is carried.  OV9760MIPI_Set720P() is
 * misnamed: despite the name it programs 1472x1104, which is what the vendor
 * HAL lists as the maximum picture size for this part
 * (config.ftbl.ov9760_mipi_raw.h: ITEM_AS_DEFAULT_("1472x1104")).
 */
#define OV9760_FULL_WIDTH		1472u
#define OV9760_FULL_HEIGHT		1104u
/*
 * VTS 0x047c = 1148 and HTS 0x06c8 = 1736, the only timing the table sets
 * (:226-229).  They match OV9760MIPI_PERIOD_LINE_NUMS/PERIOD_PIXEL_NUMS
 * (ov9760mipiraw_Sensor.h:154-155).  60 MHz / (1736 * 1148) = 30.1 fps,
 * consistent with the 30 fps the vendor's flicker table assumes.
 */
#define OV9760_FULL_VTS			1148u
#define OV9760_FULL_HTS			1736u
#define OV9760_VBLANK_MIN		(OV9760_FULL_VTS - OV9760_FULL_HEIGHT)
#define OV9760_VTS_MAX			0xffffu

/*
 * External clock.  The vendor's PLL comment block (ov9760mipiraw_Sensor.c:
 * 545-553) documents 24 MHz input producing a 60 MHz system clock, and every
 * divider in the table is chosen for that input, so other rates are refused
 * rather than silently producing a wrong data rate.
 */
#define OV9760_EXTCLK_MIN		(24 * 1000 * 1000)
#define OV9760_EXTCLK_MAX		(24 * 1000 * 1000)

#define OV9760_LANES			1

struct ov9760_reg {
	u16 addr;
	u8 val;
};

struct ov9760_ctrls {
	struct v4l2_ctrl_handler handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *test_pattern;
};

struct ov9760_device {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *extclk;
	struct regulator *vdd;
	struct regulator *vddio;

	/* Protects everything below. */
	struct mutex lock;

	struct v4l2_mbus_framefmt fmt;
	struct ov9760_ctrls ctrls;
	unsigned int lane_count;
};

static inline struct ov9760_device *to_ov9760(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov9760_device, sd);
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct ov9760_device,
			     ctrls.handler)->sd;
}

/* -------------------------------------------------------------------------
 * Register access
 */

static int ov9760_write(struct ov9760_device *dev, u16 addr, u8 val)
{
	struct i2c_client *client = dev->client;
	u8 buf[3] = { addr >> 8, addr & 0xff, val };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	return ret == sizeof(buf) ? 0 : -EIO;
}

static int ov9760_read(struct ov9760_device *dev, u16 addr, u8 *val)
{
	struct i2c_client *client = dev->client;
	u8 buf[2] = { addr >> 8, addr & 0xff };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	ret = i2c_master_recv(client, val, 1);
	if (ret < 0)
		return ret;

	return ret == 1 ? 0 : -EIO;
}

static int ov9760_write_table(struct ov9760_device *dev,
			      const struct ov9760_reg *regs, unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = ov9760_write(dev, regs[i].addr, regs[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Register sequence, transcribed from the vendor tree
 *
 * Every entry is one OV9760MIPI_write_cmos_sensor() call in
 * OV9760MIPI_Set720P() (ov9760mipiraw_Sensor.c:222-331) with its source
 * line.  The function name says 720p but the output-size registers below
 * program 1472x1104; see the note on OV9760_FULL_WIDTH.
 *
 * The vendor's step order is preserved, including the fact that streaming is
 * left OFF at :250 and turned back on at the very end (:327).
 */
static const struct ov9760_reg ov9760_init_regs[] = {
	{ 0x0103, 0x01 },	/* :224 software reset, then 10 ms (:225) */
	{ 0x0340, 0x04 },	/* :226 VTS high */
	{ 0x0341, 0x7c },	/* :227 VTS low = 1148 */
	{ 0x0342, 0x06 },	/* :228 HTS high */
	{ 0x0343, 0xc8 },	/* :229 HTS low = 1736 */
	{ 0x0344, 0x00 },	/* :230 x_addr_start high */
	{ 0x0345, 0x08 },	/* :231 x_addr_start low = 8 */
	{ 0x0346, 0x00 },	/* :232 y_addr_start high */
	{ 0x0347, 0x02 },	/* :233 y_addr_start low = 2 */
	{ 0x0348, 0x05 },	/* :234 x_addr_end high */
	{ 0x0349, 0xdf },	/* :235 x_addr_end low = 1503 */
	{ 0x034a, 0x04 },	/* :236 y_addr_end high */
	{ 0x034b, 0x65 },	/* :237 y_addr_end low = 1125 */
	{ 0x3811, 0x04 },	/* :238 x_offset */
	{ 0x3813, 0x04 },	/* :239 y_offset */
	{ 0x034c, 0x05 },	/* :240 x_output_size high */
	{ 0x034d, 0xc0 },	/* :241 x_output_size low = 1472 */
	{ 0x034e, 0x04 },	/* :242 y_output_size high */
	{ 0x034f, 0x50 },	/* :243 y_output_size low = 1104 */
	{ 0x0383, 0x01 },	/* :244 x_odd_inc: no subsampling */
	{ 0x0387, 0x01 },	/* :245 y_odd_inc: no subsampling */
	{ 0x3820, 0x00 },	/* :246 vertical binning off */
	{ 0x3821, 0x00 },	/* :247 horizontal binning off */
	{ 0x3660, 0x80 },	/* :248 analog control */
	{ 0x3680, 0xf4 },	/* :249 analog control */
	{ 0x0100, 0x00 },	/* :250 stream off */
	{ 0x3002, 0x80 },	/* :251 IO control */
	{ 0x3012, 0x08 },	/* :252 MIPI control */
	{ 0x3014, 0x04 },	/* :253 MIPI control */
	{ 0x3022, 0x02 },	/* :254 analog control */
	{ 0x3023, 0x0f },	/* :255 analog control */
	/* :256-276 PLL block; no per-register comments in the vendor source. */
	{ 0x3080, 0x00 },	/* :256 */
	{ 0x3090, 0x02 },	/* :257 */
	{ 0x3091, 0x28 },	/* :258 */
	{ 0x3092, 0x02 },	/* :259 */
	{ 0x3093, 0x02 },	/* :260 */
	{ 0x3094, 0x00 },	/* :261 */
	{ 0x3095, 0x00 },	/* :262 */
	{ 0x3096, 0x01 },	/* :263 */
	{ 0x3097, 0x00 },	/* :264 */
	{ 0x3098, 0x04 },	/* :265 */
	{ 0x3099, 0x14 },	/* :266 */
	{ 0x309a, 0x03 },	/* :267 */
	{ 0x309c, 0x00 },	/* :268 */
	{ 0x309d, 0x00 },	/* :269 */
	{ 0x309e, 0x01 },	/* :270 */
	{ 0x309f, 0x00 },	/* :271 */
	{ 0x30a2, 0x01 },	/* :272 */
	{ 0x30b0, 0x0a },	/* :273 */
	{ 0x30b3, 0x32 },	/* :274 */
	{ 0x30b4, 0x02 },	/* :275 */
	{ 0x30b5, 0x00 },	/* :276 */
	{ 0x3503, 0x27 },	/* :277 manual gain/exposure (0x17 = auto) */
	{ 0x3509, 0x10 },	/* :278 "real gain" AEC mode */
	{ 0x3600, 0x7c },	/* :279 analog control */
	{ 0x3621, 0xb8 },	/* :280 analog control */
	{ 0x3622, 0x23 },	/* :281 analog control */
	{ 0x3631, 0xe2 },	/* :282 analog control */
	{ 0x3634, 0x03 },	/* :283 analog control */
	{ 0x3662, 0x14 },	/* :284 analog control */
	{ 0x366b, 0x03 },	/* :285 analog control */
	{ 0x3682, 0x82 },	/* :286 analog control */
	{ 0x3705, 0x20 },	/* :287 sensor control; vendor gives no name */
	{ 0x3708, 0x64 },	/* :288 sensor control */
	{ 0x371b, 0x60 },	/* :289 sensor control */
	{ 0x3732, 0x40 },	/* :290 sensor control */
	{ 0x3745, 0x00 },	/* :291 sensor control */
	{ 0x3746, 0x18 },	/* :292 sensor control */
	{ 0x3780, 0x2a },	/* :293 sensor control */
	{ 0x3781, 0x8c },	/* :294 sensor control */
	{ 0x378f, 0xf5 },	/* :295 sensor control */
	{ 0x3823, 0x37 },	/* :296 internal timing */
	{ 0x383d, 0x88 },	/* :297 BLC start row */
	{ 0x4000, 0x23 },	/* :298 BLC control */
	{ 0x4001, 0x04 },	/* :299 BLC control */
	{ 0x4002, 0x45 },	/* :300 BLC control */
	{ 0x4004, 0x08 },	/* :301 BLC control */
	{ 0x4005, 0x40 },	/* :302 BLC for flashing */
	{ 0x4006, 0x40 },	/* :303 BLC control */
	{ 0x4009, 0x40 },	/* :304 BLC */
	{ 0x404f, 0x8f },	/* :305 BLC black-level stabilisation */
	{ 0x4058, 0x44 },	/* :306 BLC control */
	{ 0x4101, 0x32 },	/* :307 BLC control */
	{ 0x4102, 0xa4 },	/* :308 BLC control */
	{ 0x4520, 0xb0 },	/* :309 full-resolution setting */
	{ 0x4580, 0x08 },	/* :310 HDR gain latch bypass */
	{ 0x4582, 0x00 },	/* :311 HDR gain latch bypass */
	{ 0x4307, 0x30 },	/* :312 MIPI control */
	{ 0x4605, 0x00 },	/* :313 VFIFO control */
	{ 0x4608, 0x02 },	/* :314 VFIFO control */
	{ 0x4609, 0x00 },	/* :315 VFIFO control */
	{ 0x4801, 0x0f },	/* :316 MIPI control */
	{ 0x4819, 0xb6 },	/* :317 MIPI control */
	{ 0x4837, 0x21 },	/* :318 MIPI control */
	{ 0x4906, 0xff },	/* :319 internal timing */
	{ 0x4d00, 0x04 },	/* :320 temperature sensor */
	{ 0x4d01, 0x4b },	/* :321 temperature sensor */
	{ 0x4d02, 0xfe },	/* :322 temperature sensor */
	{ 0x4d03, 0x09 },	/* :323 temperature sensor */
	{ 0x4d04, 0x1e },	/* :324 temperature sensor */
	{ 0x4d05, 0xb7 },	/* :325 temperature sensor */
	{ 0x4800, 0x44 },	/* :326 MIPI short packet */
	{ 0x0100, 0x01 },	/* :327 stream on */
};

/* -------------------------------------------------------------------------
 * Sensor state
 */

static int ov9760_chip_id_check(struct ov9760_device *dev)
{
	u8 hi, lo;
	int ret;

	ret = ov9760_read(dev, OV9760_REG_CHIP_ID_HI, &hi);
	if (ret)
		return ret;
	ret = ov9760_read(dev, OV9760_REG_CHIP_ID_LO, &lo);
	if (ret)
		return ret;

	if (((hi << 8) | lo) != OV9760_CHIP_ID)
		return dev_err_probe(&dev->client->dev, -ENODEV,
				     "chip id %02x%02x, expected %04x\n",
				     hi, lo, OV9760_CHIP_ID);

	return 0;
}

/*
 * Orientation.  Same packing as the A5142: bit 0 horizontal, bit 1 vertical,
 * written as one byte to 0x0101 (ov9760mipiraw_Sensor.c:520-556).
 */
static int ov9760_set_orientation(struct ov9760_device *dev)
{
	u8 val = 0;

	if (dev->ctrls.hflip->val)
		val |= BIT(0);
	if (dev->ctrls.vflip->val)
		val |= BIT(1);

	return ov9760_write(dev, OV9760_REG_IMAGE_ORIENTATION, val);
}

/*
 * Exposure is 20 bits split over three registers, with the low nibble of
 * 0x3502 carrying the fractional part: the vendor writes
 *	0x3502 = (lines & 0x00f) << 4
 *	0x3501 = (lines & 0xfff) >> 4
 *	0x3500 =  lines >> 12
 * (ov9760mipiraw_Sensor.c:897-899).  The control is therefore in 1/16 line
 * units; this driver keeps that granularity so the value it writes is the
 * value the vendor writes.
 */
static int ov9760_set_exposure(struct ov9760_device *dev, u32 lines)
{
	int ret;

	ret = ov9760_write(dev, OV9760_REG_EXPOSURE_L, (lines & 0x00f) << 4);
	if (ret)
		return ret;
	ret = ov9760_write(dev, OV9760_REG_EXPOSURE_H, (lines >> 4) & 0xff);
	if (ret)
		return ret;

	return ov9760_write(dev, OV9760_REG_EXPOSURE_HH, (lines >> 12) & 0xff);
}

static int ov9760_set_gain(struct ov9760_device *dev, u32 reg_gain)
{
	int ret;

	ret = ov9760_write(dev, OV9760_REG_GAIN_L, reg_gain & 0xff);
	if (ret)
		return ret;

	return ov9760_write(dev, OV9760_REG_GAIN_H, (reg_gain >> 8) & 0xff);
}

static int ov9760_set_vts(struct ov9760_device *dev, u32 vts)
{
	int ret;

	ret = ov9760_write(dev, OV9760_REG_VTS, (vts >> 8) & 0xff);
	if (ret)
		return ret;

	return ov9760_write(dev, OV9760_REG_VTS + 1, vts & 0xff);
}

static int ov9760_power_on(struct device *pdev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(pdev);
	struct ov9760_device *dev = to_ov9760(sd);
	int ret;

	ret = regulator_enable(dev->vddio);
	if (ret) {
		dev_err(pdev, "failed to enable vddio: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(dev->vdd);
	if (ret) {
		dev_err(pdev, "failed to enable vdd: %d\n", ret);
		goto err_vddio;
	}

	ret = clk_prepare_enable(dev->extclk);
	if (ret) {
		dev_err(pdev, "failed to enable extclk: %d\n", ret);
		goto err_vdd;
	}

	/*
	 * The vendor power-on holds reset low for 10 ms before releasing it
	 * (kd_camera_hw.c, pinSet[IDX_PS_CMRST]).  The GPIO is not modelled
	 * here yet, so only the settle time is honoured.
	 */
	usleep_range(10000, 11000);

	return 0;

err_vdd:
	regulator_disable(dev->vdd);
err_vddio:
	regulator_disable(dev->vddio);
	return ret;
}

static int ov9760_power_off(struct device *pdev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(pdev);
	struct ov9760_device *dev = to_ov9760(sd);

	clk_disable_unprepare(dev->extclk);
	regulator_disable(dev->vdd);
	regulator_disable(dev->vddio);

	return 0;
}

/* -------------------------------------------------------------------------
 * V4L2 controls
 */

static int ov9760_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct ov9760_device *dev = to_ov9760(sd);
	int ret;

	/* v4l2_ctrl_lock() takes dev->lock for us. */
	if (!pm_runtime_get_if_in_use(&dev->client->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = ov9760_set_exposure(dev, ctrl->val);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov9760_set_gain(dev, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		ret = ov9760_set_vts(dev, dev->fmt.height + ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = ov9760_set_orientation(dev);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov9760_write(dev, OV9760_REG_TEST_PATTERN,
				   ctrl->val ? OV9760_TEST_PATTERN_ON : 0);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(&dev->client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov9760_ctrl_ops = {
	.s_ctrl = ov9760_s_ctrl,
};

static const char * const ov9760_test_pattern_menu[] = {
	"Disabled",
	"Enabled",
};

static int ov9760_init_controls(struct ov9760_device *dev)
{
	struct v4l2_ctrl_handler *hdl = &dev->ctrls.handler;
	const struct v4l2_ctrl_ops *ops = &ov9760_ctrl_ops;
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);
	hdl->lock = &dev->lock;
	dev->sd.ctrl_handler = hdl;

	/*
	 * Exposure in 1/16 line units: 16 * VTS is the largest value the two
	 * integer registers can hold, and the vendor clamps the frame height
	 * itself when exposure exceeds it (:846-858).
	 */
	dev->ctrls.exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
						16, 16 * OV9760_FULL_VTS, 1,
						16 * 0x20);

	dev->ctrls.analogue_gain = v4l2_ctrl_new_std(hdl, ops,
						     V4L2_CID_ANALOGUE_GAIN,
						     OV9760_GAIN_MIN,
						     OV9760_GAIN_MAX, 1,
						     OV9760_GAIN_DEFAULT);

	dev->ctrls.vblank = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VBLANK,
					      OV9760_VBLANK_MIN,
					      OV9760_VTS_MAX -
					      OV9760_FULL_HEIGHT, 1,
					      OV9760_VBLANK_MIN);

	dev->ctrls.hflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP,
					     0, 1, 1, 0);
	dev->ctrls.vflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP,
					     0, 1, 1, 0);

	dev->ctrls.test_pattern =
		v4l2_ctrl_new_std_menu_items(hdl, ops, V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(ov9760_test_pattern_menu) - 1,
					     0, 0, ov9760_test_pattern_menu);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Pad operations
 */

static int ov9760_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	/*
	 * The part outputs RAW10 on MIPI.  The vendor model is
	 * SENSOR_OUTPUT_FORMAT_RAW_B (ov9760mipiraw_Sensor.c:76), so the first
	 * Bayer phase is blue on the first row.
	 */
	code->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	return 0;
}

static int ov9760_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index || fse->code != MEDIA_BUS_FMT_SBGGR10_1X10)
		return -EINVAL;

	fse->min_width = OV9760_FULL_WIDTH;
	fse->max_width = OV9760_FULL_WIDTH;
	fse->min_height = OV9760_FULL_HEIGHT;
	fse->max_height = OV9760_FULL_HEIGHT;

	return 0;
}

static void ov9760_fill_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->width = OV9760_FULL_WIDTH;
	fmt->height = OV9760_FULL_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int ov9760_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct ov9760_device *dev = to_ov9760(sd);
	struct v4l2_mbus_framefmt *fmt;

	mutex_lock(&dev->lock);
	if (format->which == V4L2_SUBDEV_FORMAT_TRY)
		fmt = v4l2_subdev_state_get_format(state, format->pad);
	else
		fmt = &dev->fmt;
	format->format = *fmt;
	mutex_unlock(&dev->lock);

	return 0;
}

static int ov9760_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct ov9760_device *dev = to_ov9760(sd);
	struct v4l2_mbus_framefmt *fmt;

	/*
	 * The format is fixed: the vendor driver exposes exactly one mode and
	 * there is no scaling or cropping path in the table, so a request for
	 * anything else is adjusted back to it.
	 */
	ov9760_fill_format(&format->format);

	mutex_lock(&dev->lock);
	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt = v4l2_subdev_state_get_format(state, format->pad);
		*fmt = format->format;
	} else {
		dev->fmt = format->format;
	}
	mutex_unlock(&dev->lock);

	return 0;
}

static int ov9760_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	if (sel->target != V4L2_SEL_TGT_CROP &&
	    sel->target != V4L2_SEL_TGT_CROP_DEFAULT &&
	    sel->target != V4L2_SEL_TGT_CROP_BOUNDS)
		return -EINVAL;

	sel->r.left = 0;
	sel->r.top = 0;
	sel->r.width = OV9760_FULL_WIDTH;
	sel->r.height = OV9760_FULL_HEIGHT;

	return 0;
}

static const struct v4l2_subdev_pad_ops ov9760_pad_ops = {
	.enum_mbus_code = ov9760_enum_mbus_code,
	.enum_frame_size = ov9760_enum_frame_size,
	.get_fmt = ov9760_get_fmt,
	.set_fmt = ov9760_set_fmt,
	.get_selection = ov9760_get_selection,
};

/* -------------------------------------------------------------------------
 * Video operations
 */

static int ov9760_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov9760_device *dev = to_ov9760(sd);
	int ret = 0;

	mutex_lock(&dev->lock);

	if (enable) {
		ret = pm_runtime_resume_and_get(&dev->client->dev);
		if (ret)
			goto out;

		ret = ov9760_chip_id_check(dev);
		if (ret)
			goto err_pm;

		ret = ov9760_write_table(dev, ov9760_init_regs,
					 ARRAY_SIZE(ov9760_init_regs));
		if (ret)
			goto err_pm;

		/* The table ends with stream-on; make sure MSB stays manual. */
		ret = ov9760_write(dev, OV9760_REG_AEC_AGC_MANUAL,
				   OV9760_AEC_AGC_MANUAL_ON);
		if (ret)
			goto err_pm;

		ret = __v4l2_ctrl_handler_setup(&dev->ctrls.handler);
		if (ret)
			goto err_pm;
	} else {
		ret = ov9760_write(dev, OV9760_REG_MODE_SELECT, 0);
		pm_runtime_put(&dev->client->dev);
	}

	mutex_unlock(&dev->lock);
	return ret;

err_pm:
	pm_runtime_put(&dev->client->dev);
out:
	mutex_unlock(&dev->lock);
	return ret;
}

static const struct v4l2_subdev_video_ops ov9760_video_ops = {
	.s_stream = ov9760_s_stream,
};

static const struct v4l2_subdev_ops ov9760_subdev_ops = {
	.pad = &ov9760_pad_ops,
	.video = &ov9760_video_ops,
};

/* -------------------------------------------------------------------------
 * Probe / remove
 */

static int ov9760_probe(struct i2c_client *client)
{
	struct device *pdev = &client->dev;
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	struct ov9760_device *dev;
	unsigned long extclk_rate;
	int ret;

	dev = devm_kzalloc(pdev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->client = client;
	ov9760_fill_format(&dev->fmt);

	endpoint = fwnode_graph_get_endpoint_by_id(dev_fwnode(pdev), 0, 0,
						   FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!endpoint)
		return dev_err_probe(pdev, -ENODEV, "no endpoint node\n");

	ret = v4l2_fwnode_endpoint_parse(endpoint, &ep);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(pdev, ret, "failed to parse endpoint\n");

	if (ep.bus_type != V4L2_MBUS_CSI2_DPHY)
		return dev_err_probe(pdev, -EINVAL, "not a MIPI CSI-2 endpoint\n");

	dev->lane_count = ep.bus.mipi_csi2.num_data_lanes;
	if (dev->lane_count != OV9760_LANES)
		return dev_err_probe(pdev, -EINVAL,
				     "%u lanes, the vendor table is 1-lane\n",
				     dev->lane_count);

	dev->extclk = devm_clk_get(pdev, "extclk");
	if (IS_ERR(dev->extclk))
		return dev_err_probe(pdev, PTR_ERR(dev->extclk),
				     "failed to get extclk\n");

	extclk_rate = clk_get_rate(dev->extclk);
	if (extclk_rate < OV9760_EXTCLK_MIN ||
	    extclk_rate > OV9760_EXTCLK_MAX)
		return dev_err_probe(pdev, -EINVAL,
				     "extclk %lu Hz, the PLL table is built for %u Hz\n",
				     extclk_rate, OV9760_EXTCLK_MIN);

	dev->vdd = devm_regulator_get(pdev, "vdd");
	if (IS_ERR(dev->vdd))
		return dev_err_probe(pdev, PTR_ERR(dev->vdd),
				     "failed to get vdd\n");

	dev->vddio = devm_regulator_get(pdev, "vddio");
	if (IS_ERR(dev->vddio))
		return dev_err_probe(pdev, PTR_ERR(dev->vddio),
				     "failed to get vddio\n");

	mutex_init(&dev->lock);

	v4l2_i2c_subdev_init(&dev->sd, client, &ov9760_subdev_ops);
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	dev->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	ret = media_entity_pads_init(&dev->sd.entity, 1, &dev->pad);
	if (ret)
		goto err_mutex;

	ret = ov9760_init_controls(dev);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(pdev);
	pm_runtime_enable(pdev);
	pm_runtime_idle(pdev);

	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret)
		goto err_pm;

	dev_info(pdev, "OV9760 on i2c-%d at %#04x\n",
		 client->adapter->nr, client->addr);

	return 0;

err_pm:
	pm_runtime_disable(pdev);
	v4l2_ctrl_handler_free(&dev->ctrls.handler);
err_entity:
	media_entity_cleanup(&dev->sd.entity);
err_mutex:
	mutex_destroy(&dev->lock);
	return ret;
}

static void ov9760_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov9760_device *dev = to_ov9760(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&dev->ctrls.handler);
	pm_runtime_disable(&client->dev);
	mutex_destroy(&dev->lock);
}

static const struct dev_pm_ops ov9760_pm_ops = {
	SET_RUNTIME_PM_OPS(ov9760_power_off, ov9760_power_on, NULL)
};

static const struct of_device_id ov9760_of_match[] = {
	{ .compatible = "ovti,ov9760" },
	{ }
};
MODULE_DEVICE_TABLE(of, ov9760_of_match);

static struct i2c_driver ov9760_driver = {
	.driver = {
		.name = "ov9760",
		.pm = &ov9760_pm_ops,
		.of_match_table = ov9760_of_match,
	},
	.probe = ov9760_probe,
	.remove = ov9760_remove,
};
module_i2c_driver(ov9760_driver);

MODULE_DESCRIPTION("OmniVision OV9760 camera sensor driver");
MODULE_LICENSE("GPL");
