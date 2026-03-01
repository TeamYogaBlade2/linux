// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * based on mt6320-regulator.c
 *     Copyright (c) 2016 MediaTek Inc.
 *     Author: Chen Zhong <chen.zhong@mediatek.com>
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

#define MT6320_LDO_MODE_NORMAL	0
#define MT6320_LDO_MODE_LP	1

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

/*
TODO: check .enable_mask, .vselctrl_mask
*/

#define MT6320_BUCK(match, vreg, min, max, step, volt_ranges, enreg,	\
		vosel, vosel_mask, voselon, vosel_ctrl)			\
[MT6320_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.of_match = of_match_ptr(match),			\
		.ops = &mt6320_volt_range_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6320_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = (max - min)/step + 1,			\
		.linear_ranges = volt_ranges,				\
		.n_linear_ranges = ARRAY_SIZE(volt_ranges),		\
		.vsel_reg = vosel,					\
		.vsel_mask = vosel_mask,				\
		.enable_reg = enreg,					\
		.enable_mask = BIT(0),					\
	},								\
	.qi = BIT(13),							\
	.vselon_reg = voselon,						\
	.vselctrl_reg = vosel_ctrl,					\
	.vselctrl_mask = BIT(1),					\
}

#define MT6320_LDO(match, vreg, ldo_volt_table, enreg, enbit, vosel,	\
		vosel_mask, _modeset_reg, _modeset_mask)		\
[MT6320_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.of_match = of_match_ptr(match),			\
		.ops = &mt6320_volt_table_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6320_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = ARRAY_SIZE(ldo_volt_table),		\
		.volt_table = ldo_volt_table,				\
		.vsel_reg = vosel,					\
		.vsel_mask = vosel_mask,				\
		.enable_reg = enreg,					\
		.enable_mask = BIT(enbit),				\
	},								\
	.qi = BIT(15),							\
	.modeset_reg = _modeset_reg,					\
	.modeset_mask = _modeset_mask,					\
}

#define MT6320_REG_FIXED(match, vreg, enreg, enbit, volt,		\
		_modeset_reg, _modeset_mask)				\
[MT6320_ID_##vreg] = {							\
	.desc = {							\
		.name = #vreg,						\
		.of_match = of_match_ptr(match),			\
		.ops = &mt6320_volt_fixed_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.id = MT6320_ID_##vreg,					\
		.owner = THIS_MODULE,					\
		.n_voltages = 1,					\
		.enable_reg = enreg,					\
		.enable_mask = BIT(enbit),				\
		.min_uV = volt,						\
	},								\
	.qi = BIT(15),							\
	.modeset_reg = _modeset_reg,					\
	.modeset_mask = _modeset_mask,					\
}

static const struct linear_range buck_volt_range1[] = {
	REGULATOR_LINEAR_RANGE(700000, 0, 0x7f, 6250),
};

static const struct linear_range buck_volt_range2[] = {
	REGULATOR_LINEAR_RANGE(1500000, 0, 0x1f, 20000),
};

static const struct linear_range buck_volt_range3[] = {
	REGULATOR_LINEAR_RANGE(500000, 0, 0x3f, 50000),
};

static const struct linear_range buck_volt_range4[] = {
	REGULATOR_LINEAR_RANGE(1050000, 0, 0x1f, 25000),
};

static const unsigned int ldo_volt_table1[] = {
	1800000, 3300000,
};

static const unsigned int ldo_volt_table2[] = {
	3000000, 3300000,
};

static const unsigned int ldo_volt_table3[] = {
	1200000, 1300000, 1500000, 1800000, 2500000, 2800000, 3000000, 3300000,
};

static const unsigned int ldo_volt_table4[] = {
	1200000, 1100000, 1000000, 900000,
};

static const unsigned int ldo_volt_table5[] = {
	1800000, 2800000,
};

static const unsigned int ldo_volt_table6[] = {
	1800000, 2500000,
};

