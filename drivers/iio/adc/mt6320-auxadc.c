// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6320 PMIC AUXADC
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * MT6320 AUXADC programming model based on the MT6589 downstream
 * PMIC implementation.
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/stringify.h>

#include <linux/mfd/mt6320/registers.h>

#define MT6320_AUXADC_AVG_NUM		GENMASK(6, 4)
#define MT6320_AUXADC_BUF_PWD_ON	BIT(3)
#define MT6320_AUXADC_BUF_PWD_B		BIT(1)

#define MT6320_AUXADC_CHSEL		GENMASK(10, 7)
#define MT6320_AUXADC_START		BIT(0)

#define MT6320_AUXADC_READY		BIT(15)
#define MT6320_AUXADC_DATA		GENMASK(9, 0)

#define MT6320_AUXADC_VBUF_EN		BIT(4)
#define MT6320_AUXADC_VBUF_BYP		BIT(2)
#define MT6320_AUXADC_VBUF_CALEN	BIT(0)

#define MT6320_CHR_BATON_TDET_EN	BIT(2)

#define MT6320_AUXADC_AVG_NUM_VALUE	0x3
#define MT6320_AUXADC_SPL_NUM_TEMP	0x1e

#define MT6320_ACCDET_AUXADC_ENABLE	0x10b0

#define MT6320_AUXADC_FULL_SCALE_MV	1200
#define MT6320_AUXADC_RESOLUTION	1024

#define MTK_PMIC_IIO_CHAN(_name, _chan)		\
{							\
	.type = IIO_VOLTAGE,				\
	.indexed = 1,					\
	.channel = _chan,				\
	.datasheet_name = __stringify(_name),		\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
			      BIT(IIO_CHAN_INFO_SCALE),	\
}

#define MT6320_AUXADC_BATSNS		0
#define MT6320_AUXADC_ISENSE		1
#define MT6320_AUXADC_VCDT		2
#define MT6320_AUXADC_BAT_TEMP		3
#define MT6320_AUXADC_CHIP_TEMP		4
#define MT6320_AUXADC_ACCDET		5

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

/*
 * Register contents saved before mt6320_auxadc_prepare() programs anything,
 * so the read can put back every bit it changed.
 *
 * The downstream driver hard-codes the post-read values instead -
 * upmu_set_rg_spl_num(0x1), upmu_set_rg_buf_pwd_b(0),
 * upmu_set_rg_buf_pwd_on(0), upmu_set_baton_tdet_en(0),
 * upmu_set_rg_vbuf_calen(0) and accdet_auxadc_switch(0)
 * (mediatek/platform/mt6589/kernel/drivers/power/pmic_mt6320.c:475-499) -
 * which is only correct because there it is the sole owner of these registers.
 * On mainline they are shared: the IIO AUXADC and the MT6320 codec both reach
 * AUXADC_CON0, so restoring a saved value is both safer here and free of
 * invented constants.
 *
 * The one case that really bites is the ISENSE mux in AUXADC_CON14: setting
 * it is what routes a charge-current read, and with nothing to clear it again
 * the next "battery voltage" read silently returns charge current instead.
 */
#define MT6320_AUXADC_MAX_SAVED	5

struct mt6320_auxadc_saved {
	struct {
		unsigned int reg;
		unsigned int val;
	} regs[MT6320_AUXADC_MAX_SAVED];
	unsigned int nr;
};

static int mt6320_auxadc_save(struct mt6320_auxadc_saved *saved,
			      struct regmap *map, unsigned int reg)
{
	unsigned int i;

	/* A register only needs saving once, before its first write. */
	for (i = 0; i < saved->nr; i++)
		if (saved->regs[i].reg == reg)
			return 0;

	if (saved->nr == MT6320_AUXADC_MAX_SAVED)
		return -EOVERFLOW;

	if (regmap_read(map, reg, &saved->regs[saved->nr].val))
		return -EIO;

	saved->nr++;
	return 0;
}

static int mt6320_auxadc_restore(struct mt6320_auxadc_saved *saved,
				 struct regmap *map)
{
	unsigned int i;
	int first_err = 0;
	int ret;

