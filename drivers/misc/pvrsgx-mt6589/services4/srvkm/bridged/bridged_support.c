// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "img_defs.h"
#include "servicesint.h"
#include "bridged_support.h"


/*
 * Derive the internal OS specific memory handle from a secure
 * handle.
 */
PVRSRV_ERROR
PVRSRVLookupOSMemHandle(PVRSRV_HANDLE_BASE *psHandleBase, IMG_HANDLE *phOSMemHandle, IMG_HANDLE hMHandle)
{
	IMG_HANDLE hMHandleInt;
	PVRSRV_HANDLE_TYPE eHandleType;
	PVRSRV_ERROR eError;

	/*
	 * We don't know the type of the handle at this point, so we use
	 * PVRSRVLookupHandleAnyType to look it up.
	 */
	eError = PVRSRVLookupHandleAnyType(psHandleBase, &hMHandleInt,
							  &eHandleType,
							  hMHandle);
	if(eError != PVRSRV_OK)
	{
		return eError;
	}

	switch(eHandleType)
	{
#if defined(PVR_SECURE_HANDLES)
		case PVRSRV_HANDLE_TYPE_MEM_INFO:
		case PVRSRV_HANDLE_TYPE_MEM_INFO_REF:
		case PVRSRV_HANDLE_TYPE_SHARED_SYS_MEM_INFO:
		{
			PVRSRV_KERNEL_MEM_INFO *psMemInfo = (PVRSRV_KERNEL_MEM_INFO *)hMHandleInt;

			*phOSMemHandle = psMemInfo->sMemBlk.hOSMemHandle;

			break;
		}
		case PVRSRV_HANDLE_TYPE_SYNC_INFO:
		{
			PVRSRV_KERNEL_SYNC_INFO *psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)hMHandleInt;
			PVRSRV_KERNEL_MEM_INFO *psMemInfo = psSyncInfo->psSyncDataMemInfoKM;

			*phOSMemHandle = psMemInfo->sMemBlk.hOSMemHandle;

			break;
		}
		case  PVRSRV_HANDLE_TYPE_SOC_TIMER:
		{
			*phOSMemHandle = (IMG_VOID *)hMHandleInt;
			break;
		}
#else
		case  PVRSRV_HANDLE_TYPE_NONE:
			*phOSMemHandle = (IMG_VOID *)hMHandleInt;
			break;
#endif
		default:
			return PVRSRV_ERROR_BAD_MAPPING;
	}

	return PVRSRV_OK;
}
/******************************************************************************
 End of file (bridged_support.c)
******************************************************************************/
