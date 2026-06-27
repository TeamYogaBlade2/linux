// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _SYSTRACE_
#define _SYSTRACE_

#include "img_defs.h"
#include "img_types.h"

#include "services_headers.h"
#include "sgxapi_km.h"
#include "sgxinfo.h"
#include "sgxinfokm.h"

typedef enum
{
	PVRSRV_SYSTRACE_OK = 0x00,
	PVRSRV_SYSTRACE_NOT_INITIALISED,
	PVRSRV_SYSTRACE_JOB_NOT_FOUND
} PVRSRV_SYSTRACE_ERROR;


void SystraceHWPerfPackets(PVRSRV_SGXDEV_INFO *psDevInfo, PVRSRV_SGX_HWPERF_CB_ENTRY* psSGXHWPerf, IMG_UINT32 ui32DataCount, IMG_UINT32 ui32SgxClockspeed);
void SystraceTAKick(PVRSRV_SGXDEV_INFO *psDevInfo, IMG_UINT32 ui32FrameNum, IMG_UINT32 ui32RTData, IMG_BOOL bIsFirstKick);

void SystraceCreateFS(void);
void SystraceDestroyFS(void);
IMG_BOOL SystraceIsCapturingHWData(void);

#endif /* _SYSTRACE_ */
