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
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>

#include "prismrv_device.h"

void prismrv_soft_reset(struct prismrv_device *pv)
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
	    EUR_CR_SOFT_RESET_ITR_RESET_MASK |
	    /* BIF reset (hwdoc SGX544 register list bit0): flush the MMU
	     * state along with the rest; the dir-list base is reprogrammed
	     * right after in prismrv_mmu_init() */
	    EUR_CR_SOFT_RESET_BIF_RESET_MASK;
	writel(v, pv->regs + EUR_CR_SOFT_RESET);
	writel(0, pv->regs + EUR_CR_SOFT_RESET);
	udelay(100);
}

void prismrv_bif_reset(struct prismrv_device *pv)
{
	writel(0, pv->regs + EUR_CR_BIF_CTRL);
	writel(0, pv->regs + EUR_CR_BIF_BANK_SET);
	writel(0, pv->regs + EUR_CR_BIF_BANK0);
	writel(0, pv->regs + EUR_CR_BIF_DIR_LIST_BASE0);
	writel(0, pv->regs + EUR_CR_BIF_DIR_LIST_BASE1);
}

/*
 * Run the init script from start_rec to the end (or a HALT record).
 *
 * The MT6589 script is a single-shot sequence with no HALT records:
 * it programs the BIF bank window, event enables, USE_CODE_BASE_0..15
 * and the bank-switched USE private registers in one pass.  Offsets
 * are BYTE offsets into the register page, including the switched
 * banks (0x4000+, 0x8000+, 0x22000+).
 *
 * Returns the index of the next unexecuted record (== n when the whole
 * script ran), or a negative errno.
 */
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
	return n;
}

int prismrv_hw_init(struct prismrv_device *pv)
{
	const struct firmware *fw = NULL;
	unsigned int i;
	int ret;

	/*
	 * Use the init-script name derived by prismrv_fw_load() from the DT
	 * "firmware-name" property (or the compile-time default if the
	 * property is absent).  If fw_init_name is empty (first boot before
	 * fw_load ran) fall back to the MT6589 default so that the probe
	 * path from prismrv_runtime_resume() still works.
	 */
	ret = request_firmware(&fw,
			       pv->fw_init_name[0] ? pv->fw_init_name
						   : "mediatek/mt6589-sgx544-init.bin",
			       pv->drm.dev);
	if (ret) {
		dev_err(pv->drm.dev, "init script load failed (%d)\n", ret);
		return ret;
	}

	/*
	 * Run the whole init script once, before the soft reset.  The
	 * MT6589 capture has no part1/part2 HALT split — the vendor
	 * sequence programs BIF windows, event enables and
	 * USE_CODE_BASE_0..15 in a single pass, and every register it
	 * touches survives the soft reset (only the USE pipeline is
	 * reset; BIF keeps its dir-list programming).
	 */
	ret = prismrv_run_script_range(pv, fw, 0);
	release_firmware(fw);
	fw = NULL;
	if (ret < 0)
		goto out_fw;

	prismrv_read_revision(pv);	/* revision stable after clocks on */
	prismrv_errata_init(pv);	/* sets pv->errata bitmask only */

	prismrv_soft_reset(pv);

	/* default pipe count: all pipes fully enabled */
	writel(0, pv->regs + EUR_CR_POWER);

	prismrv_bif_reset(pv);

	/*
	 * mmu_init() MUST come before errata_apply().
	 *
	 * errata_apply() calls prismrv_mmu_map() for each workaround
	 * buffer, which immediately dereferences pv->pd_cpu to walk the
	 * page directory.  pv->pd_cpu is allocated and zeroed by
	 * mmu_init() — calling errata_apply() first (as the code
	 * previously did) caused a NULL dereference on every cold boot.
	 */
	ret = prismrv_mmu_init(pv);
	if (ret)
		goto out_fw;

	ret = prismrv_errata_apply(pv);
	if (ret)
		goto out_mmu;

	/* upload the uKernel into GPU address space */
	ret = prismrv_mmu_map(pv, PRISMRV_UKERNEL_VADDR,
			      pv->ukernel_dma, pv->ukernel_size);
	if (ret)
		goto out_mmu;

	/* shared HostCtl block: allocate once, reuse across hw_init calls */
	if (!pv->hostctl) {
		pv->hostctl = dma_alloc_coherent(pv->drm.dev,
						 sizeof(*pv->hostctl),
						 &pv->hostctl_dma, GFP_KERNEL);
		if (!pv->hostctl) {
			ret = -ENOMEM;
			goto out_mmu;
		}
	}

	ret = prismrv_ccb_init(pv);
	if (ret)
		goto out_hostctl;

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

	prismrv_ccb_fini(pv);
out_hostctl:
	if (pv->hostctl) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->hostctl),
				  pv->hostctl, pv->hostctl_dma);
		pv->hostctl = NULL;
	}
