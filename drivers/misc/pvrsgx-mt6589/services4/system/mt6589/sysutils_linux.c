// SPDX-License-Identifier: GPL-2.0-only

#include <linux/version.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/hardirq.h>
#include <linux/mutex.h>


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

extern unsigned int mt_gpufreq_cur_freq(void);
extern int g_pmic_cid;
extern void MtkSetKeepFreq(void);


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
    psTimingInfo->ui32CoreClockSpeed = mt_gpufreq_cur_freq()*1000; // SYS_SGX_CLOCK_SPEED;
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
	{
		return PVRSRV_OK;
	}

	PVR_DPF((PVR_DBG_MESSAGE, "EnableSGXClocks: Enabling SGX Clocks"));

#if defined(LDM_PLATFORM) && !defined(PVR_DRI_DRM_NOT_PCI) && defined(CONFIG_PM_RUNTIME)
	{

		int res = pm_runtime_get_sync(&gpsPVRLDMDev->dev);
		if (res < 0)
		{
			PVR_DPF((PVR_DBG_ERROR, "EnableSGXClocks: pm_runtime_get_sync failed (%d)", -res));
			return PVRSRV_ERROR_UNABLE_TO_ENABLE_CLOCK;
		}
	}
#endif

    MtkSetKeepFreq();

    if(( g_pmic_cid != 0) && (get_gpu_power_src()==0))
    {
        upmu_set_rg_vrf18_2_modeset(1); // force PWM mode
    }
//    printk("EnableSGXClocks ... Reg[0x%x]=0x%x\n",0x37E,upmu_get_reg_value(0x37E));


	ret = clk_prepare_enable(psSysData->clk_core);
	if (ret) return ret;

	ret = clk_prepare_enable(psSysData->clk_mem);
	if (ret) {
		clk_disable_unprepare(psSysData->clk_core);
		return ret;
	}

	ret = clk_prepare_enable(psSysData->clk_sys);
	if (ret) {
		clk_disable_unprepare(psSysData->clk_mem);
		clk_disable_unprepare(psSysData->clk_core);
		return ret;
	}

    //MFGReset
    {
        IMG_UINT32 val;
        DRV_WriteReg32(0xF0001200, DRV_Reg32(0xF0001200)&~0x400);// disable MFG way_en
        val = DRV_Reg32(SPM_MFG_PWR_CON);
        val = (val & ~0x1) | 0x10;
        DRV_WriteReg32(SPM_MFG_PWR_CON, val);// disable MFG clock and assert MFG reset
        DRV_WriteReg32(SPM_MFG_PWR_CON, DRV_Reg32(SPM_MFG_PWR_CON) | 0x00000002);// enable MFG ISO
        OSWaitus(1);
        DRV_WriteReg32(SPM_MFG_PWR_CON, DRV_Reg32(SPM_MFG_PWR_CON) & 0xFFFFFFFD);// disable MFG ISO
        DRV_WriteReg32(SPM_MFG_PWR_CON, DRV_Reg32(SPM_MFG_PWR_CON) & 0xFFFFFFEF);// enable MFG clock
        DRV_WriteReg32(SPM_MFG_PWR_CON, DRV_Reg32(SPM_MFG_PWR_CON) | 0x00000001);// dis-assert MFG reset
        OSWaitus(1);
        DRV_WriteReg32(0xF020600C, 0x1);// reset SGX544
        DRV_WriteReg32(0xF0206008, 0xf);// MFG clock on
        OSWaitus(1);
        DRV_WriteReg32(0xF0206004, 0xf);// MFG clock off
        OSWaitus(1);
        DRV_WriteReg32(0xF020600C, 0x0);// dis-assert reset SGX544
        DRV_WriteReg32(0xF0206008, 0xf);// MFG clock on
        OSWaitus(1);
        DRV_WriteReg32(0xF0001200, (DRV_Reg32(0xF0001200)& ~0x400)| 0x400);// enable MFG way_en
    }

    mt_gpufreq_gpu_clock_ratio(GPU_DVFS_CLOCK_RATIO_ON);

 	SysEnableSGXInterrupts(psSysData);

	atomic_set(&psSysSpecData->sSGXClocksEnabled, 1);

	return PVRSRV_OK;
}


IMG_VOID DisableSGXClocks(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;


	if (atomic_read(&psSysSpecData->sSGXClocksEnabled) == 0)
	{
		return;
	}

	PVR_DPF((PVR_DBG_MESSAGE, "DisableSGXClocks: Disabling SGX Clocks"));

	SysDisableSGXInterrupts(psSysData);

#if defined(LDM_PLATFORM) && !defined(PVR_DRI_DRM_NOT_PCI) && defined(CONFIG_PM_RUNTIME)
	{
		int res = pm_runtime_put_sync(&gpsPVRLDMDev->dev);
		if (res < 0)
		{
			PVR_DPF((PVR_DBG_ERROR, "DisableSGXClocks: pm_runtime_put_sync failed (%d)", -res));
		}
	}
#endif

	mt_gpufreq_gpu_clock_ratio(GPU_DVFS_CLOCK_RATIO_OFF);

	clk_disable_unprepare(psSysData->clk_sys);
	clk_disable_unprepare(psSysData->clk_mem);
	clk_disable_unprepare(psSysData->clk_core);

    if (( g_pmic_cid != 0) && (get_gpu_power_src()==0) && (subsys_is_on(SYS_MFG)==0))
    {
        upmu_set_rg_vrf18_2_modeset(0); // PFM mode
    }
//    printk("DisableSGXClocks ... Reg[0x%x]=0x%x\n",0x37E,upmu_get_reg_value(0x37E));

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
