// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_submit.c — command submission: kernel CCB + TA/3D kicks.
 *
 * Fence ownership invariant (one fence per submit):
 *
 *   ref #1  dma_fence_init()            → owned by this function until
 *                                          transferred to sync_file or freed
 *   ref #2  per-BO dma_resv refs        → dma_resv_add_fence() takes one
 *                                          ref per BO internally; driver does
 *                                          NOT take an extra get() for this
 *   ref #3  dma_fence_get() for pending → released by handle_completion()
 *                                          or hw_fini() forced-retirement
 *
 * PM reference: one pm_runtime_resume_and_get() per submit, released
 * by handle_completion() / hw_fini() on the success path, or by
 * submit_ioctl() itself on every error path.
 *
 * busy_count: incremented just before CCB publish, decremented by
 * handle_completion() / hw_fini().  Never touches the CCB timeout path
 * because ccb_schedule() returns only after the fence is signalled.
 */
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/units.h>
#include <linux/delay.h>
#include <linux/sync_file.h>
#include <linux/pm_runtime.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_exec.h>
#include <linux/iosys-map.h>
#include <linux/dma-resv.h>
#include <linux/vmalloc.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

#define HWRTDATA_SIZE			496
#define PRISMRV_CCB_DRAIN_TIMEOUT_MS	2000
#define PRISMRV_MAX_SUBMIT_BOS		256
/* uKernel service entry index per command type (userspace may only use TA) */
static const unsigned int prismrv_hostkick_instr[PRISMRV_CMD_COUNT] = {
	554,  /* TA */
	523,  /* TRANSFER */
	486,  /* 2D */
	397,  /* POWER */
	427,  /* CONTEXTSUSPEND */
	75,   /* CLEANUP */
	1115, /* GETMISCINFO */
	1017, /* PROCESS_QUEUES */
	0,    /* DATABREAKPOINT: not implemented in this uKernel */
	1023, /* SETHWPERFSTATUS */
};

#define PRISMRV_MAX_IN_FENCES		64
#define PRISMRV_HANG_TIMEOUT_MS		4000
/*
 * Soft cap on in-flight jobs (the ring itself holds 255).  Every job owns
 * a kernel snapshot of up to PRISMRV_STREAM_MAX_BYTES, so this bounds the
 * memory one device can pin on behalf of render-node clients to
 * 64 x 512 KiB = 32 MiB.
 */
#define PRISMRV_CCB_MAX_OUTSTANDING	64

static const char *prismrv_fence_name(struct dma_fence *f)
{
	return "prismrv";
}

const struct dma_fence_ops prismrv_fence_ops = {
	.get_driver_name  = prismrv_fence_name,
	.get_timeline_name = prismrv_fence_name,
};

int prismrv_ccb_init(struct prismrv_device *pv)
{
	int ret;

	spin_lock_init(&pv->ccb_lock);
	/* a fresh CCB ring restarts the 32-bit command counters */
	pv->ccb_submitted = 0;
	pv->ccb_completed = 0;
	pv->ccb_last_read = 0;
	pv->last_progress = jiffies;

	if (pv->ccb) {
		memset(pv->ccb, 0, sizeof(*pv->ccb));
		wmb();
		return 0;
	}

	pv->ccb = dma_alloc_coherent(pv->drm.dev, sizeof(*pv->ccb),
				     &pv->ccb_dma, GFP_KERNEL);
	if (!pv->ccb)
		return -ENOMEM;
	memset(pv->ccb, 0, sizeof(*pv->ccb));

	pv->hwrt = dma_alloc_coherent(pv->drm.dev, 2 * HWRTDATA_SIZE,
				      &pv->hwrt_dma, GFP_KERNEL);
	if (!pv->hwrt) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->ccb), pv->ccb,
				  pv->ccb_dma);
		pv->ccb = NULL;
		return -ENOMEM;
	}

	ret = prismrv_mmu_map(pv, PRISMRV_HOSTCTL_VADDR,
			      pv->hostctl_dma, sizeof(*pv->hostctl));
	if (ret)
		goto err_mmu_hostctl;

	ret = prismrv_mmu_map(pv, PRISMRV_CCB_VADDR,
			      pv->ccb_dma, sizeof(*pv->ccb));
	if (ret)
		goto err_mmu_ccb;

	ret = prismrv_mmu_map(pv, PRISMRV_HWRTDATA_VADDR,
			      pv->hwrt_dma, 2 * HWRTDATA_SIZE);
	if (ret)
		goto err_mmu_hwrt;

	return 0;

