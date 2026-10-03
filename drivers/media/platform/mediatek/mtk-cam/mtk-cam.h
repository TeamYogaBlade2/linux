/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6589 CAM/ISP top-level (control) driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * Only the registers and fields that are legible in the MT6589 data sheet
 * are recorded here.  Anything not verifiable is deliberately absent rather
 * than guessed; see mtk-cam.c.
 */

#ifndef _MTK_CAM_H
#define _MTK_CAM_H

#include <linux/mutex.h>
#include <media/v4l2-subdev.h>

/*
 * CAM block base 0x15004000.  Registers transcribed from the data sheet.
 *
 * The block splits its control registers three ways, and this matters when
 * writing to them:
 *
 *   - plain read/write shadow registers (CAM_CTL_START and friends)
 *   - write-only set/clear registers (CAM_CTL_EN1_SET / _CLR), which have
 *     no readback and must not be written directly
 *   - the readable status copies of the same bits
 */
#define CAM_CTL_START			0x00
#define CAM_CTL_EN1			0x04	/* shadow; use _SET/_CLR */
#define CAM_CTL_EN2			0x08	/* shadow; use _SET/_CLR */
#define CAM_CTL_DMA_EN			0x0c	/* shadow; use _SET/_CLR */
#define CAM_CTL_FMT_SEL			0x10
#define CAM_CTL_SEL			0x18
#define CAM_CTL_PIX_ID			0x1c
#define CAM_CTL_INT_EN			0x20
#define CAM_CTL_INT_STATUS		0x24
#define CAM_CTL_DMA_INT			0x28
#define CAM_CTL_TPIPE			0x50
#define CAM_CTL_TCM_EN			0x54
#define CAM_CTL_SRAM_CFG		0x58
#define CAM_CTL_SW_CTL			0x5c	/* software reset control */

/* Write-only set/clear companions of the EN1/EN2/DMA_EN shadows. */
#define CAM_CTL_EN1_SET			0x80
#define CAM_CTL_EN1_CLR			0x84
#define CAM_CTL_EN2_SET			0x88
#define CAM_CTL_EN2_CLR			0x8c
#define CAM_CTL_DMA_EN_SET		0x90
#define CAM_CTL_DMA_EN_CLR		0x94

/*
 * CAM_CTL_START (0x15004000), reset value 0x00000000.  All six documented
 * bits are type WO start strobes (data sheet page 1949-1950); there is no
 * read/write state bit in this register.
 */
#define CAM_CTL_START_CQ0B_START	BIT(6)
#define CAM_CTL_START_CQ0_START	BIT(5)
#define CAM_CTL_START_PASS2C_START	BIT(4)
#define CAM_CTL_START_FMT_START	BIT(3)
#define CAM_CTL_START_PASS2B_START	BIT(1)
#define CAM_CTL_START_PASS2_START	BIT(0)

/*
 * The individual CAM_CTL_EN1 sub-module bits.
 *
 * These ARE documented: the MT6589 data sheet page 1977 carries the
 * CAM_CTL_EN1_SET bit table, page 1978 the matching CAM_CTL_EN1_CLR table.
 * Every one of them is type WO, described as "0: No effect, 1: Set this bit
 * to 1" for _SET and "0: No effect, 1: Set this bit to 1" for _CLR.  So the
 * two registers are write-one-to-set and write-one-to-clear mirrors of
 * CAM_CTL_EN1 -- writing a 1 into _SET sets that enable, writing a 1 into
 * _CLR clears it.  There is no write-zero-to-clear anywhere in this block.
 *
 * The block-level enable the driver actually needs to touch is CAM_EN (bit
 * 30); the rest are CPIPE stages and are not enabled until someone writes
 * the pipeline itself.
 */
#define CAM_CTL_EN1_CAM_EN			BIT(30)

/*
 * CAM_CTL_SW_CTL (0x1500405c), reset value 0x00000002.  This is NOT a
 * per-module reset mask: it is a single trigger/status handshake for the
 * CAM block as a whole.  Bit by bit, from the MT6589 data sheet page 1971
 * (field table) and page 1972 (descriptions):
 *
 *   2  HW_RST      RW  0: No HW reset
 *                     1: Async HW reset.  Resets all CAM modules, except
 *                       for register-setting modules.
 *   1  SW_RST_ST   RO  Software reset status:
 *                       0: DMA is busy.
 *                       1: DMA is idle.  HW reset can be done.
 *   0  SW_RST_Trig WO  Triggers software reset.  Write 1 to trigger.
 *
 * Bits 31:3 are unnamed and undocumented in the data sheet, and the register
 * does not reset to zero, so there is no basis for treating them as
 * resettable.  Writing "all ones" here would additionally drive the
 * read-only SW_RST_ST bit and slam ~29 unknown bits.  Only the three
 * documented fields are defined below.
 */
#define CAM_SW_CTL_HW_RST			BIT(2)
#define CAM_SW_CTL_SW_RST_ST			BIT(1)
#define CAM_SW_CTL_SW_RST_TRIG		BIT(0)

struct mtk_cam {
	struct device *dev;
	void __iomem *regs;

	struct v4l2_subdev sd;
	struct mutex lock;

	bool streaming;
};

#define to_mtk_cam(sd) container_of(sd, struct mtk_cam, sd)

#endif /* _MTK_CAM_H */
