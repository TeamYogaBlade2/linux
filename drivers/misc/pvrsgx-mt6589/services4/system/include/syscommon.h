// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _SYSCOMMON_H
#define _SYSCOMMON_H

#include <linux/platform_device.h>

#include "sysconfig.h"      /* System specific system defines */
#include "sysinfo.h"		/* globally accessible system info */
#include "servicesint.h"
#include "queue.h"
#include "power.h"
#include "resman.h"
#include "ra.h"
#include "device.h"
#include "buffer_manager.h"
#include "pvr_debug.h"
#include "services.h"

/*!
 ****************************************************************************
	device id management structure
 ****************************************************************************/
typedef struct _SYS_DEVICE_ID_TAG
{
	IMG_UINT32	uiID;
	IMG_BOOL	bInUse;

} SYS_DEVICE_ID;


/*
	the max number of independent local backing stores services supports
	(grow this number if ever required)
*/
#define SYS_MAX_LOCAL_DEVMEM_ARENAS	4

typedef IMG_HANDLE (*PFN_HTIMER_CREATE) (IMG_VOID);
typedef IMG_UINT32 (*PFN_HTIMER_GETUS) (IMG_HANDLE);
typedef IMG_VOID (*PFN_HTIMER_DESTROY) (IMG_HANDLE);
/*!
 ****************************************************************************
	Top level system data structure
 ****************************************************************************/
typedef struct _SYS_DATA_TAG_
{
    IMG_UINT32                  ui32NumDevices;      	   	/*!< number of devices in system */
	SYS_DEVICE_ID				sDeviceID[SYS_DEVICE_COUNT];
    PVRSRV_DEVICE_NODE			*psDeviceNodeList;			/*!< list of private device info structures */
    PVRSRV_POWER_DEV			*psPowerDeviceList;			/*!< list of devices registered with the power manager */
	PVRSRV_RESOURCE				sPowerStateChangeResource;	/*!< lock for power state transitions */
   	PVRSRV_SYS_POWER_STATE		eCurrentPowerState;			/*!< current Kernel services power state */
   	PVRSRV_SYS_POWER_STATE		eFailedPowerState;			/*!< Kernel services power state (Failed to transition to) */
   	IMG_UINT32		 			ui32CurrentOSPowerState;	/*!< current OS specific power state */
    PVRSRV_QUEUE_INFO           *psQueueList;           	/*!< list of all command queues in the system */
   	PVRSRV_KERNEL_SYNC_INFO 	*psSharedSyncInfoList;		/*!< list of cross process syncinfos */
    IMG_PVOID                   pvEnvSpecificData;      	/*!< Environment specific data */
    IMG_PVOID                   pvSysSpecificData;    	  	/*!< Unique to system, accessible at system layer only */
	PVRSRV_RESOURCE				sQProcessResource;			/*!< Command Q processing access lock */
	IMG_VOID					*pvSOCRegsBase;				/*!< SOC registers base linear address */
    IMG_HANDLE                  hSOCTimerRegisterOSMemHandle; /*!< SOC Timer register (if present) */
	IMG_UINT32					*pvSOCTimerRegisterKM;		/*!< SOC Timer register (if present) */
	IMG_VOID					*pvSOCClockGateRegsBase;	/*!< SOC Clock gating registers (if present) */
	IMG_UINT32					ui32SOCClockGateRegsSize;

	struct _DEVICE_COMMAND_DATA_ *apsDeviceCommandData[SYS_DEVICE_COUNT];
															/*!< command complete data and callback function store for every command for every device */

	RA_ARENA					*apsLocalDevMemArena[SYS_MAX_LOCAL_DEVMEM_ARENAS]; /*!< RA Arenas for local device memory heap management */

    IMG_CHAR                    *pszVersionString;          /*!< Human readable string showing relevent system version info */
	PVRSRV_EVENTOBJECT			*psGlobalEventObject;		/*!< OS Global Event Object */

	PVRSRV_MISC_INFO_CPUCACHEOP_TYPE ePendingCacheOpType;	/*!< Deferred CPU cache op control */

	PFN_HTIMER_CREATE	pfnHighResTimerCreate;
	PFN_HTIMER_GETUS	pfnHighResTimerGetus;
	PFN_HTIMER_DESTROY	pfnHighResTimerDestroy;

	struct clk *clk_core;
	struct clk *clk_mem;
	struct clk *clk_sys;
} SYS_DATA;


