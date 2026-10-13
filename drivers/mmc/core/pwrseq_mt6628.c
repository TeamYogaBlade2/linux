// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * pwrseq_mt6628.c - power sequence support for the MT6628 combo chip
 *
 * The MT6628 is a MediaTek WLAN/BT/FM/GNSS companion ("combo") chip that
 * attaches to the host over SDIO, with the STP transport used for BT/FM/GNSS.
 * On the boards this driver was written for the chip is not always left in a
 * usable state across a warm reboot or after an aborted suspend, so SDIO
 * enumeration fails.  The chip simply never answers: the host times out on
 * the very first SDIO commands instead of getting a CRC error back.
 *
 * This driver brackets the card with the sequence the chip's vendor DFT
 * (Design For Test) flow uses, which is where the timing below comes from.
 *
 * The MMC core invokes pre_power_on() before enabling the host's card
 * supply and post_power_on() after it has been enabled. Keep MT6628 powered
 * off and held in reset while the host brings the supply up; enable PMU_EN
 * and release RESET only afterwards.
 *
 * This driver controls PMU_EN and RESET only. It does not switch any
 * additional boot-strap pinmux states.
 *
 *      pmu low -> rst low -> DFT_OFF_STABLE_TIME
 *              -> host enables VMMC
 *              -> pmu high -> DFT_ON_STABLE_TIME
 *              -> rst high -> DFT_RST_STABLE_TIME
 *
 * The three delays are named after the downstream DFT_OFF_STABLE_TIME,
 * DFT_ON_STABLE_TIME and DFT_RST_STABLE_TIME constants that specify how long
 * the chip needs to settle after each of those steps.
 *
 * The GPIO numbers are deliberately NOT hardcoded here.  Which MT6589 pins
 * drive a given board's pmu and reset lines is board-specific, so they are
 * taken from the "pmu" and "reset" DT properties and the board DTS decides.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mfd/mt6628-pwrseq.h>
#include <linux/mfd/mt6397/rtc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <linux/mmc/host.h>

#include "pwrseq.h"

/*
 * Settling delays, in milliseconds, for the DFT power sequence.  See the
 * comment at the top of the file for how they map onto the sequence steps.
 */
#define MT6628_DFT_OFF_STABLE_TIME	10	/* pmu/rst asserted low */
#define MT6628_DFT_ON_STABLE_TIME	30	/* supply enabled */
#define MT6628_DFT_RST_STABLE_TIME	30	/* released out of reset */
#define MT6628_DFT_RTC_STABLE_TIME	100	/* RTC 32 kHz output */
#define MT6628_POWER_READY_TIMEOUT_MS	10000

/*
 * The MMC host owns the normal supply reference. This provider takes a
 * second reference while UART STP or another non-MMC consumer is active.
 *
 * power_ready describes completion of the electrical power sequence.
 * stp_ready describes completion of WMT initialization and transport setup.
 * They are deliberately separate: a powered chip is not necessarily ready
 * to accept WLAN firmware download.
 */
struct mt6628_pwrseq {
	struct mmc_pwrseq pwrseq;
	struct gpio_desc *pmu_gpio;
	struct gpio_desc *reset_gpio;
	struct regulator *vmmc;
	struct mutex lock;
	struct completion power_ready;
	struct completion stp_ready;
	unsigned int consumers;
	bool power_ready_state;
	bool power_transition;
	bool skip_power_cycle;
	bool power_off_pending;
	bool vmmc_held;
	bool stp_ready_state;
};

static void mmc_pwrseq_mt6628_pre_power_on(struct mmc_host *host)
{
	struct mt6628_pwrseq *pwrseq =
		container_of(host->pwrseq, struct mt6628_pwrseq, pwrseq);
	unsigned int consumers;

	mutex_lock(&pwrseq->lock);
	consumers = pwrseq->consumers;
	if (consumers) {
		/*
		 * A shared consumer owns the powered combo chip. The MMC host
		 * may be restarting its card interface, but must not reset the
		 * UART/STP side in the middle of an operation.
		 */
		pwrseq->skip_power_cycle = true;
		pwrseq->power_off_pending = false;
		mutex_unlock(&pwrseq->lock);
		dev_warn_ratelimited(pwrseq->pwrseq.dev,
				     "retaining MT6628 power for %u active consumer(s)\n",
				     consumers);
		return;
	}

	pwrseq->skip_power_cycle = false;
	pwrseq->power_ready_state = false;
	pwrseq->power_transition = true;
	reinit_completion(&pwrseq->power_ready);
	mutex_unlock(&pwrseq->lock);

	/*
	 * PMU_EN is active-high, while RESET is active-low. Use logical
	 * values here: 0 turns PMU_EN off, and 1 asserts RESET.
	 */
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 0);
	gpiod_set_value_cansleep(pwrseq->reset_gpio, 1);
	msleep(MT6628_DFT_OFF_STABLE_TIME);

}

