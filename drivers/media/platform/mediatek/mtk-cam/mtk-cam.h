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

/* CAM_CTL_START: write-1-to-start strobes for the two passes. */
#define CAM_CTL_START_CQ0B_START	BIT(6)
#define CAM_CTL_START_CQ0_START	BIT(5)
#define CAM_CTL_START_PASS2B_START	BIT(1)
#define CAM_CTL_START_PASS2_STARTC	BIT(0)

/*
 * The individual CAM_CTL_EN1 sub-module bits are NOT recorded here.
 *
 * The data sheet lists them (CPIPE stages, memory in, statistics, raw
 * capture, and roughly twenty more such as BB/BNR/G2G/GGM/LSC/OB), but the
 * bit positions could not be recovered reliably from the extracted text,
 * and there is no vendor driver to check them against.  Guessing them would
 * produce an ISP that enables the wrong blocks.
 *
 * TODO: fill these in from the data sheet page that carries the EN1 bit
 * table, or from a vendor ISP driver, before CAM_CTL_EN1_SET is used.
 */

/*
 * CAM_CTL_SW_CTL is the software reset control for the CAM sub-modules.
 * The data sheet describes the register but its per-module bit assignment
 * was not recoverable, so CAM_SW_CTL_ALL below is a placeholder that writes
 * every bit.  That is safe only if every bit is a reset; it must be
 * confirmed before use.
 */
#define CAM_SW_CTL_ALL			0xffffffffU

struct mtk_cam {
	struct device *dev;
	void __iomem *regs;

	struct v4l2_subdev sd;
	struct mutex lock;

	bool streaming;
};

#define to_mtk_cam(sd) container_of(sd, struct mtk_cam, sd)

#endif /* _MTK_CAM_H */