err_mmu_hwrt:
	prismrv_mmu_unmap(pv, PRISMRV_CCB_VADDR, sizeof(*pv->ccb));
err_mmu_ccb:
	prismrv_mmu_unmap(pv, PRISMRV_HOSTCTL_VADDR, sizeof(*pv->hostctl));
err_mmu_hostctl:
	if (pv->hwrt) {
		dma_free_coherent(pv->drm.dev, 2 * HWRTDATA_SIZE, pv->hwrt,
				  pv->hwrt_dma);
		pv->hwrt = NULL;
	}
	if (pv->ccb) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->ccb), pv->ccb,
				  pv->ccb_dma);
		pv->ccb = NULL;
	}
	return ret;
}

void prismrv_ccb_fini(struct prismrv_device *pv)
{
	if (pv->ccb) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->ccb), pv->ccb,
				  pv->ccb_dma);
		pv->ccb = NULL;
	}
	if (pv->hwrt) {
		dma_free_coherent(pv->drm.dev, 2 * HWRTDATA_SIZE, pv->hwrt,
				  pv->hwrt_dma);
		pv->hwrt = NULL;
	}
}

/*
 * Full when PRISMRV_CCB_MAX_OUTSTANDING commands are outstanding (the
 * ring size, 255, keeps one slot free so write == read means empty).  Syncs the consumed counter first:
 * see prismrv_ccb_sync_locked() for why that makes the 8-bit to 32-bit
 * extension unambiguous.  Called with ccb_lock held.
 */
static bool prismrv_ccb_full(struct prismrv_device *pv)
{
	unsigned long flags;
	u32 outstanding;

	spin_lock_irqsave(&pv->event_lock, flags);
	prismrv_ccb_sync_locked(pv);
	outstanding = pv->ccb_submitted - pv->ccb_completed;
	spin_unlock_irqrestore(&pv->event_lock, flags);

	return outstanding >= PRISMRV_CCB_MAX_OUTSTANDING;
}

/*
 * prismrv_ccb_schedule() — acquire a CCB slot and publish the command.
 *
 * On success: pf->ccb_slot is set to the written slot index and the
 * fence has been appended to pv->pending_fences.  Returns 0.
 *
 * On CCB-full timeout: the fence is signalled with -ETIMEDOUT, removed
 * from pending_fences (it was never added here — see call site), and
 * -ETIMEDOUT is returned.  busy_count and PM ref were never incremented
 * by the time ccb_schedule() returns, so the caller does not need to
 * undo them.
 *
 * Key ordering:
 *   1. Acquire CCB slot (spin on ccb_lock).
 *   2. Write command, set pf->ccb_slot.
 *   3. Publish write_offset (wmb + WRITE_ONCE).
 *   4. Add fence to pending_fences (event_lock).
 *   5. Increment busy_count.
 *   6. Kick the GPU.
 *
 * Steps 4–6 must follow step 3 so the IRQ handler always finds a fully
 * published command when it tries to retire the fence.
 */
static int prismrv_ccb_schedule(struct prismrv_device *pv,
				enum prismrv_cmd_type type,
				const __le32 data[6],
				struct prismrv_fence *pf)
{
	struct prismrv_ccb_cmd *cmd;
	unsigned long flags;
	u32 slot;

	spin_lock(&pv->ccb_lock);

	/*
	 * The submitter waited for room BEFORE taking submit_rwsem
	 * (prismrv_wait_ccb_space()), and submit_order keeps other
	 * submitters out, so only a concurrent retirement can have changed
	 * the count since - towards "more room".  Waiting here would mean
	 * sleeping with the read side of submit_rwsem held, which blocks
	 * recovery - the very thing that frees a wedged ring.
	 */
	if (prismrv_ccb_full(pv)) {
		spin_unlock(&pv->ccb_lock);
		spin_lock(&pf->lock);
		dma_fence_set_error(&pf->base, -EBUSY);
		dma_fence_signal_locked(&pf->base);
		spin_unlock(&pf->lock);
		return -EBUSY;
	}

