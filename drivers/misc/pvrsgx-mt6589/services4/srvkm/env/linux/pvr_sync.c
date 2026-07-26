// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "pvr_sync.h"

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/anon_inodes.h>
#include <linux/seq_file.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/sync_file.h>

#include "services_headers.h"
#include "sgxutils.h"
#include "ttrace.h"
#include "mutex.h"
#include "lock.h"

//#define DEBUG_PRINT

#if defined(DEBUG_PRINT)
#define DPF(fmt, ...) PVR_DPF((PVR_DBG_BUFFERED, fmt, __VA_ARGS__))
#else
#define DPF(fmt, ...) do {} while(0)
#endif

#if !defined(PVR_LINUX_MISR_USING_WORKQUEUE) && \
    !defined(PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE)
#error The sync driver requires that the SGX MISR runs in wq context
#endif

struct PVR_SYNC_KERNEL_SYNC_INFO {
	PVRSRV_KERNEL_SYNC_INFO	*psBase;
	struct list_head	sHead;
};

struct PVR_SYNC_TIMELINE {
	u64			context;
	atomic_t		seqno;
	struct list_head	sTimelineList;
	IMG_BOOL		bSyncHasSignaled;
	spinlock_t		sTimelineLock;	/* was mutex, now spinlock for dma_fence */
	struct list_head	fence_list;
	struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo;
	char			name[32];
};

struct PVR_SYNC_DATA {
	struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo;
	atomic_t			sRefcount;
	IMG_UINT32			ui32WOPSnapshot;
	IMG_UINT64			ui64Stamp;
};

struct PVR_SYNC {
	struct dma_fence		base;
	struct PVR_SYNC_DATA		*psSyncData;
	struct PVR_SYNC_TIMELINE	*psTimeline;
	struct list_head		link;		/* node in timeline->fence_list */
};

struct PVR_ALLOC_SYNC_DATA {
	struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo;
	struct PVR_SYNC_TIMELINE	 *psTimeline;
	struct file			 *psFile;
};

struct PVR_SYNC_FENCE_WAITER {
	struct dma_fence_cb			cb;
	struct dma_fence			*fence;
	struct PVR_SYNC_KERNEL_SYNC_INFO	*psSyncInfo;
	struct PVR_SYNC_FENCE			*psSyncFence;
#ifdef MTK_DEBUG_PROC_PRINT
	int iFenceFd;
#endif
};

struct PVR_SYNC_FENCE {
	struct dma_fence	*fence;
	struct list_head	sHead;
};

static struct {
	IMG_UINT32	ui32Pid;
	IMG_HANDLE	hDevCookie;
	IMG_HANDLE	hDevMemContext;
} gsSyncServicesConnection;

static struct workqueue_struct *gpsWorkQueue;
static struct work_struct gsWork;

static LIST_HEAD(gTimelineList);
static DEFINE_MUTEX(gTimelineListLock);

static LIST_HEAD(gSyncInfoFreeList);
static DEFINE_SPINLOCK(gSyncInfoFreeListLock);

static LIST_HEAD(gFencePutList);
static DEFINE_SPINLOCK(gFencePutListLock);

static IMG_UINT64 gui64SyncPointStamp;

/* Forward declarations */
static void PVRSyncReleaseSyncInfo(struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo);
static void PVRSyncFreeSyncData(struct PVR_SYNC_DATA *psSyncData);
static struct PVR_ALLOC_SYNC_DATA *PVRSyncAllocFDGet(int fd);

/* ---------- SW helpers ---------- */
static void PVRSyncSWTakeOp(PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo)
{
	psKernelSyncInfo->psSyncData->ui32WriteOpsPending = 1;
}

static void PVRSyncSWCompleteOp(PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo)
{
	psKernelSyncInfo->psSyncData->ui32WriteOpsComplete = 1;
}

/* ---------- dma_fence_ops ---------- */
static const char *pvr_sync_get_driver_name(struct dma_fence *fence)
{
	return "pvr_sync";
}

static const char *pvr_sync_get_timeline_name(struct dma_fence *fence)
{
	struct PVR_SYNC *sync = container_of(fence, struct PVR_SYNC, base);
	return sync->psTimeline->name;
}

static bool pvr_sync_enable_signaling(struct dma_fence *fence)
{
	return true;
}

static bool pvr_sync_signaled(struct dma_fence *fence)
{
	struct PVR_SYNC *sync = container_of(fence, struct PVR_SYNC, base);
	PVRSRV_SYNC_DATA *sd = sync->psSyncData->psSyncInfo->psBase->psSyncData;
	return sd->ui32WriteOpsComplete >= sd->ui32WriteOpsPending;
}

