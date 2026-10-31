// SPDX-License-Identifier: GPL-2.0
/*
 * Lenovo B8000-H battery: voltage from the MT6320 AUXADC (channel 0, BATSNS),
 * charge from the vendor 25 C OCV table, status from the BQ24196 charger.
 *
 * B8000 has no fuel-gauge IC (vendor MTK_BQ27541_SUPPORT=no); the vendor
 * "SW fuel gauge" (mt6320_fuel_gauge_bq24196.c) does the same: AUXADC code
 * * 4 * 1200 / 1024 mV, OCV lookup in cust_fuel_gauge.h battery_profile_t2.
 * The PMIC is reached through the pwrap regmap.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define AUXADC_ADC0	0x0512	/* ch0 out [9:0], ready [15] */
#define AUXADC_CON0	0x0540	/* avg_num [6:4] */
#define AUXADC_CON1	0x0542	/* chsel [10:7], start [0] */
#define AUXADC_CON14	0x055c	/* ch0 source: norm_sel [2], lbat_sel [0] */

#define POLL_MS		30000
#define NSAMPLES	8

/* vendor battery_profile_t2_bat1: {depth of discharge %, OCV mV} */
static const u16 ocv_table[][2] = {
	{0, 4190}, {1, 4171}, {3, 4155}, {4, 4141}, {5, 4127}, {7, 4114},
	{8, 4101}, {9, 4089}, {11, 4077}, {12, 4066}, {14, 4053}, {15, 4041},
	{16, 4029}, {18, 4018}, {19, 4007}, {20, 3997}, {22, 3987}, {23, 3978},
	{24, 3969}, {26, 3961}, {27, 3952}, {28, 3944}, {30, 3935}, {31, 3927},
	{33, 3919}, {34, 3912}, {35, 3904}, {37, 3896}, {38, 3887}, {39, 3876},
	{41, 3863}, {42, 3851}, {43, 3842}, {45, 3834}, {46, 3827}, {47, 3821},
	{49, 3816}, {50, 3811}, {52, 3807}, {53, 3803}, {54, 3799}, {56, 3795},
	{57, 3792}, {58, 3789}, {60, 3786}, {61, 3784}, {62, 3781}, {64, 3779},
	{65, 3778}, {66, 3776}, {68, 3774}, {69, 3772}, {71, 3769}, {72, 3764},
	{73, 3759}, {75, 3754}, {76, 3749}, {77, 3744}, {79, 3740}, {80, 3736},
	{81, 3731}, {83, 3725}, {84, 3718}, {85, 3711}, {87, 3704}, {88, 3695},
	{89, 3685}, {91, 3681}, {92, 3677}, {94, 3672}, {95, 3662}, {96, 3629},
	{98, 3570}, {99, 3490}, {100, 3370},
};

/* Terminal voltage rises under charge current; rough IR compensation. */
static int charge_comp_mv = 60;
module_param(charge_comp_mv, int, 0644);

struct b8k_bat {
	struct device *dev;
	struct regmap *map;
	struct power_supply *psy;
	struct delayed_work work;
	struct mutex lock;
	int samples[NSAMPLES];
	int nsamples, next;
	int mv, capacity;
};

static struct platform_device *b8k_pdev;

static int b8k_read_vbat_mv(struct b8k_bat *b)
{
	unsigned int v;
	int i, ret;

	regmap_update_bits(b->map, AUXADC_CON14, BIT(2) | BIT(0), BIT(2) | BIT(0));
	regmap_update_bits(b->map, AUXADC_CON0, 0x7 << 4, 0x3 << 4);
	regmap_update_bits(b->map, AUXADC_CON1, 0xf << 7, 0);
	regmap_update_bits(b->map, AUXADC_CON1, BIT(0), 0);
	regmap_update_bits(b->map, AUXADC_CON1, BIT(0), BIT(0));
	usleep_range(500, 1000);
	for (i = 0; i < 50; i++) {
		ret = regmap_read(b->map, AUXADC_ADC0, &v);
		if (ret)
			return ret;
		if (v & BIT(15))
			return ((v & 0x3ff) * 4 * 1200) / 1024;
		usleep_range(200, 400);
	}
	return -ETIMEDOUT;
}

static int b8k_ocv_to_capacity(int mv)
{
	int i, n = ARRAY_SIZE(ocv_table);

	if (mv >= ocv_table[0][1])
		return 100;
	if (mv <= ocv_table[n - 1][1])
		return 0;
	for (i = 1; i < n; i++) {
		int v0 = ocv_table[i - 1][1], v1 = ocv_table[i][1];
		int d0 = ocv_table[i - 1][0], d1 = ocv_table[i][0];

		if (mv >= v1) {
			int dod = d0 + (d1 - d0) * (v0 - mv) / (v0 - v1);

			return 100 - dod;
		}
	}
	return 0;
}

