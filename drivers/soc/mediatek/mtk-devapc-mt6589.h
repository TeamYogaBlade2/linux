// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6589 Device APC register description
 *
 * The MT6589 DEVAPC is five independent instances (DEVAPC0..DEVAPC4), each
 * split across two register windows:
 *
 *   AO window  0x10010000 + n * 0x100   permission (APC) registers
 *   PD window  0x10207000 + n * 0x100   violation mask / status / debug
 *
 * Each instance watches up to 32 slave modules per domain master, and there
 * are four domain masters: AP (0), MD1 (1), MD2 (2) and MM (3).  A slave's
 * permission is a 2-bit field selecting one of:
 *
 *   0 L0  no protection
 *   1 L1  only RW for secure access
 *   2 L2  only RW for secure access, non-secure read allowed
 *   3 L3  forbidden - any access raises a violation
 *
 * Copyright (c) 2026 MediaTek Inc.
 */

#ifndef _MTK_DEVPAPC_MT6589_H
#define _MTK_DEVPAPC_MT6589_H

#include <linux/bits.h>

/* Window stride between consecutive DEVAPC instances. */
#define MT6589_DEVPAPC_STRIDE		0x100

/* Number of DEVAPC instances on this SoC (DEVAPC0 .. DEVAPC4). */
#define MT6589_DEVPAPC_INSTANCES	5

/*
 * Worst case module count across the five instances.  DEVAPC0 has 24 modules
 * and DEVAPC2 has 30, so 30 is what has to be programmed.
 */
#define MT6589_DEVPAPC_MAX_MODULES	30

/* Domain masters; also index the matching D<d>_VIO_MASK / D<d>_VIO_STA. */
enum mt6589_devapc_domain {
	MT6589_DOMAIN_AP	= 0,
	MT6589_DOMAIN_MD1	= 1,
	MT6589_DOMAIN_MD2	= 2,
	MT6589_DOMAIN_MM	= 3,
	MT6589_DOMAIN_COUNT	= 4,
};

/* Permission levels, matching the hardware encoding. */
enum mt6589_devapc_perm {
	MT6589_APC_L0	= 0,	/* no protection */
	MT6589_APC_L1	= 1,	/* secure RW only */
	MT6589_APC_L2	= 2,	/* secure RW, non-secure read */
	MT6589_APC_L3	= 3,	/* forbidden */
};

/* AO window register offsets, relative to an instance's AO base. */
#define MT6589_DEVPAPC_D0_APC_0		0x0000
#define MT6589_DEVPAPC_D0_APC_1		0x0004
#define MT6589_DEVPAPC_D1_APC_0		0x0008
#define MT6589_DEVPAPC_D1_APC_1		0x000c
#define MT6589_DEVPAPC_D2_APC_0		0x0010
#define MT6589_DEVPAPC_D2_APC_1		0x0014
#define MT6589_DEVPAPC_D3_APC_0		0x0018
#define MT6589_DEVPAPC_D3_APC_1		0x001c
#define MT6589_DEVPAPC_APC_CON		0x0090
#define MT6589_DEVPAPC_APC_LOCK		0x0094
#define MT6589_DEVPAPC_MAS_DOM		0x00a0
#define MT6589_DEVPAPC_MAS_SEC		0x00a4

/*
 * PD window register offsets, relative to an instance's PD base.
 *
 * There is no VIO_SHIFT_* register pair on this block: VIO_DBG0 and VIO_DBG1
 * are latched directly, so the shift mechanism the MT6779 driver relies on
 * must not be attempted here.
 */
#define MT6589_DEVPAPC_D0_VIO_MASK	0x0020
#define MT6589_DEVPAPC_D1_VIO_MASK	0x0024
#define MT6589_DEVPAPC_D2_VIO_MASK	0x0028
#define MT6589_DEVPAPC_D3_VIO_MASK	0x002c
#define MT6589_DEVPAPC_D0_VIO_STA	0x0030
#define MT6589_DEVPAPC_D1_VIO_STA	0x0034
#define MT6589_DEVPAPC_D2_VIO_STA	0x0038
#define MT6589_DEVPAPC_D3_VIO_STA	0x003c
#define MT6589_DEVPAPC_VIO_DBG0		0x0040
#define MT6589_DEVPAPC_VIO_DBG1		0x0044
#define MT6589_DEVPAPC_DXS_VIO_MASK	0x0080
#define MT6589_DEVPAPC_DXS_VIO_STA	0x0084
#define MT6589_DEVPAPC_PD_APC_CON	0x0090
#define MT6589_DEVPAPC_DEC_ERR_CON	0x00b4
#define MT6589_DEVPAPC_DEC_ERR_ADDR	0x00b8
#define MT6589_DEVPAPC_DEC_ERR_ID	0x00bc