/****************************************************************************
 *	common function prototypes
 ****************************************************************************/

#if defined (CUSTOM_DISPLAY_SEGMENT)
PVRSRV_ERROR SysGetDisplaySegmentAddress (IMG_VOID *pvDevInfo, IMG_VOID *pvPhysicalAddress, IMG_UINT32 *pui32Length);
#endif

PVRSRV_ERROR SysInitialise(struct platform_device *pdev);
PVRSRV_ERROR SysFinalise(IMG_VOID);

PVRSRV_ERROR SysDeinitialise(SYS_DATA *psSysData);
PVRSRV_ERROR SysGetDeviceMemoryMap(PVRSRV_DEVICE_TYPE eDeviceType,
									IMG_VOID **ppvDeviceMap);

IMG_VOID SysRegisterExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode);
IMG_VOID SysRemoveExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode);

IMG_UINT32 SysGetInterruptSource(SYS_DATA			*psSysData,
								 PVRSRV_DEVICE_NODE *psDeviceNode);

IMG_VOID SysClearInterrupts(SYS_DATA* psSysData, IMG_UINT32 ui32ClearBits);

PVRSRV_ERROR SysResetDevice(IMG_UINT32 ui32DeviceIndex);

PVRSRV_ERROR SysSystemPrePowerState(PVRSRV_SYS_POWER_STATE eNewPowerState);
PVRSRV_ERROR SysSystemPostPowerState(PVRSRV_SYS_POWER_STATE eNewPowerState);
PVRSRV_ERROR SysDevicePrePowerState(IMG_UINT32 ui32DeviceIndex,
									PVRSRV_DEV_POWER_STATE eNewPowerState,
									PVRSRV_DEV_POWER_STATE eCurrentPowerState);
PVRSRV_ERROR SysDevicePostPowerState(IMG_UINT32 ui32DeviceIndex,
									 PVRSRV_DEV_POWER_STATE eNewPowerState,
									 PVRSRV_DEV_POWER_STATE eCurrentPowerState);

#if defined(SYS_SUPPORTS_SGX_IDLE_CALLBACK)
IMG_VOID SysSGXIdleTransition(IMG_BOOL bSGXIdle);
#endif /* SYS_SUPPORTS_SGX_IDLE_CALLBACK */

#if defined(SYS_CUSTOM_POWERLOCK_WRAP)
PVRSRV_ERROR SysPowerLockWrap(IMG_BOOL bTryLock);
IMG_VOID SysPowerLockUnwrap(IMG_VOID);
#endif

PVRSRV_ERROR SysOEMFunction (	IMG_UINT32	ui32ID,
								IMG_VOID	*pvIn,
								IMG_UINT32  ulInSize,
								IMG_VOID	*pvOut,
								IMG_UINT32	ulOutSize);


