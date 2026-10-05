// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6320 PMIC vibrator (haptics) driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6320 drives a linear resonant actuator through an integrated LDO
 * ("VIBR"). This part has no separate amplifier enable pin and no PWM duty
 * input: the hardware only accepts
 *
 *   - an on/off control (RG_VIBR_EN), which gates the LDO, and
 *   - an output voltage (RG_VIBR_VOSEL), which on an LRA is what userspace
 *     perceives as strength / amplitude.
 *
 * Both fields live in the DIGLDO (top configuration) block:
 *
 *   DIGLDO_CON39 (0x0466)  RG_VIBR_EN         [15]
 *                          RG_VIBR_STBTD      [13:12]
 *                          QI_VIBR_MODE       [7]      (read-only)
 *                          VIBR_SRCLK_MODE_SEL[6:4]
 *                          VIBR_THER_SHEN_EN  [2]
 *                          VIBR_LP_MODE_SET   [1]
 *                          VIBR_LP_SEL        [0]
 *
 *   DIGLDO_CON40 (0x0468)  RG_VIBR_CAL        [11:8]
 *                          RG_VIBR_VOSEL      [7:5]
 *                          RG_VIBR_STB_SEL    [4]
 *                          RG_VIBR_OCFB       [2]
 *                          RG_VIBR_NDIS_EN    [0]
 *
 * There is therefore no separate "mute" path: muting is exactly what dropping
 * RG_VIBR_EN does, so the LED classdev's brightness 0 doubles as the mute.
 *
 * Interface surface: an LED class device only.  This driver deliberately
 * registers no input device and no FF_RUMBLE / EV_FF node, so there is no
 * "haptic" event device and the usual on/off + "strong/medium/weak" ff effect
 * set does not exist here.  Effects instead go through the classdev's pattern
 * API (brightness plus delay-on/delay-off), and the driver drives the actuator
 * itself from its hrtimer, which is also how the bounded one-shot pulse the
 * BSP clamp implements is produced.
 *
 * That is a deliberate omission rather than a gap in the plumbing: FF_RUMBLE
 * would mean inventing a strength scale and an effect vocabulary on top of the
 * 8-step VOSEL table, and no board in this tree has an haptics consumer
 * expecting one.  Adding an evdev node later is a self-contained change.
 *
 * Register / bit provenance, all from the vendor tree for this SoC.
 *
 * Addresses:
 *   aquaris-5/mediatek/platform/mt6589/kernel/core/include/mach/upmu_hw.h:339
 *       #define DIGLDO_CON39 0x0466
 *   aquaris-5/.../mach/upmu_hw.h:340
 *       #define DIGLDO_CON40 0x0468
 *
 * Bit positions (upmu_hw.h defines only masks, never the owning register):
 *   upmu_hw.h:3274-3275  PMIC_RG_VIBR_EN_MASK 0x1 / _SHIFT 15
 *   upmu_hw.h:3276-3277  PMIC_RG_VIBR_STBTD_MASK 0x3 / _SHIFT 12
 *   upmu_hw.h:3278-3279  PMIC_QI_VIBR_MODE_MASK 0x1 / _SHIFT 7
 *   upmu_hw.h:3280-3281  PMIC_VIBR_SRCLK_MODE_SEL_MASK 0x7 / _SHIFT 4
 *   upmu_hw.h:3282-3283  PMIC_VIBR_THER_SHEN_EN_MASK 0x1 / _SHIFT 2
 *   upmu_hw.h:3284-3285  PMIC_VIBR_LP_MODE_SET_MASK 0x1 / _SHIFT 1
 *   upmu_hw.h:3286-3287  PMIC_VIBR_LP_SEL_MASK 0x1 / _SHIFT 0
 *   upmu_hw.h:3288-3289  PMIC_RG_VIBR_CAL_MASK 0xF / _SHIFT 8
 *   upmu_hw.h:3290-3291  PMIC_RG_VIBR_VOSEL_MASK 0x7 / _SHIFT 5
 *   upmu_hw.h:3292-3293  PMIC_RG_VIBR_STB_SEL_MASK 0x1 / _SHIFT 4
 *   upmu_hw.h:3294-3295  PMIC_RG_VIBR_OCFB_MASK 0x1 / _SHIFT 2
 *   upmu_hw.h:3296-3297  PMIC_RG_VIBR_NDIS_EN_MASK 0x1 / _SHIFT 0
 *
 * Which register each field belongs to:
 *   aquaris-5/.../kernel/drivers/power/upmu_common.c:18991-19150 pairs every
 *   upmu_set_rg_vibr_* accessor with its register, e.g.
 *       upmu_set_rg_vibr_en()    -> DIGLDO_CON39 (upmu_common.c:18991-19002)
 *       upmu_set_rg_vibr_vosel() -> DIGLDO_CON40 (upmu_common.c:19098-19109)
 *   The LK carries the identical table at lk/mt_pmic.c:24629-25190.
 *
 * The authoritative power-on init, which hardcodes the same two offsets:
 *   aquaris-5/.../kernel/drivers/power/pmic_mt6320.c:3879-3880
 *       ret = pmic_config_interface(0x466,0x1,0x1,2); // [2:2]: VIBR_THER_SHEN_EN;
 *       ret = pmic_config_interface(0x468,0x1,0x1,4); // [4:4]: RG_VIBR_STB_SEL;
 *
 * VOSEL -> millivolts, from aquaris-5/.../power/pmic_mt6320.c:2030-2077
 * (dct_pmic_VIBR_sel) cross-checked against the readback path at
 * pmic_mt6320.c:3491-3518 (show_LDO_VIBR_VOLTAGE):
 *       0=1200 1=1300 2=1500 3=1800 4=2500 5=2800 6=3000 7=3300
 *
 * Board defaults, from
 * aquaris-5/mediatek/custom/eastaeon89_wet_td/kernel/vibrator/cust_vibrator.c:4-9
 *       .vib_timer = 25, .vib_limit = 9
 * and the clamp they drive in
 * aquaris-5/mediatek/kernel/drivers/vibrator/vibrator_drv.c:149-157.
 *
 * The VIBR LDO has no dedicated clock gate, and the vendor driver touches it
 * with no clock or runtime-PM handshake at all (vibrator.c:44-48,
 * vibr_Enable_HW() is a bare dct_pmic_VIBR_enable()).  There is accordingly no
 * clock or runtime-PM prepare/unprepare around the register access here.
 *
 * Note on RG_VIBR_EN: the MT6320 regulator driver already owns that bit as the
 * enable_reg of its "vibr" LDO (mt6320-regulator.c:648-650, whose
 * regulator_enable_regmap writes DIGLDO_CON39 BIT(15) - the very bit
 * dct_pmic_VIBR_enable() pokes).  Driving it from both places would make the
 * regulator's refcount lie, so this driver takes the regulator for all on/off
 * transitions and writes only VOSEL itself.
 */

