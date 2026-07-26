// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef QUEUE_H
#define QUEUE_H

#if defined(SUPPORT_PVRSRV_DEVICE_CLASS)

/*!
 * Macro to Read Offset in given command queue
 */
#define UPDATE_QUEUE_ROFF(psQueue, uSize)						\
	(psQueue)->uReadOffset = ((psQueue)->uReadOffset + (uSize))	\
	& ((psQueue)->uQueueSize - 1);

/*!
	generic cmd complete structure.
	This structure represents the storage required between starting and finishing
	a given cmd and is required to hold the generic sync object update data.
	note: for any given system we know what command types we support and
	therefore how much storage is required for any number of commands in progress
 */
 typedef struct _COMMAND_COMPLETE_DATA_
 {
	IMG_BOOL			bInUse;
	/* <arg(s) to PVRSRVProcessQueues>;	*/	/*!< TBD */
	IMG_UINT32			ui32DstSyncCount;	/*!< number of dst sync objects */
	IMG_UINT32			ui32SrcSyncCount;	/*!< number of src sync objects */
	PVRSRV_SYNC_OBJECT	*psDstSync;			/*!< dst sync ptr list,
                                        	allocated on back of this structure */
	PVRSRV_SYNC_OBJECT	*psSrcSync;			/*!< src sync ptr list,
                                       		allocated on back of this structure */
	IMG_UINT32			ui32AllocSize;		/*!< allocated size*/
	PFN_QUEUE_COMMAND_COMPLETE	pfnCommandComplete;	/*!< Command complete callback */
	IMG_HANDLE					hCallbackData;		/*!< Command complete callback data */

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	IMG_VOID			*pvCleanupFence;	/*!< Sync fence to 'put' after timeline inc() */
	IMG_VOID			*pvTimeline;		/*!< Android sync timeline to inc() */
#endif
 }COMMAND_COMPLETE_DATA, *PCOMMAND_COMPLETE_DATA;

#if !defined(USE_CODE)
IMG_VOID QueueDumpDebugInfo(IMG_VOID);

IMG_IMPORT
PVRSRV_ERROR PVRSRVProcessQueues (IMG_BOOL		bFlush);

#if defined(__linux__) && defined(__KERNEL__)
#include <linux/types.h>
#include <linux/seq_file.h>
void* ProcSeqOff2ElementQueue(struct seq_file * sfile, loff_t off);
void ProcSeqShowQueue(struct seq_file *sfile,void* el);
#endif


IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVCreateCommandQueueKM(IMG_SIZE_T uQueueSize,
													 PVRSRV_QUEUE_INFO **ppsQueueInfo);
IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVDestroyCommandQueueKM(PVRSRV_QUEUE_INFO *psQueueInfo);

IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVInsertCommandKM(PVRSRV_QUEUE_INFO	*psQueue,
												PVRSRV_COMMAND		**ppsCommand,
												IMG_UINT32			ui32DevIndex,
												IMG_UINT16			CommandType,
												IMG_UINT32			ui32DstSyncCount,
												PVRSRV_KERNEL_SYNC_INFO	*apsDstSync[],
												IMG_UINT32			ui32SrcSyncCount,
												PVRSRV_KERNEL_SYNC_INFO	*apsSrcSync[],
												IMG_SIZE_T			ui32DataByteSize,
												PFN_QUEUE_COMMAND_COMPLETE pfnCommandComplete,
												IMG_HANDLE			hCallbackData,
												IMG_HANDLE			*phFence);

IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVGetQueueSpaceKM(PVRSRV_QUEUE_INFO *psQueue,
												IMG_SIZE_T uParamSize,
												IMG_VOID **ppvSpace);

IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVSubmitCommandKM(PVRSRV_QUEUE_INFO *psQueue,
												PVRSRV_COMMAND *psCommand);

IMG_IMPORT
IMG_VOID PVRSRVCommandCompleteKM(IMG_HANDLE hCmdCookie, IMG_BOOL bScheduleMISR);

IMG_IMPORT
PVRSRV_ERROR PVRSRVRegisterCmdProcListKM(IMG_UINT32		ui32DevIndex,
										 PFN_CMD_PROC	*ppfnCmdProcList,
										 IMG_UINT32		ui32MaxSyncsPerCmd[][2],
										 IMG_UINT32		ui32CmdCount);
IMG_IMPORT
PVRSRV_ERROR PVRSRVRemoveCmdProcListKM(IMG_UINT32	ui32DevIndex,
									   IMG_UINT32	ui32CmdCount);

#endif /* !defined(USE_CODE) */

#endif /* defined(SUPPORT_PVRSRV_DEVICE_CLASS) */

#endif /* QUEUE_H */

/******************************************************************************
 End of file (queue.h)
******************************************************************************/