IMG_DEV_PHYADDR SysCpuPAddrToDevPAddr (PVRSRV_DEVICE_TYPE eDeviceType, IMG_CPU_PHYADDR cpu_paddr);
IMG_DEV_PHYADDR SysSysPAddrToDevPAddr (PVRSRV_DEVICE_TYPE eDeviceType, IMG_SYS_PHYADDR SysPAddr);
IMG_SYS_PHYADDR SysDevPAddrToSysPAddr (PVRSRV_DEVICE_TYPE eDeviceType, IMG_DEV_PHYADDR SysPAddr);
IMG_CPU_PHYADDR SysSysPAddrToCpuPAddr (IMG_SYS_PHYADDR SysPAddr);
IMG_SYS_PHYADDR SysCpuPAddrToSysPAddr (IMG_CPU_PHYADDR cpu_paddr);
#if defined(PVR_LMA)
IMG_BOOL SysVerifyCpuPAddrToDevPAddr (PVRSRV_DEVICE_TYPE eDeviceType, IMG_CPU_PHYADDR CpuPAddr);
IMG_BOOL SysVerifySysPAddrToDevPAddr (PVRSRV_DEVICE_TYPE eDeviceType, IMG_SYS_PHYADDR SysPAddr);
#endif

extern SYS_DATA* gpsSysData;


#if !defined(USE_CODE)

/*!
******************************************************************************

 @Function	SysAcquireData

 @Description returns reference to to sysdata
 				creating one on first call

 @Input    ppsSysData - pointer to copy reference into

 @Return   ppsSysData updated

******************************************************************************/
static INLINE IMG_VOID SysAcquireData(SYS_DATA **ppsSysData)
{
	/* Copy pointer back system information pointer */
	*ppsSysData = gpsSysData;

	/*
		Verify we've not been called before being initialised. Instinctively
		we should do this check first, but in the failing case we'll just write
		null back and the compiler won't warn about an uninitialised varible.
	*/
	PVR_ASSERT (gpsSysData != IMG_NULL);
}


/*!
******************************************************************************

 @Function	SysAcquireDataNoCheck

 @Description returns reference to to sysdata
 				creating one on first call

 @Input    none

 @Return   psSysData - pointer to copy reference into

******************************************************************************/
static INLINE SYS_DATA * SysAcquireDataNoCheck(IMG_VOID)
{
	/* return pointer back system information pointer */
	return gpsSysData;
}


/*!
******************************************************************************

 @Function	SysInitialiseCommon

 @Description Performs system initialisation common to all systems

 @Input    psSysData - pointer to system data

 @Return   PVRSRV_ERROR  :

******************************************************************************/
static INLINE PVRSRV_ERROR SysInitialiseCommon(SYS_DATA *psSysData)
{
	PVRSRV_ERROR	eError;

	/* Initialise Services */
	eError = PVRSRVInit(psSysData);

	return eError;
}

/*!
******************************************************************************

 @Function	SysDeinitialiseCommon

 @Description Performs system deinitialisation common to all systems

 @Input    psSysData - pointer to system data

 @Return   PVRSRV_ERROR  :

******************************************************************************/
static INLINE IMG_VOID SysDeinitialiseCommon(SYS_DATA *psSysData)
{
	/* De-initialise Services */
	PVRSRVDeInit(psSysData);

	OSDestroyResource(&psSysData->sPowerStateChangeResource);
}
#endif /* !defined(USE_CODE) */


#define	SysReadHWReg(p, o) OSReadHWReg(p, o)
#define SysWriteHWReg(p, o, v) OSWriteHWReg(p, o, v)

static INLINE IMG_HANDLE SysHighResTimerCreate(IMG_VOID)
{
	SYS_DATA *psSysData;

	SysAcquireData(&psSysData);
	return psSysData->pfnHighResTimerCreate();
}

static INLINE IMG_UINT32 SysHighResTimerGetus(IMG_HANDLE hTimer)
{
	SYS_DATA *psSysData;

	SysAcquireData(&psSysData);
	return psSysData->pfnHighResTimerGetus(hTimer);
}

static INLINE IMG_VOID SysHighResTimerDestroy(IMG_HANDLE hTimer)
{
	SYS_DATA *psSysData;

	SysAcquireData(&psSysData);
	psSysData->pfnHighResTimerDestroy(hTimer);
}
#endif

/*****************************************************************************
 End of file (syscommon.h)
*****************************************************************************/
