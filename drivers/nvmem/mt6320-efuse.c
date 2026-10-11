// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6320 PMIC eFuse NVMEM driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6320 exposes 192 bits of eFuse data through the EFUSE_DOUT registers.
 * The downstream preloader reads this latched DOUT window directly when
 * applying PMIC trimming, so Linux must not start another software read
 * transaction through EFUSE_CONx.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6320/registers.h>

#define MT6320_EFUSE_NUM_WORDS		12
#define MT6320_EFUSE_SIZE		(MT6320_EFUSE_NUM_WORDS * sizeof(u16))
#define MT6320_EFUSE_DOUT_BASE		MT6320_EFUSE_DOUT_0_15

static int mt6320_efuse_read(void *context, unsigned int off,
			     void *data, size_t len)
{
	struct regmap *regmap = context;
	u16 *buf = data;
	unsigned int val;
	size_t i;
	int ret;

	if (!len || (off & 1) || (len & 1) ||
	    off > MT6320_EFUSE_SIZE || len > MT6320_EFUSE_SIZE - off)
		return -EINVAL;

	/* PWRAP is a register bus, not a linear memory window: read each word. */
	for (i = 0; i < len / sizeof(*buf); i++) {
		ret = regmap_read(regmap,
				  MT6320_EFUSE_DOUT_BASE + off + i * sizeof(*buf),
				  &val);
		if (ret)
			return ret;

		buf[i] = val;
	}

	return 0;
}

static int mt6320_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *pmic = dev_get_drvdata(dev->parent);
	struct nvmem_device *nvmem;
	struct nvmem_config config = {
		.name = "mt6320-efuse",
		.owner = THIS_MODULE,
		.stride = 2,
		.word_size = 2,
		.size = MT6320_EFUSE_SIZE,
		.reg_read = mt6320_efuse_read,
	};

	if (!pmic || !pmic->regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get PMIC regmap\n");

	config.dev = dev;
	config.priv = pmic->regmap;

	nvmem = devm_nvmem_register(dev, &config);
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
