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

#define MT6320_INT_CON_ACCDET_SET	MT6320_INT_CON1_SET
#define MT6320_INT_CON_ACCDET_CLR	MT6320_INT_CON1_CLR

#define MT6320_ACCDET_CTRL		MT6320_ACCDET_CON1
#define MT6320_ACCDET_STATE_SWCTRL	MT6320_ACCDET_CON2
#define MT6320_ACCDET_PWM_WIDTH		MT6320_ACCDET_CON3
#define MT6320_ACCDET_PWM_THRESH	MT6320_ACCDET_CON4
#define MT6320_ACCDET_EN_DELAY_NUM	MT6320_ACCDET_CON5
#define MT6320_ACCDET_DEBOUNCE0		MT6320_ACCDET_CON6
#define MT6320_ACCDET_DEBOUNCE1		MT6320_ACCDET_CON7
#define MT6320_ACCDET_DEBOUNCE3		MT6320_ACCDET_CON9
#define MT6320_ACCDET_IRQ_STS		MT6320_ACCDET_CON11
#define MT6320_ACCDET_STATE_RG		MT6320_ACCDET_CON13


#define MT6320_ACCDET_KEY_SAMPLE_MS	60
/*
 * Settle time before the first cable-class read after a plug-in.  The stock
 * driver never reads ACCDET_STATE_RG until its debounce window has elapsed
 * (it is interrupt driven, and the interrupt is the debounced transition),
 * so sampling earlier would race the PWM.  200 ms covers DEBOUNCE1 = 0x800
 * plus EN_DELAY (rise/fall 0x01f0 cycles) with margin.
 */
#define MT6320_ACCDET_SETTLE_MS		200

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
/*
 * Hook switch debounce while a headset is present.  The stock driver
 * shortens it so a remote button press is noticed quickly during a
 * call, then restores it once the hook switch settles.
 */
#define MT6320_ACCDET_DEBOUNCE0_BTN	0x0400
/*
 * Mic bias / AUXADC switch.  This board uses ACCDET_28V_MODE, where the
 * downstream driver drives AUDENCSPARE_CON0 rather than the ACCDET_RSV
 * (ACCDET_CON0) 1.9 V encoding.
 *
 * This is the switch the *analog* mic-bias supply hangs off, not a gate on
 * jack presence, and the vendor drives it from the analog path rather than
 * from headset detection: accdet_auxadc_switch() sets it while it reads the
 * key ADC (accdet.c:159-181), and accdet_eint_work_callback() clears it when
 * it tears the detector down on plug-out (accdet.c:410-417, the
 * ACCDET_28V_MODE branch writing 0x0732 = 0x00).  Nothing in the vendor
 * detection path waits on it, so a failure to write it must not abort
 * plug-in handling either.
 */
#define MT6320_AUDENCSPARE_CON0		0x0732
#define MT6320_ACCDET_MICBIAS_ENABLE	0x01
#define MT6320_ACCDET_MICBIAS_DISABLE	0x00

struct mt6320_accdet {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk_accdet;
	struct gpio_desc *detect;
	struct iio_channel *key;
	struct snd_soc_jack *jack;
	struct mutex lock;
	struct delayed_work key_work;
	struct delayed_work eint_work;
	struct delayed_work class_work;
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

	/*
	 * A missing jack here used to be entirely silent, and that is a
	 * bad failure mode for exactly the bug this driver keeps having:
	 * mt6320_accdet_set_jack() stores whatever it is given, and the
	 * machine driver treats a negative return as fatal-but-ignorable, so
	 * if binding ever missed, every report below would be dropped and
	 * nothing in dmesg would say so - the jack simply never changes
	 * state and the speaker/headphone handover never happens.
	 *
	 * Warn once per report so dmesg answers "is the report path even
	 * connected?" without needing debugfs on the device.
	 */
	if (!priv->jack) {
		dev_warn_ratelimited(priv->dev,
				     "jack report 0x%x dropped: no snd_soc_jack bound\n",
				     report);
		return;
	}

	snd_soc_jack_report(priv->jack, report, mask);
}

