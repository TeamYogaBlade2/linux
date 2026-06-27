// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _IOCTL_
#define _IOCTL_

/*****************************************************************************
 Global vars
*****************************************************************************/

#define MAX_DBGVXD_W32_API 25

extern IMG_UINT32 (*g_DBGDrivProc[MAX_DBGVXD_W32_API])(IMG_VOID *, IMG_VOID *);

#endif

/*****************************************************************************
 End of file (IOCTL.H)
*****************************************************************************/
