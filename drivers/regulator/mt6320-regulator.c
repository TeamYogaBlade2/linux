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
 * @qi: Mask for query enable signal status of regulators.
 *      BUCKs use BIT(13), LDOs use BIT(15) (same as MT6323).
 * @vselon_reg: Register for BUCK voltage in hardware-control mode.
 * @vselctrl_reg: Register that selects SW vs HW voltage control for BUCKs.
 * @vselctrl_mask: Mask for the control-mode bit in vselctrl_reg.
 * @modeset_reg: Register for LDO Normal/Low-Power mode selection.
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
 * BUCK register layout
 * --------------------
 * Each BUCK has a contiguous CON block, 2 bytes per register:
 *   CON_base  = status_reg - 20
 *   CON7  = base+14  -> enable_reg  (EN=BIT(0))
 *   CON8  = base+16  -> vselctrl_reg (VOSEL_CTRL=BIT(1))
 *   CON10 = base+20  -> status reg   (qi=BIT(13))
 *   CON15 = base+30  -> vosel_sw     (SW-mode voltage select)
 *   CON16 = base+32  -> voselon      (HW-mode voltage select) <unknown>: verify
 *
 * Verified anchors (from downstream pmic_config_interface comments):
 *   VPROC : ctrl=0x210[1], status=0x214[13], vosel_sw=0x21E[6:0]
 *   VSRAM : ctrl=0x236[1], status=0x23A[13], vosel_sw=0x244[6:0]
 *   VCORE : ctrl=0x262[1], status=0x266[13], vosel_sw=0x270[6:0]
 *   VM    : status=0x28C[13], vosel_sw=0x296[4:0]
 *   VIO18 : status=0x30E[13], vosel_sw=0x318[4:0]
 *   VPA   : status=0x334[13], vosel_sw=0x33E[5:0]
 *   VRF18   : status=0x35E[13], vosel_sw=0x368[4:0]
 *   VRF18_2 : status=0x388[13], vosel_sw=0x392[4:0]
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

/*
 * BUCK voltage ranges
 * -------------------
 *
 * buck_volt_range1: 700 mV base, 6.25 mV/step, 7-bit (0x00-0x7F)
 *   -> VPROC (0.7-1.25 V), VSRAM (0.7-1.25 V), VCORE (0.7-1.25 V)
 *
 * buck_volt_range2: 1500 mV base, 20 mV/step, 5-bit (0x00-0x1F)
 *   -> VM (1.2/1.25/1.35/1.5 V)
 *   <unknown>: exact step size needs verification against register spec.
 *
 * buck_volt_range3: 500 mV base, 50 mV/step, 6-bit (0x00-0x3F)
 *   -> VPA (0.5-3.4 V; datasheet says 100 mV/step but downstream mask is
 *      0x3F = 64 steps; using 50 mV/step to be consistent with mask width)
 *   <unknown>: confirm step size.
 *
 * buck_volt_range4: 1050 mV base, 25 mV/step, 5-bit (0x00-0x1F)
 *   -> VRF18_2 GPU OD mode (1.05-1.25 V, 50 mV/step per datasheet)
 *   <unknown>: VRF18 and VRF18_2 are listed as fixed 1.825 V in normal mode;
 *   variable range only applies in GPU OD mode for VRF18_2. Dual-mode
 *   operation is not expressible in a single regulator_desc.
 */
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

/*
 * LDO voltage tables
 * ------------------
 * Derived from dct_pmic_XXX_sel() in pmic_mt6320.c.
 * Index matches the hardware vosel encoding (0-based).
 *
 * vosel register addresses and bit-field positions confirmed from
 * show_LDO_XXX_VOLTAGE() pmic_read_interface(reg, &val, mask, shift) calls.
 * The vsel_mask passed to regmap must be the pre-shifted mask: mask << shift.
 *
 * ldo_volt_table1: VMC  (vosel 0x44A, mask=0x01 shift=4 -> BIT(4))
 *   0=1800 mV, 1=3300 mV
 */
static const unsigned int ldo_volt_table1[] = {
	1800000, 3300000,
};

/*
 * ldo_volt_table2: VMCH, VEMC_3V3  (vosel mask=0x01 shift=7 -> BIT(7))
 *   0=3000 mV, 1=3300 mV
 */
