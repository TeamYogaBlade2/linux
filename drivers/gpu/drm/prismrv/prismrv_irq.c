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

#include "prismrv_device.h"

#define PRISMRV_IRQ_COMPLETION_EVENTS \
	(EUR_CR_EVENT_STATUS_TA_FINISHED_MASK | \
	 EUR_CR_EVENT_STATUS_PIXELBE_END_RENDER_MASK)

/* consecutive completions without a fence being signalled trigger reset */
#define PRISMRV_RECOVERY_THRESHOLD	3

static void prismrv_handle_completion(struct prismrv_device *pv)
{
	struct dma_fence *fence;

	spin_lock(&pv->event_lock);
	fence = pv->pending_fence;
	pv->pending_fence = NULL;
	spin_unlock(&pv->event_lock);

	if (fence) {
		dma_fence_signal(fence);
		dma_fence_put(fence);
		pv->missed_completions = 0;
	}
	atomic_dec(&pv->busy_count);
}

static void prismrv_check_recovery(struct prismrv_device *pv)
{
	if (++pv->missed_completions < PRISMRV_RECOVERY_THRESHOLD)
		return;

	pv->missed_completions = 0;
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

	mutex_lock(&pv->init_mutex);
	pv->hw_ready = false;
	prismrv_hw_init(pv);
	mutex_unlock(&pv->init_mutex);
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