static void pvr_sync_release(struct dma_fence *fence)
{
	struct PVR_SYNC *sync = container_of(fence, struct PVR_SYNC, base);

	if (atomic_dec_return(&sync->psSyncData->sRefcount) != 0)
		return;

	DPF("R( ): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X "
	    "WOP/C=0x%x/0x%x ROP/C=0x%x/0x%x RO2P/C=0x%x/0x%x "
	    "ID=%llu, F=%p",
	    sync->psSyncData->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
	    sync->psSyncData->psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
	    sync->psSyncData->psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr,
	    sync->psSyncData->psSyncInfo->psBase->psSyncData->ui32WriteOpsPending,
	    sync->psSyncData->psSyncInfo->psBase->psSyncData->ui32WriteOpsComplete,
	    sync->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOpsPending,
	    sync->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOpsComplete,
	    sync->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOps2Pending,
	    sync->psSyncData->psSyncInfo->psBase->psSyncData->ui32ReadOps2Complete,
	    sync->psSyncData->ui64Stamp,
	    &sync->base);

	spin_lock(&sync->psTimeline->sTimelineLock);
	list_del(&sync->link);
	spin_unlock(&sync->psTimeline->sTimelineLock);

	PVRSyncFreeSyncData(sync->psSyncData);
	dma_fence_free(&sync->base);
}

static const struct dma_fence_ops gsDmaFenceOps = {
	.get_driver_name = pvr_sync_get_driver_name,
	.get_timeline_name = pvr_sync_get_timeline_name,
	.enable_signaling = pvr_sync_enable_signaling,
	.signaled = pvr_sync_signaled,
	.wait = dma_fence_default_wait,
	.release = pvr_sync_release,
};

/* ---------- Sync creation ---------- */
static struct PVR_SYNC *
PVRSyncCreateSync(struct PVR_SYNC_TIMELINE *obj,
		  struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo)
{
	struct PVR_SYNC *psSync;
	u64 seqno;
	unsigned long flags;

	psSync = kzalloc(sizeof(*psSync), GFP_KERNEL);
	if (!psSync) {
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to allocate PVR_SYNC", __func__));
		return NULL;
	}

	psSync->psSyncData = kzalloc(sizeof(*psSync->psSyncData), GFP_KERNEL);
	if (!psSync->psSyncData) {
		kfree(psSync);
		return NULL;
	}

	atomic_set(&psSync->psSyncData->sRefcount, 1);
	psSync->psSyncData->ui32WOPSnapshot =
		obj->psSyncInfo->psBase->psSyncData->ui32WriteOpsPending;
	psSync->psSyncData->psSyncInfo = psSyncInfo;
	psSync->psTimeline = obj;

	/* All allocations done; now take spinlock to get seqno and stamp */
	spin_lock_irqsave(&obj->sTimelineLock, flags);
	seqno = atomic_inc_return(&obj->seqno);
	psSync->psSyncData->ui64Stamp = gui64SyncPointStamp++;
	list_add_tail(&psSync->link, &obj->fence_list);
	spin_unlock_irqrestore(&obj->sTimelineLock, flags);

	dma_fence_init(&psSync->base, &gsDmaFenceOps, &obj->sTimelineLock,
		       obj->context, seqno);

	DPF("C( ): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X",
	    psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
	    psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
	    psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr);
	return psSync;
}

/* ---------- Sync info management ---------- */
static IMG_BOOL PVRSyncIsSyncInfoInUse(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo)
{
	return !(psSyncInfo->psSyncData->ui32WriteOpsPending ==
		 psSyncInfo->psSyncData->ui32WriteOpsComplete &&
		 psSyncInfo->psSyncData->ui32ReadOpsPending ==
		 psSyncInfo->psSyncData->ui32ReadOpsComplete &&
		 psSyncInfo->psSyncData->ui32ReadOps2Pending ==
		 psSyncInfo->psSyncData->ui32ReadOps2Complete);
}

static void
PVRSyncReleaseSyncInfo(struct PVR_SYNC_KERNEL_SYNC_INFO *psSyncInfo)
{
	unsigned long flags;
	spin_lock_irqsave(&gSyncInfoFreeListLock, flags);
	list_add_tail(&psSyncInfo->sHead, &gSyncInfoFreeList);
	spin_unlock_irqrestore(&gSyncInfoFreeListLock, flags);
	queue_work(gpsWorkQueue, &gsWork);
}

static void PVRSyncFreeSyncData(struct PVR_SYNC_DATA *psSyncData)
{
	PVR_ASSERT(atomic_read(&psSyncData->sRefcount) == 0);
	PVRSyncReleaseSyncInfo(psSyncData->psSyncInfo);
	psSyncData->psSyncInfo = NULL;
	kfree(psSyncData);
}

/* ---------- Timeline management ---------- */
static void PVRSyncReleaseTimeline(struct PVR_SYNC_TIMELINE *psTimeline)
{
	mutex_lock(&gTimelineListLock);
	list_del(&psTimeline->sTimelineList);
	mutex_unlock(&gTimelineListLock);

	DPF("R(t): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X "
	    "WOP/C=0x%x/0x%x ROP/C=0x%x/0x%x RO2P/C=0x%x/0x%x",
	    psTimeline->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
	    psTimeline->psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
	    psTimeline->psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr,
	    psTimeline->psSyncInfo->psBase->psSyncData->ui32WriteOpsPending,
	    psTimeline->psSyncInfo->psBase->psSyncData->ui32WriteOpsComplete,
	    psTimeline->psSyncInfo->psBase->psSyncData->ui32ReadOpsPending,
	    psTimeline->psSyncInfo->psBase->psSyncData->ui32ReadOpsComplete,
	    psTimeline->psSyncInfo->psBase->psSyncData->ui32ReadOps2Pending,
	    psTimeline->psSyncInfo->psBase->psSyncData->ui32ReadOps2Complete);

	PVRSyncReleaseSyncInfo(psTimeline->psSyncInfo);
	psTimeline->psSyncInfo = NULL;
	kfree(psTimeline);
}

