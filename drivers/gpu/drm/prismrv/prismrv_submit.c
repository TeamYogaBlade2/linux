// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_submit.c — command submission: kernel CCB + TA/3D kicks.
 *
 * Mirrors the vendor flow (sgxutils.c SGXScheduleCCBCommand):
 *
 *   1. acquire a CCB slot (256 entries, 32 bytes each)
 *   2. fill the command; ui32ServiceAddress = uKernel base +
 *      hostkick_instr[type] * 8 (the USE service handler entry)
 *   3. advance ccb->write_offset (mod 256)
 *   4. kick EUR_CR_EVENT_KICK
 *
 * The command payload (ui32Data[6]) is passed through from userspace:
 * the vendor model has userspace (libsrv_um) define the per-command-type
 * data layout, and the kernel only sequences execution.
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

#define HWRTDATA_SIZE		496
#define PRISMRV_CCB_DRAIN_TIMEOUT_MS	2000
#define PRISMRV_MAX_SUBMIT_BOS	256
#define PRISMRV_MAX_IN_FENCES	64

static const char *prismrv_fence_name(struct dma_fence *f)
{
	return "prismrv";
}

const struct dma_fence_ops prismrv_fence_ops = {
	.get_driver_name = prismrv_fence_name,
	.get_timeline_name = prismrv_fence_name,
};

int prismrv_ccb_init(struct prismrv_device *pv)
{
	int ret;

	spin_lock_init(&pv->ccb_lock);

	/* re-entry (runtime resume after recovery): the shared structures
	 * were allocated on the first init and are still MMU-mapped */
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

	/* TA + 3D render contexts (HWRTData), zero-initialised */
	pv->hwrt = dma_alloc_coherent(pv->drm.dev, 2 * HWRTDATA_SIZE,
				      &pv->hwrt_dma, GFP_KERNEL);
	if (!pv->hwrt) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->ccb), pv->ccb,
				  pv->ccb_dma);
		pv->ccb = NULL;
		return -ENOMEM;
	}

	/* expose the shared structures to the uKernel through the MMU */
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
 * Vendor SGXAcquireKernelCCBSlot: the CCB is full when advancing the
 * write offset would swallow an unconsumed command.  Wait for the
 * uKernel to drain it instead of corrupting the ring.
 */
/*
 * Ring-full test: caller holds ccb_lock or otherwise serialises.
 *
 * Both offsets are always in [0,255] (they are stored masked), so the
 * u32 subtraction can never wrap through 32 bits — the expression is
 * equivalent to ((write + 1 - read) mod 256 == 0), i.e. 255 slots in
 * use.  Keep the operands masked if this ever changes.
 */
static bool prismrv_ccb_full(struct prismrv_device *pv)
{
	u32 w = le32_to_cpu(READ_ONCE(pv->ccb->write_offset)) & 255;
	u32 r = le32_to_cpu(READ_ONCE(pv->ccb->read_offset)) & 255;

	return ((w + 1 - r) & 255) == 0;
}

/*
 * returns 0 on success (pf->ccb_slot is set to the written slot),
 * or -ETIMEDOUT when the uKernel stopped draining.
 * The caller must undo the fence/busy_count bookkeeping on error.
 */
static int prismrv_ccb_schedule(struct prismrv_device *pv,
				enum prismrv_cmd_type type,
				const __le32 data[6],
				struct prismrv_fence *pf)
{
	struct prismrv_ccb_cmd *cmd;
	u32 slot;

	spin_lock(&pv->ccb_lock);

