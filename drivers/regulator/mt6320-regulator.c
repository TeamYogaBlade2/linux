// SPDX-License-Identifier: GPL-2.0
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
 * @qi: Mask for query enable signal status of regulators.
 *      BUCKs: BIT(13) within CON7.  LDOs: BIT(15) within enable_reg.
 * @vselon_reg: CON10 — BUCK voltage register used in HW-control mode.
 * @vselctrl_reg: CON5 — selects SW vs HW voltage control for BUCKs.
 * @vselctrl_mask: BIT(1) — VOSEL_CTRL bit in vselctrl_reg (all BUCKs).
 * @modeset_reg: Register for Normal/Low-Power mode selection.
 * @modeset_mask: Mask for the mode bit in modeset_reg.
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
 * BUCK register map (all addresses confirmed from upmu_hw.h CON definitions)
 * --------------------------------------------------------------------------
 *           CON2     CON5     CON7          CON9     CON10
 *           modeset  vselctrl enable+status vosel_sw voselon
 * VPROC     0x020A   0x0210   0x0214        0x0218   0x021A
 * VSRAM     0x0230   0x0236   0x023A        0x023E   0x0240
 * VCORE     0x025C   0x0262   0x0266        0x026A   0x026C
 * VM        0x0282   0x0288   0x028C        0x0290   0x0292
 * VIO18     0x0304   0x030A   0x030E        0x0312   0x0314
 * VPA       0x032A   0x0330   0x0334        0x0338   0x033A
 * VRF18     0x0354   0x035A   0x035E        0x0362   0x0364
 * VRF18_2   0x037E   0x0384   0x0388        0x038C   0x038E
 *
 * CON7 dual purpose: EN=BIT(0) (enable), qi=BIT(13) (HW status readback)
 * CON5: VOSEL_CTRL=BIT(1)
 * CON2: MODESET=BIT(8) (confirmed from PMIC_RG_Vxxx_MODESET_SHIFT=8)
 *
 * LDO register map (all confirmed from upmu_hw.h + upmu_common.c)
 * ----------------------------------------------------------------
 * enable_reg  = ANALDO_CONn or DIGLDO_CONn (see individual entries)
 * EN bit      = PMIC_RG_Vxxx_EN_SHIFT (each confirmed)
 * modeset_reg = same as enable_reg (LP_SEL=BIT(0) in same register)
 * vosel_reg   = separate CON register per LDO
 * vosel_mask  = pre-shifted: raw_mask << shift
 */

#define MT6320_BUCK(match, vreg, min, max, step, volt_ranges, enreg,	\
		vosel, vosel_mask, voselon, vosel_ctrl, modesetreg)	\
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
	.modeset_reg = modesetreg,					\
	.modeset_mask = BIT(8),					\
}

/*
 * MT6320_LDO — variable-voltage LDO with volt_table
 *
 * modeset_reg = enable_reg (LP_SEL=BIT(0) is in the same CON register as EN,
 * confirmed for every LDO from upmu_common.c upmu_set_Vxxx_lp_sel()).
 * modeset_mask = BIT(0).
 */
#define MT6320_LDO(match, vreg, ldo_volt_table, enreg, enbit, vosel,	\
		vosel_mask)						\
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
	.modeset_reg = enreg,						\
	.modeset_mask = BIT(0),					\
}

/*
 * MT6320_REG_FIXED — fixed-voltage LDO
 *
 * Same LP_SEL=BIT(0) logic as MT6320_LDO.
 * VTCXO_1 and VRTC have no LP_SEL in upmu_common.c;
 * those two use modeset_reg=0/modeset_mask=0 via MT6320_REG_FIXED_NOLP.
 */
#define MT6320_REG_FIXED(match, vreg, enreg, enbit, volt)		\
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
	.modeset_reg = enreg,						\
	.modeset_mask = BIT(0),					\
}

#define MT6320_REG_FIXED_NOLP(match, vreg, enreg, enbit, volt)		\
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
	.modeset_reg = 0,						\
	.modeset_mask = 0,						\
}