static struct PVR_SYNC_TIMELINE *PVRSyncCreateTimeline(const IMG_CHAR *pszName)
{
	struct PVR_SYNC_TIMELINE *psTimeline;
	PVRSRV_ERROR eError;

	psTimeline = kzalloc(sizeof(*psTimeline), GFP_KERNEL);
	if (!psTimeline)
		return NULL;

	psTimeline->context = dma_fence_context_alloc(1);
	atomic_set(&psTimeline->seqno, 0);
	spin_lock_init(&psTimeline->sTimelineLock);
	INIT_LIST_HEAD(&psTimeline->fence_list);
	strncpy(psTimeline->name, pszName, sizeof(psTimeline->name) - 1);

	psTimeline->psSyncInfo =
		kmalloc(sizeof(struct PVR_SYNC_KERNEL_SYNC_INFO), GFP_KERNEL);
	if (!psTimeline->psSyncInfo) {
		kfree(psTimeline);
		return NULL;
	}

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	eError = PVRSRVAllocSyncInfoKM(gsSyncServicesConnection.hDevCookie,
				       gsSyncServicesConnection.hDevMemContext,
				       &psTimeline->psSyncInfo->psBase);
	LinuxUnLockMutex(&gPVRSRVLock);
	if (eError != PVRSRV_OK) {
		kfree(psTimeline->psSyncInfo);
		kfree(psTimeline);
		return NULL;
	}

	DPF("A(t): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X T=%p %s",
	    psTimeline->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
	    psTimeline->psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
	    psTimeline->psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr,
	    psTimeline, pszName);
	return psTimeline;
}

/* ---------- File ops for /dev/pvr_sync ---------- */
static int PVRSyncOpen(struct inode *inode, struct file *file)
{
	struct PVR_SYNC_TIMELINE *psTimeline;
	IMG_CHAR name[32];

	snprintf(name, 32, "pvr-%u", current->tgid);
	psTimeline = PVRSyncCreateTimeline(name);
	if (!psTimeline)
		return -ENOMEM;

	mutex_lock(&gTimelineListLock);
	list_add_tail(&psTimeline->sTimelineList, &gTimelineList);
	mutex_unlock(&gTimelineListLock);

	file->private_data = psTimeline;
	return 0;
}

static int PVRSyncRelease(struct inode *inode, struct file *file)
{
	struct PVR_SYNC_TIMELINE *psTimeline = file->private_data;
	PVRSyncReleaseTimeline(psTimeline);
	return 0;
}

/* ---------- Alloc file for pre-creation ---------- */
static int PVRSyncFenceAllocRelease(struct inode *inode, struct file *file)
{
	struct PVR_ALLOC_SYNC_DATA *psAlloc = file->private_data;

	if (psAlloc->psSyncInfo) {
		DPF("R(a): WOCVA=0x%.8X ROCVA=0x%.8X RO2CVA=0x%.8X",
		    psAlloc->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr,
		    psAlloc->psSyncInfo->psBase->sReadOpsCompleteDevVAddr.uiAddr,
		    psAlloc->psSyncInfo->psBase->sReadOps2CompleteDevVAddr.uiAddr);
		PVRSyncReleaseSyncInfo(psAlloc->psSyncInfo);
		psAlloc->psSyncInfo = NULL;
	}
	kfree(psAlloc);
	return 0;
}

static const struct file_operations gsSyncFenceAllocFOps = {
	.release = PVRSyncFenceAllocRelease,
};

static struct PVR_ALLOC_SYNC_DATA *PVRSyncAllocFDGet(int fd)
{
	struct file *file = fget(fd);
	if (!file)
		return NULL;
	if (file->f_op != &gsSyncFenceAllocFOps)
		goto err;
	return file->private_data;
err:
	fput(file);
	return NULL;
}

/* ---------- IOCTL handlers ---------- */
static long
PVRSyncIOCTLCreate(struct PVR_SYNC_TIMELINE *psObj, void __user *pvData)
{
	struct PVR_SYNC_KERNEL_SYNC_INFO *psProvidedSyncInfo = NULL;
	struct PVR_ALLOC_SYNC_DATA *psAlloc;
	struct PVR_SYNC_CREATE_IOCTL_DATA sData;
	struct sync_file *sync_file;
	struct dma_fence *fence;
	int iFd, err = -EFAULT;

	iFd = get_unused_fd_flags(0);
	if (iFd < 0)
		return iFd;
	if (!access_ok(pvData, sizeof(sData)))
		goto err_put_fd;
	if (copy_from_user(&sData, pvData, sizeof(sData)))
		goto err_put_fd;

	psAlloc = PVRSyncAllocFDGet(sData.allocdSyncInfo);
	if (!psAlloc)
		goto err_put_fd;

	psProvidedSyncInfo = psAlloc->psSyncInfo;
	psAlloc->psSyncInfo = NULL;
	if (!psProvidedSyncInfo) {
		fput(psAlloc->psFile);
		goto err_put_fd;
	}
	fput(psAlloc->psFile);

	fence = &PVRSyncCreateSync(psObj, psProvidedSyncInfo)->base;
	if (!fence) {
		err = -ENOMEM;
		goto err_put_fd;
	}

	sData.name[sizeof(sData.name) - 1] = '\0';
	sync_file = sync_file_create(fence);
	if (!sync_file) {
		dma_fence_put(fence);
		err = -ENOMEM;
		goto err_put_fd;
	}

	if (psProvidedSyncInfo->psBase->psSyncData->ui32WriteOpsPending == 0)
		dma_fence_signal(fence);

	sData.fence = iFd;
	if (copy_to_user(pvData, &sData, sizeof(sData))) {
		fput(sync_file->file);
		goto err_put_fd;
	}

	fd_install(iFd, sync_file->file);
	return 0;

err_put_fd:
	put_unused_fd(iFd);
	return err;
}

