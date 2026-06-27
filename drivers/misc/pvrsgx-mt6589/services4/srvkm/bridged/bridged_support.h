// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __BRIDGED_SUPPORT_H__
#define __BRIDGED_SUPPORT_H__

#include "handle.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Derive the internal OS specific memory handle from a secure
 * handle.
 */
PVRSRV_ERROR PVRSRVLookupOSMemHandle(PVRSRV_HANDLE_BASE *psBase, IMG_HANDLE *phOSMemHandle, IMG_HANDLE hMHandle);

#if defined (__cplusplus)
}
#endif

#endif /* __BRIDGED_SUPPORT_H__ */

/******************************************************************************
 End of file (bridged_support.h)
******************************************************************************/
