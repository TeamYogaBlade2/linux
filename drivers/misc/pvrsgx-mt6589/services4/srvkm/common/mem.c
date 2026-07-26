// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "services_headers.h"
#include "pvr_bridge_km.h"


static PVRSRV_ERROR
FreeSharedSysMemCallBack(IMG_PVOID  pvParam,
						 IMG_UINT32 ui32Param,
						 IMG_BOOL   bDummy)
{
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo = pvParam;

	PVR_UNREFERENCED_PARAMETER(ui32Param);
	PVR_UNREFERENCED_PARAMETER(bDummy);

	OSFreePages(psKernelMemInfo->ui32Flags,
				psKernelMemInfo->uAllocSize,
				psKernelMemInfo->pvLinAddrKM,
				psKernelMemInfo->sMemBlk.hOSMemHandle);

	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
			  sizeof(PVRSRV_KERNEL_MEM_INFO),
			  psKernelMemInfo,
			  IMG_NULL);
	/*not nulling pointer, copy on stack*/

	return PVRSRV_OK;
}


IMG_EXPORT PVRSRV_ERROR
PVRSRVAllocSharedSysMemoryKM(PVRSRV_PER_PROCESS_DATA	*psPerProc,
							 IMG_UINT32					ui32Flags,
							 IMG_SIZE_T 				uSize,
							 PVRSRV_KERNEL_MEM_INFO 	**ppsKernelMemInfo)
{
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo;

	if(OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
				  sizeof(PVRSRV_KERNEL_MEM_INFO),
				  (IMG_VOID **)&psKernelMemInfo, IMG_NULL,
				  "Kernel Memory Info") != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVAllocSharedSysMemoryKM: Failed to alloc memory for meminfo"));
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}

	OSMemSet(psKernelMemInfo, 0, sizeof(*psKernelMemInfo));

	ui32Flags &= ~PVRSRV_HAP_MAPTYPE_MASK;
	ui32Flags |= PVRSRV_HAP_MULTI_PROCESS;
	psKernelMemInfo->ui32Flags = ui32Flags;
	psKernelMemInfo->uAllocSize = uSize;

	if(OSAllocPages(psKernelMemInfo->ui32Flags,
					psKernelMemInfo->uAllocSize,
					(IMG_UINT32)HOST_PAGESIZE(),
					IMG_NULL,
					0,
					IMG_NULL,
					&psKernelMemInfo->pvLinAddrKM,
					&psKernelMemInfo->sMemBlk.hOSMemHandle)
		!= PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVAllocSharedSysMemoryKM: Failed to alloc memory for block"));
		OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
				  sizeof(PVRSRV_KERNEL_MEM_INFO),
				  psKernelMemInfo,
				  0);
		return PVRSRV_ERROR_OUT_OF_MEMORY;
	}

	/* register with the resman */
	psKernelMemInfo->sMemBlk.hResItem =
				ResManRegisterRes(psPerProc->hResManContext,
								  RESMAN_TYPE_SHARED_MEM_INFO,
								  psKernelMemInfo,
								  0,
								  &FreeSharedSysMemCallBack);

	*ppsKernelMemInfo = psKernelMemInfo;

	return PVRSRV_OK;
}


IMG_EXPORT PVRSRV_ERROR
PVRSRVFreeSharedSysMemoryKM(PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo)
{
	PVRSRV_ERROR eError;

	if(psKernelMemInfo->sMemBlk.hResItem)
	{
		eError = ResManFreeResByPtr(psKernelMemInfo->sMemBlk.hResItem, CLEANUP_WITH_POLL);
	}
	else
	{
		eError = FreeSharedSysMemCallBack(psKernelMemInfo, 0, CLEANUP_WITH_POLL);
	}

	return eError;
}


IMG_EXPORT PVRSRV_ERROR
PVRSRVDissociateMemFromResmanKM(PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	if(!psKernelMemInfo)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if(psKernelMemInfo->sMemBlk.hResItem)
	{
		eError = ResManDissociateRes(psKernelMemInfo->sMemBlk.hResItem, IMG_NULL);

		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR,"PVRSRVDissociateMemFromResmanKM: ResManDissociateRes failed"));
			PVR_DBG_BREAK;
			return eError;
		}

		psKernelMemInfo->sMemBlk.hResItem = IMG_NULL;
	}

	return eError;
}

/******************************************************************************
 End of file (mem.c)
******************************************************************************/
