// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6320 headset accessory detection.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6320/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#include <sound/jack.h>
#include <sound/soc.h>

#define MT6320_ACCDET_KEY_SAMPLE_MS	60

#define MT6320_ACCDET_CTRL_EN		BIT(0)
#define MT6320_ACCDET_SWCTRL_EN		0x07
#define MT6320_ACCDET_IRQ_CLR_BIT	BIT(8)
#define MT6320_ACCDET_IRQ_STATUS_BIT	BIT(0)
#define MT6320_ACCDET_IRQ_SET_BIT	BIT(2)
#define MT6320_ACCDET_RESET_BIT		BIT(4)

#define MT6320_ACCDET_PWM_WIDTH_VALUE	0x0900
#define MT6320_ACCDET_PWM_THRESH_VALUE	0x0200
#define MT6320_ACCDET_EN_DELAY_VALUE	((1U << 15) | 0x01f0)
#define MT6320_ACCDET_DEBOUNCE0_VALUE	0x0800
#define MT6320_ACCDET_DEBOUNCE1_VALUE	0x0800
#define MT6320_ACCDET_DEBOUNCE3_VALUE	0x0020

struct mt6320_accdet {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk_accdet;
	struct gpio_desc *detect;
	struct iio_channel *key;
	struct snd_soc_jack *jack;
	struct mutex lock;
	struct delayed_work key_work;
	int accdet_irq;
	int eint_irq;
	int last_state;
	bool plugged;
};

static int mt6320_accdet_clear_irq(struct mt6320_accdet *priv)
{
	unsigned int val;
	int ret;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_IRQ_STS,
			   MT6320_ACCDET_IRQ_CLR_BIT);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(priv->regmap,
				       MT6320_ACCDET_IRQ_STS, val,
				       !(val & MT6320_ACCDET_IRQ_STATUS_BIT),
				       500, 15000);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, MT6320_ACCDET_IRQ_STS, &val);
	if (ret)
		return ret;

	return regmap_write(priv->regmap, MT6320_ACCDET_IRQ_STS,
			    val & ~MT6320_ACCDET_IRQ_CLR_BIT);
}

static void mt6320_accdet_report(struct mt6320_accdet *priv,
				 unsigned int report)
{
	static const unsigned int mask =
		SND_JACK_HEADPHONE |
		SND_JACK_HEADSET |
		SND_JACK_BTN_0 |
		SND_JACK_BTN_1 |
		SND_JACK_BTN_2;

	if (priv->jack)
		snd_soc_jack_report(priv->jack, report, mask);
}

static int mt6320_accdet_key(struct mt6320_accdet *priv)
{
	int raw, mv, ret;

	if (!priv->key)
		return -1;

	ret = iio_read_channel_raw(priv->key, &raw);
	if (ret)
		return -1;

	/* MT6320 AUXADC: 10-bit conversion, 1.2 V full-scale. */
	mv = raw * 1200 / 1024;

	/*
	 * Blade BSP thresholds:
	 *   0..89mV   middle
	 *   90..239mV up
	 *   240..499mV down
	 */
	if (mv < 90)
		return SND_JACK_BTN_0;
	if (mv < 240)
		return SND_JACK_BTN_1;
	if (mv < 500)
		return SND_JACK_BTN_2;

	return -1;
}

static void mt6320_accdet_key_work(struct work_struct *work)
{
	struct mt6320_accdet *priv =
		container_of(to_delayed_work(work),
			     struct mt6320_accdet, key_work);
	unsigned int val;
	int state;
	int button;

	mutex_lock(&priv->lock);

	if (!priv->plugged || !priv->jack || priv->last_state != 0)
		goto out;

	if (regmap_read(priv->regmap, MT6320_ACCDET_STATE_RG, &val))
		goto out;

	state = FIELD_GET(GENMASK(7, 6), val);
	if (state != 0) {
		if (priv->last_state == 0) {
			if (state == 1)
				mt6320_accdet_report(priv,
						    SND_JACK_HEADSET);
			else if (state == 3)
				mt6320_accdet_report(priv, 0);
		}
		priv->last_state = state;
		goto out;
	}

	button = mt6320_accdet_key(priv);
	if (button >= 0)
		mt6320_accdet_report(priv, SND_JACK_HEADSET | button);
	else
		mt6320_accdet_report(priv, SND_JACK_HEADSET);

	schedule_delayed_work(&priv->key_work,
			      msecs_to_jiffies(MT6320_ACCDET_KEY_SAMPLE_MS));

out:
	mutex_unlock(&priv->lock);
}