	slot = le32_to_cpu(READ_ONCE(pv->ccb->write_offset)) & 255;
	cmd  = &pv->ccb->commands[slot];

	cmd->service_address =
		cpu_to_le32(PRISMRV_UKERNEL_VADDR +
			    prismrv_hostkick_instr[type] * 8);
	cmd->cache_control = 0;
	memcpy(cmd->data, data, sizeof(cmd->data));

	/*
	 * Ordering (critical for correctness):
	 *   1. Write command to CCB slot and set ccb_slot.
	 *   2. Add fence to pending_fences (so IRQ can retire it).
	 *   3. Increment busy_count.
	 *   4. Publish write_offset (wmb + WRITE_ONCE) — GPU sees new command.
	 *   5. Kick the GPU.
	 *
	 * write_offset is published AFTER pending_fences insertion so the
	 * IRQ handler always finds the fence when it processes the completion.
	 * If we published first, the GPU could complete and fire the IRQ
	 * before the fence was in pending_fences, causing the completion to
	 * be missed and the fence to hang forever.
	 *
	 * read_offset is compared modulo 256 (ring), so a future slot being
	 * in pending_fences before write_offset is bumped is safe: the
	 * handle_completion() distance check ensures the slot is not retired
	 * until read_offset actually advances past it.
	 */
	pf->ccb_slot = (u16)slot;
	/* unambiguous 32-bit identity of this command (see irq.c) */
	pf->ccb_seq = pv->ccb_submitted++;

	/* wmb: ensure command data is visible before appending to pending */
	wmb();

	/* Step 2 & 3: register with IRQ path before GPU can see the command */
	spin_lock_irqsave(&pv->event_lock, flags);
	list_add_tail(&pf->node, &pv->pending_fences);
	spin_unlock_irqrestore(&pv->event_lock, flags);

	if (atomic_inc_return(&pv->busy_count) == 1)
		WRITE_ONCE(pv->last_progress, jiffies);

	/* Step 4: now publish — GPU may start executing after this */
	WRITE_ONCE(pv->ccb->write_offset, cpu_to_le32((slot + 1) & 255));

	spin_unlock(&pv->ccb_lock);

	/* Step 5: kick, and make sure the hang watchdog is armed */
	writel(EUR_CR_EVENT_KICK_NOW_MASK, pv->regs + EUR_CR_EVENT_KICK);
	schedule_delayed_work(&pv->hang_work,
			      msecs_to_jiffies(PRISMRV_HANG_TIMEOUT_MS / 2));
	return 0;
}

int prismrv_wait_in_fences(u32 num_fds, const u32 __user *user_fds)
{
	u32 *fds;
	unsigned int i;
	int ret = 0;

	if (!num_fds)
		return 0;
	if (num_fds > PRISMRV_MAX_IN_FENCES)
		return -EINVAL;

	fds = kmalloc_array(num_fds, sizeof(*fds), GFP_KERNEL);
	if (!fds)
		return -ENOMEM;

	if (copy_from_user(fds, user_fds, array_size(num_fds, sizeof(u32)))) {
		kfree(fds);
		return -EFAULT;
	}

	for (i = 0; i < num_fds; i++) {
		struct dma_fence *fence;

		fence = sync_file_get_fence(fds[i]);
		if (!fence) {
			ret = -EINVAL;
			break;
		}
		ret = dma_fence_wait_timeout(fence, true, MAX_SCHEDULE_TIMEOUT);
		dma_fence_put(fence);
		if (ret < 0)
			break;
	}
	kfree(fds);

	if (ret > 0)
		return 0;
	return (int)ret ?: -ETIMEDOUT;
}

