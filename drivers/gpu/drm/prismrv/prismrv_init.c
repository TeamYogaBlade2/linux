// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_init.c — hardware bring-up sequence.
 *
 * Mirrors the vendor SGXInitialise() order (services4/srvkm/devices/sgx/
 * sgxinit.c):
 *
 *   1. init script part 1        (pre-reset register writes)
 *   2. soft reset                (EUR_CR_SOFT_RESET pulse)
 *   3. pipe configuration        (EUR_CR_POWER)
 *   4. BIF context reset         (bank / dir-list registers)
 *   5. init script part 2        (post-reset register writes)
 *   6. uKernel upload + HostCtl  (clock stamp, InitStatus = 0)
 *   7. EVENT_KICK                (starts the uKernel main loop)
 *   8. poll ui32InitStatus       (PVRSRV_USSE_EDM_INIT_COMPLETE)
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>

#include "prismrv_device.h"

static void prismrv_soft_reset(struct prismrv_device *pv)
{
	u32 v;

	/* pause the BIF and clear any pending fault first */
	writel(EUR_CR_BIF_CTRL_PAUSE_MASK, pv->regs + EUR_CR_BIF_CTRL);
	udelay(10);
	v = readl(pv->regs + EUR_CR_BIF_INT_STAT);
	if (v & EUR_CR_BIF_INT_STAT_FAULT_REQ_MASK) {
		writel(EUR_CR_BIF_CTRL_PAUSE_MASK | EUR_CR_BIF_CTRL_CLEAR_FAULT_MASK,
		       pv->regs + EUR_CR_BIF_CTRL);
		udelay(10);
		writel(EUR_CR_BIF_CTRL_PAUSE_MASK, pv->regs + EUR_CR_BIF_CTRL);
	}

	v = EUR_CR_SOFT_RESET_DPM_RESET_MASK |
	    EUR_CR_SOFT_RESET_TA_RESET_MASK |
	    EUR_CR_SOFT_RESET_USE_RESET_MASK |
	    EUR_CR_SOFT_RESET_ISP_RESET_MASK |
	    EUR_CR_SOFT_RESET_ISP2_RESET_MASK |
	    EUR_CR_SOFT_RESET_TSP_RESET_MASK |
	    EUR_CR_SOFT_RESET_PDS_RESET_MASK |
	    EUR_CR_SOFT_RESET_PBE_RESET_MASK |
	    EUR_CR_SOFT_RESET_MTE_RESET_MASK |
	    EUR_CR_SOFT_RESET_TE_RESET_MASK |
	    EUR_CR_SOFT_RESET_TCU_L2_RESET_MASK |
	    EUR_CR_SOFT_RESET_UCACHEL2_RESET_MASK |
	    EUR_CR_SOFT_RESET_TEX_RESET_MASK |
	    EUR_CR_SOFT_RESET_IDXFIFO_RESET_MASK |
	    EUR_CR_SOFT_RESET_VDM_RESET_MASK |
	    EUR_CR_SOFT_RESET_DCU_L2_RESET_MASK |
	    EUR_CR_SOFT_RESET_DCU_L0L1_RESET_MASK |
	    EUR_CR_SOFT_RESET_ITR_RESET_MASK;
	writel(v, pv->regs + EUR_CR_SOFT_RESET);
	writel(0, pv->regs + EUR_CR_SOFT_RESET);
	udelay(100);
}

static void prismrv_bif_reset(struct prismrv_device *pv)
{
	writel(0, pv->regs + EUR_CR_BIF_CTRL);
	writel(0, pv->regs + EUR_CR_BIF_BANK_SET);
	writel(0, pv->regs + EUR_CR_BIF_BANK0);
	writel(0, pv->regs + EUR_CR_BIF_DIR_LIST_BASE0);
	writel(0, pv->regs + EUR_CR_BIF_DIR_LIST_BASE1);
}

/* part1 / part2 split of the firmware script is encoded as two HALT-
 * terminated sections; run_script stops at the first HALT, so we expose a
 * resume entry that continues after it. */
