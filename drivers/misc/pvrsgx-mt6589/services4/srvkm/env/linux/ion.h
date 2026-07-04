// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __IMG_LINUX_ION_H__
#define __IMG_LINUX_ION_H__

#if defined(SUPPORT_ION)

#include SUPPORT_ION_HEADER

#include "img_types.h"
#include "servicesext.h"

PVRSRV_ERROR IonInit(IMG_VOID);

IMG_VOID IonDeinit(IMG_VOID);

PVRSRV_ERROR IonImportBufferAndAcquirePhysAddr(IMG_HANDLE hIonDev,
											   IMG_UINT32 ui32NumFDs,
											   IMG_INT32  *pi32BufferFDs,
											   IMG_UINT32 *pui32PageCount,
											   IMG_SYS_PHYADDR **ppsSysPhysAddr,
											   IMG_PVOID *ppvKernAddr0,
											   IMG_HANDLE *phPriv,
											   IMG_HANDLE *phUnique);

IMG_VOID IonUnimportBufferAndReleasePhysAddr(IMG_HANDLE hPriv);

#endif /* defined(SUPPORT_ION) */

#endif /* __IMG_LINUX_ION_H__ */
