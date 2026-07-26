// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "perproc.h"
#include "sgxinfokm.h"

/* PRQA S 3410 7 */ /* macros require the absence of some brackets */
#define CCB_OFFSET_IS_VALID(type, psCCBMemInfo, psCCBKick, offset) \
	((sizeof(type) <= (psCCBMemInfo)->uAllocSize) && \
	((psCCBKick)->offset <= (psCCBMemInfo)->uAllocSize - sizeof(type)))

#define	CCB_DATA_FROM_OFFSET(type, psCCBMemInfo, psCCBKick, offset) \
	((type *)(((IMG_CHAR *)(psCCBMemInfo)->pvLinAddrKM) + \
		(psCCBKick)->offset))

extern IMG_UINT64 ui64KickCount;


IMG_IMPORT
IMG_VOID SGXTestActivePowerEvent(PVRSRV_DEVICE_NODE	*psDeviceNode,
								 IMG_UINT32			ui32CallerID);

IMG_IMPORT
PVRSRV_ERROR SGXScheduleCCBCommand(PVRSRV_DEVICE_NODE	*psDeviceNode,
								   SGXMKIF_CMD_TYPE		eCommandType,
								   SGXMKIF_COMMAND		*psCommandData,
								   IMG_UINT32			ui32CallerID,
								   IMG_UINT32			ui32PDumpFlags,
								   IMG_HANDLE			hDevMemContext,
								   IMG_BOOL				bLastInScene);
IMG_IMPORT
PVRSRV_ERROR SGXScheduleCCBCommandKM(PVRSRV_DEVICE_NODE		*psDeviceNode,
									 SGXMKIF_CMD_TYPE		eCommandType,
									 SGXMKIF_COMMAND		*psCommandData,
									 IMG_UINT32				ui32CallerID,
									 IMG_UINT32				ui32PDumpFlags,
									 IMG_HANDLE				hDevMemContext,
									 IMG_BOOL				bLastInScene);

IMG_IMPORT
PVRSRV_ERROR SGXScheduleProcessQueuesKM(PVRSRV_DEVICE_NODE *psDeviceNode);

IMG_IMPORT
IMG_BOOL SGXIsDevicePowered(PVRSRV_DEVICE_NODE *psDeviceNode);

IMG_IMPORT
IMG_HANDLE SGXRegisterHWRenderContextKM(IMG_HANDLE				psDeviceNode,
                                        IMG_CPU_VIRTADDR        *psHWRenderContextCpuVAddr,
                                        IMG_UINT32              ui32HWRenderContextSize,
                                        IMG_UINT32              ui32OffsetToPDDevPAddr,
                                        IMG_HANDLE              hDevMemContext,
                                        IMG_DEV_VIRTADDR        *psHWRenderContextDevVAddr,
										PVRSRV_PER_PROCESS_DATA *psPerProc);

IMG_IMPORT
IMG_HANDLE SGXRegisterHWTransferContextKM(IMG_HANDLE		      psDeviceNode,
                                          IMG_CPU_VIRTADDR        *psHWTransferContextCpuVAddr,
                                          IMG_UINT32              ui32HWTransferContextSize,
                                          IMG_UINT32              ui32OffsetToPDDevPAddr,
                                          IMG_HANDLE              hDevMemContext,
                                          IMG_DEV_VIRTADDR        *psHWTransferContextDevVAddr,
										  PVRSRV_PER_PROCESS_DATA *psPerProc);

IMG_IMPORT
PVRSRV_ERROR SGXFlushHWRenderTargetKM(IMG_HANDLE psSGXDevInfo,
									  IMG_DEV_VIRTADDR psHWRTDataSetDevVAddr,
									  IMG_BOOL bForceCleanup);

IMG_IMPORT
PVRSRV_ERROR SGXUnregisterHWRenderContextKM(IMG_HANDLE hHWRenderContext, IMG_BOOL bForceCleanup);

