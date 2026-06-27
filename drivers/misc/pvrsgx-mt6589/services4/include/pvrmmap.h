// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __PVRMMAP_H__
#define __PVRMMAP_H__

/*!
 **************************************************************************
 @brief         map kernel memory into user memory.

 @param			hModule - a handle to the device supplying the kernel memory
 @param			ppvLinAddr - pointer to where the user mode address should be placed
 @param			pvLinAddrKM - the base of kernel address range to map
 @param			phMappingInfo - pointer to mapping information handle
 @param			hMHandle - handle associated with memory to be mapped

 @return		PVRSRV_OK, or error code.
 ***************************************************************************/

PVRSRV_ERROR PVRPMapKMem(IMG_HANDLE hModule, IMG_VOID **ppvLinAddr, IMG_VOID *pvLinAddrKM, IMG_HANDLE *phMappingInfo, IMG_HANDLE hMHandle);


/*!
 **************************************************************************
 @brief	        Removes a kernel to userspace memory mapping.

 @param		hModule - a handle to the device supplying the kernel memory
 @param		hMappingInfo - mapping information handle
 @param		hMHandle - handle associated with memory to be mapped

 @return	IMG_BOOL indicating success or otherwise.
 ***************************************************************************/
IMG_BOOL PVRUnMapKMem(IMG_HANDLE hModule, IMG_HANDLE hMappingInfo, IMG_HANDLE hMHandle);

#endif /* _PVRMMAP_H_ */