#include <linux/hrtimer.h>
#include <linux/leds.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#include <linux/mfd/mt6397/core.h>

#define MT6320_VIBR_REG_CTRL		0x0466	/* DIGLDO_CON39 */
#define MT6320_VIBR_REG_VSEL		0x0468	/* DIGLDO_CON40 */

/* DIGLDO_CON39 */
#define MT6320_VIBR_STBTD_MASK		GENMASK(13, 12)
#define MT6320_VIBR_SRCLK_MODE_SEL_MASK	GENMASK(6, 4)
#define MT6320_VIBR_THER_SHEN_EN_BIT	2
#define MT6320_VIBR_LP_MODE_SET_BIT	1
#define MT6320_VIBR_LP_SEL_BIT		0

/* DIGLDO_CON40 */
#define MT6320_VIBR_VOSEL_MASK		GENMASK(7, 5)
#define MT6320_VIBR_VOSEL_SHIFT		5
#define MT6320_VIBR_STB_SEL_BIT		4
#define MT6320_VIBR_NDIS_EN_BIT		0

/* Ascending VOSEL settings; the index into this table *is* the VOSEL field
 * value.  Mirrors dct_pmic_VIBR_sel() and ldo_volt_table3 in mt6320-regulator.c.
 */
static const unsigned int mt6320_vibr_vsel_mv[] = {
	1200000, 1300000, 1500000, 1800000, 2500000, 2800000, 3000000, 3300000,
};

/* dct_pmic_VIBR_sel(VOL_DEFAULT) -> VOSEL 5 -> 2800 mV. */
#define MT6320_VIBR_VSEL_DEFAULT_MV	2800000

/* cust_vibrator.c: .vib_timer = 25 ms, .vib_limit = 9. */
#define MT6320_VIBR_DEFAULT_DURATION_MS	25
#define MT6320_VIBR_LIMIT_MS		9
#define MT6320_VIBR_MAX_DURATION_MS	15000

struct mt6320_vibrator {
	struct device *dev;
	struct regulator *vibr_reg;
	struct regmap *regmap;
	struct led_classdev cdev;

