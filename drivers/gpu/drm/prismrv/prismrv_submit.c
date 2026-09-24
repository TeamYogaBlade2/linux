// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_submit.c — command submission: kernel CCB + TA/3D kicks.
 *
 * Fence ownership invariant (one fence per submit):
 *
 *   ref #1  dma_fence_init()            → owned by this function until
 *                                          transferred to sync_file or freed
 *   ref #2  dma_fence_get() for resv    → released at end of submit_ioctl
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
#include <linux/iosys-map.h>
#include <linux/dma-resv.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

#define HWRTDATA_SIZE			496
#define PRISMRV_CCB_DRAIN_TIMEOUT_MS	2000
#define PRISMRV_MAX_SUBMIT_BOS		256
#define PRISMRV_MAX_IN_FENCES		64

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

static bool prismrv_ccb_full(struct prismrv_device *pv)
{
	u32 w = le32_to_cpu(READ_ONCE(pv->ccb->write_offset)) & 255;
	u32 r = le32_to_cpu(READ_ONCE(pv->ccb->read_offset))  & 255;

	return ((w + 1 - r) & 255) == 0;
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
	u32 slot;

	spin_lock(&pv->ccb_lock);

	if (prismrv_ccb_full(pv)) {
		unsigned long deadline = jiffies +
			msecs_to_jiffies(PRISMRV_CCB_DRAIN_TIMEOUT_MS);

		spin_unlock(&pv->ccb_lock);
		do {
			writel(EUR_CR_EVENT_KICK_NOW_MASK,
			       pv->regs + EUR_CR_EVENT_KICK);
			usleep_range(500, 1000);
			if (time_after(jiffies, deadline)) {
				dev_err(pv->drm.dev,
					"CCB full for %dms — scheduling recovery\n",
					PRISMRV_CCB_DRAIN_TIMEOUT_MS);
				if (READ_ONCE(pv->hw_ready))
					schedule_work(&pv->recovery_work);

				/*
				 * Signal the fence with -ETIMEDOUT using the
				 * fence's own spinlock (required by
				 * dma_fence_signal_locked).  The fence has NOT
				 * yet been added to pending_fences, so no
				 * concurrent IRQ path can race here.
				 */
				spin_lock(&pf->lock);
				dma_fence_set_error(&pf->base, -ETIMEDOUT);
				dma_fence_signal_locked(&pf->base);
				spin_unlock(&pf->lock);

				return -ETIMEDOUT;
			}
			spin_lock(&pv->ccb_lock);
		} while (prismrv_ccb_full(pv));
	}

	slot = le32_to_cpu(READ_ONCE(pv->ccb->write_offset)) & 255;
	cmd  = &pv->ccb->commands[slot];

	cmd->service_address =
		cpu_to_le32(PRISMRV_UKERNEL_VADDR +
			    prismrv_hostkick_instr[type] * 8);
	cmd->cache_control = 0;
	memcpy(cmd->data, data, sizeof(cmd->data));

	/*
	 * Record the slot BEFORE publishing write_offset.  The IRQ
	 * handler reads pf->ccb_slot under event_lock and only retires
	 * fences whose slot has been consumed.  If the slot were set
	 * after publish, the IRQ could fire first and see 0xFFFF.
	 */
	pf->ccb_slot = (u16)slot;

	wmb();
	WRITE_ONCE(pv->ccb->write_offset, cpu_to_le32((slot + 1) & 255));

	spin_unlock(&pv->ccb_lock);

	/*
	 * Add to pending_fences only AFTER the command is published in
	 * the CCB ring.  This guarantees that any IRQ that fires and
	 * walks pending_fences will only see fences with a valid ccb_slot.
	 */
	spin_lock(&pv->event_lock);
	list_add_tail(&pf->node, &pv->pending_fences);
	spin_unlock(&pv->event_lock);

	atomic_inc(&pv->busy_count);

	writel(EUR_CR_EVENT_KICK_NOW_MASK, pv->regs + EUR_CR_EVENT_KICK);
	return 0;
}

