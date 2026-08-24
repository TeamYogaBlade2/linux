// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6320 PMIC AUXADC
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6320 AUXADC is a simple 16-channel sampling ADC: writing the
 * channel bit in AUXADC_CON1 triggers a conversion, the result shows up
 * in AUXADC_ADC<n> (0x0512 + n * 2) with a ready bit at bit 12.  This is
 * the same programming model as the MT6589 SoC auxadc, unlike the
 * MT6323 which uses a different register layout.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/mfd/mt6320/registers.h>

#define AUXADC_CON2_IDLE	BIT(0)
#define AUXADC_READY		BIT(12)
#define AUXADC_DATA_MASK	GENMASK(11, 0)

/* Channel indices == CON1 bit position == ADC register index. */
#define MT6320_AUXADC_BATSNS		0
#define MT6320_AUXADC_ISENSE		1
#define MT6320_AUXADC_VCDT		2
#define MT6320_AUXADC_BAT_TEMP		3
#define MT6320_AUXADC_CHIP_TEMP		4
#define MT6320_AUXADC_ACCDET		5
#define MT6320_AUXADC_THUMP		7
#define MT6320_AUXADC_NUM_CHANNELS	16

#define MTK_PMIC_IIO_CHAN(_name, _chan)			\
{							\
	.type = IIO_VOLTAGE,				\
	.indexed = 1,					\
	.channel = _chan,				\
	.datasheet_name = __stringify(_name),		\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
			      BIT(IIO_CHAN_INFO_SCALE),	\
}

static const struct iio_chan_spec mt6320_auxadc_channels[] = {
	MTK_PMIC_IIO_CHAN(batsns,    MT6320_AUXADC_BATSNS),
	MTK_PMIC_IIO_CHAN(isense,    MT6320_AUXADC_ISENSE),
	MTK_PMIC_IIO_CHAN(vcdt,      MT6320_AUXADC_VCDT),
	MTK_PMIC_IIO_CHAN(bat_temp,  MT6320_AUXADC_BAT_TEMP),
	MTK_PMIC_IIO_CHAN(chip_temp, MT6320_AUXADC_CHIP_TEMP),
	MTK_PMIC_IIO_CHAN(accdet,    MT6320_AUXADC_ACCDET),
};

struct mt6320_auxadc {
	struct regmap *regmap;
	struct mutex lock;
};

static int mt6320_auxadc_read_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  int *val, int *val2, long mask)
{
	struct mt6320_auxadc *auxadc = iio_priv(indio_dev);
	unsigned int reg = MT6320_AUXADC_ADC0 + chan->channel * 2;
	unsigned int val32;
	u32 val16;
	int ret, mult;

	if (mask != IIO_CHAN_INFO_RAW && mask != IIO_CHAN_INFO_SCALE)
		return -EINVAL;

	guard(mutex)(&auxadc->lock);

	if (mask == IIO_CHAN_INFO_SCALE) {
		if (chan->channel == MT6320_AUXADC_ISENSE ||
		    chan->channel == MT6320_AUXADC_BATSNS)
			mult = 4;
		else
			mult = 1;

		/* 1800mV full range with 12-bit resolution. */
		*val = mult * 1800;
		*val2 = 12;

		return IIO_VAL_FRACTIONAL_LOG2;
	}

	{
		/* wait for the ADC to go idle */
		ret = regmap_read_poll_timeout(auxadc->regmap,
					       MT6320_AUXADC_CON2, val32,
					       !(val32 & AUXADC_CON2_IDLE),
					       10, 1000);
		if (ret)
			return ret;

		/* clear stale ready bit, then trigger */
		ret = regmap_read(auxadc->regmap, reg, &val32);
		if (ret)
			return ret;

		ret = regmap_set_bits(auxadc->regmap,
				      MT6320_AUXADC_CON1,
				      BIT(chan->channel));
		if (ret)
			return ret;

		/* hardware needs a delay before the sample becomes ready */
		fsleep(25);

		ret = regmap_read_poll_timeout(auxadc->regmap, reg, val32,
					       val32 & AUXADC_READY,
					       10, USEC_PER_MSEC);
		if (ret)
			return ret;

		val16 = FIELD_GET(AUXADC_DATA_MASK, val32);
		*val = val16;

		/* stop the channel again */
		return regmap_clear_bits(auxadc->regmap,
					 MT6320_AUXADC_CON1,
					 BIT(chan->channel));
	}
}

static const struct iio_info mt6320_auxadc_iio_info = {
	.read_raw = mt6320_auxadc_read_raw,
};

static int mt6320_auxadc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6320_auxadc *auxadc;
	struct regmap *regmap;
	struct iio_dev *iio;

	regmap = dev_get_regmap(dev->parent->parent, NULL);
	if (!regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get regmap\n");

	iio = devm_iio_device_alloc(dev, sizeof(*auxadc));
	if (!iio)
		return -ENOMEM;

	auxadc = iio_priv(iio);
	auxadc->regmap = regmap;
	mutex_init(&auxadc->lock);

	iio->name = "mt6320-auxadc";
	iio->info = &mt6320_auxadc_iio_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = mt6320_auxadc_channels;
	iio->num_channels = ARRAY_SIZE(mt6320_auxadc_channels);

	return devm_iio_device_register(dev, iio);
}

static const struct of_device_id mt6320_auxadc_of_match[] = {
	{ .compatible = "mediatek,mt6320-auxadc" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6320_auxadc_of_match);

static struct platform_driver mt6320_auxadc_driver = {
	.driver = {
		.name = "mt6320-auxadc",
		.of_match_table = mt6320_auxadc_of_match,
	},
	.probe	= mt6320_auxadc_probe,
};
module_platform_driver(mt6320_auxadc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6320 PMIC AUXADC Driver");