	struct mutex lock;
	struct hrtimer timer;
	struct work_struct poweroff_work;

	/*
	 * @running is also the regulator reference count, bounded at one: this
	 * driver only ever calls regulator_enable() when it is clear and
	 * regulator_disable() when it is set.  Two concurrent pulse requests
	 * cannot take two references.
	 */
	bool running;
	bool shutdown;
	u32 amplitude;		/* microvolts */
	u32 duration_ms;
};

/* Map a requested voltage onto the nearest VOSEL that is at or below it. */
static unsigned int mt6320_vibr_map_vosel(u32 mv)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6320_vibr_vsel_mv); i++)
		if (mv <= mt6320_vibr_vsel_mv[i])
			return i;

	return ARRAY_SIZE(mt6320_vibr_vsel_mv) - 1;
}

static int mt6320_vibr_set_amplitude(struct mt6320_vibrator *vib)
{
	unsigned int vosel = mt6320_vibr_map_vosel(vib->amplitude);

	return regmap_update_bits(vib->regmap, MT6320_VIBR_REG_VSEL,
				  MT6320_VIBR_VOSEL_MASK,
				  vosel << MT6320_VIBR_VOSEL_SHIFT);
}

/* Program the LDO the way the vendor power-on init does. */
static int mt6320_vibr_hw_init(struct mt6320_vibrator *vib)
{
	int ret;

	/* pmic_mt6320.c:3879 - thermal shutdown enable, which the vendor sets
	 * unconditionally at power-on; it protects the actuator against a
	 * stalled LRA.
	 */
	ret = regmap_update_bits(vib->regmap, MT6320_VIBR_REG_CTRL,
				 BIT(MT6320_VIBR_THER_SHEN_EN_BIT),
				 BIT(MT6320_VIBR_THER_SHEN_EN_BIT));
	if (ret)
		return ret;

	/* pmic_mt6320.c:3880 - standby source select. */
	return regmap_update_bits(vib->regmap, MT6320_VIBR_REG_VSEL,
				  BIT(MT6320_VIBR_STB_SEL_BIT),
				  BIT(MT6320_VIBR_STB_SEL_BIT));
}

/* Pull the rail up or down. Sleeps; call from process context only. */
static int mt6320_vibr_hw_set_power(struct mt6320_vibrator *vib, bool on)
{
	if (!vib->vibr_reg)
		return 0;

	return on ? regulator_enable(vib->vibr_reg) :
		    regulator_disable(vib->vibr_reg);
}

static enum hrtimer_restart mt6320_vibr_timer_func(struct hrtimer *timer)
{
	struct mt6320_vibrator *vib =
		container_of(timer, struct mt6320_vibrator, timer);

	/* Softirq context, so this must not sleep: drop the rail from a worker
	 * instead.  The generation is guarded by "running", which
	 * mt6320_vibr_start() sets again for a new pulse, so a stale poweroff
	 * is a no-op.
	 */
	queue_work(system_wq, &vib->poweroff_work);

	return HRTIMER_NORESTART;
}

/* Power the LRA down once the pulse has elapsed. Process context. */
static void mt6320_vibr_poweroff_work(struct work_struct *work)
{
	struct mt6320_vibrator *vib =
		container_of(work, struct mt6320_vibrator, poweroff_work);
	bool poweroff;

	mutex_lock(&vib->lock);
	/* A newer pulse has started: leave its rail alone. */
	poweroff = !vib->shutdown && vib->running;
	vib->running = false;
	mutex_unlock(&vib->lock);

	if (poweroff && mt6320_vibr_hw_set_power(vib, false))
		dev_err(vib->dev, "failed to disable vibrator\n");
}

/* Replay the board's own clamp on an arbitrary userspace duration: over
 * MT6320_VIBR_LIMIT_MS but under the board's 25 ms vib_timer, round up to
 * 25 ms; anything past 15 s is capped (vibrator_drv.c:149-157).
 */
static u32 mt6320_vibr_clamp_duration(u32 ms)
{
	if (ms > MT6320_VIBR_LIMIT_MS && ms < MT6320_VIBR_DEFAULT_DURATION_MS)
		ms = MT6320_VIBR_DEFAULT_DURATION_MS;

	return min_t(u32, ms, MT6320_VIBR_MAX_DURATION_MS);
}

/* Stop the LRA and drop the rail.  Process context (LED sysfs write, LED
 * pattern engine), so sleeping is fine.
 */
