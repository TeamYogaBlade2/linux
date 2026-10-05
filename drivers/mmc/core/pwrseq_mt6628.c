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
 * The chip is released out of reset by taking its reset line low while the
 * supply enable ("pmu") line is asserted low, then raising reset.  While held
 * in reset the chip samples its boot-mode strap pins, so they are moved to
 * their test/"strap" pinmux for the duration of the sequence and put back to
 * the board default afterwards:
 *
 *	strap pinmux	-> pmu low  -> rst low  -> DFT_OFF_STABLE_TIME
 *			-> pmu high -> DFT_ON_STABLE_TIME
 *			-> rst high -> DFT_RST_STABLE_TIME
 *			-> restore default pinmux
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
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
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

struct mmc_pwrseq_mt6628 {
	struct mmc_pwrseq pwrseq;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_strap;
	struct gpio_desc *pmu_gpio;
	struct gpio_desc *reset_gpio;
};

static void mmc_pwrseq_mt6628_pre_power_on(struct mmc_host *host)
{
	struct mmc_pwrseq_mt6628 *pwrseq =
		container_of(host->pwrseq, struct mmc_pwrseq_mt6628, pwrseq);

	/*
	 * Put the strap pins in their DFT pinmux before releasing reset, so
	 * the chip latches its boot mode while it is still held there.
	 */
	if (pwrseq->pins_strap)
		pinctrl_select_state(pwrseq->pinctrl, pwrseq->pins_strap);

	/* Hold the chip unpowered and in reset long enough to settle. */
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 0);
	gpiod_set_value_cansleep(pwrseq->reset_gpio, 0);
	msleep(MT6628_DFT_OFF_STABLE_TIME);

	/* Bring the supply up, then release reset. */
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 1);
	msleep(MT6628_DFT_ON_STABLE_TIME);

	gpiod_set_value_cansleep(pwrseq->reset_gpio, 1);
	msleep(MT6628_DFT_RST_STABLE_TIME);

	/* Hand the pins back to the board's own pinmux. */
	if (pwrseq->pins_default)
		pinctrl_select_state(pwrseq->pinctrl, pwrseq->pins_default);
}

static void mmc_pwrseq_mt6628_power_off(struct mmc_host *host)
{
	struct mmc_pwrseq_mt6628 *pwrseq =
		container_of(host->pwrseq, struct mmc_pwrseq_mt6628, pwrseq);

	/* Reset first, then drop the supply, so the chip never runs unreset. */
	gpiod_set_value_cansleep(pwrseq->reset_gpio, 0);
	msleep(MT6628_DFT_OFF_STABLE_TIME);
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 0);
}

static const struct mmc_pwrseq_ops mmc_pwrseq_mt6628_ops = {
	.pre_power_on = mmc_pwrseq_mt6628_pre_power_on,
	.power_off = mmc_pwrseq_mt6628_power_off,
};

static int mmc_pwrseq_mt6628_probe(struct platform_device *pdev)
{
	struct mmc_pwrseq_mt6628 *pwrseq;
	struct device *dev = &pdev->dev;

	pwrseq = devm_kzalloc(dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

	pwrseq->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pwrseq->pinctrl))
		return dev_err_probe(dev, PTR_ERR(pwrseq->pinctrl),
				     "cannot get pinctrl\n");

	/*
	 * "default" restores the pins to whatever the board uses once the
	 * chip is running.  "strap" is the pinmux that makes the boot-mode
	 * strap pins readable while the chip is held in reset.  A board that
	 * hardwires the straps needs neither state, so a missing lookup is
	 * not fatal; we simply skip the select/restore around the sequence.
	 */
	pwrseq->pins_default = pinctrl_lookup_state(pwrseq->pinctrl,
						    "default");
	if (IS_ERR(pwrseq->pins_default))
		pwrseq->pins_default = NULL;

	pwrseq->pins_strap = pinctrl_lookup_state(pwrseq->pinctrl, "strap");
	if (IS_ERR(pwrseq->pins_strap))
		pwrseq->pins_strap = NULL;

	/*
	 * Both lines are fetched asserted-low, so the chip starts held in
	 * reset and unpowered.  devm_gpiod_get() applies that initial value
	 * as a side effect, which is what we want even before pre_power_on()
	 * runs.
	 */
	pwrseq->pmu_gpio = devm_gpiod_get(dev, "pmu", GPIOD_OUT_LOW);
	if (IS_ERR(pwrseq->pmu_gpio))
		return dev_err_probe(dev, PTR_ERR(pwrseq->pmu_gpio),
				     "can't get pmu gpio\n");

	pwrseq->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pwrseq->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(pwrseq->reset_gpio),
				     "can't get reset gpio\n");

	pwrseq->pwrseq.dev = dev;
	pwrseq->pwrseq.ops = &mmc_pwrseq_mt6628_ops;
	pwrseq->pwrseq.owner = THIS_MODULE;
	platform_set_drvdata(pdev, pwrseq);

	return mmc_pwrseq_register(&pwrseq->pwrseq);
}

static void mmc_pwrseq_mt6628_remove(struct platform_device *pdev)
{
	struct mmc_pwrseq_mt6628 *pwrseq = platform_get_drvdata(pdev);

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
