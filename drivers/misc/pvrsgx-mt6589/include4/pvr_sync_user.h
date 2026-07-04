// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _PVR_SYNC_USER_H_
#define _PVR_SYNC_USER_H_

#include <linux/ioctl.h>

#ifdef __KERNEL__
#include "sgxapi_km.h"
#else
#include "sgxapi.h"
#endif

#include "servicesext.h" // PVRSRV_SYNC_DATA
#include "img_types.h"

/* This matches the sw_sync create ioctl data */
struct PVR_SYNC_CREATE_IOCTL_DATA
{
	/* Input: Name of this sync pt. Passed to base sync driver. */
	char	name[32];

	/* Input: An fd from a previous call to ALLOC ioctl. Cannot be <0. */
	__s32	allocdSyncInfo;

	/* Output: An fd returned from the CREATE ioctl. */
	__s32	fence;
};

struct PVR_SYNC_ALLOC_IOCTL_DATA
{
	/* Output: An fd returned from the ALLOC ioctl */
	__s32 fence;

	/* Output: IMG_TRUE if the timeline looked idle at alloc time */
	__u32 bTimelineIdle;
};

#define PVR_SYNC_DEBUG_MAX_POINTS 3

typedef struct
{
	/* Output: A globally unique stamp/ID for the sync */
	IMG_UINT64 ui64Stamp;

	/* Output: The WOP snapshot for the sync */
	IMG_UINT32 ui32WriteOpsPendingSnapshot;
}
PVR_SYNC_DEBUG;

struct PVR_SYNC_DEBUG_IOCTL_DATA
{
	/* Input: Fence to acquire debug for */
	int						iFenceFD;

	/* Output: Number of points merged into this fence */
	IMG_UINT32				ui32NumPoints;

	struct
	{
		/* Output: Metadata for sync point */
		PVR_SYNC_DEBUG		sMetaData;

		/* Output: 'Live' sync information. */
		PVRSRV_SYNC_DATA	sSyncData;
	}
	sSync[PVR_SYNC_DEBUG_MAX_POINTS];
};

#define PVR_SYNC_IOC_MAGIC	'W'

#define PVR_SYNC_IOC_CREATE_FENCE \
	_IOWR(PVR_SYNC_IOC_MAGIC, 0, struct PVR_SYNC_CREATE_IOCTL_DATA)

#define PVR_SYNC_IOC_DEBUG_FENCE \
	_IOWR(PVR_SYNC_IOC_MAGIC, 1, struct PVR_SYNC_DEBUG_IOCTL_DATA)

#define PVR_SYNC_IOC_ALLOC_FENCE \
	_IOWR(PVR_SYNC_IOC_MAGIC, 2, struct PVR_SYNC_ALLOC_IOCTL_DATA)

#define PVRSYNC_MODNAME "pvr_sync"

#endif /* _PVR_SYNC_USER_H_ */