static int mt6320_vibr_stop(struct mt6320_vibrator *vib)
{
	bool poweroff;
	int ret = 0;

	/*
	 * hrtimer_cancel() waits for a running callback, and that callback
	 * queues poweroff_work which takes the lock - so cancel with the lock
	 * dropped, or the two can deadlock.
	 */
	hrtimer_cancel(&vib->timer);

	mutex_lock(&vib->lock);
	poweroff = !vib->shutdown && vib->running;
	vib->running = false;
	mutex_unlock(&vib->lock);

	if (poweroff && mt6320_vibr_hw_set_power(vib, false))
		ret = -EIO;

	return ret;
}

static int mt6320_vibr_start(struct mt6320_vibrator *vib, u32 duration_ms)
{
	u32 ms;
	int ret;

	ms = mt6320_vibr_clamp_duration(duration_ms);

	/* Cancel outside the lock; the timer callback queues work. */
	hrtimer_cancel(&vib->timer);
	/* Retire any poweroff already queued by the previous pulse. */
	cancel_work_sync(&vib->poweroff_work);

	mutex_lock(&vib->lock);
	if (vib->shutdown) {
		mutex_unlock(&vib->lock);
		return 0;
	}

	/* Voltage must be settled before the rail is enabled. */
	ret = mt6320_vibr_set_amplitude(vib);
	if (ret)
		goto out_unlock;

	/*
	 * Only enable a rail this driver does not already hold.  @running is
	 * exactly the "this call owns one regulator_enable()" flag: it is set
	 * here and cleared by whichever path disables the rail again.  Calling
	 * regulator_enable() on a pulse that is already in flight would take a
	 * second reference that nothing ever puts, because the matching stop
	 * only ever disables once - two "1" writes to
	 * /sys/class/leds/.../brightness followed by one "0" leave the LDO
	 * energised with @running already false, so no later stop can drain it.
	 */
	if (!vib->running) {
		ret = mt6320_vibr_hw_set_power(vib, true);
		if (ret)
			goto out_unlock;

		vib->running = true;
	}

	vib->duration_ms = ms;
	hrtimer_start(&vib->timer,
		      ktime_set(ms / 1000, (ms % 1000) * 1000000LL),
		      HRTIMER_MODE_REL);
out_unlock:
	mutex_unlock(&vib->lock);

	return ret;
}

/* LED classdev: 0 is off, >0 plays the latched pattern. */
static void mt6320_vibr_led_brightness_set(struct led_classdev *cdev,
					   enum led_brightness brightness)
{
	struct mt6320_vibrator *vib =
		container_of(cdev, struct mt6320_vibrator, cdev);
	u32 duration;

	if (!brightness) {
		mt6320_vibr_stop(vib);
		return;
	}

	mutex_lock(&vib->lock);
	duration = vib->duration_ms;
	mutex_unlock(&vib->lock);

	mt6320_vibr_start(vib, duration);
}

static enum led_brightness
mt6320_vibr_led_brightness_get(struct led_classdev *cdev)
{
	struct mt6320_vibrator *vib =
		container_of(cdev, struct mt6320_vibrator, cdev);
	bool running;

	mutex_lock(&vib->lock);
	running = vib->running;
	mutex_unlock(&vib->lock);

	return running ? cdev->max_brightness : 0;
}

/*
 * LED pattern support.
 *
 * The generic haptics engine describes a pattern as a list of intervals whose
 * delta_t is a duration in milliseconds and whose brightness is a percentage.
 * This LDO can only be driven fully on or fully off for a single interval,
 * and the driver has no sequencer to alternate gaps and pulses. Rather than
 * silently play only the final interval, accept exactly the shape the
 * hardware can express - one interval at full amplitude - and reject
 * anything else so callers learn the pattern was not honoured.
 */
static int mt6320_vibr_led_pattern_set(struct led_classdev *cdev,
				       struct led_pattern *pattern, u32 len,
				       int repeat)
{
	struct mt6320_vibrator *vib =
		container_of(cdev, struct mt6320_vibrator, cdev);

	/* A repeating pattern cannot be expressed by a one-shot pulse. */
	if (repeat)
		return -EOPNOTSUPP;

	if (len != 1 || pattern[0].brightness != 100)
		return -EINVAL;

	if (!pattern[0].delta_t)
		return mt6320_vibr_stop(vib);

	return mt6320_vibr_start(vib, pattern[0].delta_t);
}

static int mt6320_vibr_led_pattern_clear(struct led_classdev *cdev)
{
	return mt6320_vibr_stop(container_of(cdev, struct mt6320_vibrator, cdev));
}

