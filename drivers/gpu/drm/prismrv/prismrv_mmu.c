// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_mmu.c — BIF MMU (2-level, 4 KiB pages, 32-bit VA).
 *
 * Page directory: 1024 entries mapping 4 MiB superpages.
 * Page tables:    1024 entries each, allocated on demand from a
 *                 dma-coherent pool.  A kernel shadow of the PT cpu
 *                 pointers is kept in pd_pts[].
 */
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/slab.h>

#include "prismrv_device.h"

#define PD_ENTRIES	1024
#define PT_ENTRIES	1024
#define PT_SIZE		(PT_ENTRIES * sizeof(u32))

int prismrv_mmu_init(struct prismrv_device *pv)
{
	unsigned int i;

	if (pv->pd_cpu) {
		/*
		 * Recovery path: the page directory already exists but the GPU
		 * hung with stale PTEs in place.  Zero every allocated page
		 * table so the next hw_init starts with a completely clean MMU
		 * state.  Leaving old PTEs would cause the re-initialised GPU
		 * to walk invalid physical addresses and hang again immediately.
		 *
		 * Also zero the page directory itself so freed PD entries that
		 * still point at freed PT DMA buffers are no longer valid.
		 */
		for (i = 0; i < PD_ENTRIES; i++) {
			if (!pv->pd_pts[i])
				continue;
			memset(pv->pd_pts[i], 0, PT_SIZE);
			dma_sync_single_for_device(pv->drm.dev,
						   pv->pd_pt_dma[i],
						   PT_SIZE,
						   DMA_TO_DEVICE);
		}
		memset(pv->pd_cpu, 0, PAGE_SIZE);
		/* Reprogram the BIF DIR_LIST register (may have been cleared
		 * by the soft reset in prismrv_hw_init). */
		writel(pv->pd_gpu_addr | SGX_MMU_PDE_VALID,
		       pv->regs + EUR_CR_BIF_DIR_LIST_BASE0);
		readl(pv->regs + EUR_CR_BIF_DIR_LIST_BASE0);
		return 0;
	}

	pv->pd_cpu = dma_alloc_coherent(pv->drm.dev, PAGE_SIZE,
					&pv->pt_dma_addr, GFP_KERNEL);
	if (!pv->pd_cpu)
		return -ENOMEM;

	pv->pd_pts = kcalloc(PD_ENTRIES, sizeof(u32 *), GFP_KERNEL);
	pv->pd_pt_dma = kcalloc(PD_ENTRIES, sizeof(dma_addr_t), GFP_KERNEL);
	if (!pv->pd_pts || !pv->pd_pt_dma) {
		kfree(pv->pd_pts);
		kfree(pv->pd_pt_dma);
		pv->pd_pts = NULL;
		pv->pd_pt_dma = NULL;
		dma_free_coherent(pv->drm.dev, PAGE_SIZE, pv->pd_cpu,
				  pv->pt_dma_addr);
		pv->pd_cpu = NULL;
		return -ENOMEM;
	}

	memset(pv->pd_cpu, 0, PAGE_SIZE);
	pv->pd_gpu_addr = pv->pt_dma_addr & EUR_CR_BIF_DIR_LIST_ADDR_MASK;

	writel(pv->pd_gpu_addr, pv->regs + EUR_CR_BIF_DIR_LIST_BASE0);
	readl(pv->regs + EUR_CR_BIF_DIR_LIST_BASE0);

	return 0;
}

void prismrv_mmu_fini(struct prismrv_device *pv)
{
	unsigned int i;

	if (pv->pd_pts && pv->pd_pt_dma) {
		for (i = 0; i < PD_ENTRIES; i++) {
			if (pv->pd_pts[i]) {
				dma_free_coherent(pv->drm.dev, PT_SIZE,
						  pv->pd_pts[i],
						  pv->pd_pt_dma[i]);
				pv->pd_pts[i] = NULL;
			}
		}
	}
	kfree(pv->pd_pts);
	kfree(pv->pd_pt_dma);
	pv->pd_pts = NULL;
	pv->pd_pt_dma = NULL;
	if (pv->pd_cpu) {
		dma_free_coherent(pv->drm.dev, PAGE_SIZE, pv->pd_cpu,
				  pv->pt_dma_addr);
		pv->pd_cpu = NULL;
	}
}

int prismrv_mmu_map(struct prismrv_device *pv, u32 vaddr,
		    dma_addr_t phys, size_t size)
{
	unsigned long n_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	unsigned long i;

	for (i = 0; i < n_pages; i++) {
		u32 va = vaddr + i * PAGE_SIZE;
		u32 pd_idx = va >> 22;
		u32 pt_idx = (va >> 12) & 0x3ff;
		u32 *pt;

		if (!(pv->pd_cpu[pd_idx] & SGX_MMU_PDE_VALID)) {
			dma_addr_t pt_dma;

			pt = dma_alloc_coherent(pv->drm.dev, PT_SIZE,
						&pt_dma, GFP_KERNEL);
			if (!pt) {
				/*
				 * Roll back the PTEs written for this
				 * request so callers that don't unmap
				 * (init/errata paths) don't leave valid
				 * mappings pointing at nothing.
				 */
				prismrv_mmu_unmap(pv, vaddr,
						  (u32)i * PAGE_SIZE);
				return -ENOMEM;
			}
			memset(pt, 0, PT_SIZE);
			pv->pd_pts[pd_idx] = pt;
			pv->pd_pt_dma[pd_idx] = pt_dma;
			pv->pd_cpu[pd_idx] =
				cpu_to_le32((pt_dma &
					     EUR_CR_BIF_DIR_LIST_ADDR_MASK) |
					    SGX_MMU_PDE_VALID |
					    SGX_MMU_PDE_PAGE_SIZE_4K);
		}
		pt = pv->pd_pts[pd_idx];
		pt[pt_idx] = cpu_to_le32(((phys + i * PAGE_SIZE) &
					  SGX_MMU_PTE_ADDR_MASK) |
					 SGX_MMU_PTE_VALID);
	}

	return 0;
}

void prismrv_mmu_unmap(struct prismrv_device *pv, u32 vaddr, size_t size)
{
	unsigned long n_pages = DIV_ROUND_UP(size, PAGE_SIZE);
	unsigned long i;
	bool flushed = false;

	for (i = 0; i < n_pages; i++) {
		u32 va = vaddr + i * PAGE_SIZE;
		u32 pd_idx = va >> 22;
		u32 pt_idx = (va >> 12) & 0x3ff;

		if (pv->pd_pts[pd_idx]) {
			pv->pd_pts[pd_idx][pt_idx] = 0;
			flushed = true;
		}
	}

	/*
	 * Flush the BIF TLB after unmapping.  Without this a stale
	 * translation survives and the next BO allocated over the same
	 * VA range can be read through the old mapping (data leak across
	 * GEM clients).  FLUSH is self-clearing.
	 */
	if (flushed) {
		writel(EUR_CR_BIF_CTRL_FLUSH_MASK,
		       pv->regs + EUR_CR_BIF_CTRL);
		readl(pv->regs + EUR_CR_BIF_CTRL);
	}
}