static long
PVRSyncIOCTLDebug(struct PVR_SYNC_TIMELINE *psObj, void __user *pvData)
{
	struct PVR_SYNC_DEBUG_IOCTL_DATA sData;
	struct dma_fence *fence;
	int i = 0;

	if (!access_ok(pvData, sizeof(sData)))
		return -EFAULT;
	if (copy_from_user(&sData, pvData, sizeof(sData)))
		return -EFAULT;

	fence = sync_file_get_fence(sData.iFenceFD);
	if (!fence)
		return -EFAULT;

	if (dma_fence_is_array(fence)) {
		struct dma_fence_array *array = to_dma_fence_array(fence);
		unsigned int j;
		for (j = 0; j < array->num_fences && i < PVR_SYNC_DEBUG_MAX_POINTS; j++) {
			struct dma_fence *f = array->fences[j];
			if (f->ops != &gsDmaFenceOps)
				continue;
			struct PVR_SYNC *ps = container_of(f, struct PVR_SYNC, base);
			PVRSRV_KERNEL_SYNC_INFO *pInfo = ps->psSyncData->psSyncInfo->psBase;
			sData.sSync[i].sSyncData = *pInfo->psSyncData;
			sData.sSync[i].sMetaData.ui64Stamp = ps->psSyncData->ui64Stamp;
			sData.sSync[i].sMetaData.ui32WriteOpsPendingSnapshot = ps->psSyncData->ui32WOPSnapshot;
			i++;
		}
	} else if (fence->ops == &gsDmaFenceOps) {
		struct PVR_SYNC *ps = container_of(fence, struct PVR_SYNC, base);
		PVRSRV_KERNEL_SYNC_INFO *pInfo = ps->psSyncData->psSyncInfo->psBase;
		sData.sSync[i].sSyncData = *pInfo->psSyncData;
		sData.sSync[i].sMetaData.ui64Stamp = ps->psSyncData->ui64Stamp;
		sData.sSync[i].sMetaData.ui32WriteOpsPendingSnapshot = ps->psSyncData->ui32WOPSnapshot;
		i++;
	}
	dma_fence_put(fence);

	sData.ui32NumPoints = i;
	if (copy_to_user(pvData, &sData, sizeof(sData)))
		return -EFAULT;
	return 0;
}

static long
PVRSyncIOCTLAlloc(struct PVR_SYNC_TIMELINE *psTimeline, void __user *pvData)
{
	struct PVR_ALLOC_SYNC_DATA *psAlloc;
	struct PVR_SYNC_ALLOC_IOCTL_DATA sData;
	PVRSRV_ERROR eError;
	struct file *psFile;
	int iFd, err = -EFAULT;

	iFd = get_unused_fd_flags(0);
	if (iFd < 0)
		return iFd;
	if (!access_ok(pvData, sizeof(sData)))
		goto err_put_fd;
	if (copy_from_user(&sData, pvData, sizeof(sData)))
		goto err_put_fd;

	psAlloc = kzalloc(sizeof(*psAlloc), GFP_KERNEL);
	if (!psAlloc) {
		err = -ENOMEM;
		goto err_put_fd;
	}
	psAlloc->psSyncInfo = kzalloc(sizeof(*psAlloc->psSyncInfo), GFP_KERNEL);
	if (!psAlloc->psSyncInfo) {
		kfree(psAlloc);
		err = -ENOMEM;
		goto err_put_fd;
	}

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	eError = PVRSRVAllocSyncInfoKM(gsSyncServicesConnection.hDevCookie,
				       gsSyncServicesConnection.hDevMemContext,
				       &psAlloc->psSyncInfo->psBase);
	LinuxUnLockMutex(&gPVRSRVLock);
	if (eError != PVRSRV_OK) {
		kfree(psAlloc->psSyncInfo);
		kfree(psAlloc);
		err = -ENOMEM;
		goto err_put_fd;
	}

	psFile = anon_inode_getfile("pvr_sync_alloc", &gsSyncFenceAllocFOps,
				    psAlloc, 0);
	if (!psFile) {
		PVRSRVReleaseSyncInfoKM(psAlloc->psSyncInfo->psBase);
		kfree(psAlloc->psSyncInfo);
		kfree(psAlloc);
		err = -ENOMEM;
		goto err_put_fd;
	}

	sData.fence = iFd;
	{
		PVRSRV_SYNC_DATA *sd;
		LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
		sd = psTimeline->psSyncInfo->psBase->psSyncData;
		sData.bTimelineIdle = (sd->ui32WriteOpsPending ==
				       sd->ui32WriteOpsComplete) ?
				       IMG_TRUE : IMG_FALSE;
		LinuxUnLockMutex(&gPVRSRVLock);
	}

	if (copy_to_user(pvData, &sData, sizeof(sData))) {
		fput(psFile);
		put_unused_fd(iFd);
		return -EFAULT;
	}

	psAlloc->psTimeline = psTimeline;
	psAlloc->psFile = psFile;
	fd_install(iFd, psFile);
	return 0;

err_put_fd:
	put_unused_fd(iFd);
	return err;
}

