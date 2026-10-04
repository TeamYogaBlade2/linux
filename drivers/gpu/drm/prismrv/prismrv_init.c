// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_init.c — hardware bring-up sequence.
 *
 * Mirrors the vendor SGXInitialise() order (services4/srvkm/devices/sgx/
 * sgxinit.c).  The init script blob captured from the vendor loader
 * (DEVINITPART2) is split by a HALT record into two halves that the
 * vendor driver runs on either side of the reset:
 *
 *   1. init script part 1        (identity check, up to the first HALT)
 *   2. soft reset                (EUR_CR_SOFT_RESET pulse)
 *   3. pipe configuration        (EUR_CR_POWER)
 *   4. BIF context reset         (bank / dir-list registers; the script
 *                                 does not program any of these, the
 *                                 MMU code does right after)
 *   5. init script part 2        (post-reset register programming:
 *                                 event enables, USE_CODE_BASE_0..15,
 *                                 banked USE/PDS/MTE registers)
 *   6. uKernel upload + HostCtl  (clock stamp, InitStatus = 0)
 *   7. EVENT_KICK                (starts the uKernel main loop)
 *   8. poll ui32InitStatus       (PVRSRV_USSE_EDM_INIT_COMPLETE)
 *
 * Everything the script writes for USE_CODE_BASE_*, event enables and
 * the banked USE state lives in blocks that the soft reset clears, so it
 * must run *after* the reset (part 2).  Running it once before the reset
 * (as an earlier revision did) silently discarded all of it.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>

#include "prismrv_device.h"

/* vendor SGXResetSleep(): wait ~100 core clocks and let posted writes land */
static void prismrv_reset_sleep(struct prismrv_device *pv)
{
	readl(pv->regs + EUR_CR_MASTER_SOFT_RESET);
	udelay(20);
}

/*
 * SGXInitClocks(): program the clock gating before anything else.
 */
void prismrv_init_clocks(struct prismrv_device *pv)
{
	writel(PRISMRV_CLKGATECTL_DEFAULT, pv->regs + EUR_CR_CLKGATECTL);
	readl(pv->regs + EUR_CR_CLKGATECTL);
	writel(PRISMRV_CLKGATECTL2_DEFAULT, pv->regs + EUR_CR_CLKGATECTL2);
	readl(pv->regs + EUR_CR_CLKGATECTL2);
}

/*
 * SGXReset() for SGX_FEATURE_MP (vendor sgxreset.c).  The single-core
 * EUR_CR_SOFT_RESET register that an earlier revision used is not what
 * resets an MP-built SGX544:
 *
 *   1. master soft reset: BIF, IPF, DPM, VDM (+MCI on recovery / first
 *      power-up), SLC (the system cache is present), and a hard reset of
 *      all four core slots even though only core 0 exists;
 *   2. master BIF control cleared;
 *   3. system-level cache setup (SLC_CTRL / SLC_CTRL_BYPASS, with the BRN
 *      31620 / 31195 bypass bits);
 *   4. resets released;
 *   5. MMU control: with BRN 32085 (prefetch) and 31620/31671 (DC TLB)
 *      present on this core revision the value is just the hash mode,
 *      written to the master and to every core.
 */
