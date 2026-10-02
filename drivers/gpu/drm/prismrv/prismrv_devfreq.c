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

/* opp-supported-hw bit selecting the SW-eFuse forced 238.333 MHz point */
#define PRISMRV_OPP_FORCED_BIT	8

/**
 * prismrv_read_gpu_grade() - fetch the fused GPU speed grade.
 *
 * MT6589 stores the grade (values 1..7) in eFuse word 0x0c bits[30:28]
 * (the vendor /dev/devmap index-3 word, bit 31 is masked off by the
 * vendor code).  The DT nvmem cell extracts exactly these three bits.
 * Grade 0 means unfused: the vendor boot code then runs a fixed default
 * frequency (286 MHz) without DVFS.  Returns the raw grade, or a
 * negative errno.
 */
static int prismrv_read_gpu_grade(struct device *dev)
{
	struct nvmem_cell *cell;
	size_t len;
	u8 *buf;
	int grade;

	cell = devm_nvmem_cell_get(dev, "gpu_grade");
	if (IS_ERR(cell)) {
		int err = PTR_ERR(cell);

		/*
		 * Only "no such cell / no nvmem" means unfused (grade 0 =
		 * slowest).  -EPROBE_DEFER must reach the caller: running
		 * with grade 0 only because the eFuse provider has not
		 * probed yet would pick the wrong OPP table for good.
		 */
		if (err == -ENOENT || err == -ENODEV)
			return 0;
		return err;
	}

	buf = nvmem_cell_read(cell, &len);
	devm_nvmem_cell_put(dev, cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	/*
	 * Validate the returned buffer length.  The DT specifies
	 *   bits = <28 3>   (3-bit field at bit 28 of a 32-bit word)
	 * so the NVMEM provider should return exactly 1 byte after
	 * bit-extraction.  Guard against a misconfigured provider
	 * returning a shorter (0-byte) or wider buffer.
	 */
	if (len < 1) {
		dev_warn(dev, "gpu_grade NVMEM cell returned %zu bytes (want 1)\n",
			 len);
		kfree(buf);
		return 0;
	}

	grade = buf[0] & 0x07;	/* 3-bit field: mask stray upper bits */
	kfree(buf);
	return grade;
}

/*
 * SW eFuse (devinfo index 10) bit 7: the vendor mtk_set_freq_init()
 * overrides the grade-derived frequency with 238.333 MHz when it is set.
 * Returns 1 if set, 0 if clear or the cell is not wired up.
 */
static int prismrv_read_sw_efuse_force(struct device *dev)
{
	struct nvmem_cell *cell;
	size_t len;
	u8 *buf;
	int val;

	cell = devm_nvmem_cell_get(dev, "gpu_sw_efuse");
	if (IS_ERR(cell)) {
		int err = PTR_ERR(cell);

		return (err == -ENOENT || err == -ENODEV) ? 0 : err;
	}
	buf = nvmem_cell_read(cell, &len);
	devm_nvmem_cell_put(dev, cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	val = len ? (buf[0] & 1) : 0;
	kfree(buf);
	return val;
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
	status->current_frequency = clk_get_rate(pv->clk_core);
	/* a single utilization update: calling it twice double-counted
	 * the elapsed interval and skewed the reported load */
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
		return grade;	/* NVMEM read error */

	/*
	 * prismrv_read_gpu_grade() already masks to 4 bits (0..15).
	 * Values 1..7 are documented speed grades; 0 means unfused.
	 * Any value > 7 from a future part gets clamped to 0 (slowest).
	 */
	if (grade > 7)
		grade = 0;
	version = BIT(grade);

	ret = prismrv_read_sw_efuse_force(pv->drm.dev);
	if (ret < 0)
		return ret;
	if (ret) {
		dev_info(pv->drm.dev, "SW eFuse bit 7 set: forcing 238.333 MHz\n");
		version = BIT(PRISMRV_OPP_FORCED_BIT);
	}
	if (grade == 0)
		dev_info(pv->drm.dev,
			 "GPU grade unfused: fixed default frequency\n");
	else
		dev_info(pv->drm.dev, "GPU speed grade %d\n", grade);

	/*
	 * Bind the OPP table to the core clock and the VRF18_2 buck (the rail
	 * the downstream mt_gpufreq switches for the MFG domain).  supported_hw
	 * selects the grade-specific entries via opp-supported-hw in the DT.
	 */
	ret = devm_pm_opp_set_config(pv->drm.dev,
		&(struct dev_pm_opp_config){
			.clk_names = (const char *[]){ "core", NULL },
			.regulator_names = (const char *[]){ "vrf18_2", NULL },
			.supported_hw = &version,
			.supported_hw_count = 1,
		});
	if (ret)
		return ret;

	ret = devm_pm_opp_of_add_table(pv->drm.dev);
	if (ret == -ENODEV || ret == -ENXIO) {
		dev_info(pv->drm.dev, "no OPP table, skipping DVFS\n");
		return 0;
	} else if (ret) {
		return ret;
	}

	cur_freq = clk_get_rate(pv->clk_core);
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
	if (pv->devfreq.devfreq) {
		devm_devfreq_remove_device(pv->drm.dev,
					   pv->devfreq.devfreq);
		pv->devfreq.devfreq = NULL;
	}
}

/*
 * DVFS must be quiescent around EVERY transition of the clock / reset /
 * MMU state, not only runtime suspend: recovery, explicit re-init and
 * teardown call hw_fini()/hw_init() directly.  devfreq_suspend_device()
 * is reference counted by the core, but our callers do not nest cleanly
 * (a failed re-init leaves the device "paused" until a later one
 * succeeds), so the pause state is tracked here with one flag and
 * pause/resume are idempotent.
 *
 * resume only takes effect while the hardware is up: a failed hw_init()
 * keeps DVFS paused until prismrv_hw_reinit() or runtime resume succeeds.
 */
void prismrv_devfreq_pause(struct prismrv_device *pv)
{
	if (pv->devfreq.devfreq &&
	    atomic_cmpxchg(&pv->devfreq_paused, 0, 1) == 0)
		devfreq_suspend_device(pv->devfreq.devfreq);
}

void prismrv_devfreq_resume(struct prismrv_device *pv)
{
	if (pv->devfreq.devfreq && READ_ONCE(pv->hw_ready) &&
	    atomic_cmpxchg(&pv->devfreq_paused, 1, 0) == 1)
		devfreq_resume_device(pv->devfreq.devfreq);
}
