// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __INCLUDED_LINUX_MUTEX_H_
#define __INCLUDED_LINUX_MUTEX_H_

#include <linux/version.h>
#include <linux/sched.h>

#include <linux/mutex.h>


#if defined(MTK_DEBUG_PROC_PRINT)

typedef struct {
	struct mutex hMutex;
	int hHeldBy;
}PVRSRV_LINUX_MUTEX;

#else

typedef struct mutex PVRSRV_LINUX_MUTEX;

#endif

enum PVRSRV_MUTEX_LOCK_CLASS
{
	PVRSRV_LOCK_CLASS_POWER,
	PVRSRV_LOCK_CLASS_BRIDGE,
	PVRSRV_LOCK_CLASS_MMAP,
	PVRSRV_LOCK_CLASS_MM_DEBUG,
	PVRSRV_LOCK_CLASS_PVR_DEBUG,
};

#if defined(CONFIG_PROVE_LOCKING)
#define LinuxInitMutex(l) mutex_init(l)
#else
extern IMG_VOID LinuxInitMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex);
#endif

extern IMG_VOID LinuxLockMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex);

extern IMG_VOID LinuxLockMutexNested(PVRSRV_LINUX_MUTEX *psPVRSRVMutex, unsigned int uiLockClass);

extern PVRSRV_ERROR LinuxLockMutexInterruptible(PVRSRV_LINUX_MUTEX *psPVRSRVMutex);

extern IMG_INT32 LinuxTryLockMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex);

extern IMG_VOID LinuxUnLockMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex);

extern IMG_BOOL LinuxIsLockedMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex);


#endif /* __INCLUDED_LINUX_MUTEX_H_ */