static void mmc_pwrseq_mt6628_post_power_on(struct mmc_host *host)
{
	struct mt6628_pwrseq *pwrseq =
		container_of(host->pwrseq, struct mt6628_pwrseq, pwrseq);

	mutex_lock(&pwrseq->lock);
	if (pwrseq->skip_power_cycle) {
		pwrseq->skip_power_cycle = false;
		mutex_unlock(&pwrseq->lock);
		return;
	}
	mutex_unlock(&pwrseq->lock);

	/* VMMC is enabled by the host before this callback is invoked. */
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 1);
	msleep(MT6628_DFT_ON_STABLE_TIME);

	/* Release active-low RESET only after PMU_EN has settled. */
	gpiod_set_value_cansleep(pwrseq->reset_gpio, 0);
	msleep(MT6628_DFT_RST_STABLE_TIME);

	mutex_lock(&pwrseq->lock);
	pwrseq->power_transition = false;
	pwrseq->power_ready_state = true;
	complete_all(&pwrseq->power_ready);
	mutex_unlock(&pwrseq->lock);
}

static void mmc_pwrseq_mt6628_power_off(struct mmc_host *host)
{
	struct mt6628_pwrseq *pwrseq =
		container_of(host->pwrseq, struct mt6628_pwrseq, pwrseq);
	unsigned int consumers;

	mutex_lock(&pwrseq->lock);
	consumers = pwrseq->consumers;
	if (consumers) {
		/*
		 * Keep PMU_EN, RESET and the extra VMMC regulator reference
		 * until the last consumer releases the combo chip. This callback
		 * cannot veto the MMC core's supply change, so the additional
		 * regulator reference is essential here.
		 */
		pwrseq->power_off_pending = true;
		mutex_unlock(&pwrseq->lock);
		dev_warn_ratelimited(pwrseq->pwrseq.dev,
				     "deferring MT6628 power-off for %u active consumer(s)\n",
				     consumers);
		return;
	}

	pwrseq->power_ready_state = false;
	pwrseq->power_transition = true;
	reinit_completion(&pwrseq->power_ready);
	mutex_unlock(&pwrseq->lock);

	/* Assert active-low RESET before removing power. */
	gpiod_set_value_cansleep(pwrseq->reset_gpio, 1);
	msleep(MT6628_DFT_OFF_STABLE_TIME);
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 0);

	mutex_lock(&pwrseq->lock);
	pwrseq->power_transition = false;
	mutex_unlock(&pwrseq->lock);
}

static void mmc_pwrseq_mt6628_consumer_put(void *data)
{
	struct mt6628_pwrseq *pwrseq = data;
	struct device *provider_dev = pwrseq->pwrseq.dev;
	struct module *owner = pwrseq->pwrseq.owner;
	bool power_off;
	int ret;

	mutex_lock(&pwrseq->lock);
	if (WARN_ON(!pwrseq->consumers)) {
		mutex_unlock(&pwrseq->lock);
		goto out_put;
	}

	if (--pwrseq->consumers) {
		mutex_unlock(&pwrseq->lock);
		goto out_put;
	}

	/*
	 * Serialize the last release with a new acquisition. Otherwise a new
	 * consumer could acquire a lease just before regulator_disable().
	 */
	pwrseq->power_transition = true;
	reinit_completion(&pwrseq->power_ready);
	power_off = pwrseq->power_off_pending;
	pwrseq->power_off_pending = false;

	if (power_off) {
		pwrseq->power_ready_state = false;
		gpiod_set_value_cansleep(pwrseq->reset_gpio, 1);
		msleep(MT6628_DFT_OFF_STABLE_TIME);
		gpiod_set_value_cansleep(pwrseq->pmu_gpio, 0);
	}

	if (pwrseq->vmmc_held) {
		ret = regulator_disable(pwrseq->vmmc);
		if (!ret)
			pwrseq->vmmc_held = false;
		else
			dev_warn(pwrseq->pwrseq.dev,
				 "failed to release retained VMMC: %d\n", ret);
	}

	pwrseq->power_transition = false;
	if (pwrseq->power_ready_state)
		complete_all(&pwrseq->power_ready);
	mutex_unlock(&pwrseq->lock);

out_put:
	/*
	 * The regulator/GPIO provider must remain alive until the consumer
	 * drops its lease. Save the module and device references above before
	 * releasing them here.
	 */
	put_device(provider_dev);
	module_put(owner);
}

