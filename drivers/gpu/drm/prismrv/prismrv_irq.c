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

/* consecutive completion events that retire no fence trigger a reset */
#define PRISMRV_RECOVERY_THRESHOLD	3
#define PRISMRV_HANG_TIMEOUT_MS		4000

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

/*
 * CCB retirement uses a 32-bit monotonic command counter instead of the
 * raw 8-bit ring offsets.
 *
 * The hardware only exposes read_offset modulo 256.  Comparing ring
 * distances with a half-range test (>= 128) breaks as soon as more than
 * 128 commands are outstanding, although prismrv_ccb_full() allows up
 * to 255.  Instead the driver extends read_offset to a 32-bit
 * "commands consumed" counter: because at most 255 commands can be
 * outstanding, the forward distance between the previously observed and
 * the current read_offset is unambiguous.  Every fence remembers the
 * counter value of its command (ccb_seq) and is retired once that value
 * is below the consumed counter.
 *
 * Returns the number of fences retired.
 */
/*
 * Extend the 8-bit hardware read_offset into the 32-bit consumed counter.
 * Must be called with event_lock held.
 *
 * The extension is unambiguous only if the hardware cannot have consumed
 * 256 or more commands since the previous sync.  It cannot consume more
 * than are outstanding (<= 255), and every submit calls this before
 * publishing a new command (prismrv_ccb_schedule()), so the number of
 * unobserved consumptions never exceeds the outstanding count even when
 * interrupt handling is delayed.
 *
 * Returns true if the counter advanced.
 */
/*
 * Ask for a GPU reset because the uKernel has stopped consuming commands.
 * The request records how many commands had been consumed; recovery_work
 * only resets if that number is still current, so a request made for a
 * job that has meanwhile finished cannot reset an unrelated job that was
 * submitted before the worker ran.
 */
void prismrv_request_recovery(struct prismrv_device *pv)
{
	unsigned long flags;

	spin_lock_irqsave(&pv->event_lock, flags);
	prismrv_ccb_sync_locked(pv);
	WRITE_ONCE(pv->recovery_req_completed, pv->ccb_completed);
	spin_unlock_irqrestore(&pv->event_lock, flags);
	schedule_work(&pv->recovery_work);
}

bool prismrv_ccb_sync_locked(struct prismrv_device *pv)
{
	u8 read_off = le32_to_cpu(READ_ONCE(pv->ccb->read_offset)) & 255;
	u8 delta = read_off - pv->ccb_last_read;

	pv->ccb_last_read = read_off;
	pv->ccb_completed += delta;
	return delta != 0;
}

static unsigned int prismrv_handle_completion(struct prismrv_device *pv)
{
	LIST_HEAD(signalled);
	unsigned int retired = 0;
	unsigned long flags;
	bool advanced;

	if (!pv->ccb)
		return 0;

	spin_lock_irqsave(&pv->event_lock, flags);
	advanced = prismrv_ccb_sync_locked(pv);
	while (!list_empty(&pv->pending_fences)) {
		struct prismrv_fence *pf =
			list_first_entry(&pv->pending_fences,
					 struct prismrv_fence, node);

		if ((s32)(pf->ccb_seq - pv->ccb_completed) >= 0)
			break;

		list_move_tail(&pf->node, &signalled);
	}
	spin_unlock_irqrestore(&pv->event_lock, flags);

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
		retired++;

		pm_runtime_mark_last_busy(pv->drm.dev);
		pm_runtime_put_autosuspend(pv->drm.dev);
	}

	if (retired || advanced) {
		WRITE_ONCE(pv->last_progress, jiffies);
		atomic_set(&pv->missed_completions, 0);
	}
	return retired;
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
	prismrv_request_recovery(pv);
}

void prismrv_recovery_work(struct work_struct *work)
{
	struct prismrv_device *pv =
		container_of(work, struct prismrv_device, recovery_work);
	int ret;

	/*
	 * Recovery only matters while work is outstanding, and outstanding
	 * work holds a runtime-PM reference, so the device cannot be
	 * suspended then.  A stale request that runs after the last fence
	 * retired (or after suspend) has nothing to recover and must not
	 * wake the GPU up just to reset it.
	 */
	if (!atomic_read(&pv->busy_count))
		return;

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

	{
		unsigned long flags;
		bool progressed;

		spin_lock_irqsave(&pv->event_lock, flags);
		prismrv_ccb_sync_locked(pv);
		progressed = pv->ccb_completed !=
			     READ_ONCE(pv->recovery_req_completed);
		spin_unlock_irqrestore(&pv->event_lock, flags);

		if (progressed) {
			dev_dbg(pv->drm.dev,
				"stale recovery request (GPU made progress)\n");
			mutex_unlock(&pv->init_mutex);
			up_write(&pv->submit_rwsem);
			pm_runtime_put_autosuspend(pv->drm.dev);
			return;
		}
	}

	if (!READ_ONCE(pv->hw_ready) || !atomic_read(&pv->busy_count)) {
		/* retired or torn down while we waited for the locks */
		mutex_unlock(&pv->init_mutex);
		up_write(&pv->submit_rwsem);
		pm_runtime_put_autosuspend(pv->drm.dev);
		return;
	}

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

/*
 * Hang watchdog: armed by every submit.  If work is outstanding and no
 * fence has retired for PRISMRV_HANG_TIMEOUT_MS, the uKernel is wedged
 * without raising any interrupt; schedule a reset.
 */
void prismrv_hang_work(struct work_struct *work)
{
	struct prismrv_device *pv =
		container_of(work, struct prismrv_device, hang_work.work);
	unsigned long last = READ_ONCE(pv->last_progress);

	if (!atomic_read(&pv->busy_count) || !READ_ONCE(pv->hw_ready))
		return;

	if (time_after(jiffies, last + msecs_to_jiffies(PRISMRV_HANG_TIMEOUT_MS))) {
		dev_err(pv->drm.dev, "no progress for %dms, resetting GPU\n",
			PRISMRV_HANG_TIMEOUT_MS);
		WRITE_ONCE(pv->last_progress, jiffies);
		prismrv_request_recovery(pv);
	}
	schedule_delayed_work(&pv->hang_work,
			      msecs_to_jiffies(PRISMRV_HANG_TIMEOUT_MS / 2));
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

	/*
	 * A completion event that does not advance read_offset (no fence
	 * retired) while work is outstanding counts towards recovery.
	 * Events that are not completion events (SW housekeeping raised
	 * by the uKernel) say nothing about progress and are ignored;
	 * a silent hang is caught by the hang watchdog instead.
	 */
	if (status & PRISMRV_IRQ_COMPLETION_EVENTS) {
		if (!prismrv_handle_completion(pv) &&
		    atomic_read(&pv->busy_count) > 0)
			prismrv_check_recovery(pv);
	}

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