static int b8k_charger_status(void)
{
	union power_supply_propval val = { .intval = POWER_SUPPLY_STATUS_UNKNOWN };
	struct power_supply *chg = power_supply_get_by_name("bq24190-charger");

	if (!chg)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	power_supply_get_property(chg, POWER_SUPPLY_PROP_STATUS, &val);
	power_supply_put(chg);
	return val.intval;
}

static void b8k_update(struct b8k_bat *b)
{
	int mv = b8k_read_vbat_mv(b), sum = 0, i, cap, st;

	if (mv <= 0) {
		dev_warn(b->dev, "AUXADC read failed: %d\n", mv);
		return;
	}
	st = b8k_charger_status();
	if (st == POWER_SUPPLY_STATUS_CHARGING)
		mv -= charge_comp_mv;

	mutex_lock(&b->lock);
	b->samples[b->next] = mv;
	b->next = (b->next + 1) % NSAMPLES;
	if (b->nsamples < NSAMPLES)
		b->nsamples++;
	for (i = 0; i < b->nsamples; i++)
		sum += b->samples[i];
	b->mv = sum / b->nsamples;
	cap = b8k_ocv_to_capacity(b->mv);
	/* while charging never report less, while discharging never more */
	if (b->capacity >= 0) {
		if (st == POWER_SUPPLY_STATUS_CHARGING && cap < b->capacity)
			cap = b->capacity;
		if (st == POWER_SUPPLY_STATUS_DISCHARGING && cap > b->capacity)
			cap = b->capacity;
	}
	i = cap != b->capacity;
	b->capacity = cap;
	mutex_unlock(&b->lock);
	if (i)
		power_supply_changed(b->psy);
}

static void b8k_work(struct work_struct *w)
{
	struct b8k_bat *b = container_of(w, struct b8k_bat, work.work);

	b8k_update(b);
	schedule_delayed_work(&b->work, msecs_to_jiffies(POLL_MS));
}

static const enum power_supply_property b8k_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
};

static int b8k_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct b8k_bat *b = power_supply_get_drvdata(psy);
	int st;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		st = b8k_charger_status();
		if (st == POWER_SUPPLY_STATUS_UNKNOWN)
			st = POWER_SUPPLY_STATUS_DISCHARGING;
		if (st == POWER_SUPPLY_STATUS_NOT_CHARGING && b->capacity >= 100)
			st = POWER_SUPPLY_STATUS_FULL;
		val->intval = st;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = b->mv * 1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = b->capacity < 0 ? 50 : b->capacity;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct power_supply_desc b8k_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = b8k_props,
	.num_properties = ARRAY_SIZE(b8k_props),
	.get_property = b8k_get_property,
};

static int b8k_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = {};
	struct device *pwrap;
	struct b8k_bat *b;

	b = devm_kzalloc(&pdev->dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = &pdev->dev;
	b->capacity = -1;
	mutex_init(&b->lock);

	pwrap = bus_find_device_by_name(&platform_bus_type, NULL, "1000f000.pwrap");
	if (!pwrap)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER, "no pwrap\n");
	b->map = dev_get_regmap(pwrap, NULL);
	put_device(pwrap);
	if (!b->map)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER, "no pwrap regmap\n");

	cfg.drv_data = b;
	b->psy = devm_power_supply_register(&pdev->dev, &b8k_desc, &cfg);
	if (IS_ERR(b->psy))
		return PTR_ERR(b->psy);
	platform_set_drvdata(pdev, b);

	INIT_DELAYED_WORK(&b->work, b8k_work);
	b8k_update(b);
	dev_info(&pdev->dev, "battery %d mV, %d %%\n", b->mv, b->capacity);
	schedule_delayed_work(&b->work, msecs_to_jiffies(POLL_MS));
	return 0;
}

static void b8k_remove(struct platform_device *pdev)
{
	struct b8k_bat *b = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&b->work);
}

static struct platform_driver b8k_driver = {
	.probe = b8k_probe,
	.remove = b8k_remove,
	.driver = { .name = "b8k-battery" },
};

static int __init b8k_init(void)
{
	int ret = platform_driver_register(&b8k_driver);

	if (ret)
		return ret;
	b8k_pdev = platform_device_register_simple("b8k-battery", -1, NULL, 0);
	if (IS_ERR(b8k_pdev)) {
		platform_driver_unregister(&b8k_driver);
		return PTR_ERR(b8k_pdev);
	}
	return 0;
}
module_init(b8k_init);

static void __exit b8k_exit(void)
{
	platform_device_unregister(b8k_pdev);
	platform_driver_unregister(&b8k_driver);
}
module_exit(b8k_exit);

MODULE_DESCRIPTION("Lenovo B8000-H battery (MT6320 AUXADC + OCV table)");
MODULE_LICENSE("GPL");
