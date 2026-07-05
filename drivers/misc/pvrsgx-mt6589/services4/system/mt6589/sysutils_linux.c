// SPDX-License-Identifier: GPL-2.0-only

#include <linux/version.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/hardirq.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/delay.h>


#include "sgxdefs.h"
#include "services_headers.h"
#include "sysinfo.h"
#include "sgxapi_km.h"
#include "sysconfig.h"
#include "sgxinfokm.h"
#include "syslocal.h"

#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define SPM_MFG_PWR_CON 0xF0006214

#define	ONE_MHZ	1000000
#define	HZ_TO_MHZ(m) ((m) / ONE_MHZ)

#define SGX_PARENT_CLOCK "core_ck"

#if defined(LDM_PLATFORM) && !defined(PVR_DRI_DRM_NOT_PCI)
extern struct platform_device *gpsPVRLDMDev;
#endif


static PVRSRV_ERROR PowerLockWrap(SYS_SPECIFIC_DATA *psSysSpecData, IMG_BOOL bTryLock)
{
	if (!in_interrupt())
	{
		if (bTryLock)
		{
			int locked = mutex_trylock(&psSysSpecData->sPowerLock);
			if (locked == 0)
			{
				return PVRSRV_ERROR_RETRY;
			}
		}
		else
		{
		mutex_lock(&psSysSpecData->sPowerLock);
	}
}

	return PVRSRV_OK;
}

static IMG_VOID PowerLockUnwrap(SYS_SPECIFIC_DATA *psSysSpecData)
{
	if (!in_interrupt())
	{
		mutex_unlock(&psSysSpecData->sPowerLock);
	}
}

PVRSRV_ERROR SysPowerLockWrap(IMG_BOOL bTryLock)
{
	SYS_DATA	*psSysData;

	SysAcquireData(&psSysData);

	return PowerLockWrap(psSysData->pvSysSpecificData, bTryLock);
}

IMG_VOID SysPowerLockUnwrap(IMG_VOID)
{
	SYS_DATA	*psSysData;

	SysAcquireData(&psSysData);

	PowerLockUnwrap(psSysData->pvSysSpecificData);
}

IMG_BOOL WrapSystemPowerChange(SYS_SPECIFIC_DATA *psSysSpecData)
{
	return IMG_TRUE;
}

IMG_VOID UnwrapSystemPowerChange(SYS_SPECIFIC_DATA *psSysSpecData)
{
}

#if defined(SGX_DYNAMIC_TIMING_INFO)
IMG_VOID SysGetSGXTimingInformation(SGX_TIMING_INFORMATION *psTimingInfo)
{
	PVR_ASSERT(atomic_read(&gpsSysSpecificData->sSGXClocksEnabled) != 0);
	psTimingInfo->ui32CoreClockSpeed = clk_get_rate(gpsSysData->clk_core);
	psTimingInfo->ui32HWRecoveryFreq = SYS_SGX_HWRECOVERY_TIMEOUT_FREQ;
	psTimingInfo->ui32uKernelFreq = SYS_SGX_PDS_TIMER_FREQ;
	psTimingInfo->bEnableActivePM = IMG_TRUE;
	psTimingInfo->ui32ActivePowManLatencyms = SYS_SGX_ACTIVE_POWER_LATENCY_MS;
}
#endif

PVRSRV_ERROR EnableSGXClocks(SYS_DATA *psSysData)
{
	int ret;
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;

	if (atomic_read(&psSysSpecData->sSGXClocksEnabled) != 0)
		return PVRSRV_OK;

	PVR_DPF((PVR_DBG_MESSAGE, "EnableSGXClocks: Enabling SGX Clocks"));

	ret = pm_runtime_get_sync(&gpsPVRLDMDev->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(&gpsPVRLDMDev->dev);
		PVR_DPF((PVR_DBG_ERROR, "EnableSGXClocks: pm_runtime_get_sync failed (%d)", -ret));
		return PVRSRV_ERROR_UNABLE_TO_ENABLE_CLOCK;
	}

	if (psSysData->vdd_reg)
		regulator_set_mode(psSysData->vdd_reg, REGULATOR_MODE_FAST);

	ret = clk_prepare_enable(psSysData->clk_hyd);
	if (ret)
		return ret;

	ret = clk_prepare_enable(psSysData->clk_core);
	if (ret)
		goto err_core;

	ret = clk_prepare_enable(psSysData->clk_mem);
	if (ret)
		goto err_mem;

	ret = clk_prepare_enable(psSysData->clk_sys);
	if (ret)
		goto err_sys;

	reset_control_assert(psSysData->rstc);
	usleep_range(1, 2);
	reset_control_deassert(psSysData->rstc);
	usleep_range(1, 2);

	SysEnableSGXInterrupts(psSysData);

	if (psSysData->devfreq) {
		unsigned long flags;
		spin_lock_irqsave(&psSysData->gpu_ratio_lock, flags);
		psSysData->gpu_last_poll = ktime_get_ns();
		psSysData->gpu_accumulated_busy = 0;
		psSysData->gpu_currently_busy = false;
		spin_unlock_irqrestore(&psSysData->gpu_ratio_lock, flags);
		devfreq_resume_device(psSysData->devfreq);
	}

	atomic_set(&psSysSpecData->sSGXClocksEnabled, 1);
	return PVRSRV_OK;

err_sys:
	clk_disable_unprepare(psSysData->clk_mem);
err_mem:
	clk_disable_unprepare(psSysData->clk_core);
err_core:
	clk_disable_unprepare(psSysData->clk_hyd);
	pm_runtime_put_sync(&gpsPVRLDMDev->dev);
	return ret;
}

