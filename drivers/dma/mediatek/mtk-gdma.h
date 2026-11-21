/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2026 MediaTek, Inc.
 *
 * Register map for the MT6589 AP_DMA "General DMA" (G_DMA) engines.
 *
 * The block is described in the MT6589 data sheet, chapter 18 ("AP DMA"):
 * section 18.4.1 covers the global control registers (p. 870-872) and
 * section 18.4.2 the GDMA registers (p. 887 onwards).  The layout below is
 * corroborated against the vendor driver
 * aquaris-5/mediatek/platform/mt6589/kernel/core/mt_dma.c:33-57, which
 * spells the very same offsets with its own DMA_BASE_CH()/DMA_xxx() macros.
 * Where the two disagree the data sheet wins; they do not, as far as
 * commit 1 looks.
 */

#ifndef _MTK_GDMA_H
#define _MTK_GDMA_H

#include <linux/bits.h>
#include <linux/io.h>

/* AP_DMA register window, data sheet p. 869 ("(+11000000h)"). */
#define MTK_GDMA_REG_BASE		0x11000000

/*
 * How many of the block's 18 engines are generic.  Data sheet p. 869: "There
 * are total 18 channels in DMA of MT6589" - the other sixteen belong to HIF,
 * IrDA, I2C and UART.  Only G_DMA0 and G_DMA1 are handed out to general
 * purpose callers; they are the two described in section 18.4.2 and they are
 * the only ones with their own interrupt line (SPI 57/58, data sheet
 * Table 7-1).  Matches mt_dma.c:26 (NR_GDMA_CHANNEL).
 */
#define MTK_GDMA_NR_CHANNELS		2

/*
 * Global control registers, data sheet p. 870-872 (section 18.4.1).  These
 * are the only "global" registers that exist; the list stops at 0x78 because
 * G_DMA0's own register block starts at 0x80.
 */
/* data sheet p.871 - per-engine interrupt flag, one bit per engine */
#define MTK_GDMA_GLOBAL_INT_FLAG	0x00
/* data sheet p.872 - bit1 HARD_RST, bit0 WARM_RST */
#define MTK_GDMA_GLOBAL_RST		0x04
/* data sheet p.872 - bit0 = G_DMA0, bit1 = G_DMA1, ... bit17 = UART2_RX */
#define MTK_GDMA_GLOBAL_RUNNING_STATUS	0x08
/* data sheet p.872 - bit0 = G_DMA0 running, bit1 = G_DMA1 running */
#define MTK_GDMA_GLOBAL_STATUS_GDMA0	BIT(0)
#define MTK_GDMA_GLOBAL_STATUS_GDMA1	BIT(1)
/* data sheet p.872 - AXI slow down control */
#define MTK_GDMA_GLOBAL_SLOW_DOWN	0x0c
/* data sheet p.872 - per-engine security enable */
#define MTK_GDMA_GLOBAL_SEC_EN		0x10
/* data sheet p.872 - global security enable, gates every global register */
#define MTK_GDMA_GLOBAL_GSEC_EN		0x14
/* data sheet p.872 - security violation debug, write-1-to-clear */
#define MTK_GDMA_GLOBAL_VIO_DBG1	0x18
#define MTK_GDMA_GLOBAL_VIO_DBG0	0x1c

/* data sheet p.872 */
#define MTK_GDMA_RST_HARD_RST		BIT(1)
#define MTK_GDMA_RST_WARM_RST		BIT(0)

/*
 * Per-engine registers.  Data sheet p. 887 gives the G_DMA0 block at
 * 0x11000080 and states "For GDMA_1 engine, it has exactly the same
 * registers with offset 0x80 to G_DMA_0", i.e. the channel stride is 0x80.
 * That agrees with mt_dma.c:36, DMA_BASE_CH(n) = AP_DMA_BASE + 0x80 * (n + 1).
 *
 * Note the vendor file's block names (EN/START) and the data sheet's (EN) are
 * the same register; mt_dma.c calls DMA_START what the data sheet calls
 * AP_DMA_G_DMA_n_EN.  Use the data sheet spelling here.
 */