static const unsigned int ldo_volt_table2[] = {
	3000000, 3300000,
};

/*
 * ldo_volt_table3: VEMC_1V8, VGP1-VGP6, VSIM1, VSIM2, VIBR
 *   (vosel mask=0x07 shift=5 -> 0x07<<5 = 0xE0)
 *   0=1200, 1=1300, 2=1500, 3=1800, 4=2500, 5=2800, 6=3000, 7=3300 mV
 */
static const unsigned int ldo_volt_table3[] = {
	1200000, 1300000, 1500000, 1800000, 2500000, 2800000, 3000000, 3300000,
};

/*
 * ldo_volt_table4: VAST  (vosel 0x444, mask=0x03 shift=13 -> 0x03<<13 = 0x6000)
 *   Descending: 0=1200, 1=1100, 2=1000, 3=900 mV
 */
static const unsigned int ldo_volt_table4[] = {
	1200000, 1100000, 1000000, 900000,
};

/*
 * ldo_volt_table5: VRF28_1, VRF28_2, VTCXO_2
 *   (vosel mask=0x01 shift=3 -> BIT(3))
 *   0=1800 mV, 1=2800 mV
 *   Note: datasheet lists VRF28_1 as fixed 2.85 V; downstream dct code
 *   offers 1800/2800 mV via 1-bit vosel. Using downstream behaviour.
 *   <unknown>: confirm 2800 vs 2850 mV.
 */
static const unsigned int ldo_volt_table5[] = {
	1800000, 2800000,
};

/*
 * ldo_volt_table6: VA  (vosel 0x410, mask=0x01 shift=6 -> BIT(6))
 *   0=1800 mV, 1=2500 mV
 */
static const unsigned int ldo_volt_table6[] = {
	1800000, 2500000,
};

/*
 * ldo_volt_table7: VCAMA  (vosel 0x414, mask=0x03 shift=6 -> 0x03<<6 = 0xC0)
 *   0=1500, 1=1800, 2=2500, 3=2800 mV
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

/*
 * The array is indexed by id (MT6320_ID_XXX).
 *
 * -----------------------------------------------------------------------
 * Address derivation
 * -----------------------------------------------------------------------
 * BUCK:
 *   All register addresses derived from verified status/vosel/ctrl anchors
 *   in the downstream driver using the CON block layout:
 *     enable_reg  = status_reg - 6   (CON7  = CON10 - 3*2)
 *     vselctrl    = status_reg - 4   (CON8  = CON10 - 2*2)
 *     vosel_sw    = status_reg + 10  (CON15 = CON10 + 5*2)
 *     voselon     = vosel_sw   + 2   (CON16)  <unknown>: verify
 *
 * LDO:
 *   enable_reg   = STATUS register (qi = BIT(15) confirmed for all LDOs)
 *   enbit        = hardware SW-enable bit within enable_reg
 *                  VTCXO_1: BIT(10) confirmed from INIT_SETTING comment
 *                  All others: <unknown> — using 14 as placeholder
 *                  (common in MT6323 DIGLDO; verify against MT6320 reg map)
 *   vosel_reg    = VOLTAGE register (from show_LDO_XXX_VOLTAGE)
 *   vsel_mask    = pre-shifted mask: raw_mask << shift
 *                  (from pmic_read_interface(reg, &val, mask, shift) args)
 *   modeset_reg/mask: <unknown> for all LDOs
 * -----------------------------------------------------------------------
 */
static struct mt6320_regulator_info mt6320_regulators[] = {
	/*
	 * ================================================================
	 * BUCKs
	 * ================================================================
	 */

	/*
	 * VPROC  CPU  0.7-1.25 V DC/DC  (6.25 mV/step, 7-bit)
	 * base=0x200: en=0x20E, ctrl=0x210, status=0x214, vosel_sw=0x21E
	 */
	MT6320_BUCK("buck_vproc", VPROC, 700000, 1493750, 6250,
		buck_volt_range1,
		0x20E,          /* enable_reg  CON7  */
		0x21E, 0x7f,    /* vosel_sw    CON15 [6:0] */
		0x220,          /* voselon     CON16 <unknown> */
		0x210),         /* vselctrl    CON8, VOSEL_CTRL=BIT(1) */