IMG_VOID DisableSGXClocks(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;

	if (atomic_read(&psSysSpecData->sSGXClocksEnabled) == 0)
		return;

	PVR_DPF((PVR_DBG_MESSAGE, "DisableSGXClocks: Disabling SGX Clocks"));

	if (psSysData->devfreq)
		devfreq_suspend_device(psSysData->devfreq);

	SysDisableSGXInterrupts(psSysData);

	reset_control_assert(psSysData->rstc);

	clk_disable_unprepare(psSysData->clk_sys);
	clk_disable_unprepare(psSysData->clk_mem);
	clk_disable_unprepare(psSysData->clk_core);
	clk_disable_unprepare(psSysData->clk_hyd);

	pm_runtime_put_sync(&gpsPVRLDMDev->dev);

	if (psSysData->vdd_reg)
		regulator_set_mode(psSysData->vdd_reg, REGULATOR_MODE_NORMAL);

	atomic_set(&psSysSpecData->sSGXClocksEnabled, 0);
}

static PVRSRV_ERROR AcquireGPTimer(SYS_SPECIFIC_DATA *psSysSpecData)
{
	PVR_UNREFERENCED_PARAMETER(psSysSpecData);

	return PVRSRV_OK;
}
static void ReleaseGPTimer(SYS_SPECIFIC_DATA *psSysSpecData)
{
	PVR_UNREFERENCED_PARAMETER(psSysSpecData);
}
//#endif

PVRSRV_ERROR EnableSystemClocks(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;

	PVR_TRACE(("EnableSystemClocks: Enabling System Clocks"));

	if (!psSysSpecData->bSysClocksOneTimeInit)
	{
		mutex_init(&psSysSpecData->sPowerLock);

		atomic_set(&psSysSpecData->sSGXClocksEnabled, 0);

		psSysSpecData->bSysClocksOneTimeInit = IMG_TRUE;
	}

	return AcquireGPTimer(psSysSpecData);
}

IMG_VOID DisableSystemClocks(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;

	PVR_TRACE(("DisableSystemClocks: Disabling System Clocks"));


	DisableSGXClocks(psSysData);

	ReleaseGPTimer(psSysSpecData);
}

PVRSRV_ERROR SysPMRuntimeRegister(void)
{
#if defined(LDM_PLATFORM) && !defined(PVR_DRI_DRM_NOT_PCI)
	pm_runtime_enable(&gpsPVRLDMDev->dev);
#endif
	return PVRSRV_OK;
}

PVRSRV_ERROR SysPMRuntimeUnregister(void)
{
#if defined(LDM_PLATFORM) && !defined(PVR_DRI_DRM_NOT_PCI)
	pm_runtime_disable(&gpsPVRLDMDev->dev);
#endif
	return PVRSRV_OK;
}

#if 0 //#if defined(MTK_USE_GDC)
IMG_UINT32 SysCacheBypass(IMG_UINT32 ui32RegVal)
{
	if (get_chip_eco_ver() == CHIP_E1) {
		/* E1 chip */
		ui32RegVal |= 0x80U;
	} else {
		/* E2 chip */
		ui32RegVal |= 0x80U;
	}
	return ui32RegVal;
}
IMG_VOID OnSGXResetDone()
{
	SYSRAM_MFG_SWITCH_BANK(true);
	//PVRSRVReleasePrintf("MFG Reset Done");
}
#endif