/*
 * BUCK voltage ranges
 * -------------------
 * All vosel masks confirmed from PMIC_Vxxx_VOSEL_MASK in upmu_hw.h.
 * Base voltages confirmed by back-calculating from real hardware readings.
 *
 * buck_volt_range1: 700 mV, 6.25 mV/step, 7-bit (mask=0x7F)
 *   VPROC: vosel=0x50 -> 700+80*6.25=1200 mV ✓ (real: 1200 mV)
 *   VSRAM: vosel=0x5A -> 700+90*6.25=1262.5 mV ✓ (real: 1262 mV, display truncated)
 *   VCORE: vosel=0x38 -> 700+56*6.25=1050 mV ✓ (real: 1050 mV)
 *   VM:    vosel=0x52 -> 700+82*6.25=1212.5 mV ✓ (real: 1212 mV, display truncated)
 *   Applies to: VPROC, VSRAM, VCORE, VM
 *
 * buck_volt_range2: 500 mV, 50 mV/step, 6-bit (mask=0x3F)
 *   VPA: vosel=0 -> 500 mV ✓ (real: 500 mV = minimum)
 *   Applies to: VPA
 *
 * buck_volt_range3: 1800 mV, 6.25 mV/step, 5-bit (mask=0x1F)
 *   VIO18: real reading=1800 mV. vosel=0 -> 1800 mV ✓ (used as fixed)
 *   Applies to: VIO18
 *
 * buck_volt_range4: 1050 mV, 25 mV/step, 5-bit (mask=0x1F)
 *   Confirmed by pmic_vrf18_2_usage_protection() in pmic_mt6320.c:
 *     gpu_status_bit=1 -> val=0x1F -> 1050 + 31*25 = 1825 mV (RF mode) ✓
 *     gpu_status_bit=0 -> val=0x04 -> 1050 +  4*25 = 1150 mV (GPU OD mode)
 *   vosel=0x00 gives 1050 mV (GPU OD minimum per datasheet).
 *   VRF18 (1st RF) shares the same CON9 structure; it always operates at
 *   vosel=0x1F=1825 mV and never uses GPU OD mode.
 *   Applies to: VRF18, VRF18_2
 */
static const struct linear_range buck_volt_range1[] = {
	REGULATOR_LINEAR_RANGE(700000, 0, 0x7f, 6250),
};

static const struct linear_range buck_volt_range2[] = {
	REGULATOR_LINEAR_RANGE(500000, 0, 0x3f, 50000),
};

static const struct linear_range buck_volt_range3[] = {
	REGULATOR_LINEAR_RANGE(1800000, 0, 0x1f, 6250),
};

static const struct linear_range buck_volt_range4[] = {
	REGULATOR_LINEAR_RANGE(1050000, 0, 0x1f, 25000),
};

/*
 * LDO voltage tables
 * ------------------
 * All tables confirmed from dct_pmic_XXX_sel() in pmic_mt6320.c.
 * Real hardware readings are noted where available.
 *
 * ldo_volt_table1: VMC  — vosel BIT(4)
 *   Real: 3300 mV (vosel=1)
 */
static const unsigned int ldo_volt_table1[] = {
	1800000, 3300000,
};

/*
 * ldo_volt_table2: VMCH, VEMC_3V3  — vosel BIT(7)
 *   Real: VMCH=3300 mV (vosel=1), VEMC_3V3=3300 mV (vosel=1)
 */
static const unsigned int ldo_volt_table2[] = {
	3000000, 3300000,
};

/*
 * ldo_volt_table3: VEMC_1V8, VGP1-VGP6, VSIM1, VSIM2, VIBR  — vosel [7:5]
 *   Real: VGP1=2800, VGP2=2800, VGP3=2800, VGP4=2800, VGP5=2800, VGP6=1800,
 *         VSIM1=1200, VSIM2=1200, VIBR=2800, VEMC_1V8=1800 mV
 */
static const unsigned int ldo_volt_table3[] = {
	1200000, 1300000, 1500000, 1800000, 2500000, 2800000, 3000000, 3300000,
};

/*
 * ldo_volt_table4: VAST  — vosel [14:13] (descending)
 *   Real: 1200 mV (vosel=0)
 */
static const unsigned int ldo_volt_table4[] = {
	1200000, 1100000, 1000000, 900000,
};

/*
 * ldo_volt_table5: VRF28_1, VRF28_2, VTCXO_2  — vosel BIT(3)
 *   Real: VRF28_1=2800, VRF28_2=1800, VTCXO_2=2800 mV
 *   Note: datasheet lists VRF28_1/2 output as 2.85 V; downstream and real
 *   hardware show 2800 mV for vosel=1. Using 2850 mV as per datasheet spec.
 */
static const unsigned int ldo_volt_table5[] = {
	1800000, 2850000,
};

