// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __OSPERPROC_H__
#define __OSPERPROC_H__

#if defined(__linux__) || defined(__QNXNTO__)
PVRSRV_ERROR OSPerProcessPrivateDataInit(IMG_HANDLE *phOsPrivateData);
PVRSRV_ERROR OSPerProcessPrivateDataDeInit(IMG_HANDLE hOsPrivateData);

PVRSRV_ERROR OSPerProcessSetHandleOptions(PVRSRV_HANDLE_BASE *psHandleBase);
#else	/* defined(__linux__) */
static INLINE PVRSRV_ERROR OSPerProcessPrivateDataInit(IMG_HANDLE *phOsPrivateData)
{
	PVR_UNREFERENCED_PARAMETER(phOsPrivateData);

	return PVRSRV_OK;
}

static INLINE PVRSRV_ERROR OSPerProcessPrivateDataDeInit(IMG_HANDLE hOsPrivateData)
{
	PVR_UNREFERENCED_PARAMETER(hOsPrivateData);

	return PVRSRV_OK;
}

static INLINE PVRSRV_ERROR OSPerProcessSetHandleOptions(PVRSRV_HANDLE_BASE *psHandleBase)
{
	PVR_UNREFERENCED_PARAMETER(psHandleBase);

	return PVRSRV_OK;
}
#endif	/* defined(__linux__) */

#endif /* __OSPERPROC_H__ */

/******************************************************************************
 End of file (osperproc.h)
******************************************************************************/