static long
PVRSyncIOCTL(struct file *file, unsigned int cmd, unsigned long __user arg)
{
	struct PVR_SYNC_TIMELINE *psTimeline = file->private_data;
	void __user *pvData = (void __user *)arg;

	switch (cmd) {
	case PVR_SYNC_IOC_CREATE_FENCE:
		return PVRSyncIOCTLCreate(psTimeline, pvData);
	case PVR_SYNC_IOC_DEBUG_FENCE:
		return PVRSyncIOCTLDebug(psTimeline, pvData);
	case PVR_SYNC_IOC_ALLOC_FENCE:
		return PVRSyncIOCTLAlloc(psTimeline, pvData);
	default:
		return -ENOTTY;
	}
}

/* ---------- Workqueue ---------- */
static void PVRSyncWorkQueueFunction(struct work_struct *data)
{
	PVRSRV_DEVICE_NODE *psDevNode =
		(PVRSRV_DEVICE_NODE *)gsSyncServicesConnection.hDevCookie;
	struct list_head sFreeList, *psEntry, *n;
	unsigned long flags;

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	SGXScheduleProcessQueuesKM(psDevNode);

	INIT_LIST_HEAD(&sFreeList);
	spin_lock_irqsave(&gSyncInfoFreeListLock, flags);
	list_for_each_safe(psEntry, n, &gSyncInfoFreeList) {
		struct PVR_SYNC_KERNEL_SYNC_INFO *psi =
			container_of(psEntry, struct PVR_SYNC_KERNEL_SYNC_INFO, sHead);
		if (!PVRSyncIsSyncInfoInUse(psi->psBase))
			list_move_tail(psEntry, &sFreeList);
	}
	spin_unlock_irqrestore(&gSyncInfoFreeListLock, flags);

	list_for_each_safe(psEntry, n, &sFreeList) {
		struct PVR_SYNC_KERNEL_SYNC_INFO *psi =
			container_of(psEntry, struct PVR_SYNC_KERNEL_SYNC_INFO, sHead);
		list_del(psEntry);
		PVRSRVReleaseSyncInfoKM(psi->psBase);
		psi->psBase = NULL;
		kfree(psi);
	}
	LinuxUnLockMutex(&gPVRSRVLock);

	INIT_LIST_HEAD(&sFreeList);
	spin_lock_irqsave(&gFencePutListLock, flags);
	list_for_each_safe(psEntry, n, &gFencePutList)
		list_move_tail(psEntry, &sFreeList);
	spin_unlock_irqrestore(&gFencePutListLock, flags);

	list_for_each_safe(psEntry, n, &sFreeList) {
		struct PVR_SYNC_FENCE *sf =
			container_of(psEntry, struct PVR_SYNC_FENCE, sHead);
		list_del(psEntry);
		dma_fence_put(sf->fence);
		kfree(sf);
	}
}

/* ---------- MISR callback ---------- */
void PVRSyncUpdateAllSyncs(void)
{
	struct list_head *psEntry;
	IMG_BOOL need_queue = IMG_FALSE;
	unsigned long flags;

	mutex_lock(&gTimelineListLock);
	list_for_each(psEntry, &gTimelineList) {
		struct PVR_SYNC_TIMELINE *tl =
			container_of(psEntry, struct PVR_SYNC_TIMELINE, sTimelineList);
		struct PVR_SYNC *ps, *tmp;

		spin_lock_irqsave(&tl->sTimelineLock, flags);
		list_for_each_entry_safe(ps, tmp, &tl->fence_list, link) {
			if (pvr_sync_signaled(&ps->base)) {
				dma_fence_signal_locked(&ps->base);
				need_queue = IMG_TRUE;
			}
		}
		spin_unlock_irqrestore(&tl->sTimelineLock, flags);
	}
	mutex_unlock(&gTimelineListLock);

	if (need_queue)
		queue_work(gpsWorkQueue, &gsWork);
}

/* ---------- Foreign fence handling ---------- */
static void ForeignSyncPtSignaled(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct PVR_SYNC_FENCE_WAITER *waiter =
		container_of(cb, struct PVR_SYNC_FENCE_WAITER, cb);
	unsigned long flags;

	PVRSyncSWCompleteOp(waiter->psSyncInfo->psBase);
	DPF("R(f): WOCVA=0x%.8X ...", waiter->psSyncInfo->psBase->sWriteOpsCompleteDevVAddr.uiAddr);
	PVRSyncReleaseSyncInfo(waiter->psSyncInfo);
	waiter->psSyncInfo = NULL;

	spin_lock_irqsave(&gFencePutListLock, flags);
	list_add_tail(&waiter->psSyncFence->sHead, &gFencePutList);
	waiter->psSyncFence = NULL;
	spin_unlock_irqrestore(&gFencePutListLock, flags);

	kfree(waiter);
}

