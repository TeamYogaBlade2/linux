// SPDX-License-Identifier: GPL-2.0-only
/*
 * FM50AF voice coil motor driver.
 *
 * The FM50AF is the autofocus actuator on the Lenovo B8000 (MT6589) main
 * camera, at I2C address 0x18 on bus 1.
 *
 * Protocol, from the vendor driver
 * (mediatek/custom/common/kernel/lens/fm50af/FM50AF.c):
 *
 *	write: two bytes, { data[9:4], (data[3:0] << 4) | sr }
 *	read:  two bytes, data = (byte0 << 4) | (byte1 >> 4)
 *
 * There is no register address: the whole transfer is a 10-bit DAC value,
 * so unlike DW9807/DW9768 there is nothing to point at first.  The low
 * nibble of the second byte carries "sr" (slew rate), which the vendor
 * driver hardcodes to 3 for every move - there is no path that sets any
 * other value, and no read-modify-write of it, so this driver uses the same
 * constant rather than inventing a control for it.
 *
 * The vendor's position range is 0 (infinity) .. 1023 (macro), which is the
 * full 10-bit DAC range and matches g_u4FM50AF_MACRO = 1023.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define FM50AF_MAX_FOCUS_POS	1023

/*
 * Slew-rate nibble written into the low nibble of the second byte.  The
 * vendor driver sets g_sr = 3 immediately before every write and never
 * changes it, so this is a constant and not a tunable.
 */
#define FM50AF_SLEW_RATE	3

/*
 * Time for the actuator to settle after a move.  The vendor driver returns
 * from its move ioctl as soon as the I2C write completes and leaves the
 * caller to wait, so this is a floor that keeps a control step from being
 * issued while the previous one is still travelling.
 */
#define FM50AF_CTRL_DELAY_US	1000

struct fm50af_device {
	struct v4l2_ctrl_handler ctrls_vcm;
	struct v4l2_subdev sd;
	u16 current_val;
};

static inline struct fm50af_device *sd_to_fm50af_vcm(struct v4l2_subdev *sd)
{
	return container_of(sd, struct fm50af_device, sd);
}

/*
 * Pack a 10-bit DAC value into the wire format.  data[9:4] goes in the
 * first byte and data[3:0] in the high nibble of the second; the low
 * nibble is the slew rate.
 */
static int fm50af_write_dac(struct i2c_client *client, u16 value)
{
	u8 buf[2];

	buf[0] = (value >> 4) & 0xff;
	buf[1] = ((value & 0x0f) << 4) | FM50AF_SLEW_RATE;

	return i2c_master_send(client, buf, sizeof(buf));
}

static int fm50af_init(struct fm50af_device *priv)
{
	struct i2c_client *client = v4l2_get_subdevdata(&priv->sd);
	int ret;

	ret = fm50af_write_dac(client, 0);
	if (ret < 0)
		return ret;

	priv->current_val = 0;
	/* Let the actuator reach the parked position before anyone moves it. */
	usleep_range(FM50AF_CTRL_DELAY_US, FM50AF_CTRL_DELAY_US + 1000);

	return 0;
}

static int fm50af_power_on(struct fm50af_device *priv)
{
	struct device *dev = priv->sd.dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = fm50af_init(priv);
	if (ret < 0)
		pm_runtime_put(dev);

	return ret;
}

static void fm50af_power_off(struct fm50af_device *priv)
{
	pm_runtime_put(priv->sd.dev);
}

static int fm50af_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct fm50af_device *priv =
		container_of(ctrl->handler, struct fm50af_device, ctrls_vcm);
	struct i2c_client *client = v4l2_get_subdevdata(&priv->sd);
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
		ret = fm50af_write_dac(client, ctrl->val);
		if (ret < 0)
			return ret;

		priv->current_val = ctrl->val;
		usleep_range(FM50AF_CTRL_DELAY_US, FM50AF_CTRL_DELAY_US + 1000);
		return 0;

	case V4L2_CID_POWER_LINE_FREQUENCY:
	case V4L2_CID_AUTOGAIN:
	case V4L2_CID_GAIN:
	case V4L2_CID_EXPOSURE_AUTO:
	case V4L2_CID_EXPOSURE_ABSOLUTE:
	case V4L2_CID_TEST_PATTERN:
		/* Not applicable to an actuator; accepted and ignored. */
		return 0;
	}

	return -EINVAL;
}

static int fm50af_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct fm50af_device *priv = sd_to_fm50af_vcm(sd);

	return fm50af_power_on(priv);
}

static int fm50af_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct fm50af_device *priv = sd_to_fm50af_vcm(sd);

	fm50af_power_off(priv);

	return 0;
}

static const struct v4l2_subdev_internal_ops fm50af_internal_ops = {
	.open = fm50af_open,
	.close = fm50af_close,
};

static const struct v4l2_ctrl_ops fm50af_ctrl_ops = {
	.s_ctrl = fm50af_s_ctrl,
};

static const struct v4l2_subdev_ops fm50af_subdev_ops = {
	/* A lens has no video-format state; the controls are the interface. */
};

static int fm50af_probe(struct i2c_client *client)
{
	struct fm50af_device *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&priv->sd, client, &fm50af_subdev_ops);
	priv->sd.internal_ops = &fm50af_internal_ops;
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	v4l2_ctrl_handler_init(&priv->ctrls_vcm, 1);
	v4l2_ctrl_new_std(&priv->ctrls_vcm, &fm50af_ctrl_ops,
			  V4L2_CID_FOCUS_ABSOLUTE, 0, FM50AF_MAX_FOCUS_POS,
			  1, 0);
	if (priv->ctrls_vcm.error) {
		ret = priv->ctrls_vcm.error;
		goto err_free_ctrls;
	}

	priv->sd.ctrl_handler = &priv->ctrls_vcm;

	ret = v4l2_ctrl_handler_setup(&priv->ctrls_vcm);
	if (ret < 0)
		goto err_free_ctrls;

	ret = v4l2_async_register_subdev(&priv->sd);
	if (ret < 0)
		goto err_free_ctrls;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);

	dev_dbg(&client->dev, "FM50AF VCM registered\n");

	return 0;

err_free_ctrls:
	v4l2_ctrl_handler_free(&priv->ctrls_vcm);
	return ret;
}

static void fm50af_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct fm50af_device *priv = sd_to_fm50af_vcm(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&priv->ctrls_vcm);
	pm_runtime_disable(&client->dev);
}

static const struct i2c_device_id fm50af_id[] = {
	{ "fm50af" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fm50af_id);

static const struct of_device_id fm50af_of_match[] = {
	{ .compatible = "mediatek,fm50af" },
	{ }
};
MODULE_DEVICE_TABLE(of, fm50af_of_match);

static struct i2c_driver fm50af_driver = {
	.driver = {
		.name = "fm50af",
		.of_match_table = fm50af_of_match,
	},
	.probe = fm50af_probe,
	.remove = fm50af_remove,
	.id_table = fm50af_id,
};

module_i2c_driver(fm50af_driver);

MODULE_DESCRIPTION("FM50AF voice coil motor driver");
MODULE_LICENSE("GPL");
