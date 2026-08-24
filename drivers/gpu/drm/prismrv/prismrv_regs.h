/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * prismrv_regs.h — SGX core register definitions.
 *
 * Register offsets are common across the SGX5xx family; a few are
 * conditional on the core generation (guarded by PRISMRV_HAS_*).
 */
#ifndef _PRISMRV_REGS_H_
#define _PRISMRV_REGS_H_

#include <linux/bits.h>

/* ---- clock gating ------------------------------------------------------ */
#define EUR_CR_CLKGATECTL			0x0000
#define EUR_CR_CLKGATECTL2			0x0004

/* ---- identification ---------------------------------------------------- */
#define EUR_CR_CORE_REVISION			0x0014
#define EUR_CR_CORE_REVISION_DESIGNER_SHIFT	24
#define EUR_CR_CORE_REVISION_MAJOR_SHIFT	16
#define EUR_CR_CORE_REVISION_MINOR_SHIFT	8
#define EUR_CR_CORE_REVISION_MAINTENANCE_SHIFT	0

#define EUR_CR_POWER				0x001C

/* ---- soft reset -------------------------------------------------------- */
#define EUR_CR_SOFT_RESET			0x0080
#define EUR_CR_SOFT_RESET_BIF_RESET		BIT(0)
#define EUR_CR_SOFT_RESET_VDM_RESET		BIT(1)
#define EUR_CR_SOFT_RESET_DPM_RESET		BIT(2)
#define EUR_CR_SOFT_RESET_TE_RESET		BIT(3)
#define EUR_CR_SOFT_RESET_MTE_RESET		BIT(4)
#define EUR_CR_SOFT_RESET_ISP_RESET		BIT(5)
#define EUR_CR_SOFT_RESET_ISP2_RESET		BIT(6)
#define EUR_CR_SOFT_RESET_TSP_RESET		BIT(7)
#define EUR_CR_SOFT_RESET_PDS_RESET		BIT(8)
#define EUR_CR_SOFT_RESET_PBE_RESET		BIT(9)
#define EUR_CR_SOFT_RESET_TCU_L2_RESET		BIT(10)
#define EUR_CR_SOFT_RESET_UCACHEL2_RESET	BIT(11)
#define EUR_CR_SOFT_RESET_ITR_RESET		BIT(13)
#define EUR_CR_SOFT_RESET_TEX_RESET		BIT(14)
#define EUR_CR_SOFT_RESET_USE_RESET		BIT(15)
#define EUR_CR_SOFT_RESET_IDXFIFO_RESET		BIT(16)
#define EUR_CR_SOFT_RESET_TA_RESET		BIT(17)
#define EUR_CR_SOFT_RESET_DCU_L2_RESET		BIT(18)
#define EUR_CR_SOFT_RESET_DCU_L0L1_RESET	BIT(19)

/* ---- events / kicks ----------------------------------------------------*/
#define EUR_CR_EVENT_STATUS			0x012C
#define EUR_CR_EVENT_STATUS_MASTER_INTERRUPT	BIT(30)
#define EUR_CR_TIMER				0x0144
#define EUR_CR_EVENT_KICK			0x0AC8
#define EUR_CR_EVENT_KICK_NOW			BIT(0)

/* ---- BIF (Bus Interface Unit + MMU) ------------------------------------ */
#define EUR_CR_BIF_CTRL				0x0C00
#define EUR_CR_BIF_CTRL_PAUSE			BIT(1)
#define EUR_CR_BIF_CTRL_CLEAR_FAULT		BIT(4)
#define EUR_CR_BIF_CTRL_MMU_BYPASS_HOST		BIT(6)

#define EUR_CR_BIF_INT_STAT			0x0C04
#define EUR_CR_BIF_INT_STAT_FAULT_REQ_MASK	GENMASK(13, 0)
#define EUR_CR_BIF_INT_STAT_FLUSH_COMPLETE	BIT(19)

#define EUR_CR_BIF_FAULT			0x0C08
#define EUR_CR_BIF_FAULT_ADDR_MASK		GENMASK(31, 12)

#define EUR_CR_BIF_BANK_SET			0x0C74
#define EUR_CR_BIF_BANK0			0x0C78
#define EUR_CR_BIF_DIR_LIST_BASE0		0x0C84
#define EUR_CR_BIF_DIR_LIST_BASE1		0x0C38
#define EUR_CR_BIF_DIR_LIST_BASE_ADDR_MASK	GENMASK(31, 12)
#define EUR_CR_BIF_MMU_CTRL			0x0CD0
#define EUR_CR_BIF_MMU_CTRL_PREFETCHING_ON	BIT(0)

/* ---- master interrupt -------------------------------------------------- */
#define EUR_CR_MASTER_IRQ			0x0D10
#define EUR_CR_MASTER_IRQ_ACTIVE		BIT(16)

/* MMU page table format (SGX5xx, 4 KiB pages) */
#define SGX_MMU_PAGE_SHIFT			12
#define SGX_MMU_PAGE_SIZE			(1U << SGX_MMU_PAGE_SHIFT)
#define SGX_MMU_PDE_VALID			BIT(0)
#define SGX_MMU_PDE_PAGE_SIZE_4K		(0UL)
#define SGX_MMU_PTE_VALID			BIT(0)
#define SGX_MMU_PTE_READONLY			BIT(2)
#define SGX_MMU_PTE_CACHECONSISTENT		BIT(3)
#define SGX_MMU_PTE_ADDR_MASK			GENMASK(31, 12)

#endif /* _PRISMRV_REGS_H_ */
