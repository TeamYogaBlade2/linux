// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_irq.c — interrupt handling.
 */
#include <linux/interrupt.h>

#include "prismrv_device.h"

irqreturn_t prismrv_irq_handler(int irq, void *data)
{
	struct prismrv_device *pv = data;
	u32 status;

	status = readl(pv->regs + EUR_CR_EVENT_STATUS);
	if (!(status & EUR_CR_EVENT_STATUS_MASTER_INTERRUPT))
		return IRQ_NONE;

	/* clear the master event and wake waiters */
	writel(EUR_CR_EVENT_KICK_NOW, pv->regs + EUR_CR_EVENT_KICK);

	return IRQ_HANDLED;
}
