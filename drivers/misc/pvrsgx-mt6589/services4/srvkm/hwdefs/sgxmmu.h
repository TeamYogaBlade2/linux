// SPDX-License-Identifier: MIT OR GPL-2.0-only

#if !defined(__SGXMMU_KM_H__)
#define __SGXMMU_KM_H__

/* to be implemented */

/* SGX MMU maps 4Kb pages */
#define SGX_MMU_PAGE_SHIFT				(12)
#define SGX_MMU_PAGE_SIZE				(1U<<SGX_MMU_PAGE_SHIFT)
#define SGX_MMU_PAGE_MASK				(SGX_MMU_PAGE_SIZE - 1U)

/* PD details */
#define SGX_MMU_PD_SHIFT				(10)
#define SGX_MMU_PD_SIZE					(1U<<SGX_MMU_PD_SHIFT)
#define SGX_MMU_PD_MASK					(0xFFC00000U)

/* PD Entry details */
#if defined(SGX_FEATURE_36BIT_MMU)
	#define SGX_MMU_PDE_ADDR_MASK			(0xFFFFFF00U)
	#define SGX_MMU_PDE_ADDR_ALIGNSHIFT		(4)
#else
	#define SGX_MMU_PDE_ADDR_MASK			(0xFFFFF000U)
	#define SGX_MMU_PDE_ADDR_ALIGNSHIFT		(0)
#endif
#define SGX_MMU_PDE_VALID				(0x00000001U)
/* variable page size control field */
#define SGX_MMU_PDE_PAGE_SIZE_4K		(0x00000000U)
#define SGX_MMU_PDE_PAGE_SIZE_16K		(0x00000002U)
#define SGX_MMU_PDE_PAGE_SIZE_64K		(0x00000004U)
#define SGX_MMU_PDE_PAGE_SIZE_256K		(0x00000006U)
#define SGX_MMU_PDE_PAGE_SIZE_1M		(0x00000008U)
#define SGX_MMU_PDE_PAGE_SIZE_4M		(0x0000000AU)
#define SGX_MMU_PDE_PAGE_SIZE_MASK		(0x0000000EU)

/* PT details */
#define SGX_MMU_PT_SHIFT				(10)
#define SGX_MMU_PT_SIZE					(1U<<SGX_MMU_PT_SHIFT)
#define SGX_MMU_PT_MASK					(0x003FF000U)

/* PT Entry details */
#if defined(SGX_FEATURE_36BIT_MMU)
	#define SGX_MMU_PTE_ADDR_MASK			(0xFFFFFF00U)
	#define SGX_MMU_PTE_ADDR_ALIGNSHIFT		(4)
#else
	#define SGX_MMU_PTE_ADDR_MASK			(0xFFFFF000U)
	#define SGX_MMU_PTE_ADDR_ALIGNSHIFT		(0)
#endif
#define SGX_MMU_PTE_VALID				(0x00000001U)
#define SGX_MMU_PTE_WRITEONLY			(0x00000002U)
#define SGX_MMU_PTE_READONLY			(0x00000004U)
#define SGX_MMU_PTE_CACHECONSISTENT		(0x00000008U)
#define SGX_MMU_PTE_EDMPROTECT			(0x00000010U)

#endif	/* __SGXMMU_KM_H__ */

/*****************************************************************************
 End of file (sgxmmu.h)
*****************************************************************************/
