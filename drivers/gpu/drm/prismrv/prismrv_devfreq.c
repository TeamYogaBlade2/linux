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
#include <linux/nvmem-consumer.h>
#include <linux/pm_opp.h>

#include "prismrv_device.h"

/**
 * prismrv_read_gpu_grade() - fetch the fused GPU speed grade.
 *
 * MT6589 stores a 4-bit grade (values 1..7) in eFuse word 0x0c
 * bits[31:28] (the vendor /dev/devmap index-3 word).  Grade 0 means
 * unfused: the vendor boot code then runs a fixed default frequency
 * without DVFS.  Returns the raw grade, or a negative errno.
 */
static int prismrv_read_gpu_grade(struct device *dev)
{
	struct nvmem_cell *cell;
	size_t len;
	u8 *buf;
	int grade;

	cell = devm_nvmem_cell_get(dev, "gpu_grade");
	if (IS_ERR(cell)) {
		/* no cell wired up: treat as unfused */
		return 0;
	}

	buf = nvmem_cell_read(cell, &len);
	devm_nvmem_cell_put(dev, cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	grade = buf[0];
	kfree(buf);
	return grade;
}

static void prismrv_devfreq_update_utilization(struct prismrv_device *pv)
{
	struct prismrv_devfreq *df = &pv->devfreq;
	ktime_t now, last;

	now = ktime_get();
	last = df->time_last_update;

	if (atomic_read(&pv->busy_count) > 0)
		df->busy_time += ktime_to_ns(ktime_sub(now, last));
	else
		df->idle_time += ktime_to_ns(ktime_sub(now, last));

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
	prismrv_devfreq_update_utilization(pv);
	status->current_frequency = clk_get_rate(pv->clocks[0].clk);
	prismrv_devfreq_update_utilization(pv);
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
	u32 version;
	int ret, grade;

	spin_lock_init(&df->lock);
	df->time_last_update = ktime_get();

	grade = prismrv_read_gpu_grade(pv->drm.dev);
	if (grade < 0)
		return grade;

	version = BIT(grade);
	ret = dev_pm_opp_set_supported_hw(pv->drm.dev, &version, 1);
	if (ret)
		return ret;
	if (grade == 0)
		dev_info(pv->drm.dev,
			 "GPU grade unfused: fixed default frequency\n");
	else
		dev_info(pv->drm.dev, "GPU speed grade %d\n", grade);

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
