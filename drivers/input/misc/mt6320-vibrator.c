// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6320 PMIC vibrator driver
 *
 * The MT6320 drives the vibrator from its integrated VIBR LDO.  The
 * regulator framework owns the LDO enable bit and voltage selector; this
 * driver sets the vibrator-specific safety bits and exposes force feedback
 * through the input subsystem.
 */

#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define MT6320_VIBR_REG_CTRL		0x0466 /* DIGLDO_CON39 */
#define MT6320_VIBR_REG_VSEL		0x0468 /* DIGLDO_CON40 */
#define MT6320_VIBR_THER_SHEN_EN	BIT(2)
#define MT6320_VIBR_STB_SEL		BIT(4)

#define MT6320_VIBR_DEFAULT_UV		2800000

static const unsigned int mt6320_vibr_voltages_uv[] = {
	1200000, 1300000, 1500000, 1800000,
	2500000, 2800000, 3000000, 3300000,
};

struct mt6320_vibrator {
	struct device *dev;
	struct input_dev *input;
	struct regulator *vibr_reg;
	struct regmap *regmap;
	struct work_struct play_work;
	spinlock_t request_lock;
	struct mutex hw_lock;

	/* Protected by request_lock. */
	u16 requested_level;
	bool shutting_down;

	/* Protected by hw_lock. */
	bool enabled;

	/* Highest output voltage allowed for FF_RUMBLE strength mapping. */
	unsigned int amplitude_uv;
};

/* Round down to a voltage supported by the MT6320 VIBR LDO. */
static unsigned int mt6320_vibr_round_voltage(unsigned int uv)
{
	int i;

	for (i = ARRAY_SIZE(mt6320_vibr_voltages_uv) - 1; i >= 0; i--)
		if (uv >= mt6320_vibr_voltages_uv[i])
			return mt6320_vibr_voltages_uv[i];

	return mt6320_vibr_voltages_uv[0];
}

static unsigned int mt6320_vibr_voltage_for_level(struct mt6320_vibrator *vib,
						  u16 level)
{
	unsigned int max_index = 0;
	unsigned int index;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6320_vibr_voltages_uv); i++) {
		if (mt6320_vibr_voltages_uv[i] > vib->amplitude_uv)
			break;
		max_index = i;
	}

	/* Map the 16-bit FF magnitude to the available LDO voltage steps. */
	index = DIV_ROUND_CLOSEST((unsigned int)level * max_index, 0xffffU);

	return mt6320_vibr_voltages_uv[index];
}

static int mt6320_vibr_hw_init(struct mt6320_vibrator *vib)
{
	int ret;

	/* Match the vendor PMIC initialization: enable thermal shutdown. */
	ret = regmap_update_bits(vib->regmap, MT6320_VIBR_REG_CTRL,
				 MT6320_VIBR_THER_SHEN_EN,
				 MT6320_VIBR_THER_SHEN_EN);
	if (ret)
		return ret;

	/* Select the standby source used by the vendor driver. */
	return regmap_update_bits(vib->regmap, MT6320_VIBR_REG_VSEL,
				  MT6320_VIBR_STB_SEL,
				  MT6320_VIBR_STB_SEL);
}

/* Called with hw_lock held; regulator operations are allowed to sleep. */
static int mt6320_vibr_apply_level(struct mt6320_vibrator *vib, u16 level)
{
	unsigned int voltage_uv;
	int ret;
	int off_ret;

	if (!level) {
		if (!vib->enabled)
			return 0;

		ret = regulator_disable(vib->vibr_reg);
		if (!ret)
			vib->enabled = false;

		return ret;
	}

	voltage_uv = mt6320_vibr_voltage_for_level(vib, level);
	ret = regulator_set_voltage(vib->vibr_reg, voltage_uv, voltage_uv);
	if (ret) {
		/* Do not leave an actuator running at a stale strength on error. */
		if (vib->enabled) {
			off_ret = regulator_disable(vib->vibr_reg);
			if (!off_ret)
				vib->enabled = false;
			else
				dev_err(vib->dev,
					"failed to disable vibrator after voltage error: %d\n",
					off_ret);
		}

		return ret;
	}

	if (vib->enabled)
		return 0;

	ret = regulator_enable(vib->vibr_reg);
	if (!ret)
		vib->enabled = true;

	return ret;
}

/* Force-feedback callbacks can run under input_dev->event_lock; do not sleep. */
static int mt6320_vibr_play_effect(struct input_dev *input, void *data,
				   struct ff_effect *effect)
{
	struct mt6320_vibrator *vib = data;
	unsigned long flags;
	u16 level;

	level = effect->u.rumble.strong_magnitude;
	if (!level)
		level = effect->u.rumble.weak_magnitude;

	spin_lock_irqsave(&vib->request_lock, flags);
	if (vib->shutting_down) {
		spin_unlock_irqrestore(&vib->request_lock, flags);
		return -ESHUTDOWN;
	}
	vib->requested_level = level;
	spin_unlock_irqrestore(&vib->request_lock, flags);

	/* All PMIC/regulator access is deferred to process context. */
	schedule_work(&vib->play_work);

	return 0;
}

