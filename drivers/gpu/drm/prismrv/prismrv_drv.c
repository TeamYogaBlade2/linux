// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_drv.c — platform driver and DRM device registration.
 */
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/dma-fence.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>

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

/*
 * Every ioctl runs inside a drm_dev_enter()/drm_dev_exit() section.
 * drm_ioctl_kernel() only tests drm_dev_is_unplugged() once, before the
 * handler starts; drm_dev_unplug() waits for the sections opened here, and
 * nothing else.  Without this, an ioctl that had passed that test could keep
 * using the VA heap, the MMU, the CCB and the register window while
 * prismrv_remove() was tearing them down.  The submit_rwsem stays a
 * separate, inner lock for the *hardware state* (reset/resume vs. submit).
 */
#define PRISMRV_GUARDED_IOCTL(name)						\
static int name##_guarded(struct drm_device *dev, void *data,			\
			  struct drm_file *file)				\
{										\
	int idx, ret;								\
										\
	if (!drm_dev_enter(dev, &idx))						\
		return -ENODEV;							\
	ret = name(dev, data, file);						\
	drm_dev_exit(idx);							\
	return ret;								\
}

PRISMRV_GUARDED_IOCTL(prismrv_gem_create_ioctl)
PRISMRV_GUARDED_IOCTL(prismrv_gem_mmap_offset_ioctl)
PRISMRV_GUARDED_IOCTL(prismrv_submit_ioctl)
PRISMRV_GUARDED_IOCTL(prismrv_get_param_ioctl)