out_mmu:
	/*
	 * errata buffers (if applied) are mapped into the MMU; release
	 * them before tearing down the page tables.
	 */
	prismrv_errata_release(pv);
	prismrv_mmu_fini(pv);
out_fw:
	return ret;
}

void prismrv_hw_fini(struct prismrv_device *pv)
{
	unsigned int i;
	LIST_HEAD(retiring);

	pv->hw_ready = false;

	/*
	 * Stop the GPU from generating new host interrupts by zeroing
	 * the EVENT_HOST_ENABLE register.  This prevents the IRQ handler
	 * from firing for events we are about to stop tracking.
	 */
	if (pv->regs)
		writel(0, pv->regs + EUR_CR_EVENT_HOST_ENABLE);

	/*
	 * Now synchronise with the Linux IRQ subsystem.
	 *
	 * disable_irq() + synchronize_irq() guarantee that after this
	 * pair returns, no IRQ handler is running and no new invocation
	 * can start.  This is necessary because the IRQ handler
	 * (prismrv_handle_completion) dereferences pv->ccb and
	 * pv->hostctl which we are about to free below.
	 *
	 * We must NOT hold any spinlock while calling synchronize_irq()
	 * because it may sleep waiting for a running handler to finish.
	 *
	 * enable_irq() is called at the end of hw_fini() so that
	 * hw_init() (if called afterwards) can arm the interrupt again.
	 */
	if (pv->irq >= 0)
		disable_irq(pv->irq);

	/* retire any fences that will never complete */
	spin_lock(&pv->event_lock);
	while (!list_empty(&pv->pending_fences)) {
		struct prismrv_fence *pf =
			list_first_entry(&pv->pending_fences,
					 struct prismrv_fence, node);
		list_del(&pf->node);

		dma_fence_set_error(&pf->base, -EIO);
		dma_fence_signal(&pf->base);
		/*
		 * Release the BO references the fence was holding.
		 * Must be done outside event_lock (which is a spinlock)
		 * because drm_gem_object_put() may sleep if the last
		 * ref triggers shmem page release.  Stash the fence
		 * pointer and do the put after unlocking.
		 *
		 * We cannot call prismrv_fence_release_bos() here while
		 * holding the spinlock, so splice the whole list first.
		 */
		list_add_tail(&pf->node, &retiring);
	}
	spin_unlock(&pv->event_lock);

	while (!list_empty(&retiring)) {
		struct prismrv_fence *pf =
			list_first_entry(&retiring, struct prismrv_fence, node);
		list_del_init(&pf->node);

		prismrv_fence_release_bos(pf);
		dma_fence_put(&pf->base);
		atomic_dec(&pv->busy_count);

		pm_runtime_mark_last_busy(pv->drm.dev);
		pm_runtime_put_autosuspend(pv->drm.dev);
	}

	prismrv_ccb_fini(pv);

	if (pv->hostctl) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->hostctl),
				  pv->hostctl, pv->hostctl_dma);
		pv->hostctl = NULL;
	}

	prismrv_errata_release(pv);
	for (i = 0; i < PRISMRV_ERRATA_BUF_COUNT; i++)
		pv->errata_buf[i].size = 0;

	/*
	 * Do NOT free the uKernel DMA buffer here.
	 *
	 * uKernel firmware is a device-lifetime resource: loading it is
	 * expensive (filesystem I/O) and the binary does not change across
	 * runtime suspend/resume or GPU recovery cycles.  Freeing and
	 * reloading it in hw_fini/hw_init would:
	 *  1. Cause hw_init() to call prismrv_mmu_map(pv->ukernel_dma) on
	 *     a DMA address that was just freed — mapping freed memory into
	 *     the GPU MMU and potentially causing memory corruption.
	 *  2. Make every recovery cycle hit the filesystem unnecessarily.
	 *
	 * The uKernel buffer is freed only in prismrv_remove() via
	 * prismrv_fw_release() after drm_dev_unplug() has stopped all
	 * new access.
	 */

	/*
	 * Tear down the MMU under mmu_lock so that a concurrent bo_free()
	 * (which also takes mmu_lock before calling mmu_unmap) cannot
	 * race with pd_pts being set to NULL.
	 */
	mutex_lock(&pv->mmu_lock);
	prismrv_mmu_fini(pv);
	mutex_unlock(&pv->mmu_lock);

	/*
	 * Re-enable the IRQ line so the next hw_init() → uKernel boot
	 * can receive completion interrupts.  Paired with the
	 * disable_irq() at the top of this function.
	 */
	if (pv->irq >= 0)
		enable_irq(pv->irq);
}