static void mt6320_accdet_handle_state(struct mt6320_accdet *priv)
{
	unsigned int val;
	int state;
	int ret;
	bool start_key_work = false;

	ret = regmap_read(priv->regmap, MT6320_ACCDET_STATE_RG, &val);
	if (ret)
		return;

	state = FIELD_GET(GENMASK(7, 6), val);

	switch (state) {
	case 0:
		if (priv->last_state == 1) {
			int button = mt6320_accdet_key(priv);

			if (button >= 0)
				mt6320_accdet_report(priv,
						    SND_JACK_HEADSET | button);
			else
				mt6320_accdet_report(priv, SND_JACK_HEADSET);
			start_key_work = true;
		} else if (priv->last_state != 0) {
			mt6320_accdet_report(priv, SND_JACK_HEADPHONE);
		}
		break;
	case 1:
		mt6320_accdet_report(priv, SND_JACK_HEADSET);
		break;
	case 3:
		mt6320_accdet_report(priv, 0);
		break;
	default:
		break;
	}

	priv->last_state = state;

	if (start_key_work)
		schedule_delayed_work(&priv->key_work,
				      msecs_to_jiffies(
					      MT6320_ACCDET_KEY_SAMPLE_MS));
}

static int mt6320_accdet_enable(struct mt6320_accdet *priv)
{
	int ret, cleanup_ret;

	ret = clk_prepare_enable(priv->clk_accdet);
	if (ret)
		return ret;

	ret = regmap_set_bits(priv->regmap, MT6320_ACCDET_STATE_SWCTRL,
			      MT6320_ACCDET_SWCTRL_EN);
	if (ret)
		goto err_clk;

	ret = regmap_set_bits(priv->regmap, MT6320_ACCDET_CTRL,
			      MT6320_ACCDET_CTRL_EN);
	if (ret)
		goto err_swctrl;

	ret = regmap_write(priv->regmap, MT6320_INT_CON_ACCDET_SET,
			   MT6320_ACCDET_IRQ_SET_BIT);
	if (ret)
		goto err_ctrl;

	return 0;

err_ctrl:
	cleanup_ret = regmap_clear_bits(priv->regmap, MT6320_ACCDET_CTRL,
					MT6320_ACCDET_CTRL_EN);
	if (!ret)
		ret = cleanup_ret;
err_swctrl:
	cleanup_ret = regmap_clear_bits(priv->regmap,
					MT6320_ACCDET_STATE_SWCTRL,
					MT6320_ACCDET_SWCTRL_EN);
	if (!ret)
		ret = cleanup_ret;
err_clk:
	clk_disable_unprepare(priv->clk_accdet);
	return ret;
}

static int mt6320_accdet_disable_hw(struct mt6320_accdet *priv)
{
	int ret;

	ret = regmap_write(priv->regmap, MT6320_INT_CON_ACCDET_CLR,
			   MT6320_ACCDET_IRQ_SET_BIT);
	if (ret)
		return ret;

	ret = mt6320_accdet_clear_irq(priv);
	if (ret)
		return ret;

	ret = regmap_clear_bits(priv->regmap, MT6320_ACCDET_CTRL,
				MT6320_ACCDET_CTRL_EN);
	if (ret)
		return ret;

	return regmap_write(priv->regmap, MT6320_ACCDET_STATE_SWCTRL, 0);
}

static int mt6320_accdet_disable(struct mt6320_accdet *priv)
{
	int ret;

	ret = mt6320_accdet_disable_hw(priv);
	clk_disable_unprepare(priv->clk_accdet);
	return ret;
}

