/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * prismrv_regs.h — SGX register definitions used by the driver.
 *
 * Register layouts come from prismrv_regs_gen.h, which is generated
 * from the per-core hardware definition headers (see mainline-tools/
 * gen_regs.py).  This file only adds what the generated data cannot
 * express: the MMU page-table field encodings (which the vendor
 * headers define through preprocessor conditionals) and small derived
 * helpers.
 */
#ifndef _PRISMRV_REGS_H_
#define _PRISMRV_REGS_H_

#include <linux/bits.h>

#include "prismrv_regs_gen.h"

/*
 * MMU page table format (2-level, 32-bit VA).
 *
 * Page directory entry: [31:12] page table address (4 KiB),
 *                       [5:1] page size, [0] valid.
 * Page table entry:     [31:12] physical page address,
 *                       [3] cache-coherent, [2] read-only, [0] valid.
 */
#define SGX_MMU_PAGE_SHIFT			12
#define SGX_MMU_PAGE_SIZE			(1U << SGX_MMU_PAGE_SHIFT)
/* page directory entry */
#define SGX_MMU_PDE_VALID			(0x00000001U)
#define SGX_MMU_PDE_PAGE_SIZE_4K		(0x00000000U)
/* page table entry */
#define SGX_MMU_PTE_VALID			(0x00000001U)
#define SGX_MMU_PTE_READONLY			(0x00000004U)
#define SGX_MMU_PTE_CACHECONSISTENT		(0x00000008U)
#define SGX_MMU_PTE_ADDR_MASK			GENMASK(31, 12)
#define EUR_CR_BIF_DIR_LIST_ADDR_MASK		GENMASK(31, 12)

#endif /* _PRISMRV_REGS_H_ */