	/*
	 * VSRAM  Memory  0.7-1.25 V DC/DC  (6.25 mV/step, 7-bit)
	 * base=0x226: en=0x234, ctrl=0x236, status=0x23A, vosel_sw=0x244
	 */
	MT6320_BUCK("buck_vsram", VSRAM, 700000, 1493750, 6250,
		buck_volt_range1,
		0x234,
		0x244, 0x7f,
		0x246,          /* <unknown> */
		0x236),

	/*
	 * VCORE  MDSYS/Infra  0.7-1.25 V DC/DC  (6.25 mV/step, 7-bit)
	 * base=0x252: en=0x260, ctrl=0x262, status=0x266, vosel_sw=0x270
	 */
	MT6320_BUCK("buck_vcore", VCORE, 700000, 1493750, 6250,
		buck_volt_range1,
		0x260,
		0x270, 0x7f,
		0x272,          /* <unknown> */
		0x262),

	/*
	 * VM  1.2/1.25/1.35/1.5 V  (20 mV/step, 5-bit)
	 * base=0x278: en=0x286, ctrl=0x288, status=0x28C, vosel_sw=0x296
	 * vselctrl_reg <unknown>: computed 0x288, not confirmed in downstream
	 */
	MT6320_BUCK("buck_vm", VM, 1500000, 2125000, 20000,
		buck_volt_range2,
		0x286,
		0x296, 0x1f,
		0x298,          /* <unknown> */
		0x288),         /* <unknown> */

	/*
	 * VIO18  IO App.  1.8 V
	 * base=0x2FA: en=0x308, ctrl=0x30A, status=0x30E, vosel_sw=0x318
	 * Datasheet lists fixed 1.8 V; downstream has 5-bit vosel register.
	 * Treating as linear_range BUCK. <unknown>: confirm variability.
	 */
	MT6320_BUCK("buck_vio18", VIO18, 700000, 1293750, 6250,
		buck_volt_range1,
		0x308,
		0x318, 0x1f,
		0x31A,          /* <unknown> */
		0x30A),         /* <unknown> */

	/*
	 * VPA  3GPA  0.5-3.4 V  (6-bit vosel, 0x3F mask)
	 * base=0x320: en=0x32E, ctrl=0x330, status=0x334, vosel_sw=0x33E
	 */
	MT6320_BUCK("buck_vpa", VPA, 500000, 3650000, 50000,
		buck_volt_range3,
		0x32E,
		0x33E, 0x3f,
		0x340,          /* <unknown> */
		0x330),         /* <unknown> */

	/*
	 * VRF18  1st RF  1.825 V
	 * base=0x34A: en=0x358, ctrl=0x35A, status=0x35E, vosel_sw=0x368
	 * Datasheet says fixed 1.825 V; downstream has 5-bit vosel.
	 * <unknown>: confirm whether SW voltage selection is used.
	 */
	MT6320_BUCK("buck_vrf18", VRF18, 700000, 1293750, 6250,
		buck_volt_range1,
		0x358,
		0x368, 0x1f,
		0x36A,          /* <unknown> */
		0x35A),         /* <unknown> */

	/*
	 * VRF18_2  2nd RF / GPU OD
	 *   Normal:   1.825 V fixed
	 *   GPU OD:   1.05-1.25 V, 50 mV/step  (buck_volt_range4)
	 * base=0x374: en=0x382, ctrl=0x384, status=0x388, vosel_sw=0x392
	 * Using GPU OD range as it is the variable case.
	 * <unknown>: dual-mode not expressible in single regulator_desc.
	 */
	MT6320_BUCK("buck_vrf18_2", VRF18_2, 1050000, 1825000, 25000,
		buck_volt_range4,
		0x382,
		0x392, 0x1f,
		0x394,          /* <unknown> */
		0x384),         /* <unknown> */

	/*
	 * ================================================================
	 * Analog LDOs  (base block ~0x400)
	 * ================================================================
	 * enable_reg = STATUS register address (qi = BIT(15))
	 * enbit: <unknown> for all except VTCXO_1
	 * modeset_reg/mask: <unknown> for all
	 */

