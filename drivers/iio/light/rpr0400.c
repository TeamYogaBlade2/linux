// SPDX-License-Identifier: GPL-2.0-only
/*
 * Rohm RPR-0400 ambient-light and proximity sensor driver.
 *
 * The RPR-0400 is an older sibling of the RPR-0521 with the same broad
 * register layout (control regs starting at 0x40, 16-bit little-endian
 * data registers from 0x44) but a different control-register split and
 * no readable ID register.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>

#define RPR0400_REG_SYSTEM_CTRL			0x40
#define RPR0400_SYSTEM_CTRL_SW_RESET		BIT(7)
#define RPR0400_SYSTEM_CTRL_INT_RESET		BIT(6)

#define RPR0400_REG_MODE_CTRL			0x41
/* MODE_CTRL bits[3:0] select a (PS-rate, ALS-rate) table entry. */
#define RPR0400_MODE_CTRL_STANDBY		0x0
#define RPR0400_MODE_CTRL_PS_ALS_100		0x6 /* PS+ALS, 100 ms each */

#define RPR0400_REG_ALSPS_CTRL			0x42
#define RPR0400_ALSPS_CTRL_LED_100MA		0x2
#define RPR0400_ALSPS_CTRL_ALS_GAIN_X2X2	(0x5 << 2)
#define RPR0400_ALSPS_CTRL_DEFAULT \
	(RPR0400_ALSPS_CTRL_LED_100MA | RPR0400_ALSPS_CTRL_ALS_GAIN_X2X2)

#define RPR0400_REG_PS_DATA			0x44
#define RPR0400_REG_ALS_DATA0			0x46
#define RPR0400_REG_ALS_DATA1			0x48
#define RPR0400_REG_MAX				0x52

#define RPR0400_RESET_DELAY_US		5000
#define RPR0400_FIRST_MEAS_DELAY_US	120000
#define RPR0400_SLEEP_DELAY_MS		2000

static const struct regmap_config rpr0400_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = RPR0400_REG_MAX,
};

struct rpr0400_data {
	struct regmap *regmap;
	struct mutex lock;
};

static int rpr0400_read_word(struct rpr0400_data *d, u8 reg, u16 *out)
{
	__le16 raw;
	int ret;

	ret = regmap_bulk_read(d->regmap, reg, &raw, sizeof(raw));
	if (ret)
		return ret;
	*out = le16_to_cpu(raw);
	return 0;
}

static int rpr0400_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct rpr0400_data *d = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(d->regmap);
	u16 raw;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	guard(mutex)(&d->lock);
	ret = rpr0400_read_word(d, chan->address, &raw);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	if (ret)
		return ret;

	*val = raw;
	return IIO_VAL_INT;
}

static const struct iio_chan_spec rpr0400_channels[] = {
	{
		.type = IIO_PROXIMITY,
		.address = RPR0400_REG_PS_DATA,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_INTENSITY,
		.channel2 = IIO_MOD_LIGHT_BOTH,
		.modified = 1,
		.address = RPR0400_REG_ALS_DATA0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_INTENSITY,
		.channel2 = IIO_MOD_LIGHT_IR,
		.modified = 1,
		.address = RPR0400_REG_ALS_DATA1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

static const struct iio_info rpr0400_info = {
	.read_raw = rpr0400_read_raw,
};

static int rpr0400_init_chip(struct rpr0400_data *d)
{
	int ret;

	ret = regmap_write(d->regmap, RPR0400_REG_SYSTEM_CTRL,
			   RPR0400_SYSTEM_CTRL_SW_RESET |
			   RPR0400_SYSTEM_CTRL_INT_RESET);
	if (ret)
		return ret;

	fsleep(RPR0400_RESET_DELAY_US);

	ret = regmap_write(d->regmap, RPR0400_REG_ALSPS_CTRL,
			   RPR0400_ALSPS_CTRL_DEFAULT);
	if (ret)
		return ret;

	/* Chip stays in standby; runtime PM resumes it on demand. */
	return regmap_write(d->regmap, RPR0400_REG_MODE_CTRL,
			    RPR0400_MODE_CTRL_STANDBY);
}

static int rpr0400_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct rpr0400_data *d = iio_priv(indio_dev);

	return regmap_write(d->regmap, RPR0400_REG_MODE_CTRL,
			    RPR0400_MODE_CTRL_STANDBY);
}

static int rpr0400_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct rpr0400_data *d = iio_priv(indio_dev);
	int ret;

	ret = regmap_write(d->regmap, RPR0400_REG_MODE_CTRL,
			   RPR0400_MODE_CTRL_PS_ALS_100);
	if (ret)
		return ret;

	fsleep(RPR0400_FIRST_MEAS_DELAY_US);
	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(rpr0400_pm_ops, rpr0400_runtime_suspend,
				 rpr0400_runtime_resume, NULL);

static const char * const rpr0400_supplies[] = { "vdd", "vddio" };

static int rpr0400_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct rpr0400_data *d;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*d));
	if (!indio_dev)
		return -ENOMEM;

	d = iio_priv(indio_dev);
	mutex_init(&d->lock);

	d->regmap = devm_regmap_init_i2c(client, &rpr0400_regmap_config);
	if (IS_ERR(d->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(d->regmap),
				     "regmap init failed\n");

	ret = devm_regulator_bulk_get_enable(&client->dev,
					     ARRAY_SIZE(rpr0400_supplies),
					     rpr0400_supplies);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "regulator setup failed\n");

	ret = rpr0400_init_chip(d);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "chip init failed\n");

	i2c_set_clientdata(client, indio_dev);

	pm_runtime_set_autosuspend_delay(&client->dev, RPR0400_SLEEP_DELAY_MS);
	pm_runtime_use_autosuspend(&client->dev);
	ret = devm_pm_runtime_enable(&client->dev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "runtime PM enable failed\n");

	indio_dev->name = "rpr0400";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = rpr0400_channels;
	indio_dev->num_channels = ARRAY_SIZE(rpr0400_channels);
	indio_dev->info = &rpr0400_info;

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id rpr0400_of_match[] = {
	{ .compatible = "rohm,rpr0400" },
	{ }
};
MODULE_DEVICE_TABLE(of, rpr0400_of_match);

static const struct i2c_device_id rpr0400_id[] = {
	{ "rpr0400" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rpr0400_id);

static struct i2c_driver rpr0400_driver = {
	.driver = {
		.name = "rpr0400",
		.of_match_table = rpr0400_of_match,
		.pm = pm_ptr(&rpr0400_pm_ops),
	},
	.probe = rpr0400_probe,
	.id_table = rpr0400_id,
};
module_i2c_driver(rpr0400_driver);

MODULE_DESCRIPTION("Rohm RPR-0400 ambient light and proximity sensor");
MODULE_LICENSE("GPL");