static void mt6320_vibr_play_work(struct work_struct *work)
{
	struct mt6320_vibrator *vib =
		container_of(work, struct mt6320_vibrator, play_work);
	unsigned long flags;
	bool shutting_down;
	u16 level;
	int ret;

	/* Consume the newest requested state; coalesce rapid FF updates. */
	for (;;) {
		spin_lock_irqsave(&vib->request_lock, flags);
		level = vib->requested_level;
		shutting_down = vib->shutting_down;
		spin_unlock_irqrestore(&vib->request_lock, flags);

		mutex_lock(&vib->hw_lock);
		ret = mt6320_vibr_apply_level(vib,
					      shutting_down ? 0 : level);
		mutex_unlock(&vib->hw_lock);

		if (ret)
			dev_err(vib->dev, "failed to %s vibrator (level %u): %d\n",
				(level && !shutting_down) ? "apply effect to" : "stop",
				level, ret);

		spin_lock_irqsave(&vib->request_lock, flags);
		if (level == vib->requested_level &&
		    shutting_down == vib->shutting_down) {
			spin_unlock_irqrestore(&vib->request_lock, flags);
			break;
		}
		spin_unlock_irqrestore(&vib->request_lock, flags);
	}
}

static void mt6320_vibr_close(struct input_dev *input)
{
	struct mt6320_vibrator *vib = input_get_drvdata(input);
	unsigned long flags;
	bool shutting_down;

	spin_lock_irqsave(&vib->request_lock, flags);
	shutting_down = vib->shutting_down;
	if (!shutting_down)
		vib->requested_level = 0;
	spin_unlock_irqrestore(&vib->request_lock, flags);

	if (!shutting_down)
		schedule_work(&vib->play_work);
	flush_work(&vib->play_work);
}

static void mt6320_vibr_quiesce(struct mt6320_vibrator *vib)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&vib->request_lock, flags);
	vib->shutting_down = true;
	vib->requested_level = 0;
	spin_unlock_irqrestore(&vib->request_lock, flags);

	cancel_work_sync(&vib->play_work);

	mutex_lock(&vib->hw_lock);
	if (vib->enabled) {
		ret = regulator_disable(vib->vibr_reg);
		if (!ret)
			vib->enabled = false;
	}
	mutex_unlock(&vib->hw_lock);

	if (ret)
		dev_err(vib->dev, "failed to power off vibrator: %d\n", ret);
}

static int mt6320_vibr_probe(struct platform_device *pdev)
{
	struct mt6397_chip *pmic;
	struct mt6320_vibrator *vib;
	u32 amplitude_uv = MT6320_VIBR_DEFAULT_UV;
	int ret;

	vib = devm_kzalloc(&pdev->dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;

	vib->dev = &pdev->dev;

	/* The MT6320 MFD parent owns the regmap shared by its child devices. */
	pmic = dev_get_drvdata(pdev->dev.parent);
	if (!pmic || !pmic->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no regmap available from MT6320 parent\n");
	vib->regmap = pmic->regmap;

	/* Regulator IDs omit -supply; the core resolves the DT's vibr-supply. */
	vib->vibr_reg = devm_regulator_get(&pdev->dev, "vibr");
	if (IS_ERR(vib->vibr_reg))
		return dev_err_probe(&pdev->dev, PTR_ERR(vib->vibr_reg),
				     "failed to get VIBR regulator\n");

	device_property_read_u32(&pdev->dev, "mediatek,vibr-amplitude-uv",
				 &amplitude_uv);
	vib->amplitude_uv = clamp_val(amplitude_uv,
				      mt6320_vibr_voltages_uv[0],
				      mt6320_vibr_voltages_uv[ARRAY_SIZE(mt6320_vibr_voltages_uv) - 1]);
	vib->amplitude_uv = mt6320_vibr_round_voltage(vib->amplitude_uv);

	ret = mt6320_vibr_hw_init(vib);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to initialize vibrator registers\n");

	spin_lock_init(&vib->request_lock);
	mutex_init(&vib->hw_lock);
	INIT_WORK(&vib->play_work, mt6320_vibr_play_work);

	vib->input = devm_input_allocate_device(&pdev->dev);
	if (!vib->input)
		return -ENOMEM;

	vib->input->name = "MT6320 Vibrator";
	vib->input->id.bustype = BUS_HOST;
	vib->input->dev.parent = &pdev->dev;
	vib->input->close = mt6320_vibr_close;
	input_set_drvdata(vib->input, vib);
	input_set_capability(vib->input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(vib->input, vib,
				      mt6320_vibr_play_effect);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to initialize force feedback\n");

	ret = input_register_device(vib->input);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register input device\n");

	platform_set_drvdata(pdev, vib);
	dev_info(&pdev->dev,
		 "registered as FF_RUMBLE, maximum drive voltage %u uV\n",
		 vib->amplitude_uv);

	return 0;
}

static void mt6320_vibr_remove(struct platform_device *pdev)
{
	mt6320_vibr_quiesce(platform_get_drvdata(pdev));
}

static void mt6320_vibr_shutdown(struct platform_device *pdev)
{
	mt6320_vibr_quiesce(platform_get_drvdata(pdev));
}

static const struct of_device_id mt6320_vibr_of_match[] = {
	{ .compatible = "mediatek,mt6320-vibrator" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6320_vibr_of_match);

static struct platform_driver mt6320_vibr_driver = {
	.probe = mt6320_vibr_probe,
	.remove = mt6320_vibr_remove,
	.shutdown = mt6320_vibr_shutdown,
	.driver = {
		.name = "mt6320-vibrator",
		.of_match_table = mt6320_vibr_of_match,
	},
};
module_platform_driver(mt6320_vibr_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6320 PMIC vibrator with force feedback");
MODULE_LICENSE("GPL");