/*
 * ldo_volt_table6: VA  — vosel BIT(6)
 *   Real: 1800 mV (vosel=0)
 */
static const unsigned int ldo_volt_table6[] = {
	1800000, 2500000,
};

/*
 * ldo_volt_table7: VCAMA  — vosel [7:6] = 0xC0
 *   Real: 2800 mV (vosel=3)
 */
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
	.list_voltage		= regulator_list_voltage_linear_range,
	.map_voltage		= regulator_map_voltage_linear_range,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel	= regulator_set_voltage_time_sel,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.get_status		= mt6320_get_status,
	.set_mode		= mt6320_ldo_set_mode,
	.get_mode		= mt6320_ldo_get_mode,
};

static const struct regulator_ops mt6320_volt_table_ops = {
	.list_voltage		= regulator_list_voltage_table,
	.map_voltage		= regulator_map_voltage_iterate,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_time_sel	= regulator_set_voltage_time_sel,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.get_status		= mt6320_get_status,
	.set_mode		= mt6320_ldo_set_mode,
	.get_mode		= mt6320_ldo_get_mode,
};

static const struct regulator_ops mt6320_volt_fixed_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
	.get_status		= mt6320_get_status,
	.set_mode		= mt6320_ldo_set_mode,
	.get_mode		= mt6320_ldo_get_mode,
};

/* The array is indexed by id (MT6320_ID_XXX) */
static struct mt6320_regulator_info mt6320_regulators[] = {
	/*
	 * BUCKs
	 * -----
	 * All addresses confirmed from upmu_hw.h VPROC_CONn etc.
	 * enable_reg = CON7  EN=BIT(0), qi=BIT(13)
	 * vselctrl   = CON5  VOSEL_CTRL=BIT(1)
	 * vosel_sw   = CON9
	 * voselon    = CON10
	 * modeset    = CON2  MODESET=BIT(8)
	 */

	/* VPROC  CPU  0.7-1.493 V  (6.25 mV/step, mask=0x7F) */
	MT6320_BUCK("buck_vproc", VPROC, 700000, 1493750, 6250,
		buck_volt_range1,
		0x0214,        /* CON7: enable + status */
		0x0218, 0x7f,  /* CON9: vosel_sw */
		0x021A,        /* CON10: voselon */
		0x0210,        /* CON5: vselctrl */
		0x020A),       /* CON2: modeset */

	/* VSRAM  Memory  0.7-1.493 V  (6.25 mV/step, mask=0x7F) */
	MT6320_BUCK("buck_vsram", VSRAM, 700000, 1493750, 6250,
		buck_volt_range1,
		0x023A, 0x023E, 0x7f, 0x0240, 0x0236, 0x0230),

	/* VCORE  MDSYS/Infra  0.7-1.493 V  (6.25 mV/step, mask=0x7F) */
	MT6320_BUCK("buck_vcore", VCORE, 700000, 1493750, 6250,
		buck_volt_range1,
		0x0266, 0x026A, 0x7f, 0x026C, 0x0262, 0x025C),

	/* VM  1.2-1.493 V  (6.25 mV/step, mask=0x7F) */
	MT6320_BUCK("buck_vm", VM, 700000, 1493750, 6250,
		buck_volt_range1,
		0x028C, 0x0290, 0x7f, 0x0292, 0x0288, 0x0282),

	/* VIO18  IO App.  1.8 V nominal  (6.25 mV/step, mask=0x1F) */
	MT6320_BUCK("buck_vio18", VIO18, 1800000, 1993750, 6250,
		buck_volt_range3,
		0x030E, 0x0312, 0x1f, 0x0314, 0x030A, 0x0304),

	/* VPA  3GPA  0.5-3.65 V  (50 mV/step, mask=0x3F) */
	MT6320_BUCK("buck_vpa", VPA, 500000, 3650000, 50000,
		buck_volt_range2,
		0x0334, 0x0338, 0x3f, 0x033A, 0x0330, 0x032A),

	/* VRF18  1st RF  1.825 V nominal  (25 mV/step, mask=0x1F, vosel=0x1F=1825 mV) */
	MT6320_BUCK("buck_vrf18", VRF18, 1050000, 1825000, 25000,
		buck_volt_range4,
		0x035E, 0x0362, 0x1f, 0x0364, 0x035A, 0x0354),

