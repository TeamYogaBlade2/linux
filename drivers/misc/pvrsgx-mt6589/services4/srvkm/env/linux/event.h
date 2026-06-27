// SPDX-License-Identifier: MIT OR GPL-2.0-only

PVRSRV_ERROR LinuxEventObjectListCreate(IMG_HANDLE *phEventObjectList);
PVRSRV_ERROR LinuxEventObjectListDestroy(IMG_HANDLE hEventObjectList);
PVRSRV_ERROR LinuxEventObjectAdd(IMG_HANDLE hOSEventObjectList, IMG_HANDLE *phOSEventObject);
PVRSRV_ERROR LinuxEventObjectDelete(IMG_HANDLE hOSEventObjectList, IMG_HANDLE hOSEventObject);
PVRSRV_ERROR LinuxEventObjectSignal(IMG_HANDLE hOSEventObjectList);
PVRSRV_ERROR LinuxEventObjectWait(IMG_HANDLE hOSEventObject, IMG_UINT32 ui32MSTimeout);