static int mt6320_accdet_key(struct mt6320_accdet *priv)
{
	int uv, mv;
	int ret;

	if (!priv->key)
		return -1;

	ret = iio_read_channel_processed(priv->key, &uv);
	if (ret)
		return -1;

	/*
	 * The AUXADC driver advertises its 1.2 V full-scale 10-bit range
	 * through IIO_CHAN_INFO_SCALE, so take the converted value from the
	 * IIO core rather than repeating the constants here and risking the
	 * two drifting apart.  The processed value is in microvolts, and
	 * the Blade BSP thresholds below are in millivolts.
	 */
	mv = uv / 1000;

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
		/*
		 * Entering MIC_BIAS: shorten the hook switch debounce so a
		 * remote button press is picked up quickly, and re-assert
		 * the mic bias duty cycle.  The stock driver restores the
		 * regular debounce on the next transition out of MIC_BIAS.
		 */
		if (priv->last_state != 1) {
			ret = regmap_write(priv->regmap,
					   MT6320_ACCDET_DEBOUNCE0,
					   MT6320_ACCDET_DEBOUNCE0_BTN);
			if (ret)
				return;
			ret = regmap_write(priv->regmap,
					   MT6320_ACCDET_PWM_WIDTH,
					   MT6320_ACCDET_PWM_WIDTH_VALUE);
			if (ret)
				return;
			/*
			 * The threshold keeps its own value.  Downstream
			 * writes the width into this register too, but only
			 * under ACCDET_PIN_RECOGNIZATION, which this board
			 * does not enable.
			 */
			ret = regmap_write(priv->regmap,
					   MT6320_ACCDET_PWM_THRESH,
					   MT6320_ACCDET_PWM_THRESH_VALUE);
			if (ret)
				return;
		}

		mt6320_accdet_report(priv, SND_JACK_HEADSET);
		break;
	case 2:
		/* Leaving MIC_BIAS: restore the regular debounce window. */
		ret = regmap_write(priv->regmap, MT6320_ACCDET_DEBOUNCE0,
				   MT6320_ACCDET_DEBOUNCE0_VALUE);
		if (ret)
			return;
		break;
	case 3:
		/*
		 * Class 3 means the detector sees no cable, which on a
		 * headphone-only plug is also what ACCDET_STATE_RG reads
		 * while the PWM is still settling.  Presence comes from the
		 * detect GPIO, so never let this clear a jack that the GPIO
		 * says is seated: report it as headphones, which is the
		 * conservative class for an unclassified seated jack.  The
		 * plug-out path reports the removal.
		 */
		mt6320_accdet_report(priv, SND_JACK_HEADPHONE);
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

/*
 * Resolve the cable class right after a plug-in, without waiting for the
 * ACCDET interrupt.
 *
 * The vendor leaves this to the interrupt, but only because its own EINT
 * work has just armed the detector.  Reading ACCDET_STATE_RG immediately
 * would sample mid-transition: the PWM needs its debounce window
 * (ACCDET_DEBOUNCE1 = 0x800 on this board) to settle before AB is valid.
 * So wait that window out in work context, then run the normal state
 * handler, which does the full headphone-vs-headset distinction and starts
 * the key scanner.
 *
 * This cannot be the key worker: that one deliberately returns early
 * unless the class is already resolved and the jack is seated.
 */
static void mt6320_accdet_class_work(struct work_struct *work)
{
	struct mt6320_accdet *priv =
		container_of(to_delayed_work(work), struct mt6320_accdet,
			     class_work);

	mutex_lock(&priv->lock);
	if (priv->plugged)
		mt6320_accdet_handle_state(priv);
	mutex_unlock(&priv->lock);
}

static void mt6320_accdet_read_class(struct mt6320_accdet *priv)
{
	schedule_delayed_work(&priv->class_work,
			      msecs_to_jiffies(MT6320_ACCDET_SETTLE_MS));
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
	if (cleanup_ret)
		dev_err_ratelimited(priv->dev,
				    "failed to roll back ACCDET control: %d\n",
				    cleanup_ret);
err_swctrl:
	cleanup_ret = regmap_clear_bits(priv->regmap,
					MT6320_ACCDET_STATE_SWCTRL,
					MT6320_ACCDET_SWCTRL_EN);
	if (cleanup_ret)
		dev_err_ratelimited(priv->dev,
				    "failed to roll back ACCDET switch control: %d\n",
				    cleanup_ret);
err_clk:
	clk_disable_unprepare(priv->clk_accdet);
	return ret;
}

static int mt6320_accdet_disable_hw(struct mt6320_accdet *priv)
{
	int ret, first_err = 0;

	ret = regmap_write(priv->regmap, MT6320_INT_CON_ACCDET_CLR,
			   MT6320_ACCDET_IRQ_SET_BIT);
	if (ret && !first_err)
		first_err = ret;

	ret = mt6320_accdet_clear_irq(priv);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(priv->regmap, MT6320_ACCDET_CTRL,
				MT6320_ACCDET_CTRL_EN);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_write(priv->regmap, MT6320_ACCDET_STATE_SWCTRL, 0);
	if (ret && !first_err)
		first_err = ret;

	return first_err;
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

/*
 * The PMIC's ACCDET interrupt only ever reports that the *debounced* cable
 * class moved, which the vendor only ever arms while a jack is already
 * seated: enable_accdet() runs from the EINT plug-in work
 * (accdet.c:344-360) and disable_accdet() from the plug-out work
 * (accdet.c:288-303).  Our detect GPIO handler below is the authority on
 * presence, so this handler reports a class change and nothing else.  It
 * must not be used to decide plugged/unplugged: with the jack out the
 * detector is torn down and this line never fires at all.
 */
static irqreturn_t mt6320_accdet_irq(int irq, void *data)
{
	struct mt6320_accdet *priv = data;
	unsigned int val;

	mutex_lock(&priv->lock);

	if (!regmap_read(priv->regmap, MT6320_ACCDET_IRQ_STS, &val) &&
	    (val & MT6320_ACCDET_IRQ_STATUS_BIT) && priv->plugged)
		mt6320_accdet_handle_state(priv);

	mt6320_accdet_clear_irq(priv);
	mutex_unlock(&priv->lock);

	return IRQ_HANDLED;
}

static irqreturn_t mt6320_accdet_eint(int irq, void *data);

static void mt6320_accdet_eint_work(struct work_struct *work)
{
	struct mt6320_accdet *priv =
		container_of(to_delayed_work(work), struct mt6320_accdet,
				     eint_work);
	int inserted;
	int ret;

	inserted = gpiod_get_value_cansleep(priv->detect);
	if (inserted < 0)
		goto retry;

	mutex_lock(&priv->lock);
	if (!!inserted != priv->plugged) {
		/* Recover a transition missed while switching the IRQ edge. */
		mutex_unlock(&priv->lock);
		mt6320_accdet_eint(priv->eint_irq, priv);
		return;
	}

	ret = irq_set_irq_type(priv->eint_irq,
			       priv->plugged ? IRQ_TYPE_LEVEL_HIGH :
					       IRQ_TYPE_LEVEL_LOW);
	mutex_unlock(&priv->lock);
	if (!ret)
		return;

retry:
	schedule_delayed_work(&priv->eint_work, msecs_to_jiffies(100));
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

	/*
	 * Stop the key scanner before taking the lock on plug-out: the
	 * worker needs the lock itself, so cancelling it underneath would
	 * deadlock.  It would otherwise re-arm itself, read the ACCDET
	 * state again and could report a second removal, and it reads the
	 * AUXADC, which no longer makes sense once the jack is empty.
	 */
	if (!inserted) {
		cancel_delayed_work_sync(&priv->class_work);
		cancel_delayed_work_sync(&priv->key_work);
	}

	mutex_lock(&priv->lock);

	if (!!inserted == priv->plugged)
		goto out;

	if (!!inserted) {
		/*
		 * The mic-bias switch is not a prerequisite for detection: the
		 * vendor turns it on to read the key ADC and off again when it
		 * tears the detector down, but its own plug-in path never waits
		 * on it.  Treat a failure here as non-fatal, otherwise a single
		 * unwritable analog register disables headset detection
		 * entirely and the speaker route is never handed over.
		 */
		ret = regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				   MT6320_ACCDET_MICBIAS_ENABLE);
		if (ret)
			dev_err_ratelimited(priv->dev,
					    "failed to enable mic bias switch: %d\n",
					    ret);

		ret = mt6320_accdet_enable(priv);
		if (ret) {
			dev_err_ratelimited(priv->dev,
					    "failed to enable ACCDET: %d\n",
					    ret);
			if (regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
					 MT6320_ACCDET_MICBIAS_DISABLE))
				dev_err_ratelimited(priv->dev,
						    "failed to roll back mic bias switch\n");
			goto out;
		}

		/*
		 * detect-gpios is GPIO_ACTIVE_LOW, so the line is asserted
		 * (logical 1 / physical low) while the jack is in.  Wait for
		 * it to leave that state, i.e. level-high in logical terms.
		 */
		ret = irq_set_irq_type(priv->eint_irq,
				       IRQ_TYPE_LEVEL_HIGH);
		if (ret) {
			dev_err_ratelimited(priv->dev,
					    "failed to configure plug-in IRQ: %d\n",
					    ret);
			priv->plugged = false;
			if (mt6320_accdet_disable(priv))
				dev_err_ratelimited(priv->dev,
						    "failed to roll back ACCDET after IRQ setup failure\n");
			if (regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
					 MT6320_ACCDET_MICBIAS_DISABLE))
				dev_err_ratelimited(priv->dev,
						    "failed to roll back mic bias switch\n");
			schedule_delayed_work(&priv->eint_work,
					      msecs_to_jiffies(100));
			goto out;
		}

		priv->plugged = true;

		/*
		 * Report the jack.  The detect GPIO is the authority on
		 * presence - the vendor only ever arms the PMIC ACCDET unit
		 * from this same plug-in work (accdet.c accdet_eint_work_callback
		 * -> accdet_init()/enable_accdet()) and tears it down on
		 * plug-out, so the ACCDET interrupt cannot be relied on to
		 * announce the jack in the first place.
		 *
		 * This is the whole of problem 1: the plug-in path used to set
		 * priv->plugged and return without ever reporting, and the only
		 * other caller of mt6320_accdet_report() is the ACCDET
		 * interrupt, which does not fire while the detector is off.
		 * The jack therefore never left its empty state, the Speaker
		 * pin was never disabled, and plugging headphones changed
		 * nothing.
		 *
		 * Report the conservative class first (headphone only) so the
		 * speaker is handed over even if the detector does not settle,
		 * then let the poll below refine it to a headset with mic.
		 * mt6320_accdet_handle_state() runs the same distinction once
		 * ACCDET reports.
		 */
		priv->last_state = 3;
		mt6320_accdet_report(priv, SND_JACK_HEADPHONE);

		/*
		 * Give the detector time to resolve the cable class before
		 * reading it.  The stock driver arms the same PWM
		 * (ACCDET_PWM_WIDTH 0x900, DEBOUNCE1 0x800) and only reads
		 * ACCDET_STATE_RG from its interrupt, i.e. after the debounce
		 * window has elapsed; we read it directly, so wait at least
		 * that long rather than sampling mid-transition.
		 */
		mt6320_accdet_read_class(priv);
	} else {
		/*
		 * Drop the mic-bias switch, as the vendor does at the end of
		 * its plug-out work: accdet_eint_work_callback() writes
		 * 0x0732 = 0x00 in the ACCDET_28V_MODE branch
		 * (mediatek/platform/mt6589/kernel/drivers/accdet/accdet.c,
		 * "ACCDET diable switch in 2.8v mode").  The 0x1090 word in
		 * the same place belongs to the 1.9 V ACCDET_RSV encoding,
		 * which this board does not use.
		 *
		 * This is the analog mic-bias supply switch, not a presence
		 * gate: the AUXADC driver asserts it for each voltage read, so
		 * it has to be dropped again on the way out or it keeps
		 * driving bias into an unpopulated connector.
		 */
		ret = regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				   MT6320_ACCDET_MICBIAS_DISABLE);
		if (ret)
			dev_err_ratelimited(priv->dev,
					    "failed to disable mic bias: %d\n",
					    ret);

		ret = mt6320_accdet_disable(priv);
		if (ret)
			dev_err_ratelimited(priv->dev,
					    "failed to disable ACCDET: %d\n",
					    ret);

		priv->plugged = false;
		priv->last_state = 3;
		mt6320_accdet_report(priv, 0);

		/* Jack is out; wait for insertion, i.e. the active level. */
		ret = irq_set_irq_type(priv->eint_irq,
				       IRQ_TYPE_LEVEL_LOW);
		if (ret) {
			dev_err_ratelimited(priv->dev,
					    "failed to configure next plug-in IRQ: %d\n",
					    ret);
			schedule_delayed_work(&priv->eint_work,
					      msecs_to_jiffies(100));
		}
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
	if (!jack) {
		mutex_unlock(&priv->lock);
		return 0;
	}
	/*
	 * Always publish a full-mask report.  The Speaker jack pin is
	 * inverted on SND_JACK_HEADPHONE; without an initial report DAPM
	 * never enables it and the built-in speaker stays silent even
	 * though the AFE period IRQ is running.
	 *
	 * When a jack is already seated at bind time the detector has not
	 * been armed (probe only runs the hardware init; the detector is
	 * enabled from the plug-in work), so ACCDET_STATE_RG carries
	 * whatever it held last rather than a live class.  Report the
	 * conservative headphone-only class and let the settle worker
	 * refine it once the detector is running - reporting straight from
	 * handle_state() here would trust a stale register.
	 */
	if (priv->plugged) {
		mt6320_accdet_report(priv, SND_JACK_HEADPHONE);
		mt6320_accdet_read_class(priv);
	} else {
		mt6320_accdet_report(priv, 0);
	}
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
	INIT_DELAYED_WORK(&priv->eint_work, mt6320_accdet_eint_work);
	INIT_DELAYED_WORK(&priv->class_work, mt6320_accdet_class_work);
	platform_set_drvdata(pdev, priv);

	priv->detect = devm_gpiod_get(&pdev->dev, "detect", GPIOD_IN);
	if (IS_ERR(priv->detect))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->detect),
				     "failed to get detect GPIO\n");

	priv->key = devm_iio_channel_get(&pdev->dev, "key");
	if (IS_ERR(priv->key))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->key),
				     "failed to get key ADC\n");

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

	/*
	 * Arm for the level that reports the *next* transition.
	 *
	 * The detect line is level-triggered on the vendor board, not edge.
	 * mediatek/custom/blade10/kernel/dct/dct/cust_eint.h declares
	 * CUST_EINT_ACCDET_SENSITIVE = CUST_EINT_LEVEL_SENSITIVE with
	 * CUST_EINT_ACCDET_TYPE = CUST_EINTF_TRIGGER_LOW and
	 * CUST_EINT_ACCDET_DEBOUNCE_CN = 256, and the vendor handler
	 * accdet_eint_func() (kernel_blade10_row_wifi.elf @ c054c75c) drives
	 * mt_eint_set_polarity(10, ...) on every interrupt to flip which level
	 * is active.  mt_eint_set_polarity() writes EINT_POL_SET/CLR and then
	 * acks - the polarity register is the level control, so the vendor is
	 * genuinely running the line level-triggered and re-arming it by
	 * inverting the sense.
	 *
	 * detect-gpios is GPIO_ACTIVE_LOW, so the line is asserted (logical 1)
	 * while the jack is in.  A line sitting at its active level would
	 * storm an edge-irrelevant handler, but this one is IRQF_ONESHOT and
	 * changes the polarity before returning, exactly as the vendor does,
	 * so the level deasserts and the next interrupt is the opposite
	 * transition.
	 */
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

	/*
	 * Program the EINT debounce only now that the line is ours.
	 *
	 * gpiod_set_debounce() ends in mtk_eint_set_debounce(), which masks
	 * and then *unmasks* the EINT line it touches.  Unmasking before
	 * request_irq() has run is not a harmless early enable: the EINT's
	 * polarity register still holds its reset value (mtk_eint_set_type()
	 * is what programs it, and that runs just above), the pin is still a
	 * GPIO (mtk_eint_irq_request_resources() is what calls
	 * set_gpio_as_eint(), and that only runs from the request) and no
	 * handler is registered.  An edge arriving in that window latches the
	 * EINT status bit that mtk_eint_irq_handler() has to see and ack on
	 * behalf of a *child* handler; with no child registered, nothing acks
	 * it, the status register stays non-zero, and the chained handler
	 * spins on it forever.  That is the observed symptom of the jack
	 * interrupt never arriving at all, and it is permanent for the boot.
	 *
	 * -EINVAL is tolerated: mtk_eint_can_en_debounce() accepts a debounce
	 * value only for a pin some other consumer marked debounce-capable,
	 * and nothing in this SoC's pin table does.  The debounce is an
	 * optimisation for a mechanical jack switch, not a requirement.
	 */
	ret = gpiod_set_debounce(priv->detect, 256000);
	if (ret && ret != -ENOTSUPP && ret != -EINVAL)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to set detect debounce\n");

	if (priv->plugged) {
		ret = regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				   MT6320_ACCDET_MICBIAS_ENABLE);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "failed to enable mic bias switch\n");

		ret = mt6320_accdet_enable(priv);
		if (ret) {
			if (regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
					 MT6320_ACCDET_MICBIAS_DISABLE))
				dev_err(&pdev->dev, "failed to roll back mic bias switch\n");
			return dev_err_probe(&pdev->dev, ret,
					     "failed to enable ACCDET\n");
		}
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

static void mt6320_accdet_remove(struct platform_device *pdev)
{
	struct mt6320_accdet *priv = platform_get_drvdata(pdev);
	int ret = 0;
	int bias_ret;

	cancel_delayed_work_sync(&priv->key_work);
	cancel_delayed_work_sync(&priv->eint_work);

	mutex_lock(&priv->lock);
	if (priv->plugged)
		ret = mt6320_accdet_disable(priv);
	priv->plugged = false;
	priv->last_state = 3;
	mt6320_accdet_report(priv, 0);

	/* The switch is ACCDET-owned and must be off even on driver removal. */
	bias_ret = regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				MT6320_ACCDET_MICBIAS_DISABLE);
	if (!ret)
		ret = bias_ret;
	mutex_unlock(&priv->lock);

	if (ret)
		dev_err(&pdev->dev,
			"failed to clean up ACCDET during remove: %d\n", ret);
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