/* APC_CON bit2 must be cleared for an instance to report violations. */
#define MT6589_DEVPAPC_APC_CON_STOP	BIT(2)

/*
 * VIO_DBG0 - latched violation descriptor (per DEVAPC instance, PD window).
 *
 * NOTE: this layout is NOT the MT6779/MT8186 one.  The upstream in-tree
 * comment claiming it was "shared with the other MediaTek DEVAPC blocks"
 * was wrong; the fields below are what MT6589 hardware actually decodes.
 *
 *   Bit(s)   Field       Description
 *   ------   ----------  ----------------------------------------------
 *   10:0     MASTER_ID   Violation master ID {AXI ID:8, Port ID:3}.
 *                       Identifies which domain master attempted the
 *                       access that was refused.
 *   13:12    DOMAIN_ID   Violation domain ID (2 bits): which of the four
 *                       domain masters (AP/MD1/MD2/MM) was the master.
 *   28       W_VIO       Set if the abort was caused by a WRITE.
 *   29       R_VIO       Set if the abort was caused by a READ.
 *   31       CLR         Write-1-to-clear; SW clears the whole debug latch.
 *                       (Reading this register does NOT clear it - decode
 *                       before clearing or the information is lost forever.)
 *
 * Bits 30, 27:14 are reserved/unused.
 *
 * VIO_DBG1 is the full 32-bit faulting address (the vendor prints it
 * directly; no split address field exists in VIO_DBG0 on this block).
 *
 * Evidence for every field:
 *   - aquaris-5 mediatek/platform/mt6589/kernel/drivers/devapc/devapc.c
 *       master_ID    = dbg0 & 0x000007FF            -> [10:0]
 *       domain_ID    = (dbg0 >> 12) & 0x3           -> [13:12]
 *       r_w_violation= (dbg0 >> 28) & 0x3           -> [29:28]  (1=W, else R)
 *       writes 0x80000000 to VIO_DBG0 to clear      -> bit31 CLR
 *   - MT6589 datasheet, EMI MPU EMI_MPUQ/S/T VIO_DBG0 (same block family
 *     and same latch semantics, section 18.x / EMI MPUS ~p.622):
 *       "13:12 DOMAIN_ID  Violation domain ID"
 *       "10:0  MASTER_ID  Records the violation master ID {AXI ID, Port ID}"
 *       "29    R_VID      Read violation"
 *       "28    W_VID      Write violation"
 *       "31    CLR        SW write CLR to 1 will clear ... to be 0"
 */
#define MT6589_DEVPAPC_VIO_DBG0_MSTID	GENMASK(10, 0)
#define MT6589_DEVPAPC_VIO_DBG0_DMNID	GENMASK(13, 12)
#define MT6589_DEVPAPC_VIO_DBG0_VIO_W	BIT(28)
#define MT6589_DEVPAPC_VIO_DBG0_VIO_R	BIT(29)

/* VIO_DBG0 bit31 is write-1-to-clear; it releases the debug latch. */
#define MT6589_DEVPAPC_VIO_DBG0_CLR	BIT(31)

/* Each slave gets a 2-bit field; 16 slaves per APC register. */
#define MT6589_DEVPAPC_PERM_SHIFT(m)	(2 * ((m) % 16))
#define MT6589_DEVPAPC_PERM_MASK(m)	GENMASK(1 + MT6589_DEVPAPC_PERM_SHIFT(m), \
						  MT6589_DEVPAPC_PERM_SHIFT(m))

/*
 * Permission registers for domain d sit at d * 8, and the low/high half
 * register pair is selected by module / 16.
 */
#define MT6589_DEVPAPC_APC_REG(dom, mod)	((u32)(dom) * 8 + ((mod) / 16) * 4)

/* Violation mask/status registers for domain d are contiguous, 4 bytes apart. */
#define MT6589_DEVPAPC_VIO_MASK_REG(dom) \
	(MT6589_DEVPAPC_D0_VIO_MASK + (u32)(dom) * 4)
#define MT6589_DEVPAPC_VIO_STA_REG(dom) \
	(MT6589_DEVPAPC_D0_VIO_STA + (u32)(dom) * 4)

#endif /* _MTK_DEVPAPC_MT6589_H */
