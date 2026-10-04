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

/*
 * MT6589 is an SGX544 built as "MP" with ONE core
 * (SGX_FEATURE_MP, SGX_FEATURE_MP_CORE_COUNT 1 in the vendor
 * config_kernel_eng.h), so it uses the Hydra register layout:
 *
 *   0x0000-0x3fff  hydra top level (EVENT_*, BIF dir-list/bank, ...)
 *   0x4000-0x7fff  master bank  (EUR_CR_MASTER_*)
 *   0x8000+        core n at (n + 2) * 0x4000
 *
 * The stock init script confirms it: part 1 and several part-2 records
 * address 0x4xxx (master) and 0x8xxx (core 0).  The MP reset sequence
 * (vendor sgxreset.c, !SGX_FEATURE_MP vs. SGX_FEATURE_MP) works through the
 * master registers below, not through EUR_CR_SOFT_RESET.  Values from
 * vendor services4/srvkm/hwdefs/sgxmpdefs.h.
 */
#define PRISMRV_MP_CORE_SELECT(reg, core)	((reg) + (((core) + 2) * 0x4000))

#define EUR_CR_MASTER_SOFT_RESET		0x4080
#define  MASTER_SOFT_RESET_CORE(i)		BIT(i)
#define  MASTER_SOFT_RESET_IPF			BIT(4)
#define  MASTER_SOFT_RESET_DPM			BIT(5)
#define  MASTER_SOFT_RESET_VDM			BIT(6)
#define  MASTER_SOFT_RESET_SLC			BIT(7)
#define  MASTER_SOFT_RESET_BIF			BIT(8)
#define  MASTER_SOFT_RESET_MCI			BIT(9)
#define EUR_CR_MASTER_BIF_CTRL			0x4c00
#define EUR_CR_MASTER_BIF_MMU_CTRL		0x4cd0
#define EUR_CR_MASTER_SLC_CTRL			0x4d00
#define  MASTER_SLC_CTRL_USSE_INVAL_REQ0	0x00040000U
#define  MASTER_SLC_CTRL_ARB_PAGE_SIZE_SHIFT	12
#define EUR_CR_MASTER_SLC_CTRL_BYPASS		0x4d04
#define  MASTER_SLC_BYPASS_REQ_MMU		0x00000010U
#define  MASTER_SLC_BYPASS_REQ_TA		0x00000040U
#define  MASTER_SLC_BYPASS_REQ_USE0		0x00000200U
#define  MASTER_SLC_BYPASS_REQ_USE1		0x00000400U
#define  MASTER_SLC_BYPASS_REQ_USE2		0x00000800U
#define  MASTER_SLC_BYPASS_REQ_USE3		0x00001000U
#define  MASTER_SLC_BYPASS_BYP_CC		0x04000000U

/* BIF_MMU_CTRL field values shared by the master and the per-core copy */
#define BIF_MMU_CTRL_PREFETCHING_ON		BIT(0)
#define BIF_MMU_CTRL_ADDR_HASH_MODE_SHIFT	1
#define BIF_MMU_CTRL_ENABLE_DC_TLB		BIT(4)

/*
 * Clock gating written by the vendor SGXInitClocks() before the reset;
 * values from the vendor SGX_BRIDGE_INIT_INFO dump (emu/sgx_initinfo_dump.bin:
 * ui32ClkGateCtl / ui32ClkGateCtl2).
 */
#define PRISMRV_CLKGATECTL_DEFAULT		0x002aaaaaU
#define PRISMRV_CLKGATECTL2_DEFAULT		0x0a8a8aaaU

#endif /* _PRISMRV_REGS_H_ */
