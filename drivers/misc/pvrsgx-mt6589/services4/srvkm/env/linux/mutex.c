// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include <linux/version.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/module.h>

#include <img_defs.h>
#include <services.h>

#include "mutex.h"


#include "mtk_pp.h"

#if defined(MTK_DEBUG_PROC_PRINT)
extern PVRSRV_LINUX_MUTEX gPVRSRVLock; /* bridge lock */
extern PVRSRV_LINUX_MUTEX gsPMMutex; /* power lock */

static inline char mtk_GetMutexName(PVRSRV_LINUX_MUTEX *psPVRSRVMutex)
{
	if (psPVRSRVMutex == &gPVRSRVLock) return 'B';
	if (psPVRSRVMutex == &gsPMMutex) return 'P';
	return ' ';
}

#endif

#if !defined(CONFIG_PROVE_LOCKING)
IMG_VOID LinuxInitMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex)
{
    mutex_init(&psPVRSRVMutex->hMutex);
}
#endif

IMG_VOID LinuxLockMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex)
{
	MTKPP_LOG(MTKPP_ID_MUTEX, "Lock %c %p: %d (current:%d)",
		mtk_GetMutexName(psPVRSRVMutex), psPVRSRVMutex, psPVRSRVMutex->hHeldBy, current->pid);

    mutex_lock(&psPVRSRVMutex->hMutex);

#if defined(MTK_DEBUG_PROC_PRINT)
	psPVRSRVMutex->hHeldBy = current->pid;
#endif
}

IMG_VOID LinuxLockMutexNested(PVRSRV_LINUX_MUTEX *psPVRSRVMutex, unsigned int uiLockClass)
{
	MTKPP_LOG(MTKPP_ID_MUTEX, "LockNested %c %p,c%d: %d (current:%d)",
		mtk_GetMutexName(psPVRSRVMutex), psPVRSRVMutex, uiLockClass, psPVRSRVMutex->hHeldBy, current->pid);

	mutex_lock_nested(&psPVRSRVMutex->hMutex, uiLockClass);

#if defined(MTK_DEBUG_PROC_PRINT)
	psPVRSRVMutex->hHeldBy = current->pid;
#endif
}

PVRSRV_ERROR LinuxLockMutexInterruptible(PVRSRV_LINUX_MUTEX *psPVRSRVMutex)
{
	MTKPP_LOG(MTKPP_ID_MUTEX, "LockInterruptible %c %p: %d (current:%d)",
		mtk_GetMutexName(psPVRSRVMutex), psPVRSRVMutex, psPVRSRVMutex->hHeldBy, current->pid);

    if(mutex_lock_interruptible(&psPVRSRVMutex->hMutex) == -EINTR)
    {
        return PVRSRV_ERROR_MUTEX_INTERRUPTIBLE_ERROR;
    }
    else
    {
#if defined(MTK_DEBUG_PROC_PRINT)
    	psPVRSRVMutex->hHeldBy = current->pid;
#endif
        return PVRSRV_OK;
    }
}

IMG_INT32 LinuxTryLockMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex)
{
	IMG_INT32 ret;

	MTKPP_LOG(MTKPP_ID_MUTEX, "TryLock %c %p: %d (current:%d)",
		mtk_GetMutexName(psPVRSRVMutex), psPVRSRVMutex, psPVRSRVMutex->hHeldBy, current->pid);

	ret = mutex_trylock(&psPVRSRVMutex->hMutex);

#if defined(MTK_DEBUG_PROC_PRINT)
	if (ret) psPVRSRVMutex->hHeldBy = current->pid;
#endif

	return ret;
}

IMG_VOID LinuxUnLockMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex)
{
	MTKPP_LOG(MTKPP_ID_MUTEX, "UnLock %c %p: %d (current:%d)",
		mtk_GetMutexName(psPVRSRVMutex), psPVRSRVMutex, psPVRSRVMutex->hHeldBy, current->pid);

#if defined(MTK_DEBUG_PROC_PRINT)
	psPVRSRVMutex->hHeldBy = 0;
#endif

    mutex_unlock(&psPVRSRVMutex->hMutex);
}

IMG_BOOL LinuxIsLockedMutex(PVRSRV_LINUX_MUTEX *psPVRSRVMutex)
{
    return (mutex_is_locked(&psPVRSRVMutex->hMutex)) ? IMG_TRUE : IMG_FALSE;
}