static int mt6320_accdet_hw_init(struct mt6320_accdet *priv)
{
	int ret;

	ret = clk_prepare_enable(priv->clk_accdet);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, MT6320_TOP_RST_ACCDET_SET,
			   MT6320_ACCDET_RESET_BIT);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_TOP_RST_ACCDET_CLR,
			   MT6320_ACCDET_RESET_BIT);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_PWM_WIDTH,
			   MT6320_ACCDET_PWM_WIDTH_VALUE);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_PWM_THRESH,
			   MT6320_ACCDET_PWM_THRESH_VALUE);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_STATE_SWCTRL,
			   MT6320_ACCDET_SWCTRL_EN);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_EN_DELAY_NUM,
			   MT6320_ACCDET_EN_DELAY_VALUE);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_DEBOUNCE0,
			   MT6320_ACCDET_DEBOUNCE0_VALUE);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_DEBOUNCE1,
			   MT6320_ACCDET_DEBOUNCE1_VALUE);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_DEBOUNCE3,
			   MT6320_ACCDET_DEBOUNCE3_VALUE);
	if (ret)
		goto err_clk;

	ret = regmap_write(priv->regmap, MT6320_INT_CON_ACCDET_CLR,
			   MT6320_ACCDET_IRQ_SET_BIT);
	if (ret)
		goto err_clk;

	ret = mt6320_accdet_clear_irq(priv);
	clk_disable_unprepare(priv->clk_accdet);

	return ret;

err_clk:
	clk_disable_unprepare(priv->clk_accdet);
	return ret;
}

static irqreturn_t mt6320_accdet_irq(int irq, void *data)
{
	struct mt6320_accdet *priv = data;
	unsigned int val;

	mutex_lock(&priv->lock);

	if (!regmap_read(priv->regmap, MT6320_ACCDET_IRQ_STS, &val) &&
	    (val & MT6320_ACCDET_IRQ_STATUS_BIT))
		mt6320_accdet_handle_state(priv);

	mt6320_accdet_clear_irq(priv);
	mutex_unlock(&priv->lock);

	return IRQ_HANDLED;
}

static irqreturn_t mt6320_accdet_eint(int irq, void *data)
{
	struct mt6320_accdet *priv = data;
	int inserted;
	int ret;

	inserted = gpiod_get_value_cansleep(priv->detect);
	if (inserted < 0) {
		dev_err_ratelimited(priv->dev,
				    "failed to read detect GPIO: %d\n",
				    inserted);
		return IRQ_HANDLED;
	}

	mutex_lock(&priv->lock);

	if (!!inserted == priv->plugged)
		goto out;

	if (!!inserted) {
		ret = mt6320_accdet_enable(priv);
		if (ret) {
			dev_err_ratelimited(priv->dev,
					    "failed to enable ACCDET: %d\n",
					    ret);
			goto out;
		}

		priv->plugged = true;

		ret = irq_set_irq_type(priv->eint_irq,
				       IRQ_TYPE_LEVEL_HIGH);
		if (ret)
			dev_err_ratelimited(priv->dev,
					    "failed to configure plug-in IRQ: %d\n",
					    ret);
	} else {
		ret = mt6320_accdet_disable(priv);
		if (ret)
			dev_err_ratelimited(priv->dev,
					    "failed to disable ACCDET: %d\n",
					    ret);

		priv->plugged = false;
		priv->last_state = 3;
		mt6320_accdet_report(priv, 0);

		ret = irq_set_irq_type(priv->eint_irq,
				       IRQ_TYPE_LEVEL_LOW);
		if (ret)
			dev_err_ratelimited(priv->dev,
					    "failed to configure plug-out IRQ: %d\n",
					    ret);
	}

out:
	mutex_unlock(&priv->lock);
	return IRQ_HANDLED;
}

static int mt6320_accdet_set_jack(struct snd_soc_component *component,
				  struct snd_soc_jack *jack, void *data)
{
	struct mt6320_accdet *priv =
		snd_soc_component_get_drvdata(component);

	mutex_lock(&priv->lock);
	priv->jack = jack;
	if (priv->plugged)
		mt6320_accdet_handle_state(priv);
	mutex_unlock(&priv->lock);

	return 0;
}

static const struct snd_soc_component_driver mt6320_accdet_component = {
	.name = "mt6320-accdet",
	.set_jack = mt6320_accdet_set_jack,
};

