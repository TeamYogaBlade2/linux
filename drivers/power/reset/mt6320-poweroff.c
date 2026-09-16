// SPDX-License-Identifier: GPL-2.0
/*
 * Power-off driver for MediaTek MT6320 PMIC
 *
 * Implements the full shutdown sequence required by the MT6320 RTC block,
 * derived from the MT6589/MT6320 downstream kernel:
 *   mediatek/platform/mt6589/kernel/drivers/rtc/mtk_rtc_hal.c
 *   hal_rtc_bbpu_pwdn()
 *
 * Sequence:
 *   1. Enable PMIC poweroff sequence (STRUP_CON9)
 *   2. Unlock RTC write interface (PROT = 0x586a, then 0x9136)
 *   3. Disable 32K GPIO export if no users (CON |= F32KOB)
 *   4. Pull PWRBB low (BBPU = KEY | AUTO | PWREN, then WRTGR)
 *
 * Copyright (C) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reboot.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6320/rtc.h>

struct mt6320_pwrc {
	struct device	*dev;
	struct regmap	*regmap;
	u32		base;
};

/**
 * mt6320_rtc_write_trigger - commit a pending RTC register write
 *
 * Writes 1 to WRTGR and waits for the CBUSY bit in BBPU to clear,
 * indicating the RTC has latched the value.
 */
static int mt6320_rtc_write_trigger(struct mt6320_pwrc *pwrc)
{
	unsigned int val;
	int ret;

	ret = regmap_write(pwrc->regmap, pwrc->base + MT6320_RTC_WRTGR, 1);
	if (ret)
		return ret;

	return regmap_read_poll_timeout(pwrc->regmap,
					pwrc->base + MT6320_RTC_BBPU, val,
					!(val & MT6320_RTC_BBPU_CBUSY),
					MT6320_RTC_POLL_DELAY_US,
					MT6320_RTC_POLL_TIMEOUT);
}

static int mt6320_do_pwroff(struct sys_off_data *data)
{
	struct mt6320_pwrc *pwrc = data->cb_data;
	unsigned int val;
	int ret;

	/* Step 1: Unlock RTC write interface (rtc_writeif_unlock) */
	ret = regmap_write(pwrc->regmap,
			   pwrc->base + MT6320_RTC_PROT,
			   MT6320_RTC_PROT_UNLOCK1);
	if (ret)
		goto fail;

	ret = mt6320_rtc_write_trigger(pwrc);
	if (ret)
		goto fail;

	ret = regmap_write(pwrc->regmap,
			   pwrc->base + MT6320_RTC_PROT,
			   MT6320_RTC_PROT_UNLOCK2);
	if (ret)
		goto fail;

	ret = mt6320_rtc_write_trigger(pwrc);
	if (ret)
		goto fail;

	/* Step 2: Disable 32K GPIO export when there are no RTC GPIO users */
	ret = regmap_read(pwrc->regmap, pwrc->base + MT6320_RTC_PDN1, &val);
	if (ret)
		goto fail;

	if (!(val & MT6320_RTC_GPIO_USER_MASK)) {
		ret = regmap_update_bits(pwrc->regmap,
					 pwrc->base + MT6320_RTC_CON,
					 MT6320_RTC_CON_F32KOB,
					 MT6320_RTC_CON_F32KOB);
		if (ret)
			goto fail;

		ret = mt6320_rtc_write_trigger(pwrc);
		if (ret)
			goto fail;
	}

	/* Step 3: Pull PWRBB low — BBPU = KEY | AUTO | PWREN */
	ret = regmap_write(pwrc->regmap,
			   pwrc->base + MT6320_RTC_BBPU,
			   MT6320_RTC_BBPU_KEY |
			   MT6320_RTC_BBPU_AUTO |
			   MT6320_RTC_BBPU_PWREN);
	if (ret)
		goto fail;

	ret = mt6320_rtc_write_trigger(pwrc);
	if (ret)
		goto fail;

	/* Wait for hardware to cut power; warn if it doesn't */
	mdelay(1000);
	WARN_ONCE(1, "Unable to power off system\n");
	return NOTIFY_DONE;

fail:
	dev_err(pwrc->dev, "poweroff sequence failed: %d\n", ret);
	return NOTIFY_DONE;
}

static int mt6320_pwrc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *mt6397_chip = dev_get_drvdata(pdev->dev.parent);
	struct mt6320_pwrc *pwrc;
	struct resource *res;
	int ret;

	pwrc = devm_kzalloc(&pdev->dev, sizeof(*pwrc), GFP_KERNEL);
	if (!pwrc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	pwrc->base = res->start;
	pwrc->regmap = mt6397_chip->regmap;
	pwrc->dev = &pdev->dev;

	/*
	 * Enable the PMIC poweroff sequence before registering the handler.
	 * STRUP_CON9[0] = STRUP_PWROFF_SEQ_EN
	 * STRUP_CON9[1] = STRUP_PWROFF_PREOFF_EN
	 *
	 * Without this, writing BBPU does not actually cut VSYS on MT6320.
	 * The STRUP block address is an absolute PMIC register (not RTC-relative).
	 */
	ret = regmap_update_bits(pwrc->regmap,
				 MT6320_STRUP_CON9,
				 MT6320_STRUP_PWROFF_SEQ_EN |
				 MT6320_STRUP_PWROFF_PREOFF_EN,
				 MT6320_STRUP_PWROFF_SEQ_EN |
				 MT6320_STRUP_PWROFF_PREOFF_EN);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable PMIC poweroff sequence\n");

	ret = devm_register_sys_off_handler(&pdev->dev,
					    SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_DEFAULT,
					    mt6320_do_pwroff,
					    pwrc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register power-off handler\n");

	dev_info(&pdev->dev, "MT6320 poweroff driver registered (RTC base 0x%04x)\n",
		 pwrc->base);
	return 0;
}

static const struct of_device_id mt6320_pwrc_dt_match[] = {
	{ .compatible = "mediatek,mt6320-pwrc" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6320_pwrc_dt_match);

static struct platform_driver mt6320_pwrc_driver = {
	.probe	= mt6320_pwrc_probe,
	.driver	= {
		.name		= "mt6320-pwrc",
		.of_match_table	= mt6320_pwrc_dt_match,
	},
};
module_platform_driver(mt6320_pwrc_driver);

MODULE_DESCRIPTION("Power-off driver for MT6320 PMIC");
MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_LICENSE("GPL");
