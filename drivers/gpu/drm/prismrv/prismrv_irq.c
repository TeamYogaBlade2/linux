// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_irq.c — interrupt handling.
 *
 * Mirrors the vendor SGX_ISRHandler flow:
 *   1. read EUR_CR_EVENT_STATUS and mask with EUR_CR_EVENT_HOST_ENABLE
 *   2. write the matching bits plus MASTER_INTERRUPT to
 *      EUR_CR_EVENT_HOST_CLEAR
 *   3. on render-completion events (TA_FINISHED, PIXELBE_END_RENDER),
 *     signal the pending submission fence and schedule recovery if the
 *     uKernel stopped making progress
 */
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/rwsem.h>
#include <drm/drm_gem.h>

#include "prismrv_device.h"

#define PRISMRV_IRQ_COMPLETION_EVENTS \
	(EUR_CR_EVENT_STATUS_TA_FINISHED_MASK | \
	 EUR_CR_EVENT_STATUS_PIXELBE_END_RENDER_MASK)

/* consecutive completions without a fence being signalled trigger reset */
#define PRISMRV_RECOVERY_THRESHOLD	3

/*
 * Release the GEM object references that submit_ioctl() transferred
 * into the fence.  Called from handle_completion() (normal path) and
 * from hw_fini() forced-retirement path.
 *
 * Must be called AFTER dma_fence_signal() so that any waiter waking
 * up cannot observe a fence that is signalled but whose BOs are still
 * being referenced.
 */
void prismrv_fence_release_bos(struct prismrv_fence *pf)
{
	u32 i;

	if (!pf->bos)
		return;
	for (i = 0; i < pf->num_bos; i++)
		if (pf->bos[i])
			drm_gem_object_put(pf->bos[i]);
	kvfree(pf->bos);
	pf->bos = NULL;
	pf->num_bos = 0;
}

static void prismrv_handle_completion(struct prismrv_device *pv)
{
	LIST_HEAD(signalled);
	u32 read_off;

	if (pv->ccb)
		read_off = le32_to_cpu(READ_ONCE(pv->ccb->read_offset)) & 255;
	else
		read_off = 0;

	spin_lock(&pv->event_lock);
	while (!list_empty(&pv->pending_fences)) {
		struct prismrv_fence *pf =
			list_first_entry(&pv->pending_fences,
					 struct prismrv_fence, node);

		if (pf->ccb_slot != 0xFFFF &&
		    ((read_off - pf->ccb_slot - 1) & 255) >= 128)
			break;

		list_del(&pf->node);
		list_add_tail(&pf->node, &signalled);
	}
	spin_unlock(&pv->event_lock);

	while (!list_empty(&signalled)) {
		struct prismrv_fence *pf =
			list_first_entry(&signalled, struct prismrv_fence, node);
		list_del_init(&pf->node);

		dma_fence_signal(&pf->base);
		/*
		 * Release the BO references AFTER signalling: waiters that
		 * wake up on the fence will see all BOs still referenced
		 * until we are done here.
		 */
		prismrv_fence_release_bos(pf);
		dma_fence_put(&pf->base);
		atomic_dec(&pv->busy_count);

		pm_runtime_mark_last_busy(pv->drm.dev);
		pm_runtime_put_autosuspend(pv->drm.dev);
	}

	atomic_set(&pv->missed_completions, 0);
}

static void prismrv_check_recovery(struct prismrv_device *pv)
{
	if (atomic_inc_return(&pv->missed_completions) <
	    PRISMRV_RECOVERY_THRESHOLD)
		return;

	atomic_set(&pv->missed_completions, 0);
	dev_err(pv->drm.dev,
		"%d completions without progress, resetting GPU\n",
		PRISMRV_RECOVERY_THRESHOLD);

	/* HWRecoveryResetSGX equivalent: soft reset + re-run the init
	 * sequence from a work item (sleeping allocations are not legal
	 * in IRQ context). */
	schedule_work(&pv->recovery_work);
}

