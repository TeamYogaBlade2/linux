// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_irq.c — interrupt handling.
 *
 * Mirrors the vendor SGX_ISRHandler flow:
 *   1. read EUR_CR_EVENT_STATUS and mask with EUR_CR_EVENT_HOST_ENABLE
 *   2. on SW_EVENT: write the matching bit plus MASTER_INTERRUPT to
 *      EUR_CR_EVENT_HOST_CLEAR
 *   3. wake completion waiters
 */
#include <linux/interrupt.h>

#include "prismrv_device.h"

irqreturn_t prismrv_irq_handler(int irq, void *data)
{
	struct prismrv_device *pv = data;
	u32 status, enable, clear;

	status = readl(pv->regs + EUR_CR_EVENT_STATUS);
	enable = readl(pv->regs + EUR_CR_EVENT_HOST_ENABLE);
	status &= enable;

	clear = status & EUR_CR_EVENT_STATUS_SW_EVENT_MASK;
	if (!clear)
		return IRQ_NONE;

	clear |= EUR_CR_EVENT_HOST_CLEAR_MASTER_INTERRUPT_MASK;
	writel(clear, pv->regs + EUR_CR_EVENT_HOST_CLEAR);

	return IRQ_HANDLED;
}
