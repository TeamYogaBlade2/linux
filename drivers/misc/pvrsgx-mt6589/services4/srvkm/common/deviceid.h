// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __DEVICEID_H__
#define __DEVICEID_H__

#include "services.h"
#include "syscommon.h"

PVRSRV_ERROR AllocateDeviceID(SYS_DATA *psSysData, IMG_UINT32 *pui32DevID);
PVRSRV_ERROR FreeDeviceID(SYS_DATA *psSysData, IMG_UINT32 ui32DevID);

#endif /* __DEVICEID_H__ */