void prismrv_recovery_work(struct work_struct *work)
{
	struct prismrv_device *pv =
		container_of(work, struct prismrv_device, recovery_work);
	int ret;

	ret = pm_runtime_resume_and_get(pv->drm.dev);
	if (ret) {
		dev_err(pv->drm.dev, "recovery: resume failed (%d)\n", ret);
		return;
	}

	/*
	 * Step 1: take the submit write-lock FIRST.
	 *
	 * down_write() waits until every concurrent submit_ioctl() has
	 * released its read-lock.  Only after all in-flight submits have
	 * returned from CCB/MMU operations do we reset the GPU.
	 *
	 * The previous order (soft_reset then down_write) allowed a
	 * submit that already held the read-lock to touch CCB/MMU after
	 * the GPU had been reset, corrupting whatever re-init followed.
	 */
	down_write(&pv->submit_rwsem);
	mutex_lock(&pv->init_mutex);

	WRITE_ONCE(pv->hw_ready, false);

	/*
	 * Step 2: GPU soft-reset.
	 *
	 * Now that no submit can be in-flight (write-lock is held), it
	 * is safe to stop the GPU.  This halts all DMA so subsequent
	 * teardown of CCB/MMU memory cannot race live GPU accesses.
	 */
	prismrv_soft_reset(pv);

	/*
	 * Step 3: invalidate all BO GPU VAs under mmu_lock.
	 */
	mutex_lock(&pv->mmu_lock);
	prismrv_mmu_invalidate_all_bos(pv);
	mutex_unlock(&pv->mmu_lock);

	/*
	 * Step 4: tear down old HW state and reinitialise.
	 */
	prismrv_hw_fini(pv);
	prismrv_hw_init(pv);

	mutex_unlock(&pv->init_mutex);
	up_write(&pv->submit_rwsem);

	pm_runtime_mark_last_busy(pv->drm.dev);
	pm_runtime_put_autosuspend(pv->drm.dev);
}

irqreturn_t prismrv_irq_handler(int irq, void *data)
{
	struct prismrv_device *pv = data;
	u32 status, enable, clear;

	status = readl(pv->regs + EUR_CR_EVENT_STATUS);
	enable = readl(pv->regs + EUR_CR_EVENT_HOST_ENABLE);
	status &= enable;

	clear = status & (EUR_CR_EVENT_HOST_CLEAR_SW_EVENT_MASK |
			  PRISMRV_IRQ_COMPLETION_EVENTS);
	if (!clear)
		return IRQ_NONE;

	if (status & PRISMRV_IRQ_COMPLETION_EVENTS)
		prismrv_handle_completion(pv);
	else if (atomic_read(&pv->busy_count) > 0)
		/*
		 * An unrelated event arrived while a submission is in
		 * flight.  Only then does it hint at a wedged uKernel;
		 * counting SW events while idle used to schedule a
		 * bogus GPU reset.
		 */
		prismrv_check_recovery(pv);

	/*
	 * HostCtl flag path (REVIEW R6): the uKernel raises bits in
	 * ui32InterruptFlags to request host-side services; the host
	 * acknowledges by writing the same mask into ui32ClearFlags
	 * (vendor SGXMKIF_HOST_CTL convention).  Actual service dispatch
	 * (e.g. PROCESS_QUEUES follow-up) happens through the CCB.
	 */
	if (pv->hostctl) {
		u32 flags = le32_to_cpu(READ_ONCE(pv->hostctl->ui32InterruptFlags));

		if (flags) {
			WRITE_ONCE(pv->hostctl->ui32ClearFlags,
				   cpu_to_le32(flags));
			wmb();
			writel(EUR_CR_EVENT_KICK_NOW_MASK,
			       pv->regs + EUR_CR_EVENT_KICK);
		}
	}

	clear |= EUR_CR_EVENT_HOST_CLEAR_MASTER_INTERRUPT_MASK;
	writel(clear, pv->regs + EUR_CR_EVENT_HOST_CLEAR);

	return IRQ_HANDLED;
}
