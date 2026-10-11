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
#include <linux/mfd/mt6397/rtc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
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
#define MT6628_DFT_RTC_STABLE_TIME	100	/* RTC 32 kHz output */

struct mmc_pwrseq_mt6628 {
	struct mmc_pwrseq pwrseq;
	struct gpio_desc *pmu_gpio;
	struct gpio_desc *reset_gpio;
};

static void mmc_pwrseq_mt6628_pre_power_on(struct mmc_host *host)
{
	struct mmc_pwrseq_mt6628 *pwrseq =
		container_of(host->pwrseq, struct mmc_pwrseq_mt6628, pwrseq);

	/*
	 * PMU_EN is active-high, while RESET is active-low. Use logical
	 * values here: 0 turns PMU_EN off, and 1 asserts RESET.
	 */
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 0);
	gpiod_set_value_cansleep(pwrseq->reset_gpio, 1);
	msleep(MT6628_DFT_OFF_STABLE_TIME);

	/* Bring the supply up, then release reset. */
	gpiod_set_value_cansleep(pwrseq->pmu_gpio, 1);
	msleep(MT6628_DFT_ON_STABLE_TIME);

	gpiod_set_value_cansleep(pwrseq->reset_gpio, 0);
	msleep(MT6628_DFT_RST_STABLE_TIME);
}

static void mmc_pwrseq_mt6628_power_off(struct mmc_host *host)
{
	struct mmc_pwrseq_mt6628 *pwrseq =
		container_of(host->pwrseq, struct mmc_pwrseq_mt6628, pwrseq);

	/* Assert active-low RESET before removing power. */
	gpiod_set_value_cansleep(pwrseq->reset_gpio, 1);
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
	struct device_node *rtc_np;
	struct platform_device *rtc_pdev;
	int ret;

	pwrseq = devm_kzalloc(dev, sizeof(*pwrseq), GFP_KERNEL);
	if (!pwrseq)
		return -ENOMEM;

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
