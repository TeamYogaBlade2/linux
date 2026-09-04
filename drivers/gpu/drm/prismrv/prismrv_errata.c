// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_errata.c — hardware errata (BRN) handling.
 *
 * The SGX family ships several RTL revisions per core type.  Some revisions
 * require software workarounds, identified by BRN numbers.  The table below
 * mirrors the associations between core revisions and workarounds that the
 * vendor kernel applies (services4/srvkm/hwdefs/sgxerrata.h), expressed as
 * runtime data instead of compile-time #ifdefs so that a single kernel image
 * can drive any revision.
 */
#include "prismrv_device.h"

/* BRN bit assignments */
#define PRISMRV_BRN_29954	BIT(0)	/* disable regbank split */
#define PRISMRV_BRN_31093	BIT(1)
#define PRISMRV_BRN_31195	BIT(2)
#define PRISMRV_BRN_31272	BIT(3)
#define PRISMRV_BRN_31542	BIT(4)
#define PRISMRV_BRN_31620	BIT(5)
#define PRISMRV_BRN_31671	BIT(6)
#define PRISMRV_BRN_31780	BIT(7)	/* PTLA write-back workaround */
#define PRISMRV_BRN_32044	BIT(8)
#define PRISMRV_BRN_32085	BIT(9)
#define PRISMRV_BRN_33920	BIT(10)
#define PRISMRV_BRN_36513	BIT(11)	/* clear-clip WA: extra buffers */
#define PRISMRV_BRN_31542_BIT	BIT(12)	/* internal: clear-clip family marker */

struct prismrv_errata_entry {
	u32 core_id;
	u32 rev;		/* EUR_CR_CORE_REVISION major value */
	u32 brns;
};

static const struct prismrv_errata_entry prismrv_errata_table[] = {
	/* SGX544 revisions */
	{ PRISMRV_CORE_SGX544, 104,
	  PRISMRV_BRN_29954 | PRISMRV_BRN_31093 | PRISMRV_BRN_31195 |
	  PRISMRV_BRN_31272 | PRISMRV_BRN_31542 | PRISMRV_BRN_31620 |
	  PRISMRV_BRN_31671 | PRISMRV_BRN_31780 | PRISMRV_BRN_32044 |
	  PRISMRV_BRN_32085 | PRISMRV_BRN_33920 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 105,
	  PRISMRV_BRN_31780 | PRISMRV_BRN_33920 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 112,
	  PRISMRV_BRN_31272 | PRISMRV_BRN_33920 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 114,
	  PRISMRV_BRN_31780 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 115,
	  PRISMRV_BRN_31780 | PRISMRV_BRN_36513 | PRISMRV_BRN_31542_BIT },
	{ PRISMRV_CORE_SGX544, 116,
	  PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 117,
	  PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 118,
	  PRISMRV_BRN_33920 },
};

/**
 * prismrv_read_revision() — read the core revision registers.
 *
 * EUR_CR_CORE_REVISION layout:
 *   [31:24] designer   [23:16] major   [15:8] minor   [7:0] maintenance
 *
 * The major field is the RTL head revision the vendor driver keys its
 * errata tables on (e.g. 115 for MT6589).
 */
u32 prismrv_read_revision(struct prismrv_device *pv)
{
	pv->core_revision = readl(pv->regs + EUR_CR_CORE_REVISION);
	pv->core_rev_major =
		(pv->core_revision >> EUR_CR_CORE_REVISION_MAJOR_SHIFT) & 0xff;
	pv->core_rev_minor =
		(pv->core_revision >> EUR_CR_CORE_REVISION_MINOR_SHIFT) & 0xff;

	return pv->core_rev_major;
}

void prismrv_errata_init(struct prismrv_device *pv)
{
	unsigned int i;

	pv->errata = 0;

	for (i = 0; i < ARRAY_SIZE(prismrv_errata_table); i++) {
		const struct prismrv_errata_entry *e = &prismrv_errata_table[i];

		if (e->core_id == pv->info->core_id &&
		    e->rev == pv->core_rev_major) {
			pv->errata = e->brns;
			break;
		}
	}

	dev_info(pv->drm.dev, "%s rev %u.%u: active errata mask %#x\n",
		 pv->info->name, pv->core_rev_major, pv->core_rev_minor,
		 pv->errata);
}

