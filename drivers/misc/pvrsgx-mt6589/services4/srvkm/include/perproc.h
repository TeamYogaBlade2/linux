// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __PERPROC_H__
#define __PERPROC_H__

#if defined (__cplusplus)
extern "C" {
#endif

#include "img_types.h"
#include "resman.h"

#include "handle.h"

typedef struct _PVRSRV_PER_PROCESS_DATA_
{
	IMG_UINT32		ui32PID;
	IMG_HANDLE		hBlockAlloc;
	PRESMAN_CONTEXT 	hResManContext;
	IMG_HANDLE		hPerProcData;
	PVRSRV_HANDLE_BASE 	*psHandleBase;
#if defined (PVR_SECURE_HANDLES)
	/* Handles are being allocated in batches */
	IMG_BOOL		bHandlesBatched;
#endif  /* PVR_SECURE_HANDLES */
	IMG_UINT32		ui32RefCount;

	/* True if the process is the initialisation server. */
	IMG_BOOL		bInitProcess;
#if defined(PDUMP)
	/* True if pdump data from the process is 'persistent' */
	IMG_BOOL		bPDumpPersistent;
#if defined(SUPPORT_PDUMP_MULTI_PROCESS)
	/* True if this process is marked for pdumping. This flag is
	 * significant in a multi-app environment.
	 */
	IMG_BOOL		bPDumpActive;
#endif /* SUPPORT_PDUMP_MULTI_PROCESS */
#endif
	/*
	 * OS specific data can be stored via this handle.
	 * See osperproc.h for a generic mechanism for initialising
	 * this field.
	 */
	IMG_HANDLE		hOsPrivateData;
} PVRSRV_PER_PROCESS_DATA;

PVRSRV_PER_PROCESS_DATA *PVRSRVPerProcessData(IMG_UINT32 ui32PID);

PVRSRV_ERROR PVRSRVPerProcessDataConnect(IMG_UINT32	ui32PID, IMG_UINT32 ui32Flags);
IMG_VOID PVRSRVPerProcessDataDisconnect(IMG_UINT32	ui32PID);

PVRSRV_ERROR PVRSRVPerProcessDataInit(IMG_VOID);
PVRSRV_ERROR PVRSRVPerProcessDataDeInit(IMG_VOID);

static INLINE
PVRSRV_PER_PROCESS_DATA *PVRSRVFindPerProcessData(IMG_VOID)
{
	return PVRSRVPerProcessData(OSGetCurrentProcessIDKM());
}


static INLINE
IMG_HANDLE PVRSRVProcessPrivateData(PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	return (psPerProc != IMG_NULL) ? psPerProc->hOsPrivateData : IMG_NULL;
}


static INLINE
IMG_HANDLE PVRSRVPerProcessPrivateData(IMG_UINT32 ui32PID)
{
	return PVRSRVProcessPrivateData(PVRSRVPerProcessData(ui32PID));
}

static INLINE
IMG_HANDLE PVRSRVFindPerProcessPrivateData(IMG_VOID)
{
	return PVRSRVProcessPrivateData(PVRSRVFindPerProcessData());
}

#if defined (__cplusplus)
}
#endif

#endif /* __PERPROC_H__ */

/******************************************************************************
 End of file (perproc.h)
******************************************************************************/
