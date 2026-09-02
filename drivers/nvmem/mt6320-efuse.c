// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6320 PMIC eFuse NVMEM driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6320 has a 192-bit (12 words of 16 bits) one-time-programmable
 * array behind the pwrap bus.  Reading a word requires setting the row
 * address in EFUSE_CON1, pulsing EFUSE_RD_TRIG in EFUSE_CON4 and
 * waiting for the BUSY flag in EFUSE_CON6 to clear; the data then reads
 * back from EFUSE_DOUT_<n> registers.  Programming is not supported by
 * this driver.
 */

#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6320/registers.h>

#define MT6320_EFUSE_NUM_WORDS		12

/* EFUSE_CON1 */
#define MT6320_EFUSE_ROW_ADDR		GENMASK(7, 0)
/* EFUSE_CON4 */
#define MT6320_EFUSE_RD_TRIG		BIT(0)
/* EFUSE_CON6 */
#define MT6320_EFUSE_BUSY		BIT(0)

struct mt6320_efuse {
	struct regmap *regmap;
	struct nvmem_config config;
};

static int mt6320_efuse_read_word(struct mt6320_efuse *efuse,
				  unsigned int row, u16 *out)
{
	unsigned int val;
	int ret;

	/* select the row */
	ret = regmap_update_bits(efuse->regmap, MT6320_EFUSE_CON1,
				 MT6320_EFUSE_ROW_ADDR,
				 FIELD_PREP(MT6320_EFUSE_ROW_ADDR, row));
	if (ret)
		return ret;

	/* enable efuse read path and pulse the read trigger */
	ret = regmap_write(efuse->regmap, MT6320_EFUSE_CON2, 1);
	if (ret)
		return ret;

	ret = regmap_write(efuse->regmap, MT6320_EFUSE_CON4,
			   MT6320_EFUSE_RD_TRIG);
	if (ret)
		return ret;
	ret = regmap_write(efuse->regmap, MT6320_EFUSE_CON4, 0);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(efuse->regmap, MT6320_EFUSE_CON6,
				       val, !(val & MT6320_EFUSE_BUSY),
				       50, 10000);
	if (ret)
		return ret;

	ret = regmap_read(efuse->regmap,
			  MT6320_EFUSE_DOUT_0_15 + row * 2, &val);

	*out = val & 0xffff;

	return ret;
}

static int mt6320_efuse_read(void *context, unsigned int off,
			     void *data, size_t len)
{
	struct mt6320_efuse *efuse = context;
	u16 *buf = data;
	int i, ret;

	if (len != MT6320_EFUSE_NUM_WORDS * sizeof(*buf))
		return -EINVAL;

	for (i = 0; i < len / sizeof(*buf); i++) {
		ret = mt6320_efuse_read_word(efuse, off / 2 + i, &buf[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int mt6320_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *pmic = dev_get_drvdata(dev->parent);
	struct nvmem_device *nvmem;
	struct mt6320_efuse *efuse;

	efuse = devm_kzalloc(dev, sizeof(*efuse), GFP_KERNEL);
	if (!efuse)
		return -ENOMEM;

	efuse->regmap = pmic->regmap;
	efuse->config.dev = dev;
	efuse->config.name = "mt6320-efuse";
	efuse->config.owner = THIS_MODULE;
	efuse->config.stride = 2;
	efuse->config.word_size = 2;
	efuse->config.size = MT6320_EFUSE_NUM_WORDS * 2;
	efuse->config.reg_read = mt6320_efuse_read;
	efuse->config.priv = efuse;

	nvmem = devm_nvmem_register(dev, &efuse->config);
	if (IS_ERR(nvmem))
		return dev_err_probe(dev, PTR_ERR(nvmem),
				     "failed to register nvmem\n");

	return 0;
}

static const struct of_device_id mt6320_efuse_of_match[] = {
	{ .compatible = "mediatek,mt6320-efuse" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6320_efuse_of_match);

static struct platform_driver mt6320_efuse_driver = {
	.driver = {
		.name = "mt6320-efuse",
		.of_match_table = mt6320_efuse_of_match,
	},
	.probe = mt6320_efuse_probe,
};
module_platform_driver(mt6320_efuse_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6320 PMIC eFuse driver");
MODULE_LICENSE("GPL");