static const struct drm_ioctl_desc prismrv_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PRISMRV_GEM_CREATE, prismrv_gem_create_ioctl_guarded,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PRISMRV_GEM_MMAP_OFFSET, prismrv_gem_mmap_offset_ioctl_guarded,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PRISMRV_SUBMIT, prismrv_submit_ioctl_guarded,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(PRISMRV_GET_PARAM, prismrv_get_param_ioctl_guarded,
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

/*
 * Clock sequencing mirrors the vendor EnableSGXClocks()/DisableSGXClocks()
 * (services4/system/mt6589/sysutils_linux.c): HYD first, then G3D, MEM,
 * AXI; disabled in exactly the reverse order (AXI, MEM, G3D, HYD).  DT
 * lists core, mem, sys, hyd; "hyd" is therefore pulled out of the list.
 */
static void prismrv_clks_off(struct prismrv_device *pv, int upto)
{
	int i;

	for (i = upto - 1; i >= 0; i--)
		if (pv->clocks[i].clk != pv->clk_hyd)
			clk_disable_unprepare(pv->clocks[i].clk);
	if (pv->clk_hyd)
		clk_disable_unprepare(pv->clk_hyd);
}

static int prismrv_clks_on(struct prismrv_device *pv)
{
	int i, ret;

	if (pv->clk_hyd) {
		ret = clk_prepare_enable(pv->clk_hyd);
		if (ret)
			return ret;
	}
	for (i = 0; i < pv->nr_clocks; i++) {
		if (pv->clocks[i].clk == pv->clk_hyd)
			continue;
		ret = clk_prepare_enable(pv->clocks[i].clk);
		if (ret) {
			prismrv_clks_off(pv, i);
			return ret;
		}
	}
	return 0;
}

static void prismrv_va_release(struct drm_device *drm, void *arg)
{
	prismrv_va_fini(arg);
}

static void prismrv_teardown(struct platform_device *pdev);
static int prismrv_runtime_suspend(struct device *dev);
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
	mutex_init(&pv->mmu_lock);
	spin_lock_init(&pv->bo_list_lock);
	INIT_LIST_HEAD(&pv->bo_list);
	init_rwsem(&pv->submit_rwsem);
	prismrv_va_init(pv);
	ret = drmm_add_action_or_reset(&pv->drm, prismrv_va_release, pv);
	if (ret)
		return ret;
	INIT_WORK(&pv->recovery_work, prismrv_recovery_work);
	INIT_DELAYED_WORK(&pv->hang_work, prismrv_hang_work);
	init_waitqueue_head(&pv->init_wq);

	/*
	 * The BIF MMU (non-36bit variant) stores 32-bit physical page
	 * addresses in PDEs/PTEs and the DIR_LIST base register, and the
	 * uKernel/CCB/HostCtl are addressed with 32-bit pointers: every
	 * buffer the GPU can see must live below 4 GiB.
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "no 32-bit DMA\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pv->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pv->regs))
		return PTR_ERR(pv->regs);
	pv->regs_size = resource_size(res);

	ret = devm_clk_bulk_get_all(&pdev->dev, &pv->clocks);
	if (ret < 0)
		return ret;
	pv->nr_clocks = ret;
	for (ret = 0; ret < pv->nr_clocks; ret++) {
		if (!strcmp(pv->clocks[ret].id, "core"))
			pv->clk_core = pv->clocks[ret].clk;
		else if (!strcmp(pv->clocks[ret].id, "hyd"))
			pv->clk_hyd = pv->clocks[ret].clk;
	}
	if (!pv->clk_core)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing \"core\" clock\n");

	pv->rstc = devm_reset_control_get_optional_exclusive(&pdev->dev,
							     "g3d");
	if (IS_ERR(pv->rstc))
		return PTR_ERR(pv->rstc);

	/* the interrupt is mandatory: completion is interrupt driven and
	 * there is no polling fallback.  Propagate -EPROBE_DEFER etc. */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	pv->irq = irq;
	pv->fence_context = dma_fence_context_alloc(1);
	mutex_init(&pv->submit_order);
	ret = devm_request_threaded_irq(&pdev->dev, irq, prismrv_irq_handler,
					prismrv_irq_thread, IRQF_SHARED,
					dev_name(&pdev->dev), pv);
	if (ret)
		return ret;

	ret = drm_dev_register(&pv->drm, 0);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, pv);

	/*
	 * Runtime PM state machine: the device starts RPM_SUSPENDED with
	 * PM disabled.  Bring the hardware up by hand (clocks, reset,
	 * hw_init), and only if that worked tell the PM core the device
	 * is RPM_ACTIVE before enabling runtime PM, so the core's view
	 * matches the real hardware state.  On failure the clocks have
	 * been released again by runtime_resume() and the device stays
	 * RPM_SUSPENDED; the first submit retries through
	 * pm_runtime_resume_and_get().
	 */
	pm_runtime_set_autosuspend_delay(&pdev->dev, 100);
	pm_runtime_use_autosuspend(&pdev->dev);

	ret = prismrv_fw_load(pv);
	if (ret == 0)
		ret = prismrv_runtime_resume(&pdev->dev);
	if (ret) {
		dev_warn(&pdev->dev,
			 "GPU bring-up deferred (%d); will retry on first submit\n",
			 ret);
		pm_runtime_enable(&pdev->dev);
	} else {
		pm_runtime_set_active(&pdev->dev);
		pm_runtime_get_noresume(&pdev->dev);
		pm_runtime_enable(&pdev->dev);
		pm_runtime_mark_last_busy(&pdev->dev);
		pm_runtime_put_autosuspend(&pdev->dev);
	}

	ret = prismrv_devfreq_init(pv);
	if (ret == -EPROBE_DEFER) {
		prismrv_teardown(pdev);
		return ret;
	}

	dev_info(&pdev->dev, "%s probed\n", pv->info->name);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define PRISMRV_REMOVE_RET void
#else
#define PRISMRV_REMOVE_RET int
#endif

