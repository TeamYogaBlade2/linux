// SPDX-License-Identifier: MIT OR GPL-2.0-only

#if !defined (__KERNELBUFFER_H__)
#define __KERNELBUFFER_H__

#if defined (__cplusplus)
extern "C" {
#endif

/*
	Function table and pointers for SRVKM->BUFFER
*/
typedef PVRSRV_ERROR (*PFN_OPEN_BC_DEVICE)(IMG_UINT32, IMG_HANDLE*);
typedef PVRSRV_ERROR (*PFN_CLOSE_BC_DEVICE)(IMG_UINT32, IMG_HANDLE);
typedef PVRSRV_ERROR (*PFN_GET_BC_INFO)(IMG_HANDLE, BUFFER_INFO*);
typedef PVRSRV_ERROR (*PFN_GET_BC_BUFFER)(IMG_HANDLE, IMG_UINT32, PVRSRV_SYNC_DATA*, IMG_HANDLE*);

typedef struct PVRSRV_BC_SRV2BUFFER_KMJTABLE_TAG
{
	IMG_UINT32							ui32TableSize;
	PFN_OPEN_BC_DEVICE					pfnOpenBCDevice;
	PFN_CLOSE_BC_DEVICE					pfnCloseBCDevice;
	PFN_GET_BC_INFO						pfnGetBCInfo;
	PFN_GET_BC_BUFFER					pfnGetBCBuffer;
	PFN_GET_BUFFER_ADDR					pfnGetBufferAddr;

} PVRSRV_BC_SRV2BUFFER_KMJTABLE;


/*
	Function table and pointers for BUFFER->SRVKM
*/
typedef PVRSRV_ERROR (*PFN_BC_REGISTER_BUFFER_DEV)(PVRSRV_BC_SRV2BUFFER_KMJTABLE*, IMG_UINT32*);
typedef IMG_VOID (*PFN_BC_SCHEDULE_DEVICES)(IMG_VOID);
typedef PVRSRV_ERROR (*PFN_BC_REMOVE_BUFFER_DEV)(IMG_UINT32);

typedef struct PVRSRV_BC_BUFFER2SRV_KMJTABLE_TAG
{
	IMG_UINT32							ui32TableSize;
	PFN_BC_REGISTER_BUFFER_DEV			pfnPVRSRVRegisterBCDevice;
	PFN_BC_SCHEDULE_DEVICES				pfnPVRSRVScheduleDevices;
	PFN_BC_REMOVE_BUFFER_DEV			pfnPVRSRVRemoveBCDevice;

} PVRSRV_BC_BUFFER2SRV_KMJTABLE, *PPVRSRV_BC_BUFFER2SRV_KMJTABLE;

/* function to retrieve kernel services function table from kernel services */
typedef IMG_BOOL (*PFN_BC_GET_PVRJTABLE) (PPVRSRV_BC_BUFFER2SRV_KMJTABLE);

/* Prototype for platforms that access the JTable via linkage */
IMG_IMPORT IMG_BOOL PVRGetBufferClassJTable(PVRSRV_BC_BUFFER2SRV_KMJTABLE *psJTable);

#if defined (__cplusplus)
}
#endif

#endif/* #if !defined (__KERNELBUFFER_H__) */