static PVRSRV_KERNEL_SYNC_INFO *ForeignSyncPointToSyncInfo(int iFenceFd)
{
	PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo;
	struct PVR_SYNC_FENCE_WAITER *waiter;
	struct PVR_SYNC_FENCE *psSyncFence;
	struct dma_fence *fence;
	PVRSRV_ERROR eError;
	int err;

	waiter = kzalloc(sizeof(*waiter), GFP_KERNEL);
	if (!waiter)
		return NULL;
	waiter->psSyncInfo = kzalloc(sizeof(*waiter->psSyncInfo), GFP_KERNEL);
	if (!waiter->psSyncInfo) {
		kfree(waiter);
		return NULL;
	}

	fence = sync_file_get_fence(iFenceFd);
	if (!fence) {
		kfree(waiter->psSyncInfo);
		kfree(waiter);
		return NULL;
	}

	psSyncFence = kzalloc(sizeof(*psSyncFence), GFP_KERNEL);
	if (!psSyncFence) {
		dma_fence_put(fence);
		kfree(waiter->psSyncInfo);
		kfree(waiter);
		return NULL;
	}
	psSyncFence->fence = fence;
	waiter->psSyncFence = psSyncFence;

	eError = PVRSRVAllocSyncInfoKM(gsSyncServicesConnection.hDevCookie,
				       gsSyncServicesConnection.hDevMemContext,
				       &psKernelSyncInfo);
	if (eError != PVRSRV_OK) {
		dma_fence_put(fence);
		kfree(psSyncFence);
		kfree(waiter->psSyncInfo);
		kfree(waiter);
		return NULL;
	}

	PVRSyncSWTakeOp(psKernelSyncInfo);
	waiter->psSyncInfo->psBase = psKernelSyncInfo;

	err = dma_fence_add_callback(fence, &waiter->cb, ForeignSyncPtSignaled);
	if (err == -ENOENT) {
		ForeignSyncPtSignaled(fence, &waiter->cb);
	} else if (err != 0) {
		PVRSyncSWCompleteOp(psKernelSyncInfo);
		PVRSRVReleaseSyncInfoKM(psKernelSyncInfo);
		dma_fence_put(fence);
		kfree(psSyncFence);
		kfree(waiter->psSyncInfo);
		kfree(waiter);
		return NULL;
	}

	return psKernelSyncInfo;
}

/* ---------- Sync patching utilities ---------- */
static void
CopyKernelSyncInfoToDeviceSyncObject(PVRSRV_KERNEL_SYNC_INFO *psInfo,
				     PVRSRV_DEVICE_SYNC_OBJECT *psObj)
{
	psObj->sReadOpsCompleteDevVAddr  = psInfo->sReadOpsCompleteDevVAddr;
	psObj->sWriteOpsCompleteDevVAddr = psInfo->sWriteOpsCompleteDevVAddr;
	psObj->sReadOps2CompleteDevVAddr = psInfo->sReadOps2CompleteDevVAddr;
	psObj->ui32WriteOpsPendingVal = psInfo->psSyncData->ui32WriteOpsPending;
	psObj->ui32ReadOpsPendingVal  = psInfo->psSyncData->ui32ReadOpsPending;
	psObj->ui32ReadOps2PendingVal = psInfo->psSyncData->ui32ReadOps2Pending;
}

static IMG_BOOL FenceHasForeignPoints(struct dma_fence *fence)
{
	if (dma_fence_is_array(fence)) {
		struct dma_fence_array *array = to_dma_fence_array(fence);
		unsigned int i;
		for (i = 0; i < array->num_fences; i++)
			if (array->fences[i]->ops != &gsDmaFenceOps)
				return IMG_TRUE;
		return IMG_FALSE;
	}
	return fence->ops != &gsDmaFenceOps ? IMG_TRUE : IMG_FALSE;
}

static IMG_BOOL
AddSyncInfoToArray(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo,
		   IMG_UINT32 limit, IMG_UINT32 *pui32Num,
		   PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[])
{
	if (*pui32Num == limit) {
		PVR_DPF((PVR_DBG_WARNING, "%s: Ran out of source syncs", __func__));
		return IMG_FALSE;
	}
	apsSyncInfo[*pui32Num] = psSyncInfo;
	(*pui32Num)++;
	return IMG_TRUE;
}