	/* Unwind in reverse, so the last thing changed is undone first. */
	for (i = saved->nr; i > 0; i--) {
		ret = regmap_write(map, saved->regs[i - 1].reg,
				   saved->regs[i - 1].val);
		if (ret && !first_err)
			first_err = ret;
	}

	saved->nr = 0;
	return first_err;
}

static unsigned int mt6320_auxadc_hw_channel(unsigned int channel)
{
	/*
	 * The downstream PMIC implementation selects channel 0 when
	 * reading the ISENSE path, while routing the source through
	 * the channel-0 mux.
	 */
	return channel == MT6320_AUXADC_ISENSE ? MT6320_AUXADC_BATSNS :
						 channel;
}

static unsigned int mt6320_auxadc_result_reg(unsigned int channel)
{
	/*
	 * Raw conversion status/data is available in AUXADC_ADC0..ADC7.
	 * The trim=1 path used by the downstream driver reads the
	 * corresponding trimmed result from AUXADC_ADC11..ADC18.
	 */
	return MT6320_AUXADC_ADC11 + channel * 2;
}

/*
 * Program the mux and the per-channel setup for one read.
 *
 * Every register written here is saved into "saved" immediately before its
 * first modification, so mt6320_auxadc_restore() can put it back: this driver
 * shares these registers with the MT6320 codec, and leaving the ISENSE mux or
 * the VBUF/buffer-power state asserted would silently corrupt later reads
 * from either side.
 */
static int mt6320_auxadc_prepare(struct mt6320_auxadc *auxadc,
				 unsigned int channel,
				 struct mt6320_auxadc_saved *saved,
				 unsigned int *hw_channel)
{
	struct regmap *map = auxadc->regmap;
	int ret;

	*hw_channel = mt6320_auxadc_hw_channel(channel);

	ret = mt6320_auxadc_save(saved, map, MT6320_AUXADC_CON1);
	if (ret)
		return ret;

	ret = mt6320_auxadc_save(saved, map, MT6320_AUXADC_CON0);
	if (ret)
		return ret;

	if (channel == MT6320_AUXADC_ISENSE) {
		ret = mt6320_auxadc_save(saved, map, MT6320_AUXADC_CON14);
		if (ret)
			return ret;

		ret = regmap_set_bits(map, MT6320_AUXADC_CON14,
				      BIT(2) | BIT(0));
		if (ret)
			return ret;
	}

	ret = regmap_update_bits(map, MT6320_AUXADC_CON1,
				 MT6320_AUXADC_CHSEL,
				 FIELD_PREP(MT6320_AUXADC_CHSEL,
					    *hw_channel));
	if (ret)
		return ret;

	ret = regmap_update_bits(map, MT6320_AUXADC_CON0,
				 MT6320_AUXADC_AVG_NUM,
				 FIELD_PREP(MT6320_AUXADC_AVG_NUM,
					    MT6320_AUXADC_AVG_NUM_VALUE));
	if (ret)
		return ret;

	switch (channel) {
	case MT6320_AUXADC_BAT_TEMP:
		ret = mt6320_auxadc_save(saved, map, MT6320_CHR_CON7);
		if (ret)
			return ret;

		ret = regmap_set_bits(map, MT6320_AUXADC_CON0,
				      MT6320_AUXADC_BUF_PWD_ON |
				      MT6320_AUXADC_BUF_PWD_B);
		if (ret)
			return ret;

		ret = regmap_set_bits(map, MT6320_CHR_CON7,
				      MT6320_CHR_BATON_TDET_EN);
		if (ret)
			return ret;

		msleep(20);
		break;

	case MT6320_AUXADC_CHIP_TEMP:
		/*
		 * Match PMIC_IMM_GetOneChannelValue(..., 4, ..., 2):
		 * enable VBUF, disable bypass, select the PMIC-temperature
		 * calibration path and increase the sample count.
		 */
		ret = mt6320_auxadc_save(saved, map, MT6320_AUXADC_CON12);
		if (ret)
			return ret;

		ret = regmap_update_bits(map, MT6320_AUXADC_CON12,
					 MT6320_AUXADC_VBUF_EN |
					 MT6320_AUXADC_VBUF_BYP |
					 MT6320_AUXADC_VBUF_CALEN,
					 MT6320_AUXADC_VBUF_EN);
		if (ret)
			return ret;

		ret = regmap_update_bits(map, MT6320_AUXADC_CON0,
					 GENMASK(11, 7),
					 FIELD_PREP(GENMASK(11, 7),
						    MT6320_AUXADC_SPL_NUM_TEMP));
		if (ret)
			return ret;

		msleep(1);
		break;

	case MT6320_AUXADC_ACCDET:
		ret = mt6320_auxadc_save(saved, map, MT6320_ACCDET_CON0);
		if (ret)
			return ret;

		ret = regmap_write(map, MT6320_ACCDET_CON0,
				   MT6320_ACCDET_AUXADC_ENABLE);
		if (ret)
			return ret;
		break;
	}

	return 0;
}

