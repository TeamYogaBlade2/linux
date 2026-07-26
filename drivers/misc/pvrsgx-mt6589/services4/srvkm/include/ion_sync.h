// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "img_defs.h"
#include "img_types.h"
#include "servicesint.h"

#ifndef __ION_SYNC_H__
#define __ION_SYNC_H__

typedef struct _PVRSRV_ION_SYNC_INFO_ {
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
	IMG_HANDLE				hUnique;
	IMG_UINT32				ui32RefCount;
	IMG_UINT64				ui64Stamp;
} PVRSRV_ION_SYNC_INFO;

PVRSRV_ERROR PVRSRVIonBufferSyncAcquire(IMG_HANDLE hUnique,
										IMG_HANDLE hDevCookie,
										IMG_HANDLE hDevMemContext,
										PVRSRV_ION_SYNC_INFO **ppsIonSyncInfo);

IMG_VOID PVRSRVIonBufferSyncRelease(PVRSRV_ION_SYNC_INFO *psIonSyncInfo);

static INLINE PVRSRV_KERNEL_SYNC_INFO *IonBufferSyncGetKernelSyncInfo(PVRSRV_ION_SYNC_INFO *psIonSyncInfo)
{
	return psIonSyncInfo->psSyncInfo;
}

static INLINE IMG_UINT64 IonBufferSyncGetStamp(PVRSRV_ION_SYNC_INFO *psIonSyncInfo)
{
	return psIonSyncInfo->ui64Stamp;
}

#endif /* __ION_SYNC_H__ */