static IMG_BOOL
ExpandAndDeDuplicateFenceSyncs(IMG_UINT32 ui32NumSyncs,
			       int aiFenceFds[],
			       IMG_UINT32 ui32SyncPointLimit,
			       struct dma_fence *apsFence[],
			       IMG_UINT32 *pui32NumRealSyncs,
			       PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[])
{
	IMG_UINT32 i, j, idx = 0;
	IMG_BOOL ret = IMG_TRUE;
	*pui32NumRealSyncs = 0;

	for (i = 0; i < ui32NumSyncs; i++) {
		struct dma_fence *fence;
		if (aiFenceFds[i] < 0)
			continue;

		fence = sync_file_get_fence(aiFenceFds[i]);
		if (!fence) {
			PVR_DPF((PVR_DBG_ERROR, "%s: Bad fd %d", __func__, aiFenceFds[i]));
			ret = IMG_FALSE;
			goto out;
		}
		apsFence[idx] = fence;

		if (FenceHasForeignPoints(fence)) {
			PVRSRV_KERNEL_SYNC_INFO *psi = ForeignSyncPointToSyncInfo(aiFenceFds[i]);
			if (psi) {
				if (!AddSyncInfoToArray(psi, ui32SyncPointLimit,
						        pui32NumRealSyncs, apsSyncInfo))
					goto out;
			}
			idx++;
			continue;
		}

		if (dma_fence_is_array(fence)) {
			struct dma_fence_array *arr = to_dma_fence_array(fence);
			unsigned int k;
			for (k = 0; k < arr->num_fences; k++) {
				struct dma_fence *f = arr->fences[k];
				if (f->ops != &gsDmaFenceOps)
					continue;
				struct PVR_SYNC *ps = container_of(f, struct PVR_SYNC, base);
				PVRSRV_KERNEL_SYNC_INFO *psi = ps->psSyncData->psSyncInfo->psBase;
				for (j = 0; j < *pui32NumRealSyncs; j++)
					if (apsSyncInfo[j] == psi)
						break;
				if (j == *pui32NumRealSyncs &&
				    !AddSyncInfoToArray(psi, ui32SyncPointLimit,
						        pui32NumRealSyncs, apsSyncInfo))
					goto out;
			}
		} else {
			struct PVR_SYNC *ps = container_of(fence, struct PVR_SYNC, base);
			PVRSRV_KERNEL_SYNC_INFO *psi = ps->psSyncData->psSyncInfo->psBase;
			if (!AddSyncInfoToArray(psi, ui32SyncPointLimit,
					        pui32NumRealSyncs, apsSyncInfo))
				goto out;
		}
		idx++;
	}
out:
	return ret;
}

IMG_INTERNAL PVRSRV_ERROR
PVRSyncPatchCCBKickSyncInfos(IMG_HANDLE ahSyncs[SGX_MAX_SRC_SYNCS_TA],
			     PVRSRV_DEVICE_SYNC_OBJECT asDevSyncs[SGX_MAX_SRC_SYNCS_TA],
			     IMG_UINT32 *pui32NumSrcSyncs)
{
	PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[SGX_MAX_SRC_SYNCS_TA];
	struct dma_fence *apsFence[SGX_MAX_SRC_SYNCS_TA] = {};
	IMG_UINT32 i, ui32NumRealSrcSyncs;
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (!ExpandAndDeDuplicateFenceSyncs(*pui32NumSrcSyncs,
					    (int *)ahSyncs,
					    SGX_MAX_SRC_SYNCS_TA,
					    apsFence,
					    &ui32NumRealSrcSyncs,
					    apsSyncInfo)) {
		eError = PVRSRV_ERROR_HANDLE_NOT_FOUND;
		goto err_put_fence;
	}

	for (i = 0; i < ui32NumRealSrcSyncs; i++) {
		PVRSRV_KERNEL_SYNC_INFO *psi = apsSyncInfo[i];
		CopyKernelSyncInfoToDeviceSyncObject(psi, &asDevSyncs[i]);
		psi->psSyncData->ui32ReadOpsPending++;
		ahSyncs[i] = psi;
	}
	*pui32NumSrcSyncs = ui32NumRealSrcSyncs;

err_put_fence:
	for (i = 0; i < SGX_MAX_SRC_SYNCS_TA && apsFence[i]; i++)
		dma_fence_put(apsFence[i]);
	return eError;
}

IMG_INTERNAL PVRSRV_ERROR
PVRSyncPatchTransferSyncInfos(IMG_HANDLE ahSyncs[SGX_MAX_SRC_SYNCS_TA],
			      PVRSRV_DEVICE_SYNC_OBJECT asDevSyncs[SGX_MAX_SRC_SYNCS_TA],
			      IMG_UINT32 *pui32NumSrcSyncs)
{
	struct PVR_ALLOC_SYNC_DATA *psTransferSyncData;
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (*pui32NumSrcSyncs != 1)
		PVR_DPF((PVR_DBG_ERROR, "%s: Invalid num syncs", __func__));

	psTransferSyncData = PVRSyncAllocFDGet((int)ahSyncs[0]);
	if (!psTransferSyncData) {
		eError = PVRSRV_ERROR_HANDLE_NOT_FOUND;
		goto err_out;
	}

	psSyncInfo = psTransferSyncData->psSyncInfo->psBase;

	CopyKernelSyncInfoToDeviceSyncObject(psSyncInfo, &asDevSyncs[0]);
	CopyKernelSyncInfoToDeviceSyncObject(psTransferSyncData->psTimeline->psSyncInfo->psBase,
					     &asDevSyncs[1]);

	psSyncInfo->psSyncData->ui32WriteOpsPending++;
	psTransferSyncData->psTimeline->psSyncInfo->psBase->psSyncData->ui32WriteOpsPending++;

	ahSyncs[0] = psSyncInfo;
	ahSyncs[1] = psTransferSyncData->psTimeline->psSyncInfo->psBase;
	*pui32NumSrcSyncs = 2;

	fput(psTransferSyncData->psFile);
err_out:
	return eError;
}