/* data sheet p.887 */
#define MTK_GDMA_INT_FLAG		0x00
/* data sheet p.887 - bit0 INTEN gates delivery, the flag still sets without it */
#define MTK_GDMA_INT_EN			0x04
/* data sheet p.887 - bit0 EN; set to 1 to start, hardware clears when done */
#define MTK_GDMA_EN			0x08
/* data sheet p.888 - bit1 HARD_RST, bit0 WARM_RST */
#define MTK_GDMA_RST			0x0c
/* data sheet p.888 - bit0 PAUSE */
#define MTK_GDMA_STOP			0x10
/* data sheet p.887 */
#define MTK_GDMA_FLUSH			0x14
/* data sheet p.887, bitfields p. 890-891 */
#define MTK_GDMA_CON			0x18
/* data sheet p.892 */
#define MTK_GDMA_SRC			0x1c
#define MTK_GDMA_DST			0x20
/* data sheet p.893 - both are 20 bit, hence the 1 MiB ceiling below */
#define MTK_GDMA_LEN1			0x24
#define MTK_GDMA_LEN2			0x28
/* data sheet p.893 - only takes effect when CON.WRAP_EN is set */
#define MTK_GDMA_JUMP_ADDR		0x2c
/* data sheet p.894 - read-only, bytes sitting in the internal buffer */
#define MTK_GDMA_INT_BUF_SIZE		0x30
/* data sheet p.894 - request/ack wiring for the peripheral-facing engines */
#define MTK_GDMA_CONNECT		0x34
/* data sheet p.894 - AWCACHE/ARCACHE plus the coherent-bus USER bits */
#define MTK_GDMA_AXIATTR		0x38
/* data sheet p.894 */
#define MTK_GDMA_DEBUG_STATUS		0x50

#define MTK_GDMA_INT_FLAG_BIT		BIT(0)
#define MTK_GDMA_INT_EN_BIT		BIT(0)
#define MTK_GDMA_EN_BIT			BIT(0)
#define MTK_GDMA_FLUSH_BIT		BIT(0)
#define MTK_GDMA_PAUSE_BIT		BIT(0)
#define MTK_GDMA_FLAG			BIT(0)

/*
 * CON bitfields, data sheet p. 890-891.  mt_dma.c:69-85 encodes the same
 * values (DMA_CON_RSIZE_2BYTE = 0x10000000 etc.) but only exercises the
 * subset that mtk-cqdma-style memory-to-memory transfers need.
 */
#define MTK_GDMA_CON_RSIZE_MASK		GENMASK(29, 28)
#define MTK_GDMA_CON_RSIZE_1BYTE	0x00000000
#define MTK_GDMA_CON_RSIZE_2BYTE	0x10000000
#define MTK_GDMA_CON_RSIZE_4BYTE	0x20000000
#define MTK_GDMA_CON_WSIZE_MASK		GENMASK(25, 24)
#define MTK_GDMA_CON_WSIZE_1BYTE	0x00000000
#define MTK_GDMA_CON_WSIZE_2BYTE	0x01000000
#define MTK_GDMA_CON_WSIZE_4BYTE	0x02000000
/* 0: wrap the source pointer, 1: wrap the destination pointer */
#define MTK_GDMA_CON_WRAP_SEL		BIT(20)
/* 0 is a 1-beat burst, the data sheet calls 3 "the best case" */
#define MTK_GDMA_CON_BURST_LEN_MASK	GENMASK(18, 16)
#define MTK_GDMA_CON_BURST_LEN_4	0x00030000
#define MTK_GDMA_CON_WRAP_EN		BIT(15)
/* insert up to 1023 idle cycles; read side only, throttles throughput */
#define MTK_GDMA_CON_SLOW_CNT_MASK	GENMASK(14, 5)
#define MTK_GDMA_CON_RADDR_FIX_EN	BIT(4)
#define MTK_GDMA_CON_WADDR_FIX_EN	BIT(3)
#define MTK_GDMA_CON_SLOW_EN		BIT(2)
/* repeat inserting SRC as a fixed pattern; outranks the two FIX_EN bits */
#define MTK_GDMA_CON_FIX_EN		BIT(1)
#define MTK_GDMA_CON_DIR		BIT(0)

/*
 * AXI attribute bits, data sheet p. 894.  mt_dma.c:93-94 uses these to reach
 * SYSRAM through the coherent bus.
 */
#define MTK_GDMA_AXIATTR_WUSER		BIT(20)
#define MTK_GDMA_AXIATTR_RUSER		BIT(4)

/*
 * Data sheet p. 893: LEN1 and LEN2 are 19:0, so a single length cannot exceed
 * 0xFFFFF bytes.  mt_dma.c:68-69 spells the same limit (MAX_TRANSFER_LEN1).
 * How the vendor splits a transfer larger than that across the two length
 * registers is *not* documented in the data sheet and is not implemented
 * here - see the commit message.
 */
#define MTK_GDMA_LEN_MASK		GENMASK(19, 0)
#define MTK_GDMA_MAX_LEN			0xfffff

#endif /* _MTK_GDMA_H */