	/*
	 * VTCXO_1  MDSYS  2.8 V fixed
	 * enable_reg=0x402, enbit=10 confirmed (RG_VTCXO_EN at bit[10]
	 * from INIT_SETTING: pmic_config_interface(ANALDO_CON1,1,0x1,10))
	 */
	MT6320_REG_FIXED("ldo_vtcxo_1", VTCXO_1, 0x402, 10, 2800000,
		-1, 0),

	/*
	 * VTCXO_2  MDSYS  1.8/2.8 V
	 * vosel: 0x416, mask=0x01 shift=3 -> BIT(3)
	 */
	MT6320_LDO("ldo_vtcxo_2", VTCXO_2, ldo_volt_table5,
		0x41C, 14 /* <unknown> */, 0x416, BIT(3),
		-1, 0),

	/*
	 * VA  1.8/2.5 V
	 * vosel: 0x410, mask=0x01 shift=6 -> BIT(6)
	 */
	MT6320_LDO("ldo_va", VA, ldo_volt_table6,
		0x404, 14 /* <unknown> */, 0x410, BIT(6),
		-1, 0),

	/*
	 * VA28  2.8 V fixed
	 * No vosel in downstream (dct_pmic_VA28_sel is empty).
	 */
	MT6320_REG_FIXED("ldo_va28", VA28, 0x406, 14 /* <unknown> */, 2800000,
		-1, 0),

	/*
	 * VRF28_1  MDSYS  2.85 V (datasheet) / 1.8 or 2.8 V (downstream)
	 * vosel: 0x412, mask=0x01 shift=3 -> BIT(3)
	 * <unknown>: datasheet says 2.85 V fixed; downstream offers 1800/2800 mV.
	 */
	MT6320_LDO("ldo_vrf28_1", VRF28_1, ldo_volt_table5,
		0x400, 14 /* <unknown> */, 0x412, BIT(3),
		-1, 0),

	/*
	 * VRF28_2  General  1.8/2.85 V (datasheet) / 1.8 or 2.8 V (downstream)
	 * vosel: 0x418, mask=0x01 shift=3 -> BIT(3)
	 */
	MT6320_LDO("ldo_vrf28_2", VRF28_2, ldo_volt_table5,
		0x41A, 14 /* <unknown> */, 0x418, BIT(3),
		-1, 0),

	/*
	 * VCAMA  1.5/1.8/2.5/2.8 V
	 * vosel: 0x414, mask=0x03 shift=6 -> 0x03<<6 = 0xC0
	 */
	MT6320_LDO("ldo_vcama", VCAMA, ldo_volt_table7,
		0x408, 14 /* <unknown> */, 0x414, 0x03 << 6,
		-1, 0),

	/*
	 * ================================================================
	 * Digital LDOs  (base block ~0x420)
	 * ================================================================
	 * vosel masks are pre-shifted: raw_mask << shift
	 * All enbit values <unknown>; using 14 as placeholder.
	 * modeset_reg/mask: <unknown> for all.
	 */

	/* VIO28  2.8 V fixed */
	MT6320_REG_FIXED("ldo_vio28", VIO28, 0x420, 14 /* <unknown> */, 2800000,
		-1, 0),

	/* VUSB  3.3 V fixed */
	MT6320_REG_FIXED("ldo_vusb", VUSB, 0x422, 14 /* <unknown> */, 3300000,
		-1, 0),

	/*
	 * VMC  T-Card  1.8/3.3 V
	 * vosel: 0x44A, mask=0x01 shift=4 -> BIT(4)
	 */
	MT6320_LDO("ldo_vmc", VMC, ldo_volt_table1,
		0x424, 14 /* <unknown> */, 0x44A, BIT(4),
		-1, 0),

	/*
	 * VMCH  T-Card  3.0/3.3 V
	 * vosel: 0x44C, mask=0x01 shift=7 -> BIT(7)
	 */
	MT6320_LDO("ldo_vmch", VMCH, ldo_volt_table2,
		0x426, 14 /* <unknown> */, 0x44C, BIT(7),
		-1, 0),

