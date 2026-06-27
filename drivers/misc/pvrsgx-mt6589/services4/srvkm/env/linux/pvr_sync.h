/* SPDX-License-Identifier: MIT OR GPL-2.0-only */

#ifndef _PVR_SYNC_H
#define _PVR_SYNC_H

#include <linux/seq_file.h>
#include <linux/version.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>

#include "pvr_sync_user.h"
#include "servicesint.h" /* PVRSRV_DEVICE_SYNC_OBJECT */

struct dma_fence;
struct sync_file;

/* services4 internal interface */

int PVRSyncDeviceInit(void);
void PVRSyncDeviceDeInit(void);
void PVRSyncUpdateAllSyncs(void);

PVRSRV_ERROR
PVRSyncPatchCCBKickSyncInfos(IMG_HANDLE    ahSyncs[SGX_MAX_SRC_SYNCS_TA],
			     PVRSRV_DEVICE_SYNC_OBJECT asDevSyncs[SGX_MAX_SRC_SYNCS_TA],
			     IMG_UINT32 *pui32NumSrcSyncs);
PVRSRV_ERROR
PVRSyncPatchTransferSyncInfos(IMG_HANDLE    ahSyncs[SGX_MAX_SRC_SYNCS_TA],
			      PVRSRV_DEVICE_SYNC_OBJECT asDevSyncs[SGX_MAX_SRC_SYNCS_TA],
			      IMG_UINT32 *pui32NumSrcSyncs);
PVRSRV_ERROR
PVRSyncFencesToSyncInfos(PVRSRV_KERNEL_SYNC_INFO *apsSyncs[],
			 IMG_UINT32 *pui32NumSyncs,
			 struct dma_fence *apsFence[SGX_MAX_SRC_SYNCS_TA]);

#endif /* _PVR_SYNC_H */