struct mt6628_pwrseq *devm_mt6628_pwrseq_get(struct device *dev)
{
	struct device_node *np;
	struct device_link *link;
	struct platform_device *pdev;
	struct mt6628_pwrseq *pwrseq;
	struct module *owner;
	unsigned long deadline, remaining;
	int ret;

	if (!dev->of_node)
		return NULL;

	np = of_parse_phandle(dev->of_node, "mediatek,combo-pwrseq", 0);
	if (!np)
		return NULL;

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);

	if (!pdev->dev.driver) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	/*
	 * The lease below stores a pointer into the provider's devm state.
	 * Order unbind so this consumer releases its lease before the
	 * supplier's remove callback can free that state.
	 */
	link = device_link_add(dev, &pdev->dev,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(&pdev->dev);
		return ERR_PTR(-ENOMEM);
	}

	owner = pdev->dev.driver->owner;
	if (!try_module_get(owner)) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	pwrseq = platform_get_drvdata(pdev);
	if (!pwrseq) {
		ret = -EPROBE_DEFER;
		goto err_put;
	}

	/*
	 * Wait for the MMC power sequence to finish, not for an arbitrary
	 * interval after UART probe. Recheck the state under the same lock
	 * used by pre/post_power_on so that a power-off cannot race acquisition.
	 */
	deadline = jiffies +
		   msecs_to_jiffies(MT6628_POWER_READY_TIMEOUT_MS);
	for (;;) {
		mutex_lock(&pwrseq->lock);
		if (pwrseq->power_ready_state &&
		    !pwrseq->power_transition) {
			/*
			 * Hold a second reference to the actual VMMC supply.
			 * The MMC host may drop its own reference while UART STP
			 * or WLAN is still using the combo chip.
			 */
			if (!pwrseq->consumers && !pwrseq->vmmc_held) {
				ret = regulator_enable(pwrseq->vmmc);
				if (ret) {
					mutex_unlock(&pwrseq->lock);
					goto err_put;
				}
				pwrseq->vmmc_held = true;
			}
			pwrseq->consumers++;
			mutex_unlock(&pwrseq->lock);
			break;
		}
		mutex_unlock(&pwrseq->lock);

		if (!time_before(jiffies, deadline)) {
			ret = -EPROBE_DEFER;
			goto err_put;
		}

		remaining = deadline - jiffies;
		if (!wait_for_completion_timeout(&pwrseq->power_ready,
						 remaining)) {
			ret = -EPROBE_DEFER;
			goto err_put;
		}
	}

	ret = devm_add_action_or_reset(dev,
				       mmc_pwrseq_mt6628_consumer_put,
				       pwrseq);
	if (ret)
		return ERR_PTR(ret);

	return pwrseq;

err_put:
	put_device(&pdev->dev);
	module_put(owner);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(devm_mt6628_pwrseq_get);

void mt6628_pwrseq_set_stp_ready(struct mt6628_pwrseq *pwrseq, bool ready)
{
	if (!pwrseq)
		return;

	mutex_lock(&pwrseq->lock);
	if (pwrseq->stp_ready_state != ready) {
		pwrseq->stp_ready_state = ready;
		if (ready)
			complete_all(&pwrseq->stp_ready);
		else
			reinit_completion(&pwrseq->stp_ready);
	}
	mutex_unlock(&pwrseq->lock);
}
EXPORT_SYMBOL_GPL(mt6628_pwrseq_set_stp_ready);

