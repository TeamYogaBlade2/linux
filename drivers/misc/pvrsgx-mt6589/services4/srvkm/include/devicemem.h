// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "img_defs.h"
#include "img_types.h"
#include "servicesext.h"

#ifndef __DEVICEMEM_H__
#define __DEVICEMEM_H__

PVRSRV_ERROR IMG_CALLCONV PVRSRVInitDeviceMem(IMG_VOID);
IMG_VOID IMG_CALLCONV PVRSRVDeInitDeviceMem(IMG_VOID);

#endif /* __DEVICEMEM_H__ */