static int prismrv_read_bo(void *ctx, struct drm_gem_object *bo, u64 off,
			   void *dst, size_t len)
{
	struct iosys_map map;
	long w;
	int ret;

	if (off > bo->size || len > bo->size - off)
		return -EINVAL;

	/*
	 * The snapshot must see the BO's final contents: a job that is
	 * still writing it (e.g. it is that job's render target) has to
	 * finish first, and the GPU's writes have to be visible to the CPU.
	 * Wait for the existing fences under the reservation lock so
	 * nothing new can be queued between the wait and the read.
	 */
	ret = dma_resv_lock_interruptible(bo->resv, NULL);
	if (ret)
		return ret;
	w = dma_resv_wait_timeout(bo->resv, DMA_RESV_USAGE_WRITE, true,
				  MAX_SCHEDULE_TIMEOUT);
	if (w < 0) {
		dma_resv_unlock(bo->resv);
		return w;
	}
	ret = drm_gem_shmem_vmap_locked(to_drm_gem_shmem_obj(bo), &map);
	if (ret) {
		dma_resv_unlock(bo->resv);
		return ret;
	}
	prismrv_bo_sync_for_cpu(bo);
	iosys_map_memcpy_from(dst, &map, off, len);
	drm_gem_shmem_vunmap_locked(to_drm_gem_shmem_obj(bo), &map);
	dma_resv_unlock(bo->resv);
	return 0;
}

/*
 * Build the kernel-owned snapshot the GPU will execute.
 *
 * The user's command stream and every TA packet block it references are
 * copied into kernel memory, validated there (never in user memory), and
 * laid out in one BO that has no handle and cannot be mapped by
 * userspace; the DRAW addresses are rewritten to point into it.  objs[0]
 * (the user's command BO) is replaced by the snapshot.  Textures and
 * render targets stay where they are: they are data, and their extents
 * were validated.  Sleeps; call outside submit_rwsem.
 */