	/*
	 * Wait for a free slot with a hard timeout.  If the uKernel is
	 * wedged (never drains), spinning forever here would hang the
	 * submitting task with runtime PM held.  After the timeout we
	 * schedule recovery and return -ETIMEDOUT; userspace resubmits.
	 */
	if (prismrv_ccb_full(pv)) {
		unsigned long deadline = jiffies +
			msecs_to_jiffies(PRISMRV_CCB_DRAIN_TIMEOUT_MS);

		spin_unlock(&pv->ccb_lock);
		do {
			/* nudge the uKernel main loop so it drains */
			writel(EUR_CR_EVENT_KICK_NOW_MASK,
			       pv->regs + EUR_CR_EVENT_KICK);
			usleep_range(500, 1000);
			if (time_after(jiffies, deadline)) {
				dev_err(pv->drm.dev,
					"CCB full for %dms — scheduling recovery\n",
					PRISMRV_CCB_DRAIN_TIMEOUT_MS);
				/*
				 * Only schedule recovery if the hardware was
				 * still marked ready: if hw_ready is already
				 * false a previous recovery is already in
				 * progress (protected by init_mutex) and
				 * scheduling another would race with it.
				 */
				if (READ_ONCE(pv->hw_ready))
					schedule_work(&pv->recovery_work);

				/*
				 * Retire the fence under event_lock: the
				 * IRQ completion handler splices this same
				 * list and may already have signalled+put
				 * it (recovery can complete during the
				 * spin).  The empty check makes the retire
				 * idempotent against that; the refcount
				 * taken at enqueue keeps the put safe.
				 */
				spin_lock(&pv->event_lock);
				if (!list_empty_careful(&pf->node)) {
					list_del_init(&pf->node);
					dma_fence_set_error(&pf->base,
							    -ETIMEDOUT);
					dma_fence_signal_locked(&pf->base);
					dma_fence_put(&pf->base);
				}
				spin_unlock(&pv->event_lock);
				return -ETIMEDOUT;
			}
			spin_lock(&pv->ccb_lock);
		} while (prismrv_ccb_full(pv));
	}

	slot = le32_to_cpu(READ_ONCE(pv->ccb->write_offset)) & 255;
	cmd = &pv->ccb->commands[slot];

	cmd->service_address =
		cpu_to_le32(PRISMRV_UKERNEL_VADDR +
			    prismrv_hostkick_instr[type] * 8);
	cmd->cache_control = 0;
	memcpy(cmd->data, data, sizeof(cmd->data));

	/*
	 * Record the slot before publishing: prismrv_handle_completion()
	 * reads pf->ccb_slot under event_lock to decide whether this fence
	 * can be signalled.  The assignment must happen before write_offset
	 * is bumped so the IRQ path (which fires after the kick) always
	 * sees a valid slot.
	 */
	pf->ccb_slot = (u16)slot;

	/*
	 * publish the command before bumping write_offset; the uKernel
	 * polls write_offset on the other side of a coherent mapping
	 */
	wmb();
	WRITE_ONCE(pv->ccb->write_offset, cpu_to_le32((slot + 1) & 255));

	spin_unlock(&pv->ccb_lock);

	writel(EUR_CR_EVENT_KICK_NOW_MASK, pv->regs + EUR_CR_EVENT_KICK);
	return 0;
}

/*
 * Import the user-passed sync_file fds and wait for every dependency
 * before the CCB command is published (implicit-sync semantics).
 */
