// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __RESMAN_H__
#define __RESMAN_H__

#if defined (__cplusplus)
extern "C" {
#endif

/******************************************************************************
 * resman definitions
 *****************************************************************************/

enum {
	/* SGX: */
	RESMAN_TYPE_SHARED_PB_DESC = 1,					/*!< Parameter buffer kernel stubs */
	RESMAN_TYPE_SHARED_PB_DESC_CREATE_LOCK,				/*!< Shared parameter buffer creation lock */
	RESMAN_TYPE_HW_RENDER_CONTEXT,					/*!< Hardware Render Context Resource */
	RESMAN_TYPE_HW_TRANSFER_CONTEXT,				/*!< Hardware transfer Context Resource */
	RESMAN_TYPE_HW_2D_CONTEXT,						/*!< Hardware 2D Context Resource */
	RESMAN_TYPE_TRANSFER_CONTEXT,					/*!< Transfer Queue context */

	/* VGX: */
	RESMAN_TYPE_DMA_CLIENT_FIFO_DATA,				/*!< VGX DMA Client FIFO data */

	/* DISPLAY CLASS: */
	RESMAN_TYPE_DISPLAYCLASS_SWAPCHAIN_REF,			/*!< Display Class Swapchain Reference Resource */
	RESMAN_TYPE_DISPLAYCLASS_DEVICE,				/*!< Display Class Device Resource */

	/* BUFFER CLASS: */
	RESMAN_TYPE_BUFFERCLASS_DEVICE,					/*!< Buffer Class Device Resource */

	/* OS specific User mode Mappings: */
	RESMAN_TYPE_OS_USERMODE_MAPPING,				/*!< OS specific User mode mappings */

	/* COMMON: */
	RESMAN_TYPE_DEVICEMEM_CONTEXT,					/*!< Device Memory Context Resource */
	RESMAN_TYPE_DEVICECLASSMEM_MAPPING,				/*!< Device Memory Mapping Resource */
	RESMAN_TYPE_DEVICEMEM_MAPPING,					/*!< Device Memory Mapping Resource */
	RESMAN_TYPE_DEVICEMEM_WRAP,						/*!< Device Memory Wrap Resource */
	RESMAN_TYPE_DEVICEMEM_ALLOCATION,				/*!< Device Memory Allocation Resource */
	RESMAN_TYPE_DEVICEMEM_ION,						/*!< Device Memory Ion Resource */
	RESMAN_TYPE_EVENT_OBJECT,						/*!< Event Object */
    RESMAN_TYPE_SHARED_MEM_INFO,                    /*!< Shared system memory meminfo */
    RESMAN_TYPE_MODIFY_SYNC_OPS,					/*!< Syncobject synchronisation Resource*/
    RESMAN_TYPE_SYNC_INFO,					        /*!< Syncobject Resource*/

	/* KERNEL: */
	RESMAN_TYPE_KERNEL_DEVICEMEM_ALLOCATION			/*!< Device Memory Allocation Resource */
};

#define RESMAN_CRITERIA_ALL				0x00000000	/*!< match by criteria all */
#define RESMAN_CRITERIA_RESTYPE			0x00000001	/*!< match by criteria type */
#define RESMAN_CRITERIA_PVOID_PARAM		0x00000002	/*!< match by criteria param1 */
#define RESMAN_CRITERIA_UI32_PARAM		0x00000004	/*!< match by criteria param2 */

typedef PVRSRV_ERROR (*RESMAN_FREE_FN)(IMG_PVOID pvParam, IMG_UINT32 ui32Param, IMG_BOOL bForceCleanup);

typedef struct _RESMAN_ITEM_ *PRESMAN_ITEM;
typedef struct _RESMAN_CONTEXT_ *PRESMAN_CONTEXT;

/******************************************************************************
 * resman functions
 *****************************************************************************/

/*
	Note:
	Resource cleanup can fail with retry in which case we don't remove
	it from resman's list and either UM or KM will try to release the
	resource at a later date (and will keep trying until a non-retry
	error is returned)
*/

PVRSRV_ERROR ResManInit(IMG_VOID);
IMG_VOID ResManDeInit(IMG_VOID);

PRESMAN_ITEM ResManRegisterRes(PRESMAN_CONTEXT	hResManContext,
							   IMG_UINT32		ui32ResType,
							   IMG_PVOID		pvParam,
							   IMG_UINT32		ui32Param,
							   RESMAN_FREE_FN	pfnFreeResource);

PVRSRV_ERROR ResManFreeResByPtr(PRESMAN_ITEM	psResItem,
								IMG_BOOL		bForceCleanup);

PVRSRV_ERROR ResManFreeResByCriteria(PRESMAN_CONTEXT	hResManContext,
									 IMG_UINT32			ui32SearchCriteria,
									 IMG_UINT32			ui32ResType,
									 IMG_PVOID			pvParam,
									 IMG_UINT32			ui32Param);

PVRSRV_ERROR ResManDissociateRes(PRESMAN_ITEM		psResItem,
							 PRESMAN_CONTEXT	psNewResManContext);

PVRSRV_ERROR ResManFindResourceByPtr(PRESMAN_CONTEXT	hResManContext,
									 PRESMAN_ITEM		psItem);

PVRSRV_ERROR PVRSRVResManConnect(IMG_HANDLE			hPerProc,
								 PRESMAN_CONTEXT	*phResManContext);
IMG_VOID PVRSRVResManDisconnect(PRESMAN_CONTEXT hResManContext,
								IMG_BOOL		bKernelContext);

#if defined (__cplusplus)
}
#endif

#endif /* __RESMAN_H__ */

/******************************************************************************
 End of file (resman.h)
******************************************************************************/
