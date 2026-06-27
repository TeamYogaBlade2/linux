// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "services_headers.h"
#include "ttrace_common.h"
#include "ttrace_tokens.h"

#ifndef __TTRACE_H__
#define __TTRACE_H__

#if defined(TTRACE)

	#define PVR_TTRACE(group, class, token) \
			PVRSRVTimeTrace(group, class, token)
	#define PVR_TTRACE_UI8(group, class, token, val) \
			PVRSRVTimeTraceUI8(group, class, token, val)
	#define PVR_TTRACE_UI16(group, class, token, val) \
			PVRSRVTimeTraceUI16(group, class, token, val)
	#define PVR_TTRACE_UI32(group, class, token, val) \
			PVRSRVTimeTraceUI32(group, class, token, val)
	#define PVR_TTRACE_UI64(group, class, token, val) \
			PVRSRVTimeTraceUI64(group, class, token, val)
	#define PVR_TTRACE_DEV_VIRTADDR(group, class, token, val) \
			PVRSRVTimeTraceDevVirtAddr(group, class, token, val)
	#define PVR_TTRACE_CPU_PHYADDR(group, class, token, val) \
			PVRSRVTimeTraceCpuPhyAddr(group, class, token, val)
	#define PVR_TTRACE_DEV_PHYADDR(group, class, token, val) \
			PVRSRVTimeTraceDevPhysAddr(group, class, token, val)
	#define PVR_TTRACE_SYS_PHYADDR(group, class, token, val) \
			PVRSRVTimeTraceSysPhysAddr(group, class, token, val)
	#define PVR_TTRACE_SYNC_OBJECT(group, token, syncobj, op) \
			PVRSRVTimeTraceSyncObject(group, token, syncobj, op)

IMG_IMPORT IMG_VOID IMG_CALLCONV PVRSRVTimeTraceArray(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
							IMG_UINT32 ui32Token, IMG_UINT32 ui32TypeSize,
							IMG_UINT32 ui32Count, IMG_UINT8 *ui8Data);

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTrace)
#endif
static INLINE IMG_VOID PVRSRVTimeTrace(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, 0, 0, NULL);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceUI8)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceUI8(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_UINT8 ui8Value)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, PVRSRV_TRACE_TYPE_UI8,
				1, &ui8Value);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceUI16)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceUI16(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_UINT16 ui16Value)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, PVRSRV_TRACE_TYPE_UI16,
				1, (IMG_UINT8 *) &ui16Value);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceUI32)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceUI32(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_UINT32 ui32Value)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, PVRSRV_TRACE_TYPE_UI32,
				1, (IMG_UINT8 *) &ui32Value);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceUI64)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceUI64(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_UINT64 ui64Value)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, PVRSRV_TRACE_TYPE_UI64,
				1, (IMG_UINT8 *) &ui64Value);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceDevVirtAddr)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceDevVirtAddr(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_DEV_VIRTADDR psVAddr)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, PVRSRV_TRACE_TYPE_UI32,
				1, (IMG_UINT8 *) &psVAddr.uiAddr);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceCpuPhyAddr)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceCpuPhyAddr(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_CPU_PHYADDR psPAddr)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, PVRSRV_TRACE_TYPE_UI32,
				1, (IMG_UINT8 *) &psPAddr.uiAddr);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceDevPhysAddr)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceDevPhysAddr(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_DEV_PHYADDR psPAddr)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, PVRSRV_TRACE_TYPE_UI32,
				1, (IMG_UINT8 *) &psPAddr.uiAddr);
}

#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVTimeTraceSysPhysAddr)
#endif
static INLINE IMG_VOID PVRSRVTimeTraceSysPhysAddr(IMG_UINT32 ui32Group, IMG_UINT32 ui32Class,
						IMG_UINT32 ui32Token, IMG_SYS_PHYADDR psPAddr)
{
	PVRSRVTimeTraceArray(ui32Group, ui32Class, ui32Token, sizeof(psPAddr.uiAddr),
				1, (IMG_UINT8 *) &psPAddr.uiAddr);
}

#else /* defined(PVRSRV_NEED_PVR_TIME_TRACE) */

	#define PVR_TTRACE(group, class, token) \
			((void) 0)
	#define PVR_TTRACE_UI8(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_UI16(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_UI32(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_UI64(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_DEV_VIRTADDR(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_CPU_PHYADDR(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_DEV_PHYADDR(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_SYS_PHYADDR(group, class, token, val) \
			((void) 0)
	#define PVR_TTRACE_SYNC_OBJECT(group, token, syncobj, op) \
			((void) 0)

#endif /* defined(PVRSRV_NEED_PVR_TIME_TRACE) */

IMG_IMPORT PVRSRV_ERROR PVRSRVTimeTraceInit(IMG_VOID);
IMG_IMPORT IMG_VOID PVRSRVTimeTraceDeinit(IMG_VOID);

IMG_IMPORT IMG_VOID PVRSRVTimeTraceSyncObject(IMG_UINT32 ui32Group, IMG_UINT32 ui32Token,
					      PVRSRV_KERNEL_SYNC_INFO *psSync, IMG_UINT8 ui8SyncOp);
IMG_IMPORT PVRSRV_ERROR PVRSRVTimeTraceBufferCreate(IMG_UINT32 ui32PID);
IMG_IMPORT PVRSRV_ERROR PVRSRVTimeTraceBufferDestroy(IMG_UINT32 ui32PID);

IMG_IMPORT IMG_VOID PVRSRVDumpTimeTraceBuffers(IMG_VOID);
#endif /* __TTRACE_H__ */