static int prismrv_snapshot_stream(struct prismrv_device *pv,
				   struct drm_gem_object **objs,
				   unsigned int num_bos, u32 cmd_size)
{
	struct iosys_map dmap;
	struct drm_gem_object *snap;
	size_t size;
	void *copy;
	int ret;

	if (!cmd_size || cmd_size % 4 || cmd_size > PRISMRV_STREAM_MAX_BYTES)
		return -EINVAL;

	copy = kvmalloc(cmd_size, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;

	ret = prismrv_read_bo(NULL, objs[0], 0, copy, cmd_size);
	if (ret)
		goto out_free;

	/* objs[1..] are the BOs the stream may reference */
	ret = prismrv_validate_stream(pv, copy, cmd_size / 4, objs + 1, num_bos);
	if (ret) {
		dev_dbg(pv->drm.dev, "command stream rejected\n");
		goto out_free;
	}

	size = prismrv_stream_snapshot_size(copy, cmd_size / 4);
	if (size > PRISMRV_STREAM_MAX_BYTES) {
		ret = -E2BIG;
		goto out_free;
	}

	snap = prismrv_bo_create(pv, size, 0);
	if (IS_ERR(snap)) {
		ret = PTR_ERR(snap);
		goto out_free;
	}
	ret = drm_gem_vmap(snap, &dmap);
	if (ret || dmap.is_iomem) {
		if (!ret)
			drm_gem_vunmap(snap, &dmap);
		drm_gem_object_put(snap);
		ret = ret ?: -EIO;
		goto out_free;
	}
	ret = prismrv_stream_snapshot_fill(copy, cmd_size / 4, objs + 1,
					   num_bos, prismrv_bo_gpuva(snap),
					   dmap.vaddr, prismrv_read_bo, NULL);
	drm_gem_vunmap(snap, &dmap);
	if (ret) {
		dev_dbg(pv->drm.dev, "TA stream rejected\n");
		drm_gem_object_put(snap);
		goto out_free;
	}

	drm_gem_object_put(objs[0]);
	objs[0] = snap;		/* we own the creation reference */
out_free:
	kvfree(copy);
	return ret;
}

/*
 * Wait, WITHOUT submit_rwsem, until the ring has room.  Only the software
 * counters are consulted (no CCB memory is touched, so this is safe while a
 * recovery is tearing the ring down).  A wedged GPU is cleared by the hang
 * watchdog / recovery, which retire the jobs and wake us.  Caller holds
 * submit_order, so nobody else can take the room we see.
 */
static int prismrv_wait_ccb_space(struct prismrv_device *pv)
{
	unsigned long deadline = jiffies +
		msecs_to_jiffies(PRISMRV_CCB_DRAIN_TIMEOUT_MS);
	long r;

	for (;;) {
		unsigned long flags;
		u32 outstanding;

		spin_lock_irqsave(&pv->event_lock, flags);
		outstanding = pv->ccb_submitted - pv->ccb_completed;
		spin_unlock_irqrestore(&pv->event_lock, flags);
		if (outstanding < PRISMRV_CCB_MAX_OUTSTANDING)
			return 0;

		if (time_after(jiffies, deadline)) {
			dev_err(pv->drm.dev, "CCB full for %dms\n",
				PRISMRV_CCB_DRAIN_TIMEOUT_MS);
			if (READ_ONCE(pv->hw_ready))
				prismrv_request_recovery(pv);
			return -EBUSY;
		}
		r = wait_event_interruptible_timeout(
			pv->ccb_wq,
			READ_ONCE(pv->ccb_submitted) - READ_ONCE(pv->ccb_completed) <
				PRISMRV_CCB_MAX_OUTSTANDING,
			msecs_to_jiffies(100));
		if (r < 0)
			return r;
	}
}

int prismrv_submit_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_submit *args = data;
	struct drm_gem_object **objs = NULL;
	struct prismrv_fence *f = NULL;
	struct sync_file *sf = NULL;
	struct drm_exec exec;
	u32 __user *in_fds;
	__le32 cmd_data[6] = {};
	int ret = 0, fd = -1;
	bool exec_active = false, rwsem_held = false;
	unsigned int i;

	ret = pm_runtime_resume_and_get(pv->drm.dev);
	if (ret)
		return ret;

	/* a failed probe/recovery leaves hw_ready false with the PM core
	 * believing the device is active: retry the bring-up here */
	if (!READ_ONCE(pv->hw_ready)) {
		ret = prismrv_hw_reinit(pv);
		if (ret) {
			dev_err(pv->drm.dev, "GPU re-initialisation failed (%d)\n", ret);
			ret = -ENODEV;
			goto err_pm;
		}
	}

	/* ----------------------------------------------------------------
	 * Phase 1 — outside submit_rwsem: validate, GEM lookup, fence wait.
	 *
	 * All operations that can sleep (fence waits) MUST happen before
	 * taking the read-lock so that recovery_work() can take the
	 * write-lock without deadlocking against a sleeping submit.
	 * ---------------------------------------------------------------- */
	/*
	 * Only TA jobs are available to userspace.  The other service
	 * routines (power, context suspend, cleanup, queue processing,
	 * misc info, perf status, ...) are driver-internal and must never
	 * be selectable through a render node; DATABREAKPOINT has no
	 * routine in this uKernel at all (entry 0).  The enum keeps the
	 * full list for the driver's own use.
	 */
	if (args->cmd_type != PRISMRV_CMD_TA ||
	    args->num_bos > PRISMRV_MAX_SUBMIT_BOS ||
	    args->num_in_fences > PRISMRV_MAX_IN_FENCES) {
		ret = -EINVAL;
		goto err_pm;
	}

	objs = kvcalloc(args->num_bos + 1, sizeof(*objs), GFP_KERNEL);
	if (!objs) {
		ret = -ENOMEM;
		goto err_pm;
	}

	in_fds = u64_to_user_ptr(args->in_fences);
	ret = prismrv_wait_in_fences(args->num_in_fences, in_fds);
	if (ret)
		goto err_objs;

	objs[0] = drm_gem_object_lookup(file, args->cmd_handle);
	if (!objs[0]) {
		ret = -ENOENT;
		goto err_objs;
	}
	if (args->cmd_size > objs[0]->size) {
		ret = -EINVAL;
		drm_gem_object_put(objs[0]);
		objs[0] = NULL;
		goto err_objs;
	}

	if (args->num_bos > 0) {
		struct drm_gem_object **user_objs = NULL;

		ret = drm_gem_objects_lookup(file,
			u64_to_user_ptr(args->bos), args->num_bos,
			&user_objs);
		if (ret)
			goto err_objs;
		memcpy(objs + 1, user_objs,
		       args->num_bos * sizeof(*user_objs));
		kvfree(user_objs);
	}

	/*
	 * Validate + snapshot the stream.  The snapshot is a kernel-owned
	 * BO that has no handle; from here on objs[0] is that snapshot.
	 */
	ret = prismrv_snapshot_stream(pv, objs, args->num_bos, args->cmd_size);
	if (ret)
		goto err_objs;

	/* ----------------------------------------------------------------
	 * Phase 2 — still no submit_rwsem.  Everything that can sleep for a
	 * long time happens here, in this order:
	 *   pages / sg tables  (takes reservation locks internally)
	 *   submit_order       (one submitter at a time: seqno == CCB order)
	 *   ring room          (waits for retirement / recovery)
	 *   reservation locks of every BO (drm_exec) + wait for their fences
	 * Phase 3 then takes submit_rwsem (read) for a short, non-sleeping
	 * stretch.  submit_rwsem must never be held across any wait above:
	 * recovery takes its write side and has to be able to run while a job
	 * is hung - the job whose fence we may be waiting for.
	 * ---------------------------------------------------------------- */
	ret = prismrv_gem_prepare(objs, args->num_bos + 1);
	if (ret)
		goto err_objs;

	ret = mutex_lock_interruptible(&pv->submit_order);
	if (ret)
		goto err_objs;
	ret = prismrv_wait_ccb_space(pv);
	if (ret)
		goto err_unlock_order;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		ret = -ENOMEM;
		goto err_unlock_order;
	}
	spin_lock_init(&f->lock);
	INIT_LIST_HEAD(&f->node);
	f->ccb_slot = 0xFFFF;
	/*
	 * One timeline for the whole device: the CCB runs jobs in order, so
	 * fences share a context and seqno order equals CCB order
	 * (submit_order is held until the command is published).
	 */
	dma_fence_init(&f->base, &prismrv_fence_ops, &f->lock,
		       pv->fence_context,
		       atomic64_inc_return(&pv->fence_seqno));
	/* ref #1: held by this function; transferred to sync_file below */

	/* BO references now belong to the fence */
	f->bos     = objs;
	f->num_bos = args->num_bos + 1;
	objs       = NULL;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_fence_bos;
	}

	/* sync_file_create() takes its own reference on the fence */
	sf = sync_file_create(&f->base);
	if (!sf) {
		ret = -ENOMEM;
		goto err_put_fd;
	}
	/* ref #1 is now owned by sf; drop our copy */
	dma_fence_put(&f->base);
	/* f is still valid: sf holds a ref, and we have ref #2 and #3 below */

	/*
	 * Take every reservation lock (drm_exec: ww_mutex deadlock
	 * avoidance, duplicates tolerated) and reserve one fence slot each;
	 * slots only survive while the lock is held.
	 */
	drm_exec_init(&exec,
		      DRM_EXEC_INTERRUPTIBLE_WAIT | DRM_EXEC_IGNORE_DUPLICATES,
		      f->num_bos);
	exec_active = true;
	drm_exec_until_all_locked(&exec) {
		for (i = 0; i < f->num_bos; i++) {
			ret = drm_exec_prepare_obj(&exec, f->bos[i], 1);
			drm_exec_retry_on_contention(&exec);
			if (ret)
				goto err_unlock;
		}
	}
	/*
	 * Implicit sync, atomically with the fence installation: wait for
	 * the fences already on each BO while its reservation is held, so
	 * no other submitter can slip a fence in between our wait and our
	 * add.  The fences waited for are signalled by the IRQ thread or by
	 * recovery, neither of which takes a reservation lock or
	 * submit_order, and we hold no submit_rwsem here.
	 */
	for (i = 0; i < f->num_bos; i++) {
		long r = dma_resv_wait_timeout(f->bos[i]->resv,
					       DMA_RESV_USAGE_READ, true,
					       MAX_SCHEDULE_TIMEOUT);

		if (r < 0) {
			ret = r;
			goto err_unlock;
		}
	}

	/* ----------------------------------------------------------------
	 * Phase 3 — submit_rwsem (read): hardware state is stable.  Short
	 * and non-sleeping apart from mmu_lock.
	 * ---------------------------------------------------------------- */
	down_read(&pv->submit_rwsem);
	rwsem_held = true;

	if (!pv->hw_ready) {
		ret = -ENODEV;
		goto err_unlock;
	}

	ret = prismrv_gem_populate(pv, f->bos, f->num_bos);
	if (ret)
		goto err_unlock;

	cmd_data[0] = cpu_to_le32(prismrv_bo_gpuva(f->bos[0]));
	cmd_data[1] = cpu_to_le32(args->cmd_size);
	if (args->num_bos >= 1 && f->bos[1])
		cmd_data[2] = cpu_to_le32(prismrv_bo_gpuva(f->bos[1]));

	for (i = 0; i < f->num_bos; i++)
		dma_resv_add_fence(f->bos[i]->resv, &f->base,
				   DMA_RESV_USAGE_WRITE);
	drm_exec_fini(&exec);
	exec_active = false;

	/*
	 * Take ref #3 for pending_fences before ccb_schedule() publishes
	 * the command.  ccb_schedule() appends the fence to pending_fences
	 * only AFTER write_offset is bumped, so the IRQ always sees a
	 * fence with a valid ccb_slot.
	 */
	dma_fence_get(&f->base);	/* ref #3: for pending_fences */

	/*
	 * Hand every BO of the job to the device: write back whatever the
	 * CPU left in its caches (vertex data, textures, the kernel-written
	 * snapshot) before the uKernel can see the command.
	 */
	for (i = 0; i < f->num_bos; i++)
		prismrv_bo_sync_for_device(f->bos[i]);

	ret = prismrv_ccb_schedule(pv, args->cmd_type, cmd_data, f);
	if (ret) {
		/*
		 * CCB timeout: ccb_schedule() signalled the fence with
		 * -ETIMEDOUT using pf->lock (correct lock for
		 * dma_fence_signal_locked) and returned before adding
		 * the fence to pending_fences.  busy_count was never
		 * incremented.  Drop ref #3 (never consumed by the
		 * pending list) and release BO refs.
		 */
		dma_fence_put(&f->base);	/* drop ref #3 */
		/* BOs, sync_file, PM ref: released at err_unlock */
		goto err_unlock;
	}

	/*
	 * Success: publish the fd to userspace NOW, after the CCB slot is
	 * secured.  If fd_install were done before ccb_schedule(), a
	 * failing ioctl would leave a dangling sync_file fd in the process.
	 */
	args->out_fence_fd = fd;
	fd_install(fd, sf->file);

	up_read(&pv->submit_rwsem);
	mutex_unlock(&pv->submit_order);
	/* PM ref is kept; handle_completion() / hw_fini() will release it */
	return 0;

err_unlock:
	if (rwsem_held)
		up_read(&pv->submit_rwsem);
	if (exec_active)
		drm_exec_fini(&exec);
	mutex_unlock(&pv->submit_order);
	/* f owns the BOs; sf owns the fence */
	prismrv_fence_release_bos(f);
	fput(sf->file);		/* drops sf + its fence ref */
	put_unused_fd(fd);
	goto err_pm;

err_put_fd:
	put_unused_fd(fd);
err_fence_bos:
	prismrv_fence_release_bos(f);
	mutex_unlock(&pv->submit_order);
	dma_fence_put(&f->base);	/* drop ref #1 */
	goto err_pm;

err_unlock_order:
	mutex_unlock(&pv->submit_order);
err_objs:
	if (objs) {
		for (i = 0; i <= args->num_bos; i++)
			if (objs[i])
				drm_gem_object_put(objs[i]);
		kvfree(objs);
	}
err_pm:
	pm_runtime_mark_last_busy(pv->drm.dev);
	pm_runtime_put_autosuspend(pv->drm.dev);
	return ret;
}