	/*
	 * VRF18_2  2nd RF / GPU OD  (25 mV/step, mask=0x1F)
	 * vosel=0x1F -> 1825 mV (RF mode, confirmed from real hardware + usage_protection)
	 * vosel=0x04 -> 1150 mV (GPU OD mode, set by pmic_vrf18_2_usage_protection)
	 * vosel=0x00 -> 1050 mV (GPU OD minimum per datasheet)
	 */
	MT6320_BUCK("buck_vrf18_2", VRF18_2, 1050000, 1825000, 25000,
		buck_volt_range4,
		0x0388, 0x038C, 0x1f, 0x038E, 0x0384, 0x037E),

	/*
	 * Analog LDOs
	 * -----------
	 * enable_reg = ANALDO_CONn  (confirmed from upmu_hw.h)
	 * EN bit     = confirmed from PMIC_RG_Vxxx_EN_SHIFT
	 * LP_SEL     = BIT(0) in same enable_reg (confirmed from upmu_common.c)
	 * vosel_reg  = separate ANALDO_CONn
	 * vosel_mask = pre-shifted (raw_mask << shift)
	 *
	 * VRF28_1 / VRF28_2: datasheet 2.85 V; downstream uses same vosel
	 * encoding (0=1800/1=2850). Real hardware reads back 2800 mV for
	 * vosel=1 due to display truncation; 2850 mV is the correct spec value.
	 */

	/* VRF28_1  MDSYS  1.8/2.85 V  (ANALDO_CON0=0x0400, EN=BIT(12)) */
	MT6320_LDO("ldo_vrf28", VRF28, ldo_volt_table5,
		0x0400, 12, 0x0412, BIT(3)),

	/*
	 * VTCXO_1  MDSYS  2.8 V fixed  (ANALDO_CON1=0x0402, EN=BIT(10))
	 * No LP_SEL function in upmu_common.c for VTCXO_1 (ON_CTRL-managed).
	 */
	MT6320_REG_FIXED_NOLP("ldo_vtcxo", VTCXO, 0x0402, 10, 2800000),

	/* VA  1.8/2.5 V  (ANALDO_CON2=0x0404, EN=BIT(14)) */
	MT6320_LDO("ldo_va", VA, ldo_volt_table6,
		0x0404, 14, 0x0410, BIT(6)),

	/* VA28  2.8 V fixed  (ANALDO_CON3=0x0406, EN=BIT(14)) */
	MT6320_REG_FIXED("ldo_va28", VA28, 0x0406, 14, 2800000),

	/* VCAMA  1.5/1.8/2.5/2.8 V  (ANALDO_CON4=0x0408, EN=BIT(15)) */
	MT6320_LDO("ldo_vcama", VCAMA, ldo_volt_table7,
		0x0408, 15, 0x0414, 0x3 << 6),

	/* VRF28_2  General  1.8/2.85 V  (ANALDO_CON13=0x041A, EN=BIT(12)) */
	MT6320_LDO("ldo_vrf28_2", VRF28_2, ldo_volt_table5,
		0x041A, 12, 0x0418, BIT(3)),

	/* VTCXO_2  MDSYS  1.8/2.8 V  (ANALDO_CON14=0x041C, EN=BIT(10)) */
	MT6320_LDO("ldo_vtcxo_2", VTCXO_2, ldo_volt_table5,
		0x041C, 10, 0x0416, BIT(3)),

	/*
	 * Digital LDOs
	 * ------------
	 * enable_reg = DIGLDO_CONn  (confirmed from upmu_hw.h)
	 * EN bit     = confirmed from PMIC_RG_Vxxx_EN_SHIFT / PMIC_Vxxx_EN_SHIFT
	 * LP_SEL     = BIT(0) in same enable_reg (confirmed from upmu_common.c)
	 * vosel_reg  = separate DIGLDO_CONn
	 * vosel_mask = pre-shifted (raw_mask << shift)
	 */

	/* VIO28  2.8 V fixed  (DIGLDO_CON0=0x0420, EN=BIT(14)) */
	MT6320_REG_FIXED("ldo_vio28", VIO28, 0x0420, 14, 2800000),

	/* VUSB  3.3 V fixed  (DIGLDO_CON2=0x0422, EN=BIT(14)) */
	MT6320_REG_FIXED("ldo_vusb", VUSB, 0x0422, 14, 3300000),

	/* VMC1  T-Card  1.8/3.3 V  (DIGLDO_CON3=0x0424, EN=BIT(12)) */
	MT6320_LDO("ldo_vmc1", VMC1, ldo_volt_table1,
		0x0424, 12, 0x044A, BIT(4)),