IMG_IMPORT
PVRSRV_ERROR SGXUnregisterHWTransferContextKM(IMG_HANDLE hHWTransferContext, IMG_BOOL bForceCleanup);

IMG_IMPORT
PVRSRV_ERROR SGXSetRenderContextPriorityKM(IMG_HANDLE       hDeviceNode,
                                           IMG_HANDLE       hHWRenderContext,
                                           IMG_UINT32       ui32Priority,
                                           IMG_UINT32       ui32OffsetOfPriorityField);

IMG_IMPORT
PVRSRV_ERROR SGXSetTransferContextPriorityKM(IMG_HANDLE       hDeviceNode,
                                             IMG_HANDLE       hHWTransferContext,
                                             IMG_UINT32       ui32Priority,
                                             IMG_UINT32       ui32OffsetOfPriorityField);

#if defined(SGX_FEATURE_2D_HARDWARE)
IMG_IMPORT
IMG_HANDLE SGXRegisterHW2DContextKM(IMG_HANDLE				psDeviceNode,
                                    IMG_CPU_VIRTADDR        *psHW2DContextCpuVAddr,
                                    IMG_UINT32              ui32HW2DContextSize,
                                    IMG_UINT32              ui32OffsetToPDDevPAddr,
                                    IMG_HANDLE              hDevMemContext,
                                    IMG_DEV_VIRTADDR        *psHW2DContextDevVAddr,
									PVRSRV_PER_PROCESS_DATA *psPerProc);

IMG_IMPORT
PVRSRV_ERROR SGXUnregisterHW2DContextKM(IMG_HANDLE hHW2DContext, IMG_BOOL bForceCleanup);
#endif

IMG_UINT32 SGXConvertTimeStamp(PVRSRV_SGXDEV_INFO	*psDevInfo,
							   IMG_UINT32			ui32TimeWraps,
							   IMG_UINT32			ui32Time);

/*!
*******************************************************************************

 @Function	SGXWaitClocks

 @Description

 Wait for a specified number of SGX clock cycles to elapse.

 @Input psDevInfo - SGX Device Info
 @Input ui32SGXClocks - number of clock cycles to wait

 @Return   IMG_VOID

******************************************************************************/
IMG_VOID SGXWaitClocks(PVRSRV_SGXDEV_INFO	*psDevInfo,
					   IMG_UINT32			ui32SGXClocks);

PVRSRV_ERROR SGXCleanupRequest(PVRSRV_DEVICE_NODE	*psDeviceNode,
							IMG_DEV_VIRTADDR	*psHWDataDevVAddr,
							IMG_UINT32			ui32CleanupType,
							IMG_BOOL			bForceCleanup);

IMG_IMPORT
PVRSRV_ERROR PVRSRVGetSGXRevDataKM(PVRSRV_DEVICE_NODE* psDeviceNode, IMG_UINT32 *pui32SGXCoreRev,
				IMG_UINT32 *pui32SGXCoreID);

/*!
******************************************************************************

 @Function	SGXContextSuspend

 @Description - Interface to the SGX microkernel to instruct it to suspend or
				resume processing on a given context. This will interrupt current
				processing of this context if a task is already running and is
				interruptable.

 @Input psDeviceNode			SGX device node
 @Input psHWContextDevVAddr		SGX virtual address of the context to be suspended
								or resumed. Can be of type SGXMKIF_HWRENDERCONTEXT,
								SGXMKIF_HWTRANSFERCONTEXT or SGXMKIF_HW2DCONTEXT
 @Input bResume					IMG_TRUE to put a context into suspend state,
								IMG_FALSE to resume a previously suspended context

******************************************************************************/
PVRSRV_ERROR SGXContextSuspend(PVRSRV_DEVICE_NODE	*psDeviceNode,
							   IMG_DEV_VIRTADDR		*psHWContextDevVAddr,
							   IMG_BOOL				bResume);

/******************************************************************************
 End of file (sgxutils.h)
******************************************************************************/