/*
 * Errata work-around buffers.
 *
 * The vendor loader allocates these in userspace and hands them to the
 * kernel through the init-info handles; we allocate them in-kernel when
 * the corresponding BRN bit is active for the detected revision.
 * Sizes follow the vendor usage pattern (page-granular staging areas);
 * they have not been validated against real hardware yet.
 */
#include <linux/dma-mapping.h>

#define PTLA_WB_SIZE		SZ_4K		/* BRN_31780 write-back */
#define CC_DM_STREAM_SIZE	SZ_16K		/* BRN_36513 clear-clip WA */
#define CC_INDEX_STREAM_SIZE	SZ_4K
#define CC_PDS_SIZE		SZ_64K
#define CC_USE_SIZE		SZ_64K
#define CC_PARAM_SIZE		SZ_256K

/* GPU-VA placement: inside the slot-0xad (0xa00000) region the uKernel
 * services reference, above the HWRTData pair */
#define ERRATA_VA_BASE		(PRISMRV_HWRTDATA_VADDR + SZ_1K)

/* HWRTData occupies 2*496 bytes from PRISMRV_HWRTDATA_VADDR; the +SZ_1K
 * gap above is the only thing keeping these apart — fail the build if
 * someone changes the sizes so they collide */
#define HWRTDATA_TOTAL		(2 * 496)
#if (ERRATA_VA_BASE - PRISMRV_HWRTDATA_VADDR) < HWRTDATA_TOTAL
#error "errata VA region overlaps HWRTData"
#endif

int prismrv_errata_apply(struct prismrv_device *pv)
{
	static const size_t desc[PRISMRV_ERRATA_BUF_COUNT] = {
		[PRISMRV_ERRATA_BUF_PTLA_WB]   = PTLA_WB_SIZE,
		[PRISMRV_ERRATA_BUF_CC_DM]     = CC_DM_STREAM_SIZE,
		[PRISMRV_ERRATA_BUF_CC_INDEX]  = CC_INDEX_STREAM_SIZE,
		[PRISMRV_ERRATA_BUF_CC_PDS]    = CC_PDS_SIZE,
		[PRISMRV_ERRATA_BUF_CC_USE]    = CC_USE_SIZE,
		[PRISMRV_ERRATA_BUF_CC_PARAM]  = CC_PARAM_SIZE,
	};
	bool need_cc = pv->errata & PRISMRV_BRN_36513;
	unsigned int i;
	u32 va;

	/* re-entry (runtime resume after recovery): buffers are already
	 * allocated and still MMU-mapped */
	if (pv->errata_buf[0].cpu || pv->errata_buf[1].cpu)
		return 0;

	va = ERRATA_VA_BASE;

	for (i = 0; i < PRISMRV_ERRATA_BUF_COUNT; i++) {
		bool need = false;

		if (i == PRISMRV_ERRATA_BUF_PTLA_WB)
			need = pv->errata & PRISMRV_BRN_31780;
		else
			need = need_cc;

		if (!need)
			continue;

		pv->errata_buf[i].size = desc[i];
		pv->errata_buf[i].cpu =
			dma_alloc_coherent(pv->drm.dev, desc[i],
					   &pv->errata_buf[i].dma,
					   GFP_KERNEL);
		if (!pv->errata_buf[i].cpu) {
			prismrv_errata_release(pv);
			return -ENOMEM;
		}
		pv->errata_buf[i].vaddr = va;
		{
			int mret = prismrv_mmu_map(pv, va,
						   pv->errata_buf[i].dma,
						   desc[i]);
			if (mret) {
				/*
				 * MMU mapping failed: free the DMA buffer we
				 * just allocated (the slot's .cpu is set so
				 * errata_release will not free it again —
				 * clear it first to avoid a double-free).
				 */
				dma_free_coherent(pv->drm.dev, desc[i],
						  pv->errata_buf[i].cpu,
						  pv->errata_buf[i].dma);
				pv->errata_buf[i].cpu = NULL;
				pv->errata_buf[i].size = 0;
				prismrv_errata_release(pv);
				return mret;
			}
		}
		va += PAGE_ALIGN(desc[i]);
	}

	return 0;
}

void prismrv_errata_release(struct prismrv_device *pv)
{
	unsigned int i;

	for (i = 0; i < PRISMRV_ERRATA_BUF_COUNT; i++) {
		if (pv->errata_buf[i].cpu) {
			dma_free_coherent(pv->drm.dev,
					  pv->errata_buf[i].size,
					  pv->errata_buf[i].cpu,
					  pv->errata_buf[i].dma);
			pv->errata_buf[i].cpu = NULL;
		}
	}
}
