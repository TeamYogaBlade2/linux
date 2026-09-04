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

#include "prismrv_device.h"

#define PRISMRV_IRQ_COMPLETION_EVENTS \
	(EUR_CR_EVENT_STATUS_TA_FINISHED_MASK | \
	 EUR_CR_EVENT_STATUS_PIXELBE_END_RENDER_MASK)

/* consecutive completions without a fence being signalled trigger reset */
#define PRISMRV_RECOVERY_THRESHOLD	3

static void prismrv_handle_completion(struct prismrv_device *pv)
{
	LIST_HEAD(signalled);
	u32 read_off;

	/*
	 * Read the uKernel's CCB read_offset: every slot up to (but not
	 * including) this offset has been consumed.  We retire exactly those
	 * fences whose CCB slot lies before the current read_offset, in
	 * submission order.
	 *
	 * The CCB is a power-of-2 ring (256 entries) so we compare slot
	 * numbers modulo 256.  A slot is "done" when the distance from the
	 * fence's slot to read_offset (mod 256) is less than 128 — i.e. the
	 * read pointer has advanced past it without wrapping twice.
	 *
	 * If the driver has no CCB yet (very early IRQ before init) treat
	 * read_offset as 0 and retire nothing.
	 */
	if (pv->ccb)
		read_off = le32_to_cpu(READ_ONCE(pv->ccb->read_offset)) & 255;
	else
		read_off = 0;

	spin_lock(&pv->event_lock);
	while (!list_empty(&pv->pending_fences)) {
		struct prismrv_fence *pf =
			list_first_entry(&pv->pending_fences,
					 struct prismrv_fence, node);

		/*
		 * Check whether pf->ccb_slot has been consumed by the uKernel.
		 * Slot s is done when ((read_off - s - 1) & 255) < 128, which
		 * is equivalent to "read_off is strictly ahead of s (mod 256)".
		 * If no slot was recorded (ccb_slot == 0xFFFF) retire it
		 * unconditionally — this handles the pre-CCB-slot era fences
		 * and hw_fini retirement.
		 */
		if (pf->ccb_slot != 0xFFFF &&
		    ((read_off - pf->ccb_slot - 1) & 255) >= 128)
			break;	/* this and all later slots are still pending */

		list_del(&pf->node);
		list_add_tail(&pf->node, &signalled);
	}
	spin_unlock(&pv->event_lock);

	/* signal completed fences outside the spinlock */
	while (!list_empty(&signalled)) {
		struct prismrv_fence *pf =
			list_first_entry(&signalled, struct prismrv_fence, node);
		list_del_init(&pf->node);

		dma_fence_signal(&pf->base);
		dma_fence_put(&pf->base);
		atomic_dec(&pv->busy_count);

		/*
		 * Release the runtime PM reference that was taken (and
		 * intentionally held) by prismrv_submit_ioctl() for the
		 * lifetime of this command.  One get per submit, one put
		 * per completion.
		 */
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

	/*
	 * Hold a runtime PM reference for the whole re-init: hw_init
	 * touches registers and must not race a suspend halfway through.
	 */
	ret = pm_runtime_resume_and_get(pv->drm.dev);
	if (ret) {
		dev_err(pv->drm.dev, "recovery: resume failed (%d)\n", ret);
		return;
	}

	mutex_lock(&pv->init_mutex);
	/*
	 * Fully tear down the old hardware state before re-initialising.
	 * hw_fini() retires any pending fences (signalling them with an
	 * error so waiters unblock), frees the CCB, HostCtl and errata
	 * DMA buffers, and tears down the MMU page tables — leaving a
	 * completely clean slate for hw_init().  Skipping this step would
	 * cause double-allocation of DMA buffers and stale MMU mappings.
	 */
	prismrv_hw_fini(pv);
	prismrv_hw_init(pv);
	mutex_unlock(&pv->init_mutex);

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