IMG_INTERNAL PVRSRV_ERROR
PVRSyncFencesToSyncInfos(PVRSRV_KERNEL_SYNC_INFO *apsSyncs[],
			 IMG_UINT32 *pui32NumSyncs,
			 struct dma_fence *apsFence[SGX_MAX_SRC_SYNCS_TA])
{
	PVRSRV_KERNEL_SYNC_INFO *apsSyncInfo[SGX_MAX_SRC_SYNCS_TA];
	IMG_UINT32 ui32NumRealSrcSyncs;
	PVRSRV_ERROR eError = PVRSRV_OK;
	IMG_UINT32 i;

	memset(apsFence, 0, sizeof(struct dma_fence *) * SGX_MAX_SRC_SYNCS_TA);

	if (!ExpandAndDeDuplicateFenceSyncs(*pui32NumSyncs,
					    (int *)apsSyncs,
					    *pui32NumSyncs,
					    apsFence,
					    &ui32NumRealSrcSyncs,
					    apsSyncInfo)) {
		for (i = 0; i < SGX_MAX_SRC_SYNCS_TA && apsFence[i]; i++)
			dma_fence_put(apsFence[i]);
		return PVRSRV_ERROR_HANDLE_NOT_FOUND;
	}

	PVR_ASSERT(ui32NumRealSrcSyncs <= *pui32NumSyncs);
	for (i = 0; i < ui32NumRealSrcSyncs; i++)
		apsSyncs[i] = apsSyncInfo[i];
	*pui32NumSyncs = ui32NumRealSrcSyncs;
	return eError;
}

/* ---------- Module init / exit ---------- */
static PVRSRV_ERROR PVRSyncInitServices(void)
{
	IMG_BOOL bCreated, bShared[PVRSRV_MAX_CLIENT_HEAPS];
	PVRSRV_HEAP_INFO sHeapInfo[PVRSRV_MAX_CLIENT_HEAPS];
	IMG_UINT32 ui32ClientHeapCount = 0;
	PVRSRV_PER_PROCESS_DATA	*psPerProc;
	PVRSRV_ERROR eError;

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	gsSyncServicesConnection.ui32Pid = OSGetCurrentProcessIDKM();
	eError = PVRSRVProcessConnect(gsSyncServicesConnection.ui32Pid, 0);
	if (eError != PVRSRV_OK)
		goto err_unlock;

	psPerProc = PVRSRVFindPerProcessData();
	if (!psPerProc)
		goto err_disconnect;

	eError = PVRSRVAcquireDeviceDataKM(0, PVRSRV_DEVICE_TYPE_SGX,
					   &gsSyncServicesConnection.hDevCookie);
	if (eError != PVRSRV_OK)
		goto err_disconnect;

	eError = PVRSRVCreateDeviceMemContextKM(gsSyncServicesConnection.hDevCookie,
						psPerProc,
						&gsSyncServicesConnection.hDevMemContext,
						&ui32ClientHeapCount,
						&sHeapInfo[0],
						&bCreated,
						&bShared[0]);
	if (eError != PVRSRV_OK)
		goto err_disconnect;

	LinuxUnLockMutex(&gPVRSRVLock);
	return PVRSRV_OK;

err_disconnect:
	PVRSRVProcessDisconnect(gsSyncServicesConnection.ui32Pid);
err_unlock:
	LinuxUnLockMutex(&gPVRSRVLock);
	return eError;
}

static void PVRSyncCloseServices(void)
{
	IMG_BOOL bDummy;
	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	PVRSRVDestroyDeviceMemContextKM(gsSyncServicesConnection.hDevCookie,
					gsSyncServicesConnection.hDevMemContext,
					&bDummy);
	gsSyncServicesConnection.hDevMemContext = NULL;
	gsSyncServicesConnection.hDevCookie = NULL;
	PVRSRVProcessDisconnect(gsSyncServicesConnection.ui32Pid);
	gsSyncServicesConnection.ui32Pid = 0;
	LinuxUnLockMutex(&gPVRSRVLock);
}

static const struct file_operations gsPVRSyncFOps = {
	.owner			= THIS_MODULE,
	.open			= PVRSyncOpen,
	.release		= PVRSyncRelease,
	.unlocked_ioctl	= PVRSyncIOCTL,
};

static struct miscdevice gsPVRSyncDev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "pvr_sync",
	.fops  = &gsPVRSyncFOps,
};

IMG_INTERNAL int PVRSyncDeviceInit(void)
{
	int err = -1;

	if (PVRSyncInitServices() != PVRSRV_OK) {
		PVR_DPF((PVR_DBG_ERROR, "%s: Services init failed", __func__));
		return err;
	}

	gpsWorkQueue = create_freezable_workqueue("pvr_sync_workqueue");
	if (!gpsWorkQueue) {
		PVRSyncCloseServices();
		return err;
	}
	INIT_WORK(&gsWork, PVRSyncWorkQueueFunction);

	err = misc_register(&gsPVRSyncDev);
	if (err) {
		destroy_workqueue(gpsWorkQueue);
		PVRSyncCloseServices();
	}
	return err;
}

IMG_INTERNAL void PVRSyncDeviceDeInit(void)
{
	misc_deregister(&gsPVRSyncDev);
	destroy_workqueue(gpsWorkQueue);
	PVRSyncCloseServices();
}
