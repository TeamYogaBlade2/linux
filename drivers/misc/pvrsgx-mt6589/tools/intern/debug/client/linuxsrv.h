// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _LINUXSRV_H__
#define _LINUXSRV_H__

typedef struct tagIOCTL_PACKAGE
{
	IMG_UINT32 ui32Cmd;              // ioctl command
	IMG_UINT32 ui32Size;			   // needs to be correctly set
	IMG_VOID 	*pInBuffer;          // input data buffer
	IMG_UINT32  ui32InBufferSize;     // size of input data buffer
	IMG_VOID    *pOutBuffer;         // output data buffer
	IMG_UINT32  ui32OutBufferSize;    // size of output data buffer
} IOCTL_PACKAGE;

IMG_UINT32 DeviceIoControl(IMG_UINT32 hDevice,
						IMG_UINT32 ui32ControlCode,
						IMG_VOID *pInBuffer,
						IMG_UINT32 ui32InBufferSize,
						IMG_VOID *pOutBuffer,
						IMG_UINT32 ui32OutBufferSize,
						IMG_UINT32 *pui32BytesReturned);

#endif /* _LINUXSRV_H__*/