bool mt6628_pwrseq_is_stp_ready(struct mt6628_pwrseq *pwrseq)
{
	bool ready;

	if (!pwrseq)
		return false;

	mutex_lock(&pwrseq->lock);
	ready = pwrseq->stp_ready_state;
	mutex_unlock(&pwrseq->lock);
	return ready;
}
EXPORT_SYMBOL_GPL(mt6628_pwrseq_is_stp_ready);

static const struct mmc_pwrseq_ops mmc_pwrseq_mt6628_ops = {
	.pre_power_on = mmc_pwrseq_mt6628_pre_power_on,
	.post_power_on = mmc_pwrseq_mt6628_post_power_on,
	.power_off = mmc_pwrseq_mt6628_power_off,
};

static int mmc_pwrseq_mt6628_probe(struct platform_device *pdev)
{
	struct mt6628_pwrseq *pwrseq;
	struct device *dev = &pdev->dev;
	struct device_node *rtc_np;
	struct platform_device *rtc_pdev;
	int ret;

	pwrseq = devm_kzalloc(dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	pwrseq->vmmc = devm_regulator_get(dev, "vmmc");
	if (IS_ERR(pwrseq->vmmc))
		return dev_err_probe(dev, PTR_ERR(pwrseq->vmmc),
				     "can't get VMMC supply\n");

	mutex_init(&pwrseq->lock);
	init_completion(&pwrseq->power_ready);
	init_completion(&pwrseq->stp_ready);

	/*
	 * The GPIO descriptors use logical values. PMU_EN is active-high
	 * and starts low; RESET is active-low and must start asserted,
	 * which requires logical 1 (physical low).
	 */
	pwrseq->pmu_gpio = devm_gpiod_get(dev, "pmu", GPIOD_OUT_LOW);
	if (IS_ERR(pwrseq->pmu_gpio))
		return dev_err_probe(dev, PTR_ERR(pwrseq->pmu_gpio),
				     "can't get pmu gpio\n");

	pwrseq->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pwrseq->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pwrseq->reset_gpio),
				     "can't get reset gpio\n");

	/*
	 * The vendor MT6589 sequence explicitly enables RTC_GPIO 32 kHz
	 * for RTC_GPIO_USER_GPS and waits for it to stabilize before
	 * powering up the combo chip. Defer until the RTC provider is
	 * usable so the first SDIO enumeration cannot race RTC setup.
	 */
	rtc_np = of_parse_phandle(dev->of_node, "mediatek,rtc", 0);
	if (!rtc_np)
		return dev_err_probe(dev, -EINVAL,
				     "missing mediatek,rtc phandle\n");

	rtc_pdev = of_find_device_by_node(rtc_np);
	of_node_put(rtc_np);
	if (!rtc_pdev)
		return -EPROBE_DEFER;

	ret = mt6397_rtc_enable_32k_gpio(&rtc_pdev->dev);
	put_device(&rtc_pdev->dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable RTC 32 kHz output\n");

	msleep(MT6628_DFT_RTC_STABLE_TIME);

	pwrseq->pwrseq.dev = dev;
	pwrseq->pwrseq.ops = &mmc_pwrseq_mt6628_ops;
	pwrseq->pwrseq.owner = THIS_MODULE;
	platform_set_drvdata(pdev, pwrseq);

	ret = mmc_pwrseq_register(&pwrseq->pwrseq);
	if (ret)
		platform_set_drvdata(pdev, NULL);

	return ret;
}

static void mmc_pwrseq_mt6628_remove(struct platform_device *pdev)
{
	struct mt6628_pwrseq *pwrseq = platform_get_drvdata(pdev);

	mmc_pwrseq_unregister(&pwrseq->pwrseq);
}

static const struct of_device_id mmc_pwrseq_mt6628_of_match[] = {
	{ .compatible = "mediatek,mt6628-pwrseq" },
	{/* sentinel */},
};
MODULE_DEVICE_TABLE(of, mmc_pwrseq_mt6628_of_match);

static struct platform_driver mmc_pwrseq_mt6628_driver = {
	.probe = mmc_pwrseq_mt6628_probe,
	.remove = mmc_pwrseq_mt6628_remove,
	.driver = {
		.name = "pwrseq_mt6628",
		.of_match_table = mmc_pwrseq_mt6628_of_match,
	},
};

module_platform_driver(mmc_pwrseq_mt6628_driver);
MODULE_DESCRIPTION("Power sequence support for MT6628 combo chip");
MODULE_LICENSE("GPL");
