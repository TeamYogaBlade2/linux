// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __ENV_PERPROC_H__
#define __ENV_PERPROC_H__

#include <linux/list.h>

#include "proc.h"
#include "services.h"
#include "handle.h"

#define ION_CLIENT_NAME_SIZE	50
typedef struct _PVRSRV_ENV_PER_PROCESS_DATA_
{
	IMG_HANDLE hBlockAlloc;
	struct proc_dir_entry *psProcDir;
#if defined(SUPPORT_DRI_DRM) && defined(PVR_SECURE_DRM_AUTH_EXPORT)
	struct list_head sDRMAuthListHead;
#endif
#if defined(SUPPORT_ION)
 	struct ion_client *psIONClient;
	IMG_CHAR azIonClientName[ION_CLIENT_NAME_SIZE];
#endif
} PVRSRV_ENV_PER_PROCESS_DATA;

IMG_VOID RemovePerProcessProcDir(PVRSRV_ENV_PER_PROCESS_DATA *psEnvPerProc);

PVRSRV_ERROR LinuxMMapPerProcessConnect(PVRSRV_ENV_PER_PROCESS_DATA *psEnvPerProc);

IMG_VOID LinuxMMapPerProcessDisconnect(PVRSRV_ENV_PER_PROCESS_DATA *psEnvPerProc);

PVRSRV_ERROR LinuxMMapPerProcessHandleOptions(PVRSRV_HANDLE_BASE *psHandleBase);

IMG_HANDLE LinuxTerminatingProcessPrivateData(IMG_VOID);

#endif /* __ENV_PERPROC_H__ */

/******************************************************************************
 End of file (env_perproc.h)
******************************************************************************/