static int prismrv_wait_in_fences(u32 num_fds, const u32 __user *user_fds)
{
	u32 *fds;
	unsigned int i;
	int ret = 0;

	if (!num_fds)
		return 0;

	/* user_fds is a __user pointer: copy it in with the proper
	 * accessor instead of handing it to kmemdup_array() */
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
	struct prismrv_fence *f;
	struct sync_file *sf;
	u32 __user *in_fds;
	__le32 cmd_data[6] = {};
	int ret = 0, fd;
	unsigned int i;

	ret = pm_runtime_resume_and_get(pv->drm.dev);
	if (ret)
		return ret;

	if (!pv->hw_ready) {
		pm_runtime_put_sync(pv->drm.dev);
		return -ENODEV;
	}
	if (args->cmd_type >= PRISMRV_CMD_COUNT ||
	    args->num_bos > PRISMRV_MAX_SUBMIT_BOS ||
	    args->num_in_fences > PRISMRV_MAX_IN_FENCES) {
		pm_runtime_put_sync(pv->drm.dev);
		return -EINVAL;
	}

	/* slot 0 = command BO, 1..num_bos = user bos */
	objs = kvcalloc(args->num_bos + 1, sizeof(*objs), GFP_KERNEL);
	if (!objs) {
		pm_runtime_put_sync(pv->drm.dev);
		return -ENOMEM;
	}

	/* block on explicit dependencies first */
	in_fds = u64_to_user_ptr(args->in_fences);
	ret = prismrv_wait_in_fences(args->num_in_fences, in_fds);
	if (ret)
		goto out_put;

	/* the command BO is always referenced object 0 so it is pinned,
	 * DMA-mapped and MMU-mapped along with the rest */
	objs[0] = drm_gem_object_lookup(file, args->cmd_handle);
	if (!objs[0]) {
		ret = -ENOENT;
		goto out_put;
	}
	if (args->cmd_size > objs[0]->size) {
		ret = -EINVAL;
		drm_gem_object_put(objs[0]);
		goto out_put;
	}

	/* pin + DMA-map + MMU-map every referenced BO */
	{
		struct drm_gem_object **lut = objs + 1;

		ret = drm_gem_objects_lookup(file,
			u64_to_user_ptr(args->bos), args->num_bos, &lut);
	}
	if (ret)
		goto out_put;

	/*
	 * Implicit sync: wait for any exclusive fence other drivers
	 * left on the buffers we are about to read/write (PRIME-shared
	 * camera/display/v4l2 buffers).  Without this the GPU can read
	 * a scanout buffer mid-write.
	 */
	for (i = 0; i <= args->num_bos; i++) {
		struct dma_resv *resv = objs[i]->resv;
		long ret2 = dma_resv_wait_timeout(resv, DMA_RESV_USAGE_READ,
						  true, MAX_SCHEDULE_TIMEOUT);
		if (ret2 < 0) {
			ret = ret2;
			goto out_put;
		}
	}
	ret = prismrv_gem_populate(pv, objs, args->num_bos + 1);
	if (ret)
		goto out_put;

	/* pass the command stream by reference: data[0] = GPU VA of the
	 * command BO, data[1] = valid byte count.  The uKernel-side client
	 * CCB handler (and the emulator's HostCtl shim) reads the packet
	 * stream from there; the vendor model also keeps payload bodies
	 * out of the 24-byte kernel CCB slot. */
	cmd_data[0] = cpu_to_le32(prismrv_bo_gpuva(objs[0]));
	cmd_data[1] = cpu_to_le32(args->cmd_size);
	if (args->num_bos >= 1 && objs[1])
		/* bos array convention: user bos[0] (objs[1]) is the TA
		 * packet-stream BO for draw submissions */
		cmd_data[2] = cpu_to_le32(prismrv_bo_gpuva(objs[1]));

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		ret = -ENOMEM;
		goto out_put;
	}
	spin_lock_init(&f->lock);
	INIT_LIST_HEAD(&f->node);
	f->ccb_slot = 0xFFFF;	/* not yet assigned; set by ccb_schedule() */
	dma_fence_init(&f->base, &prismrv_fence_ops, &f->lock,
		       atomic_inc_return(&pv->fence_context),
		       atomic_inc_return(&pv->fence_seqno));

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		kfree(f);
		ret = fd;
		goto out_put;
	}

	sf = sync_file_create(&f->base);
	dma_fence_put(&f->base);
	if (!sf) {
		put_unused_fd(fd);
		kfree(f);
		ret = -ENOMEM;
		goto out_put;
	}

	args->out_fence_fd = fd;
	fd_install(fd, sf->file);

	/* record the fence so the IRQ handler can signal it on completion */
	dma_fence_get(&f->base);
	spin_lock(&pv->event_lock);
	list_add_tail(&f->node, &pv->pending_fences);
	spin_unlock(&pv->event_lock);

	/*
	 * Increment BEFORE scheduling: if the CCB times out,
	 * ccb_schedule() has already retired the fence (removed from the
	 * pending list, signalled with -ETIMEDOUT) and the IRQ path can
	 * no longer touch it — so busy_count must come back down here.
	 *
	 * The runtime PM reference taken at the top of this function is
	 * intentionally NOT released here on the success path.  Instead it
	 * is transferred to the fence and released by prismrv_handle_completion()
	 * (or prismrv_hw_fini() on teardown) once the GPU actually finishes
	 * the submitted command.  This keeps the device awake until the work
	 * is done, satisfying the autosuspend contract.
	 */
	atomic_inc(&pv->busy_count);
	ret = prismrv_ccb_schedule(pv, args->cmd_type, cmd_data, f);
	if (ret) {
		/* CCB schedule failed (timeout): fence already retired by
		 * ccb_schedule(), busy_count was pre-incremented so undo it,
		 * and release the PM reference we held. */
		atomic_dec(&pv->busy_count);
		pm_runtime_mark_last_busy(pv->drm.dev);
		pm_runtime_put_autosuspend(pv->drm.dev);
	}
	/* Both success and CCB-timeout fall through to out_objs to clean up
	 * the object references.  The PM put on the success path happens later
	 * in prismrv_handle_completion(). */
	goto out_objs;
out_put:
	pm_runtime_mark_last_busy(pv->drm.dev);
	pm_runtime_put_autosuspend(pv->drm.dev);
out_objs:
	/* objects 0..num_bos are referenced (0 = cmd BO) */
	for (i = 0; i <= args->num_bos && objs; i++)
		if (objs[i])
			drm_gem_object_put(objs[i]);
	kvfree(objs);
	return ret;
}