static int mt6320_accdet_probe(struct platform_device *pdev)
{
	struct mt6320_accdet *priv;
	struct mt6397_chip *pmic;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	pmic = dev_get_drvdata(pdev->dev.parent);
	if (!pmic || !pmic->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "missing PMIC regmap\n");
	priv->regmap = pmic->regmap;

	priv->clk_accdet = devm_clk_get(&pdev->dev, "accdet");
	if (IS_ERR(priv->clk_accdet))
		return dev_err_probe(&pdev->dev,
				     PTR_ERR(priv->clk_accdet),
				     "failed to get ACCDET clock\n");

	mutex_init(&priv->lock);
	INIT_DELAYED_WORK(&priv->key_work, mt6320_accdet_key_work);
	platform_set_drvdata(pdev, priv);

	priv->detect = devm_gpiod_get(&pdev->dev, "detect", GPIOD_IN);
	if (IS_ERR(priv->detect))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->detect),
				     "failed to get detect GPIO\n");

	priv->key = devm_iio_channel_get(&pdev->dev, "key");
	if (IS_ERR(priv->key))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->key),
				     "failed to get key ADC\n");

	ret = gpiod_set_debounce(priv->detect, 256000);
	if (ret && ret != -ENOTSUPP)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to set detect debounce\n");

	ret = mt6320_accdet_hw_init(priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to initialize ACCDET\n");

	priv->accdet_irq =
		platform_get_irq_byname(pdev, "accdet_irq");
	if (priv->accdet_irq < 0)
		return priv->accdet_irq;

	ret = devm_request_threaded_irq(&pdev->dev, priv->accdet_irq,
					NULL, mt6320_accdet_irq,
					IRQF_ONESHOT,
					"mt6320-accdet", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request ACCDET IRQ\n");

	priv->eint_irq = gpiod_to_irq(priv->detect);
	if (priv->eint_irq < 0)
		return dev_err_probe(&pdev->dev, priv->eint_irq,
				     "failed to map detect GPIO to IRQ\n");

	ret = gpiod_get_value_cansleep(priv->detect);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to read detect GPIO\n");
	priv->plugged = !!ret;
	priv->last_state = 3;

	ret = irq_set_irq_type(priv->eint_irq,
			       priv->plugged ?
			       IRQ_TYPE_LEVEL_HIGH : IRQ_TYPE_LEVEL_LOW);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to configure detect IRQ\n");

	ret = devm_request_threaded_irq(&pdev->dev, priv->eint_irq,
					NULL, mt6320_accdet_eint,
					IRQF_ONESHOT,
					"mt6320-accdet-eint", priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request detect IRQ\n");

	if (priv->plugged) {
		ret = mt6320_accdet_enable(priv);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to enable ACCDET\n");
	} else {
		ret = clk_prepare_enable(priv->clk_accdet);
		if (!ret) {
			ret = mt6320_accdet_disable_hw(priv);
			clk_disable_unprepare(priv->clk_accdet);
		}
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to disable idle ACCDET\n");
	}

	return devm_snd_soc_register_component(&pdev->dev,
					       &mt6320_accdet_component,
					       NULL, 0);
}

static int mt6320_accdet_remove(struct platform_device *pdev)
{
	struct mt6320_accdet *priv = platform_get_drvdata(pdev);
	int ret = 0;

	cancel_delayed_work_sync(&priv->key_work);

	mutex_lock(&priv->lock);
	if (priv->plugged)
		ret = mt6320_accdet_disable(priv);
	priv->plugged = false;
	priv->last_state = 3;
	mt6320_accdet_report(priv, 0);
	mutex_unlock(&priv->lock);

	return ret;
}

static const struct of_device_id mt6320_accdet_of_match[] = {
	{ .compatible = "mediatek,mt6320-accdet" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6320_accdet_of_match);

static struct platform_driver mt6320_accdet_driver = {
	.probe = mt6320_accdet_probe,
	.remove = mt6320_accdet_remove,
	.driver = {
		.name = "mt6320-accdet",
		.of_match_table = mt6320_accdet_of_match,
	},
};
module_platform_driver(mt6320_accdet_driver);

MODULE_DESCRIPTION("MediaTek MT6320 headset accessory detection");
MODULE_LICENSE("GPL");