static int prismrv_wait_in_fences(u32 num_fds, const u32 __user *user_fds)
{
	u32 *fds;
	unsigned int i;
	int ret = 0;

	if (!num_fds)
		return 0;

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

int prismrv_submit_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_submit *args = data;
	struct drm_gem_object **objs = NULL;
	struct prismrv_fence *f = NULL;
	struct sync_file *sf = NULL;
	u32 __user *in_fds;
	__le32 cmd_data[6] = {};
	int ret = 0, fd = -1;
	unsigned int i;

	ret = pm_runtime_resume_and_get(pv->drm.dev);
	if (ret)
		return ret;

	/* ----------------------------------------------------------------
	 * Phase 1 — outside submit_rwsem: validate, GEM lookup, fence wait.
	 *
	 * All operations that can sleep (fence waits) MUST happen before
	 * taking the read-lock so that recovery_work() can take the
	 * write-lock without deadlocking against a sleeping submit.
	 * ---------------------------------------------------------------- */
	if (args->cmd_type >= PRISMRV_CMD_COUNT ||
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

	/* implicit sync: wait for existing GPU fences on every BO */
	for (i = 0; i <= args->num_bos; i++) {
		long r;

		if (!objs[i])
			continue;
		r = dma_resv_wait_timeout(objs[i]->resv,
					  DMA_RESV_USAGE_READ,
					  true, MAX_SCHEDULE_TIMEOUT);
		if (r < 0) {
			ret = r;
			goto err_objs;
		}
	}

	/* ----------------------------------------------------------------
	 * Allocate the fence and sync_file before taking the lock.
	 * fd_install() is deferred until CCB publish succeeds so that a
	 * failing ioctl never leaks a userspace-visible fd.
	 * ---------------------------------------------------------------- */
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		ret = -ENOMEM;
		goto err_objs;
	}
	spin_lock_init(&f->lock);
	INIT_LIST_HEAD(&f->node);
	f->ccb_slot = 0xFFFF;
	dma_fence_init(&f->base, &prismrv_fence_ops, &f->lock,
		       atomic_inc_return(&pv->fence_context),
		       atomic_inc_return(&pv->fence_seqno));
	/* ref #1: held by this function; transferred to sync_file below */

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_fence_put;
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

	/* ----------------------------------------------------------------
	 * Phase 2 — inside submit_rwsem (read): hw_ready check + enqueue.
	 * Nothing that blocks on a GPU fence may run here.
	 * ---------------------------------------------------------------- */
	down_read(&pv->submit_rwsem);

	if (!pv->hw_ready) {
		ret = -ENODEV;
		goto err_unlock;
	}

	ret = prismrv_gem_populate(pv, objs, args->num_bos + 1);
	if (ret)
		goto err_unlock;

	cmd_data[0] = cpu_to_le32(prismrv_bo_gpuva(objs[0]));
	cmd_data[1] = cpu_to_le32(args->cmd_size);
	if (args->num_bos >= 1 && objs[1])
		cmd_data[2] = cpu_to_le32(prismrv_bo_gpuva(objs[1]));

	/*
	 * Transfer BO refs to the fence (ref #3 path for BO lifetime).
	 * objs is set to NULL so err_unlock does not double-put them.
	 */
	f->bos     = objs;
	f->num_bos = args->num_bos + 1;
	objs       = NULL;

	/*
	 * Register the completion fence on every BO's dma_resv so that
	 * subsequent submits or CPU transfers to the same BO wait for us.
	 *
	 * API contract for dma_resv_add_fence():
	 *   1. dma_resv_lock()
	 *   2. dma_resv_reserve_fences(obj, 1)   ← allocates capacity
	 *   3. dma_resv_add_fence()
	 *   4. dma_resv_unlock()
	 *
	 * Skipping step 2 causes NULL dereference on first submit (fobj
	 * is NULL for a freshly-created BO) and BUG_ON on subsequent
	 * submits when the list is full.
	 */
	dma_fence_get(&f->base);	/* ref #2: for dma_resv */
	for (i = 0; i < f->num_bos; i++) {
		struct dma_resv *resv = f->bos[i]->resv;
		int rerr;

		dma_resv_lock(resv, NULL);
		rerr = dma_resv_reserve_fences(resv, 1);
		if (!rerr)
			dma_resv_add_fence(resv, &f->base,
					   DMA_RESV_USAGE_WRITE);
		dma_resv_unlock(resv);

		if (rerr) {
			/* reservation failed: drop the ref we just took */
			dma_fence_put(&f->base);
			ret = rerr;
			goto err_unlock_bos_set;
		}
	}
	/* resv objects now hold ref #2 collectively */

	/*
	 * Take ref #3 for pending_fences before ccb_schedule() publishes
	 * the command.  ccb_schedule() appends the fence to pending_fences
	 * only AFTER write_offset is bumped, so the IRQ always sees a
	 * fence with a valid ccb_slot.
	 */
	dma_fence_get(&f->base);	/* ref #3: for pending_fences */

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
		prismrv_fence_release_bos(f);
		/* PM ref released below at err_unlock */
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
	/* PM ref is kept; handle_completion() / hw_fini() will release it */
	return 0;

err_unlock_bos_set:
	/* f->bos was set but dma_resv registration failed mid-loop.
	 * Release the BO refs now; f itself will be freed when sf drops ref. */
	prismrv_fence_release_bos(f);
err_unlock:
	up_read(&pv->submit_rwsem);
	fput(sf->file);		/* drops sf + its fence ref */
	put_unused_fd(fd);
	pm_runtime_mark_last_busy(pv->drm.dev);
	pm_runtime_put_autosuspend(pv->drm.dev);
	/* objs may have been transferred to f->bos; only free if still set */
	if (objs) {
		for (i = 0; i <= args->num_bos; i++)
			if (objs[i])
				drm_gem_object_put(objs[i]);
		kvfree(objs);
	}
	return ret;

err_put_fd:
	put_unused_fd(fd);
err_fence_put:
	dma_fence_put(&f->base);	/* drop ref #1 */
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
