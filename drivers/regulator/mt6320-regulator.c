// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * based on mt6397-regulator.c
 *     Copyright (c) 2014 MediaTek Inc.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6320/registers.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/mt6320-regulator.h>
#include <linux/regulator/of_regulator.h>
#include <dt-bindings/regulator/mediatek,mt6320-regulator.h>

/*
 * MT6320 regulators' information
 *
 * @desc: standard fields of regulator description.
 * @qi: Mask for query enable signal status of regulators
 * @vselon_reg: Register sections for hardware control mode of bucks
 * @vselctrl_reg: Register for controlling the buck control mode.
 * @vselctrl_mask: Mask for query buck's voltage control mode.
 */
struct mt6320_regulator_info {
	struct regulator_desc desc;
	u32 qi;
	u32 vselon_reg;
	u32 vselctrl_reg;
	u32 vselctrl_mask;
	u32 modeset_reg;
	u32 modeset_mask;
};

static unsigned int mt6320_map_mode(unsigned int mode)
{
	switch (mode) {
	case MT6320_BUCK_MODE_AUTO:
		return REGULATOR_MODE_NORMAL;
	case MT6320_BUCK_MODE_FORCE_PWM:
		return REGULATOR_MODE_FAST;
	default:
		return REGULATOR_MODE_INVALID;
	}
}

static int mt6320_regulator_set_mode(struct regulator_dev *rdev,
				     unsigned int mode)
{
	struct mt6320_regulator_info *info = rdev_get_drvdata(rdev);
	int ret, val;

	switch (mode) {
	case REGULATOR_MODE_FAST:
		val = MT6320_BUCK_MODE_FORCE_PWM;
		break;
	case REGULATOR_MODE_NORMAL:
		val = MT6320_BUCK_MODE_AUTO;
		break;
	default:
		ret = -EINVAL;
		goto err_mode;
	}

	dev_dbg(&rdev->dev, "mt6320 buck set_mode %#x, %#x, %#x\n",
		info->modeset_reg, info->modeset_mask, val);

	val <<= ffs(info->modeset_mask) - 1;

	ret = regmap_update_bits(rdev->regmap, info->modeset_reg,
				 info->modeset_mask, val);
err_mode:
	if (ret != 0) {
		dev_err(&rdev->dev,
			"Failed to set mt6320 buck mode: %d\n", ret);
		return ret;
	}

	return 0;
}

static unsigned int mt6320_regulator_get_mode(struct regulator_dev *rdev)
{
	struct mt6320_regulator_info *info = rdev_get_drvdata(rdev);
	int ret, regval;

	ret = regmap_read(rdev->regmap, info->modeset_reg, &regval);
	if (ret != 0) {
		dev_err(&rdev->dev,
			"Failed to get mt6320 buck mode: %d\n", ret);
		return ret;
	}

	regval &= info->modeset_mask;
	regval >>= ffs(info->modeset_mask) - 1;

	switch (regval) {
	case MT6320_BUCK_MODE_AUTO:
		return REGULATOR_MODE_NORMAL;
	case MT6320_BUCK_MODE_FORCE_PWM:
		return REGULATOR_MODE_FAST;
	default:
		return -EINVAL;
	}
}

static int mt6320_get_status(struct regulator_dev *rdev)
{
	int ret;
	u32 regval;
	struct mt6320_regulator_info *info = rdev_get_drvdata(rdev);

	ret = regmap_read(rdev->regmap, info->desc.enable_reg, &regval);
	if (ret != 0) {
		dev_err(&rdev->dev, "Failed to get enable reg: %d\n", ret);
		return ret;
	}

	return (regval & info->qi) ? REGULATOR_STATUS_ON : REGULATOR_STATUS_OFF;
}

static const struct regulator_ops mt6320_volt_range_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6320_get_status,
	.set_mode = mt6320_regulator_set_mode,
	.get_mode = mt6320_regulator_get_mode,
};

static const struct regulator_ops mt6320_volt_table_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6320_get_status,
};

static const struct regulator_ops mt6320_volt_fixed_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6320_get_status,
};

/* The array is indexed by id(MT6320_ID_XXX) */
static struct mt6320_regulator_info mt6320_regulators[] = {
};

static int mt6320_set_buck_vosel_reg(struct platform_device *pdev)
{
	struct mt6397_chip *mt6320 = dev_get_drvdata(pdev->dev.parent);
	int i;
	u32 regval;

	for (i = 0; i < MT6320_MAX_REGULATOR; i++) {
		if (mt6320_regulators[i].vselctrl_reg) {
			if (regmap_read(mt6320->regmap,
				mt6320_regulators[i].vselctrl_reg,
				&regval) < 0) {
				dev_err(&pdev->dev,
					"Failed to read buck ctrl\n");
				return -EIO;
			}

			if (regval & mt6320_regulators[i].vselctrl_mask) {
				mt6320_regulators[i].desc.vsel_reg =
				mt6320_regulators[i].vselon_reg;
			}
		}
	}

	return 0;
}

static int mt6320_regulator_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6320 = dev_get_drvdata(pdev->dev.parent);
	struct regulator_config config = {};
	struct regulator_dev *rdev;
	int i;
	u32 reg_value, version;

	/* Query buck controller to select activated voltage register part */
	if (mt6320_set_buck_vosel_reg(pdev))
		return -EIO;

	/* Read PMIC chip revision to update constraints and voltage table */
	if (regmap_read(mt6320->regmap, MT6320_CID, &reg_value) < 0) {
		dev_err(&pdev->dev, "Failed to read Chip ID\n");
		return -EIO;
	}
	dev_info(&pdev->dev, "Chip ID = 0x%x\n", reg_value);

	version = (reg_value & 0xFF);
	switch (version) {
	default:
		break;
	}

	for (i = 0; i < MT6320_MAX_REGULATOR; i++) {
		config.dev = &pdev->dev;
		config.driver_data = &mt6320_regulators[i];
		config.regmap = mt6320->regmap;
		rdev = devm_regulator_register(&pdev->dev,
				&mt6320_regulators[i].desc, &config);
		if (IS_ERR(rdev)) {
			dev_err(&pdev->dev, "failed to register %s\n",
				mt6320_regulators[i].desc.name);
			return PTR_ERR(rdev);
		}
	}

	return 0;
}

static const struct platform_device_id mt6320_platform_ids[] = {
	{"mt6320-regulator", 0},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, mt6320_platform_ids);

static const struct of_device_id mt6320_of_match[] __maybe_unused = {
	{ .compatible = "mediatek,mt6320-regulator", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mt6320_of_match);

static struct platform_driver mt6320_regulator_driver = {
	.driver = {
		.name = "mt6320-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = of_match_ptr(mt6320_of_match),
	},
	.probe = mt6320_regulator_probe,
	.id_table = mt6320_platform_ids,
};

module_platform_driver(mt6320_regulator_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("Regulator Driver for MediaTek MT6320 PMIC");
MODULE_LICENSE("GPL");