static int mt6320_vibr_probe(struct platform_device *pdev)
{
	struct mt6320_vibrator *vib;
	struct led_classdev *cdev;
	struct regulator *rdev;
	u32 amplitude;
	int ret;

	vib = devm_kzalloc(&pdev->dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;

	vib->dev = &pdev->dev;
	vib->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!vib->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "no regmap available from parent\n");

	mutex_init(&vib->lock);
	hrtimer_setup(&vib->timer, mt6320_vibr_timer_func, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	INIT_WORK(&vib->poweroff_work, mt6320_vibr_poweroff_work);
	vib->duration_ms = MT6320_VIBR_DEFAULT_DURATION_MS;

	amplitude = MT6320_VIBR_VSEL_DEFAULT_MV;
	if (of_property_read_u32(pdev->dev.of_node, "mediatek,vibr-amplitude-uv",
				 &amplitude))
		amplitude = MT6320_VIBR_VSEL_DEFAULT_MV;

	/* Clamp into the VOSEL range rather than overflowing the field. */
	if (amplitude < mt6320_vibr_vsel_mv[0])
		amplitude = mt6320_vibr_vsel_mv[0];
	if (amplitude > mt6320_vibr_vsel_mv[ARRAY_SIZE(mt6320_vibr_vsel_mv) - 1])
		amplitude =
			mt6320_vibr_vsel_mv[ARRAY_SIZE(mt6320_vibr_vsel_mv) - 1];
	vib->amplitude = amplitude;

	rdev = devm_regulator_get_optional(&pdev->dev, "vibr-supply");
	if (IS_ERR(rdev))
		return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
				     "failed to get vibr regulator\n");
	vib->vibr_reg = rdev;

	ret = mt6320_vibr_hw_init(vib);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to program vibrator defaults\n");

	cdev = &vib->cdev;
	cdev->max_brightness = 1;
	cdev->brightness_set = mt6320_vibr_led_brightness_set;
	cdev->brightness_get = mt6320_vibr_led_brightness_get;
	cdev->pattern_set = mt6320_vibr_led_pattern_set;
	cdev->pattern_clear = mt6320_vibr_led_pattern_clear;

	ret = devm_led_classdev_register(&pdev->dev, cdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register LED classdev\n");
	platform_set_drvdata(pdev, vib);
	dev_info(&pdev->dev, "registered, amplitude %u uV (VOSEL %u)\n",
		 vib->amplitude, mt6320_vibr_map_vosel(vib->amplitude));

	return 0;
}

static void mt6320_vibr_remove(struct platform_device *pdev)
{
	struct mt6320_vibrator *vib = platform_get_drvdata(pdev);
	bool poweroff;

	/* Latch shutdown first so no new pulse can start. */
	mutex_lock(&vib->lock);
	vib->shutdown = true;
	poweroff = vib->running;
	vib->running = false;
	mutex_unlock(&vib->lock);

	hrtimer_cancel(&vib->timer);
	cancel_work_sync(&vib->poweroff_work);

	/*
	 * Only give back a reference this driver actually took.  The LED core
	 * already runs brightness_set(LED_OFF) from led_classdev_unregister(),
	 * which is what stops a live pulse and clears @running; an unconditional
	 * regulator_disable() here would then be one more disable than enables,
	 * tripping WARN_ON(enable_count == 0) in the regulator core and
	 * returning -EIO.
	 */
	if (poweroff && mt6320_vibr_hw_set_power(vib, false))
		dev_err(&pdev->dev, "failed to disable vibrator\n");
}

static void mt6320_vibr_shutdown(struct platform_device *pdev)
{
	struct mt6320_vibrator *vib = platform_get_drvdata(pdev);
	bool poweroff;

	mutex_lock(&vib->lock);
	vib->shutdown = true;
	poweroff = vib->running;
	vib->running = false;
	mutex_unlock(&vib->lock);

	hrtimer_cancel(&vib->timer);
	cancel_work_sync(&vib->poweroff_work);

	/* Leave the rail down for good: mt6320_vibr_stop() would refuse once
	 * shutdown is latched, so cut it directly.  As in remove(), only a
	 * reference that is still held is given back.
	 */
	if (poweroff && mt6320_vibr_hw_set_power(vib, false))
		dev_err(&pdev->dev, "failed to disable vibrator\n");
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
	},
};

module_platform_driver(mt6320_vibr_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("Vibrator driver for MediaTek MT6320 PMIC");
MODULE_LICENSE("GPL v2");