static int prismrv_run_script_range(struct prismrv_device *pv,
				    const struct firmware *fw,
				    size_t start_rec)
{
	const struct prismrv_init_rec *rec =
		(const void *)(fw->data) + start_rec * sizeof(*rec);
	size_t n = fw->size / sizeof(*rec);
	size_t i;

	for (i = start_rec; i < n; i++, rec++) {
		u32 op = le32_to_cpu(rec->op);

		switch (op) {
		case PRISMRV_INIT_OP_WRITE:
			writel(le32_to_cpu(rec->value),
			       pv->regs + le32_to_cpu(rec->offset));
			readl(pv->regs + le32_to_cpu(rec->offset));
			break;
		case PRISMRV_INIT_OP_READ:
			readl(pv->regs + le32_to_cpu(rec->offset));
			break;
		case PRISMRV_INIT_OP_HALT:
			return i + 1;
		}
	}
	return -EINVAL;
}

int prismrv_hw_init(struct prismrv_device *pv)
{
	const struct firmware *fw = NULL;
	unsigned int i;
	int ret, next;

	ret = request_firmware(&fw, "mediatek/mt6589-sgx544-init.bin",
			       pv->drm.dev);
	if (ret) {
		dev_err(pv->drm.dev, "init script load failed (%d)\n", ret);
		return ret;
	}

	/* part 1: before reset */
	next = prismrv_run_script_range(pv, fw, 0);
	if (next < 0) {
		release_firmware(fw);
		return next;
	}

	prismrv_read_revision(pv);	/* revision stable after clocks on */
	prismrv_errata_init(pv);
	ret = prismrv_errata_apply(pv);
	if (ret)
		goto out_fw;

	prismrv_soft_reset(pv);

	/* default pipe count: all pipes fully enabled */
	writel(0, pv->regs + EUR_CR_POWER);

	prismrv_bif_reset(pv);
	prismrv_mmu_init(pv);

	/* part 2: after reset */
	ret = prismrv_run_script_range(pv, fw, next);
	release_firmware(fw);
	fw = NULL;
	if (ret < 0)
		goto out_errata;

	/* upload the uKernel into GPU address space */
	ret = prismrv_mmu_map(pv, PRISMRV_UKERNEL_VADDR,
			      pv->ukernel_dma, pv->ukernel_size);
	if (ret)
		return ret;

	/* shared HostCtl block: allocate once, reuse across hw_init calls */
	if (!pv->hostctl) {
		pv->hostctl = dma_alloc_coherent(pv->drm.dev,
						 sizeof(*pv->hostctl),
						 &pv->hostctl_dma, GFP_KERNEL);
		if (!pv->hostctl)
			return -ENOMEM;
	}

	ret = prismrv_ccb_init(pv);
	if (ret)
		return ret;

	pv->hostctl->ui32HostClock = cpu_to_le32(jiffies_to_usecs(jiffies));
	pv->hostctl->ui32InitStatus = 0;
	wmb();

	/* start the uKernel */
	writel(EUR_CR_EVENT_KICK_NOW_MASK, pv->regs + EUR_CR_EVENT_KICK);

	/* wait for the uKernel to report initialisation complete */
	for (i = 0; i < 500; i++) {
		if (le32_to_cpu(READ_ONCE(pv->hostctl->ui32InitStatus)) &
		    PRISMRV_EDM_INIT_COMPLETE) {
			pv->hw_ready = true;
			dev_info(pv->drm.dev,
				 "uKernel initialised after %d polls\n", i);
			return 0;
		}
		usleep_range(900, 1100);
	}

	dev_err(pv->drm.dev, "uKernel init timeout\n");
	ret = -ETIMEDOUT;

out_errata:
	prismrv_errata_release(pv);
out_fw:
	return ret;
}

void prismrv_hw_fini(struct prismrv_device *pv)
{
	unsigned int i;

	pv->hw_ready = false;

	/* retire any fences that will never complete */
	spin_lock(&pv->event_lock);
	while (!list_empty(&pv->pending_fences)) {
		struct prismrv_fence *pf =
			list_first_entry(&pv->pending_fences,
					 struct prismrv_fence, node);
		list_del(&pf->node);

		dma_fence_signal(&pf->base);
		dma_fence_put(&pf->base);
	}
	spin_unlock(&pv->event_lock);

	prismrv_ccb_fini(pv);

	if (pv->hostctl) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->hostctl),
				  pv->hostctl, pv->hostctl_dma);
		pv->hostctl = NULL;
	}

	prismrv_errata_release(pv);
	for (i = 0; i < PRISMRV_ERRATA_BUF_COUNT; i++)
		pv->errata_buf[i].size = 0;

	if (pv->ukernel_cpu) {
		dma_free_coherent(pv->drm.dev, pv->ukernel_size,
				  pv->ukernel_cpu, pv->ukernel_dma);
		pv->ukernel_cpu = NULL;
	}

	prismrv_mmu_fini(pv);
}