	/*
	 * VEMC_3V3  eMMC Core  3.0/3.3 V
	 * vosel: 0x44E, mask=0x01 shift=7 -> BIT(7)
	 */
	MT6320_LDO("ldo_vemc_3v3", VEMC_3V3, ldo_volt_table2,
		0x428, 14 /* <unknown> */, 0x44E, BIT(7),
		-1, 0),

	/*
	 * VEMC_1V8  eMMC  1.2/1.3/1.5/1.8/2.5/2.8/3.0/3.3 V
	 * vosel: 0x464, mask=0x07 shift=5 -> 0x07<<5 = 0xE0
	 */
	MT6320_LDO("ldo_vemc_1v8", VEMC_1V8, ldo_volt_table3,
		0x462, 14 /* <unknown> */, 0x464, 0x07 << 5,
		-1, 0),

	/*
	 * VGP1-VGP6  1.2/1.3/1.5/1.8/2.5/2.8/3.0/3.3 V
	 * vosel: 0x450-0x45A, mask=0x07 shift=5 -> 0xE0 each
	 */
	MT6320_LDO("ldo_vgp1", VGP1, ldo_volt_table3,
		0x42A, 14 /* <unknown> */, 0x450, 0x07 << 5,
		-1, 0),
	MT6320_LDO("ldo_vgp2", VGP2, ldo_volt_table3,
		0x42C, 14 /* <unknown> */, 0x452, 0x07 << 5,
		-1, 0),
	MT6320_LDO("ldo_vgp3", VGP3, ldo_volt_table3,
		0x42E, 14 /* <unknown> */, 0x454, 0x07 << 5,
		-1, 0),
	MT6320_LDO("ldo_vgp4", VGP4, ldo_volt_table3,
		0x430, 14 /* <unknown> */, 0x456, 0x07 << 5,
		-1, 0),
	MT6320_LDO("ldo_vgp5", VGP5, ldo_volt_table3,
		0x432, 14 /* <unknown> */, 0x458, 0x07 << 5,
		-1, 0),
	MT6320_LDO("ldo_vgp6", VGP6, ldo_volt_table3,
		0x434, 14 /* <unknown> */, 0x45A, 0x07 << 5,
		-1, 0),

	/*
	 * VSIM1 / VSIM2  1.2/1.3/1.5/1.8/2.5/2.8/3.0/3.3 V
	 * vosel: 0x45C / 0x45E, mask=0x07 shift=5 -> 0xE0
	 * Note: downstream INIT_SETTING sets VSIM1/2 vosel to 0 at boot
	 * (pmic_config_interface(0x45C, 0x0, 0x7, 5)).
	 */
	MT6320_LDO("ldo_vsim1", VSIM1, ldo_volt_table3,
		0x436, 14 /* <unknown> */, 0x45C, 0x07 << 5,
		-1, 0),
	MT6320_LDO("ldo_vsim2", VSIM2, ldo_volt_table3,
		0x438, 14 /* <unknown> */, 0x45E, 0x07 << 5,
		-1, 0),

	/* VRTC  RTC Block  2.8 V fixed */
	MT6320_REG_FIXED("ldo_vrtc", VRTC, 0x43A, 14 /* <unknown> */, 2800000,
		-1, 0),

	/*
	 * VAST  MT6168  0.9/1.0/1.1/1.2 V
	 * vosel: 0x444, mask=0x03 shift=13 -> 0x03<<13 = 0x6000
	 * Note: enable_reg and vosel_reg share the same address (0x444 =
	 * DIGLDO_CON20).  enbit is confirmed to be in this register
	 * (pmic_config_interface(DIGLDO_CON20, 0x0, PMIC_RG_VAST_EN_MASK, ...))
	 * but the exact bit position is <unknown>.
	 */
	MT6320_LDO("ldo_vast", VAST, ldo_volt_table4,
		0x444, 14 /* <unknown> */, 0x444, 0x03 << 13,
		-1, 0),

	/*
	 * VIBR  Vibrator  1.2/1.3/1.5/1.8/2.5/2.8/3.0/3.3 V
	 * vosel: 0x468, mask=0x07 shift=5 -> 0xE0
	 */
	MT6320_LDO("ldo_vibr", VIBR, ldo_volt_table3,
		0x466, 14 /* <unknown> */, 0x468, 0x07 << 5,
		-1, 0),
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
