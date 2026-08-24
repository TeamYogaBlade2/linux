// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_devfreq.c — DVFS via the OPP framework + devfreq.
 *
 * Modelled on lima/panfrost: an operating-points-v2 table in DT drives
 * the "core" clock; utilisation is tracked with busy/idle counters that
 * are updated around submissions and completions.
 */
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/pm_opp.h>

#include "prismrv_device.h"

static void prismrv_devfreq_update_utilization(struct prismrv_devfreq *df)
{
	ktime_t now, last;

	now = ktime_get();
	last = df->time_last_update;

	if (df->busy_count > 0)
		df->busy_time += ktime_sub(now, last);
	else
		df->idle_time += ktime_sub(now, last);

	df->time_last_update = now;
}

static int prismrv_devfreq_target(struct device *dev, unsigned long *freq,
				  u32 flags)
{
	struct dev_pm_opp *opp;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	return dev_pm_opp_set_rate(dev, *freq);
}

static int prismrv_devfreq_get_dev_status(struct device *dev,
					  struct devfreq_dev_status *status)
{
	struct prismrv_device *pv = dev_get_drvdata(dev);
	struct prismrv_devfreq *df = &pv->devfreq;
	unsigned long irqflags;

	spin_lock_irqsave(&df->lock, irqflags);
	prismrv_devfreq_update_utilization(df);
	status->current_frequency = clk_get_rate(pv->clocks[0].clk);
	status->busy_time = df->busy_time;
	status->total_time = df->busy_time + df->idle_time;

	df->busy_time = 0;
	df->idle_time = 0;
	spin_unlock_irqrestore(&df->lock, irqflags);

	return 0;
}

static struct devfreq_dev_profile prismrv_devfreq_profile = {
	.polling_ms = 50,
	.target = prismrv_devfreq_target,
	.get_dev_status = prismrv_devfreq_get_dev_status,
};

int prismrv_devfreq_init(struct prismrv_device *pv)
{
	struct prismrv_devfreq *df = &pv->devfreq;
	struct dev_pm_opp *opp;
	unsigned long cur_freq;
	int ret;

	spin_lock_init(&df->lock);
	df->time_last_update = ktime_get();

	ret = devm_pm_opp_set_clkname(pv->drm.dev, pv->clocks[0].id);
	if (ret)
		return ret;

	ret = devm_pm_opp_of_add_table(pv->drm.dev);
	if (ret == -ENODEV || ret == -ENXIO) {
		dev_info(pv->drm.dev, "no OPP table, skipping DVFS\n");
		return 0;
	} else if (ret) {
		return ret;
	}

	cur_freq = clk_get_rate(pv->clocks[0].clk);
	opp = devfreq_recommended_opp(pv->drm.dev, &cur_freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	df->devfreq = devm_devfreq_add_device(pv->drm.dev,
					      &prismrv_devfreq_profile,
					      DEVFREQ_GOV_SIMPLE_ONDEMAND,
					      NULL);
	if (IS_ERR(df->devfreq))
		return PTR_ERR(df->devfreq);

	return 0;
}

void prismrv_devfreq_fini(struct prismrv_device *pv)
{
}