static int mt6320_auxadc_read_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  int *val, int *val2, long mask)
{
	struct mt6320_auxadc *auxadc = iio_priv(indio_dev);
	struct regmap *map = auxadc->regmap;
	struct mt6320_auxadc_saved saved = {};
	unsigned int hw_channel;
	unsigned int status_reg;
	unsigned int result_reg;
	unsigned int regval;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		/*
		 * MT6320 AUXADC has a 1.2V full-scale range and
		 * 10-bit conversion result.
		 */
		*val = MT6320_AUXADC_FULL_SCALE_MV;
		*val2 = MT6320_AUXADC_RESOLUTION;

		return IIO_VAL_FRACTIONAL;

	case IIO_CHAN_INFO_RAW:
		break;

	default:
		return -EINVAL;
	}

	guard(mutex)(&auxadc->lock);

	ret = mt6320_auxadc_prepare(auxadc, chan->channel, &saved,
				    &hw_channel);
	if (ret) {
		/* prepare() may fail after saving some registers already. */
		if (mt6320_auxadc_restore(&saved, map))
			dev_err_ratelimited(&indio_dev->dev,
					    "failed to restore AUXADC state: %d\n",
					    ret);
		return ret;
	}

	status_reg = MT6320_AUXADC_ADC0 + hw_channel * 2;
	result_reg = mt6320_auxadc_result_reg(hw_channel);

	/*
	 * Match the downstream sequence:
	 *
	 *   START = 0
	 *   START = 1
	 *   delay 50 us
	 *   wait for the raw channel ready bit
	 *   read the trimmed result
	 */
	ret = regmap_clear_bits(map, MT6320_AUXADC_CON1,
				MT6320_AUXADC_START);
	if (ret)
		goto stop;

	ret = regmap_set_bits(map, MT6320_AUXADC_CON1,
			      MT6320_AUXADC_START);
	if (ret)
		goto stop;

	fsleep(50);

	ret = regmap_read_poll_timeout(map, status_reg, regval,
				       regval & MT6320_AUXADC_READY,
				       10, 10000);
	if (ret)
		goto stop;

	ret = regmap_read(map, result_reg, &regval);
	if (ret)
		goto stop;

	*val = FIELD_GET(MT6320_AUXADC_DATA, regval);

stop:
	/*
	 * Unlike the downstream implementation, clear START when the
	 * conversion is finished so a failed read cannot leave the
	 * conversion request asserted indefinitely.
	 *
	 * Then put back every register prepare() touched, so the read
	 * leaves no state behind: without this an ISENSE read would leave
	 * the charge-current mux selected and the next battery-voltage
	 * read would return charge current instead.  This runs on every
	 * exit path, including the timeout above.
	 */
	if (regmap_clear_bits(map, MT6320_AUXADC_CON1,
			      MT6320_AUXADC_START) && !ret)
		ret = -EIO;

	if (mt6320_auxadc_restore(&saved, map) && !ret)
		ret = -EIO;

	if (ret)
		return ret;

	return IIO_VAL_INT;
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
		return dev_err_probe(dev, -ENODEV,
				    "failed to get regmap\n");

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
	.probe = mt6320_auxadc_probe,
};
module_platform_driver(mt6320_auxadc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek MT6320 PMIC AUXADC Driver");
