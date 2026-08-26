// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_drv.c — platform driver and DRM device registration.
 */
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>

#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_managed.h>
#include <drm/drm_of.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

static const struct prismrv_chip_info prismrv_sgx544_info = {
	.name = "sgx544",
	.core_id = PRISMRV_CORE_SGX544,
	.num_cores = 1,
	.has_isp2 = true,
	.has_multi_event_kick = false,
};

static const struct of_device_id prismrv_of_match[] = {
	{ .compatible = "mediatek,mt6589-gpu", .data = &prismrv_sgx544_info },
	{ .compatible = "img,powervr-sgx544", .data = &prismrv_sgx544_info },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, prismrv_of_match);

static const struct drm_ioctl_desc prismrv_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PRISMRV_GEM_CREATE, prismrv_gem_create_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PRISMRV_GEM_MMAP_OFFSET, prismrv_gem_mmap_offset_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PRISMRV_SUBMIT, prismrv_submit_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PRISMRV_GET_PARAM, prismrv_get_param_ioctl,
			  DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(prismrv_fops);

static const struct drm_driver prismrv_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	.gem_create_object = prismrv_gem_create_object,
	.ioctls = prismrv_ioctls,
	.num_ioctls = ARRAY_SIZE(prismrv_ioctls),
	.fops = &prismrv_fops,
	.name = "prismrv",
	.desc = "PrismRV SGX",
	.major = 1,
	.minor = 0,
};

static int prismrv_runtime_resume(struct device *dev);

static int prismrv_probe(struct platform_device *pdev)
{
	struct prismrv_device *pv;
	struct resource *res;
	int irq, ret;

	pv = devm_drm_dev_alloc(&pdev->dev, &prismrv_drm_driver,
				struct prismrv_device, drm);
	if (IS_ERR(pv))
		return PTR_ERR(pv);
	pv->pdev = pdev;
	pv->info = of_device_get_match_data(&pdev->dev);
	spin_lock_init(&pv->event_lock);
	INIT_LIST_HEAD(&pv->pending_fences);
	mutex_init(&pv->init_mutex);
	INIT_WORK(&pv->recovery_work, prismrv_recovery_work);
	init_waitqueue_head(&pv->init_wq);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pv->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pv->regs))
		return PTR_ERR(pv->regs);
	pv->regs_size = resource_size(res);

	ret = devm_clk_bulk_get_all(&pdev->dev, &pv->clocks);
	if (ret < 0)
		return ret;
	pv->nr_clocks = ret;

	pv->rstc = devm_reset_control_get_optional_exclusive(&pdev->dev,
							     "g3d");
	if (IS_ERR(pv->rstc))
		return PTR_ERR(pv->rstc);

	irq = platform_get_irq(pdev, 0);
	if (irq >= 0) {
		ret = devm_request_irq(&pdev->dev, irq, prismrv_irq_handler,
				       IRQF_SHARED, dev_name(&pdev->dev), pv);
		if (ret)
			return ret;
	}

	ret = drm_dev_register(&pv->drm, 0);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, pv);

	/* firmware + hardware bring-up can be deferred if the firmware
	 * files are not yet installed */
	ret = prismrv_fw_load(pv);
	if (ret == 0) {
		pm_runtime_get_noresume(&pdev->dev);
		ret = prismrv_runtime_resume(&pdev->dev);
		pm_runtime_put_noidle(&pdev->dev);
	}
	if (ret)
		dev_warn(&pdev->dev,
			 "GPU bring-up deferred (%d); will retry on open\n",
			 ret);

	/* runtime autosuspend: GPU idles 100ms after the last submit */
	pm_runtime_set_autosuspend_delay(&pdev->dev, 100);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_mark_last_busy(&pdev->dev);
	pm_runtime_put_autosuspend(&pdev->dev);

	ret = prismrv_devfreq_init(pv);
	if (ret == -EPROBE_DEFER)
		goto err_devfreq;

	dev_info(&pdev->dev, "%s probed\n", pv->info->name);
	return 0;

err_devfreq:
	drm_dev_unplug(&pv->drm);
	return ret;
}

static void prismrv_remove(struct platform_device *pdev)
{
	struct prismrv_device *pv = platform_get_drvdata(pdev);

	/*
	 * Unplug first: drm_dev_unplug() makes drm_dev_enter() fail for
	 * every new ioctl, so no new submissions can start.  In-flight
	 * ioctls keep the device alive through their own references.
	 */
	drm_dev_unplug(&pv->drm);

	/* stop accepting recovery re-inits before tearing down hw */
	cancel_work_sync(&pv->recovery_work);

	mutex_lock(&pv->init_mutex);
	pv->hw_ready = false;
	mutex_unlock(&pv->init_mutex);

	/*
	 * Retire pending fences BEFORE disabling runtime PM: hw_fini
	 * signals them (with the device still awake), so waiters see an
	 * error instead of hanging, and nobody can touch pv afterwards
	 * through a fence callback.
	 */
	pm_runtime_get_sync(&pdev->dev);
	mutex_lock(&pv->init_mutex);
	prismrv_hw_fini(pv);   /* retires pending fences */
	mutex_unlock(&pv->init_mutex);

	/*
	 * Balance the reference taken above (and the one probe held)
	 * BEFORE disabling runtime PM, otherwise the usage count leaks
	 * and the module unload warns.  devfreq is unregistered first so
	 * its 50ms poll can no longer touch the clocks we are about to
	 * gate.
	 */
	prismrv_devfreq_fini(pv);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static int prismrv_runtime_suspend(struct device *dev)
{
	struct prismrv_device *pv = dev_get_drvdata(dev);

	pv->hw_ready = false;

	/* assert the G3D reset line before gating the clocks */
	reset_control_assert(pv->rstc);
	clk_bulk_disable_unprepare(pv->nr_clocks, pv->clocks);
	return 0;
}

static int prismrv_runtime_resume(struct device *dev)
{
	struct prismrv_device *pv = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(pv->nr_clocks, pv->clocks);
	if (ret)
		return ret;

	/* release the G3D block from reset (vendor EnableSGXClocks order) */
	reset_control_deassert(pv->rstc);
	udelay(2);

	if (!pv->hw_ready) {
		mutex_lock(&pv->init_mutex);
		/*
		 * Retry the firmware load: the first attempt at probe
		 * may have run before the rootfs mounted.  Without this
		 * a probe-time failure is permanent despite the probe
		 * message claiming a retry.
		 */
		if (!pv->ukernel_cpu)
			prismrv_fw_load(pv);
		if (pv->ukernel_cpu)
			ret = prismrv_hw_init(pv);
		else
			ret = 0;   /* firmware still unavailable: idle */
		mutex_unlock(&pv->init_mutex);
		if (ret) {
			clk_bulk_disable_unprepare(pv->nr_clocks,
						   pv->clocks);
			return ret;
		}
	}
	return 0;
}

static const struct dev_pm_ops prismrv_pm_ops = {
	RUNTIME_PM_OPS(prismrv_runtime_suspend, prismrv_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver prismrv_platform_driver = {
	.probe = prismrv_probe,
	.remove = prismrv_remove,
	.driver = {
		.name = "prismrv",
		.of_match_table = prismrv_of_match,
		.pm = &prismrv_pm_ops,
	},
};
module_platform_driver(prismrv_platform_driver);

MODULE_AUTHOR("PrismRV project");
MODULE_DESCRIPTION("DRM driver for PowerVR SGX GPUs");
MODULE_LICENSE("GPL");