static void prismrv_teardown(struct platform_device *pdev)
{
	struct prismrv_device *pv = platform_get_drvdata(pdev);

	/*
	 * Unplug first: drm_dev_unplug() makes drm_dev_enter() fail for
	 * every new ioctl, so no new submissions can start.  In-flight
	 * ioctls keep the device alive through their own DRM file refs.
	 */
	drm_dev_unplug(&pv->drm);

	/* stop accepting recovery re-inits before tearing down hw */
	cancel_work_sync(&pv->recovery_work);

	/*
	 * Take the submit write-lock so any submit that slipped through
	 * before drm_dev_unplug() has returned from CCB/MMU before we
	 * start tearing them down.  After down_write() returns, no
	 * submit_ioctl() can be accessing CCB or MMU structures.
	 */
	down_write(&pv->submit_rwsem);
	mutex_lock(&pv->init_mutex);
	WRITE_ONCE(pv->hw_ready, false);
	mutex_unlock(&pv->init_mutex);
	up_write(&pv->submit_rwsem);

	/*
	 * Retire pending fences BEFORE disabling runtime PM: hw_fini
	 * signals them (with the device still awake), so waiters see an
	 * error instead of hanging.
	 */
	pm_runtime_get_sync(&pdev->dev);
	prismrv_devfreq_pause(pv);
	mutex_lock(&pv->init_mutex);
	prismrv_hw_fini(pv);   /* retires pending fences */
	prismrv_fw_release(pv); /* free uKernel DMA — device is going away */
	mutex_unlock(&pv->init_mutex);

	prismrv_devfreq_fini(pv);
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static PRISMRV_REMOVE_RET prismrv_remove(struct platform_device *pdev)
{
	prismrv_teardown(pdev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

static int prismrv_runtime_suspend(struct device *dev)
{
	struct prismrv_device *pv = dev_get_drvdata(dev);

	/*
	 * Stop DVFS first: devfreq changes the core clock and reads its
	 * rate from its own worker, which must not run while the clocks and
	 * the reset line are being switched off.  Resume re-enables it only
	 * after the hardware is back up.
	 */
	prismrv_devfreq_pause(pv);

	/*
	 * Take the submit write-lock so no new submits can start while
	 * we tear down, and wait for any in-flight submit to finish.
	 */
	down_write(&pv->submit_rwsem);
	mutex_lock(&pv->init_mutex);

	WRITE_ONCE(pv->hw_ready, false);

	/*
	 * Invalidate every BO's gpu_va before zeroing the page tables
	 * so that the next resume re-maps everything into the fresh MMU.
	 * Without this, pin_and_map() sees gpu_va != 0 and skips re-map,
	 * leaving the GPU to walk zeroed PTEs after resume.
	 */
	mutex_lock(&pv->mmu_lock);
	prismrv_mmu_invalidate_all_bos(pv);
	mutex_unlock(&pv->mmu_lock);

	/*
	 * Fully tear down hardware state: retires pending fences with
	 * -EIO, frees CCB/HostCtl/errata DMA buffers, tears down the
	 * MMU page tables.  This ensures resume starts from a clean
	 * slate, and that all fixed GPU-VA mappings (CCB, HostCtl,
	 * errata, uKernel) are re-established by hw_init().
	 */
	prismrv_hw_fini(pv);

	mutex_unlock(&pv->init_mutex);
	up_write(&pv->submit_rwsem);

	/* registers are off-limits from here on; wait out any handler */
	prismrv_power_down_irq(pv);
	/* assert the G3D reset line before gating the clocks */
	reset_control_assert(pv->rstc);
	prismrv_clks_off(pv, pv->nr_clocks);
	return 0;
}

static int prismrv_runtime_resume(struct device *dev)
{
	struct prismrv_device *pv = dev_get_drvdata(dev);
	int ret;

	ret = prismrv_clks_on(pv);
	if (ret)
		return ret;

	/* release the G3D block from reset (vendor EnableSGXClocks order) */
	reset_control_deassert(pv->rstc);
	udelay(2);
	WRITE_ONCE(pv->hw_powered, true);

	/*
	 * Re-initialise hardware under the submit write-lock so no
	 * submit can race the CCB/MMU rebuild.
	 */
	down_write(&pv->submit_rwsem);
	mutex_lock(&pv->init_mutex);

	if (!pv->hw_ready) {
		if (!pv->ukernel_cpu) {
			ret = prismrv_fw_load(pv);
			if (ret) {
				/*
				 * Firmware unavailable: fail the PM resume.
				 * The previous behaviour (ret=0, idle) left
				 * the GPU clocks on and hw_ready=false, mixing
				 * PM success with device unavailability.
				 *
				 * Returning an error here causes the PM core
				 * to mark the device as suspended again, so
				 * the next submit will trigger another resume
				 * attempt (which may succeed if the filesystem
				 * is now available).
				 */
				mutex_unlock(&pv->init_mutex);
				up_write(&pv->submit_rwsem);
				prismrv_power_down_irq(pv);
				reset_control_assert(pv->rstc);
				prismrv_clks_off(pv, pv->nr_clocks);
				dev_err(pv->drm.dev,
					"resume: firmware load failed (%d)\n",
					ret);
				return ret;
			}
		}
		ret = prismrv_hw_init(pv);
	}

	mutex_unlock(&pv->init_mutex);
	up_write(&pv->submit_rwsem);

	if (ret) {
		/* hw_init() already quiesced and released everything it
		 * set up; make sure no handler can touch the registers once
		 * the clocks are gone */
		prismrv_power_down_irq(pv);
		reset_control_assert(pv->rstc);
		prismrv_clks_off(pv, pv->nr_clocks);
		return ret;
	}
	prismrv_devfreq_resume(pv);
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