void prismrv_soft_reset(struct prismrv_device *pv, bool hw_recovery)
{
	const u32 brn = pv->errata;
	u32 v, mmu;

	v = MASTER_SOFT_RESET_BIF | MASTER_SOFT_RESET_IPF |
	    MASTER_SOFT_RESET_DPM | MASTER_SOFT_RESET_VDM |
	    MASTER_SOFT_RESET_SLC |
	    MASTER_SOFT_RESET_CORE(0) | MASTER_SOFT_RESET_CORE(1) |
	    MASTER_SOFT_RESET_CORE(2) | MASTER_SOFT_RESET_CORE(3);
	if (hw_recovery || !pv->hw_inited_once)
		v |= MASTER_SOFT_RESET_MCI;
	writel(v, pv->regs + EUR_CR_MASTER_SOFT_RESET);
	prismrv_reset_sleep(pv);

	writel(0, pv->regs + EUR_CR_MASTER_BIF_CTRL);
	readl(pv->regs + EUR_CR_MASTER_BIF_CTRL);
	prismrv_reset_sleep(pv);

	writel(MASTER_SLC_CTRL_USSE_INVAL_REQ0 |
	       (0xc << MASTER_SLC_CTRL_ARB_PAGE_SIZE_SHIFT),
	       pv->regs + EUR_CR_MASTER_SLC_CTRL);
	readl(pv->regs + EUR_CR_MASTER_SLC_CTRL);

	v = MASTER_SLC_BYPASS_BYP_CC;
	if (brn & PRISMRV_BRN_31620)
		v |= MASTER_SLC_BYPASS_REQ_MMU;
	if (brn & PRISMRV_BRN_31195)
		v |= MASTER_SLC_BYPASS_REQ_USE0 | MASTER_SLC_BYPASS_REQ_USE1 |
		     MASTER_SLC_BYPASS_REQ_USE2 | MASTER_SLC_BYPASS_REQ_USE3 |
		     MASTER_SLC_BYPASS_REQ_TA;
	writel(v, pv->regs + EUR_CR_MASTER_SLC_CTRL_BYPASS);
	readl(pv->regs + EUR_CR_MASTER_SLC_CTRL_BYPASS);
	prismrv_reset_sleep(pv);

	writel(0, pv->regs + EUR_CR_MASTER_SOFT_RESET);
	prismrv_reset_sleep(pv);

	if (brn & (PRISMRV_BRN_31620 | PRISMRV_BRN_31671 | PRISMRV_BRN_32085)) {
		mmu = 1U << BIF_MMU_CTRL_ADDR_HASH_MODE_SHIFT;
		if (!(brn & PRISMRV_BRN_32085))	/* (31278 also disables it) */
			mmu |= BIF_MMU_CTRL_PREFETCHING_ON;
		if (!(brn & (PRISMRV_BRN_31620 | PRISMRV_BRN_31671)))
			mmu |= BIF_MMU_CTRL_ENABLE_DC_TLB;
		writel(mmu, pv->regs + EUR_CR_MASTER_BIF_MMU_CTRL);
		readl(pv->regs + EUR_CR_MASTER_BIF_MMU_CTRL);
		writel(mmu, pv->regs + PRISMRV_MP_CORE_SELECT(EUR_CR_BIF_MMU_CTRL, 0));
		readl(pv->regs + PRISMRV_MP_CORE_SELECT(EUR_CR_BIF_MMU_CTRL, 0));
	}
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
 * Run the init script from start_rec up to (and including) the next HALT
 * record, or to the end of the blob.
 *
 * Offsets are BYTE offsets into the register page, including the
 * bank-switched windows (0x4000+, 0x8000+; the stock blob stays below 0x8ba0, inside the 64 KiB DT aperture).
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
		u32 off = le32_to_cpu(rec->offset);

		if (off >= pv->regs_size || (off & 3)) {
			dev_err(pv->drm.dev,
				"init script rec %zu: bad offset 0x%x\n", i, off);
			return -EINVAL;
		}

		switch (op) {
		case PRISMRV_INIT_OP_WRITE:
			writel(le32_to_cpu(rec->value), pv->regs + off);
			readl(pv->regs + off);
			break;
		case PRISMRV_INIT_OP_READ:
			readl(pv->regs + off);
			break;
		case PRISMRV_INIT_OP_HALT:
			return i + 1;
		default:
			dev_err(pv->drm.dev,
				"init script rec %zu: bad op %u\n", i, op);
			return -EINVAL;
		}
	}
	return n;
}

