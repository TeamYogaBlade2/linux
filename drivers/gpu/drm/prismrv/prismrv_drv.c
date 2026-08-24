// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_drv.c — platform driver and DRM device registration.
 */
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

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
	{ .compatible = "img,powervr-sgx540", .data = &prismrv_sgx544_info },
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
	.ioctls = prismrv_ioctls,
	.num_ioctls = ARRAY_SIZE(prismrv_ioctls),
	.fops = &prismrv_fops,
	.name = "prismrv",
	.desc = "PrismRV SGX",
	.major = 1,
	.minor = 0,
};

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
	init_waitqueue_head(&pv->init_wq);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pv->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pv->regs))
		return PTR_ERR(pv->regs);
	pv->regs_size = resource_size(res);

	ret = devm_clk_bulk_get_all_enabled(&pdev->dev, &pv->clocks);
	if (ret < 0)
		return ret;
	pv->nr_clocks = ret;

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
	if (ret == 0)
		ret = prismrv_hw_init(pv);
	else
		dev_warn(&pdev->dev,
			 "firmware unavailable, GPU left uninitialised (%d)\n",
			 ret);

	prismrv_devfreq_init(pv);

	dev_info(&pdev->dev, "%s probed\n", pv->info->name);
	return 0;
}

static void prismrv_remove(struct platform_device *pdev)
{
	struct prismrv_device *pv = platform_get_drvdata(pdev);

	drm_dev_unplug(&pv->drm);
}

static const struct dev_pm_ops prismrv_pm_ops = {
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
