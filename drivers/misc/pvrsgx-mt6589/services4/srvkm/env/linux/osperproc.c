// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "services_headers.h"
#include "osperproc.h"

#include "env_perproc.h"

extern IMG_UINT32 gui32ReleasePID;

PVRSRV_ERROR OSPerProcessPrivateDataInit(IMG_HANDLE *phOsPrivateData)
{
	PVRSRV_ERROR eError;
	IMG_HANDLE hBlockAlloc;
	PVRSRV_ENV_PER_PROCESS_DATA *psEnvPerProc;

	eError = OSAllocMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
				sizeof(PVRSRV_ENV_PER_PROCESS_DATA),
				phOsPrivateData,
				&hBlockAlloc,
				"Environment per Process Data");

	if (eError != PVRSRV_OK)
	{
		*phOsPrivateData = IMG_NULL;

		PVR_DPF((PVR_DBG_ERROR, "%s: OSAllocMem failed (%d)", __FUNCTION__, eError));
		return eError;
	}

	psEnvPerProc = (PVRSRV_ENV_PER_PROCESS_DATA *)*phOsPrivateData;
	OSMemSet(psEnvPerProc, 0, sizeof(*psEnvPerProc));

	psEnvPerProc->hBlockAlloc = hBlockAlloc;

	/* Linux specific mmap processing */
	LinuxMMapPerProcessConnect(psEnvPerProc);

#if defined(SUPPORT_DRI_DRM) && defined(PVR_SECURE_DRM_AUTH_EXPORT)
	/* Linked list of PVRSRV_FILE_PRIVATE_DATA structures */
	INIT_LIST_HEAD(&psEnvPerProc->sDRMAuthListHead);
#endif

	return PVRSRV_OK;
}

PVRSRV_ERROR OSPerProcessPrivateDataDeInit(IMG_HANDLE hOsPrivateData)
{
	PVRSRV_ERROR eError;
	PVRSRV_ENV_PER_PROCESS_DATA *psEnvPerProc;

	if (hOsPrivateData == IMG_NULL)
	{
		return PVRSRV_OK;
	}

	psEnvPerProc = (PVRSRV_ENV_PER_PROCESS_DATA *)hOsPrivateData;

	/* Linux specific mmap processing */
	LinuxMMapPerProcessDisconnect(psEnvPerProc);

	/* Remove per process /proc entries */
	RemovePerProcessProcDir(psEnvPerProc);

	eError = OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
				sizeof(PVRSRV_ENV_PER_PROCESS_DATA),
				hOsPrivateData,
				psEnvPerProc->hBlockAlloc);
	/*not nulling pointer, copy on stack*/

	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: OSFreeMem failed (%d)", __FUNCTION__, eError));
	}

	return PVRSRV_OK;
}

PVRSRV_ERROR OSPerProcessSetHandleOptions(PVRSRV_HANDLE_BASE *psHandleBase)
{
	return LinuxMMapPerProcessHandleOptions(psHandleBase);
}

IMG_HANDLE LinuxTerminatingProcessPrivateData(IMG_VOID)
{
	if(!gui32ReleasePID)
		return NULL;
	return PVRSRVPerProcessPrivateData(gui32ReleasePID);
}