static const unsigned int ldo_volt_table7[] = {
	1500000, 1800000, 2500000, 2800000,
};

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

static int mt6320_ldo_set_mode(struct regulator_dev *rdev, unsigned int mode)
{
	int ret, val = 0;
	struct mt6320_regulator_info *info = rdev_get_drvdata(rdev);

	if (!info->modeset_mask) {
		dev_err(&rdev->dev, "regulator %s doesn't support set_mode\n",
			info->desc.name);
		return -EINVAL;
	}

	switch (mode) {
	case REGULATOR_MODE_STANDBY:
		val = MT6320_LDO_MODE_LP;
		break;
	case REGULATOR_MODE_NORMAL:
		val = MT6320_LDO_MODE_NORMAL;
		break;
	default:
		return -EINVAL;
	}

	val <<= ffs(info->modeset_mask) - 1;

	ret = regmap_update_bits(rdev->regmap, info->modeset_reg,
				  info->modeset_mask, val);

	return ret;
}

static unsigned int mt6320_ldo_get_mode(struct regulator_dev *rdev)
{
	unsigned int val;
	unsigned int mode;
	int ret;
	struct mt6320_regulator_info *info = rdev_get_drvdata(rdev);

	if (!info->modeset_mask) {
		dev_err(&rdev->dev, "regulator %s doesn't support get_mode\n",
			info->desc.name);
		return -EINVAL;
	}

	ret = regmap_read(rdev->regmap, info->modeset_reg, &val);
	if (ret < 0)
		return ret;

	val &= info->modeset_mask;
	val >>= ffs(info->modeset_mask) - 1;

	if (val & 0x1)
		mode = REGULATOR_MODE_STANDBY;
	else
		mode = REGULATOR_MODE_NORMAL;

	return mode;
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
	.set_mode = mt6320_ldo_set_mode,
	.get_mode = mt6320_ldo_get_mode,
};

static const struct regulator_ops mt6320_volt_fixed_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_status = mt6320_get_status,
	.set_mode = mt6320_ldo_set_mode,
	.get_mode = mt6320_ldo_get_mode,
};

/* The array is indexed by id(MT6320_ID_XXX) */
static struct mt6320_regulator_info mt6320_regulators[] = {
/*
	BUCK_VPROC
	BUCK_VSRAM
	BUCK_VCORE
	BUCK_VM
	BUCK_VIO18
	BUCK_VPA
	BUCK_VRF18
	BUCK_VRF18_2

	//Digital LDO
	LDO_VIO28
	LDO_VUSB
	LDO_VMC1
	LDO_VMCH1
	LDO_VEMC_3V3
	LDO_VEMC_1V8
	LDO_VGP1
	LDO_VGP2
	LDO_VGP3
	LDO_VGP4
	LDO_VGP5
	LDO_VGP6
	LDO_VSIM1
	LDO_VSIM2
	LDO_VIBR
	LDO_VRTC
	LDO_VAST

	//Analog LDO
	LDO_VRF28
	LDO_VRF28_2
	LDO_VTCXO
	LDO_VTCXO_2
	LDO_VA
	LDO_VA28
	LDO_VCAMA
*/
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
	u32 reg_value;

	/* Query buck controller to select activated voltage register part */
	if (mt6320_set_buck_vosel_reg(pdev))
		return -EIO;

	/* Read PMIC chip revision to update constraints and voltage table */
	if (regmap_read(mt6320->regmap, MT6320_CID, &reg_value) < 0) {
		dev_err(&pdev->dev, "Failed to read Chip ID\n");
		return -EIO;
	}
	dev_info(&pdev->dev, "Chip ID = 0x%x\n", reg_value);

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

static struct platform_driver mt6320_regulator_driver = {
	.driver = {
		.name = "mt6320-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = mt6320_regulator_probe,
	.id_table = mt6320_platform_ids,
};

module_platform_driver(mt6320_regulator_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("Regulator Driver for MediaTek MT6320 PMIC");
MODULE_LICENSE("GPL v2");