int prismrv_hw_init(struct prismrv_device *pv)
{
	const struct firmware *fw = NULL;
	unsigned int i;
	int ret, next;
	bool quiesced = false;

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

	if (!fw->size || fw->size % sizeof(struct prismrv_init_rec)) {
		dev_err(pv->drm.dev, "init script size %zu is not a multiple of %zu\n",
			fw->size, sizeof(struct prismrv_init_rec));
		release_firmware(fw);
		return -EINVAL;
	}

	/* vendor SGXInitClocks() precedes everything else */
	prismrv_init_clocks(pv);

	/* part 1: pre-reset (identity check).  Ends at the first HALT. */
	next = prismrv_run_script_range(pv, fw, 0);
	if (next < 0) {
		ret = next;
		release_firmware(fw);
		goto out_fw;
	}

	prismrv_read_revision(pv);	/* revision stable after clocks on */
	prismrv_errata_init(pv);	/* sets pv->errata bitmask only */

	prismrv_soft_reset(pv, pv->hw_recovery);

	/* default pipe count: all pipes fully enabled */
	writel(0, pv->regs + EUR_CR_POWER);

	prismrv_bif_reset(pv);

	/*
	 * part 2: post-reset register programming.  A blob without a HALT
	 * separator (legacy single-shot capture) has next == n here, in
	 * which case the whole script is replayed after the reset so the
	 * USE_CODE_BASE / event-enable writes still take effect.
	 */
	if (next >= (int)(fw->size / sizeof(struct prismrv_init_rec))) {
		dev_warn(pv->drm.dev,
			 "init script has no HALT separator, replaying it after reset\n");
		next = 0;
	}
	ret = prismrv_run_script_range(pv, fw, next);
	release_firmware(fw);
	fw = NULL;
	if (ret < 0)
		goto out_fw;

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
			pv->hw_inited_once = true;
			pv->hw_recovery = false;
			pv->hw_ready = true;
			dev_info(pv->drm.dev,
				 "uKernel initialised after %d polls\n", i);
			return 0;
		}
		usleep_range(900, 1100);
	}

	dev_err(pv->drm.dev, "uKernel init timeout\n");
	ret = -ETIMEDOUT;

	/*
	 * Failure cleanup follows the same rule as hw_fini(): mask the
	 * interrupt sources and wait out any handler BEFORE the CCB,
	 * HostCtl and MMU the handler dereferences are freed.  (The init
	 * script has already enabled SW_EVENT by now.)
	 */
	prismrv_hw_irq_quiesce(pv);
	quiesced = true;
	prismrv_ccb_fini(pv);
out_hostctl:
	if (!quiesced) {
		prismrv_hw_irq_quiesce(pv);
		quiesced = true;
	}
	if (pv->hostctl) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->hostctl),
				  pv->hostctl, pv->hostctl_dma);
		pv->hostctl = NULL;
	}
out_mmu:
	if (!quiesced) {
		prismrv_hw_irq_quiesce(pv);
		quiesced = true;
	}
	/*
	 * errata buffers (if applied) are mapped into the MMU; release
	 * them before tearing down the page tables.
	 */
	prismrv_errata_release(pv);
	prismrv_mmu_fini(pv);
out_fw:
	if (!quiesced) {
		/* an init-script write may already have armed an event */
		prismrv_hw_irq_quiesce(pv);
		quiesced = true;
	}
	/* the line stays masked at the source (HOST_ENABLE = 0); rebalance
	 * the disable depth so the next hw_init() can arm it again */
	if (pv->irq >= 0)
		enable_irq(pv->irq);
	return ret;
}

void prismrv_hw_fini(struct prismrv_device *pv)
{
	unsigned int i;
	unsigned long flags;
	struct prismrv_fence *it;
	LIST_HEAD(retiring);

	pv->hw_ready = false;

	/*
	 * Stop the GPU from generating new host interrupts by zeroing
	 * the EVENT_HOST_ENABLE register.  This prevents the IRQ handler
	 * from firing for events we are about to stop tracking.
	 */

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
	prismrv_hw_irq_quiesce(pv);

	/* stop the hang watchdog before tearing the rings down */
	cancel_delayed_work_sync(&pv->hang_work);

	/*
	 * Retire any fences that will never complete.  Detach them from
	 * the pending list under event_lock, but signal them only after
	 * the lock is dropped: dma_fence_signal() runs arbitrary callbacks
	 * (possibly from other subsystems) that must not execute under a
	 * driver spinlock that the IRQ handler also takes.
	 */
	spin_lock_irqsave(&pv->event_lock, flags);
	list_splice_init(&pv->pending_fences, &retiring);
	spin_unlock_irqrestore(&pv->event_lock, flags);

	list_for_each_entry(it, &retiring, node) {
		dma_fence_set_error(&it->base, -EIO);
		dma_fence_signal(&it->base);
	}

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