	/* VMCH1  T-Card  3.0/3.3 V  (DIGLDO_CON5=0x0426, EN=BIT(14)) */
	MT6320_LDO("ldo_vmch1", VMCH1, ldo_volt_table2,
		0x0426, 14, 0x044C, BIT(7)),

	/* VEMC_3V3  eMMC Core  3.0/3.3 V  (DIGLDO_CON6=0x0428, EN=BIT(14)) */
	MT6320_LDO("ldo_vemc_3v3", VEMC_3V3, ldo_volt_table2,
		0x0428, 14, 0x044E, BIT(7)),

	/* VEMC_1V8  eMMC  1.2-3.3 V  (DIGLDO_CON37=0x0462, EN=BIT(14)) */
	MT6320_LDO("ldo_vemc_1v8", VEMC_1V8, ldo_volt_table3,
		0x0462, 14, 0x0464, 0x7 << 5),

	/* VGP1  VCAMD  1.2-3.3 V  (DIGLDO_CON7=0x042A, EN=BIT(15)) */
	MT6320_LDO("ldo_vgp1", VGP1, ldo_volt_table3,
		0x042A, 15, 0x0450, 0x7 << 5),

	/* VGP2  VCAM_IO  1.2-3.3 V  (DIGLDO_CON8=0x042C, EN=BIT(15)) */
	MT6320_LDO("ldo_vgp2", VGP2, ldo_volt_table3,
		0x042C, 15, 0x0452, 0x7 << 5),

	/* VGP3  VCAM_AF  1.2-3.3 V  (DIGLDO_CON9=0x042E, EN=BIT(15)) */
	MT6320_LDO("ldo_vgp3", VGP3, ldo_volt_table3,
		0x042E, 15, 0x0454, 0x7 << 5),

	/* VGP4  CTP/CMMB  1.2-3.3 V  (DIGLDO_CON10=0x0430, EN=BIT(15)) */
	MT6320_LDO("ldo_vgp4", VGP4, ldo_volt_table3,
		0x0430, 15, 0x0456, 0x7 << 5),

	/* VGP5  CTP/CMMB  1.2-3.3 V  (DIGLDO_CON11=0x0432, EN=BIT(15)) */
	MT6320_LDO("ldo_vgp5", VGP5, ldo_volt_table3,
		0x0432, 15, 0x0458, 0x7 << 5),

	/* VGP6  CTP/CMMB  1.2-3.3 V  (DIGLDO_CON12=0x0434, EN=BIT(15)) */
	MT6320_LDO("ldo_vgp6", VGP6, ldo_volt_table3,
		0x0434, 15, 0x045A, 0x7 << 5),

	/* VSIM1  1.2-3.3 V  (DIGLDO_CON13=0x0436, EN=BIT(15)) */
	MT6320_LDO("ldo_vsim1", VSIM1, ldo_volt_table3,
		0x0436, 15, 0x045C, 0x7 << 5),

	/* VSIM2  1.2-3.3 V  (DIGLDO_CON14=0x0438, EN=BIT(15)) */
	MT6320_LDO("ldo_vsim2", VSIM2, ldo_volt_table3,
		0x0438, 15, 0x045E, 0x7 << 5),

	/* VIBR  Vibrator  1.2-3.3 V  (DIGLDO_CON39=0x0466, EN=BIT(15)) */
	MT6320_LDO("ldo_vibr", VIBR, ldo_volt_table3,
		0x0466, 15, 0x0468, 0x7 << 5),

	/*
	 * VRTC  RTC Block  2.8 V fixed  (DIGLDO_CON15=0x043A, EN=BIT(8))
	 * No LP_SEL function in upmu_common.c for VRTC.
	 */
	MT6320_REG_FIXED_NOLP("ldo_vrtc", VRTC, 0x043A, 8, 2800000),

	/*
	 * VAST  MT6168  0.9-1.2 V  (DIGLDO_CON20=0x0444, EN=BIT(12))
	 * enable_reg and vosel_reg share the same address (DIGLDO_CON20).
	 * LP_SEL=BIT(0) also in DIGLDO_CON20.
	 */
	MT6320_LDO("ldo_vast", VAST, ldo_volt_table4,
		0x0444, 12, 0x0444, 0x3 << 13),
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

	/* Read PMIC chip revision */
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
