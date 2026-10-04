// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * G2D - 2D blitter in the MediaTek display subsystem.
 *
 * Register map, bit fields and the programming sequence all come from the
 * MT6589 data sheet, section 53 ("G2D").  What is implemented here is the
 * part that can be expressed without guessing: a bitblt operation between
 * source and destination surfaces, plus a constant-colour fill.
 *
 * Two details from the data sheet are easy to get wrong and are called out
 * where they are used:
 *
 *  - The pitch registers are in BYTES, not pixels, and the pitch divided by
 *    the format's bytes-per-pixel must be at least the ROI width.  This is
 *    the opposite of the OVL and RDMA pitch registers, which count pixels.
 *
 *  - The engine resets itself once it has fired, so no explicit reset is
 *    needed between operations.  G2D_START bit 0 wants a 0 written before
 *    the 1 that triggers the operation.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include "mtk-g2d.h"

/*
 * Control block - documented 16 bits wide in the data sheet's address map.
 *
 * These five registers are nonetheless read and written as 32-bit values on
 * purpose.  The vendor HAL for this same block does exactly that: ddp_reg.h
 * defines DISP_REG_GET as a "volatile unsigned int" load and DISP_REG_SET as
 * mt65xx_reg_sync_writel(), which sync_write.h expands to writel(), and
 * ddp_drv.c reads G2D_STATUS and G2D_IRQ - both documented 16 bits here -
 * only through those 32-bit macros.  MediaTek's APB registers are commonly
 * 32-bit-accessible even when only the low 16 bits are defined, so the
 * documented width is a field width, not an access constraint.  The register
 * offsets and the bit assignments agree with the data sheet; only the
 * documented width differs, and the vendor is the authority on how the
 * hardware is actually driven.
 */
#define G2D_START			0x00
#define G2D_MODE_CON			0x04
#define G2D_RESET			0x08
#define G2D_STATUS			0x0c
#define G2D_IRQ				0x10

/* Data path - 32 bit wide. */
#define G2D_ALP_CON			0x18
#define G2D_W2M_CON			0x40
#define G2D_W2M_ADDR			0x44
#define G2D_W2M_PITCH			0x48
#define G2D_W2M_SIZE			0x50
#define G2D_DST_CON			0x80
#define G2D_DST_ADDR			0x84
#define G2D_DST_PITCH			0x88
#define G2D_DST_COLOR			0x94
#define G2D_SRC_CON			0xc0
#define G2D_SRC_ADDR			0xc4
#define G2D_SRC_PITCH			0xc8
#define G2D_SRC_COLOR			0xd4
/*
 * W2M_SIZE carries the destination scan window: WIDTH in bits [27:16] and
 * HEIGHT in bits [11:0].  It is the only size field in the block.
 *
 * DI_MAT_0/DI_MAT_1 are the *dither* matrix, not a rectangle - they are
 * left at their reset values.  There is no ROI register; the source
 * rectangle is implied by the scan window and the source base address.
 */
#define G2D_DI_MAT_0			0xd8
#define G2D_DI_MAT_1			0xdc

#define G2D_W2M_SIZE_WIDTH		GENMASK(27, 16)
#define G2D_W2M_SIZE_HEIGHT		GENMASK(11, 0)

/* G2D_START */
#define G2D_START_START		BIT(0)

/* G2D_MODE_CON */
#define G2D_MODE_CON_ENG_MODE		BIT(0)
#define G2D_MODE_CON_ONE_PXL		BIT(1)

/* G2D_RESET */
#define G2D_RESET_APB_RESET		BIT(2)
#define G2D_RESET_HRST			BIT(1)
#define G2D_RESET_WRST			BIT(0)

/* G2D_STATUS - reads 0 once the engine is idle. */
#define G2D_STATUS_BUSY		BIT(0)

/*
 * G2D_IRQ mixes an interrupt status flag with the enable: IRQ_STA[8] is
 * write-0-to-clear and EN[0] is a plain read/write enable, and both live in
 * the same register.  Writing the whole register as 0 therefore clears the
 * pending interrupt *and* disables the interrupt source; since the line is
 * negative level sensitive, nothing would ever raise it again.
 */
#define G2D_IRQ_IRQ_STA		BIT(8)
#define G2D_IRQ_EN		BIT(0)

/*
 * *_CON format and attribute fields.
 *
 * FLIP, RB_SWP, BYTE_SWP and CLRFMT exist in all three of SRC_CON, DST_CON
 * and W2M_CON.  DI_ALP_MUL, DITHER_EN and COLOR_EN only exist in SRC_CON and
 * W2M_CON - DST_CON has no bit 9 at all - so COLOR_EN below is valid on
 * W2M_CON (which is what the constant-colour fill programs) but must never be
 * OR'd into a DST_CON value.
 */
#define G2D_CON_DI_ALP_MUL		BIT(13)
#define G2D_CON_DITHER_EN		BIT(12)
#define G2D_CON_FLIP			GENMASK(11, 10)
#define G2D_CON_COLOR_EN		BIT(9)
#define G2D_CON_RB_SWP			BIT(4)
#define G2D_CON_BYTE_SWP		BIT(3)
#define G2D_CON_CLRFMT			GENMASK(2, 0)

/* G2D_ALP_CON */
#define G2D_ALP_CON_MODE		GENMASK(1, 0)

/* The pitch registers hold 14 bits, but 0x2000 is the usable maximum. */
#define G2D_PITCH_MASK		GENMASK(13, 0)
#define G2D_PITCH_MAX		0x2000

/* W2M_SIZE WIDTH/HEIGHT are 12 bits with a documented range of 1..2048. */
#define G2D_MAX_WIDTH		2048
#define G2D_MAX_HEIGHT		2048

#define G2D_TIMEOUT_US			100000

/* Bounded poll budget for the warm-reset sequence's "while (G2D_STATUS)" loop. */
#define G2D_RESET_TIMEOUT_US		100000

struct mtk_g2d {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk_engine;
	struct clk *clk_smi;
	struct mutex lock;
	int irq;
};

/* Formats the CLRFMT field encodes. */
static const struct g2d_format_info g2d_formats[] = {
	[g2d_clrfmt_rgb565]		= {
		.clrfmt		= 0b001,
		.bytes_per_pixel	= 2,
		.address_align		= 2,
	},
	[g2d_clrfmt_pargb8888]		= {
		.clrfmt		= 0b101,
		.bytes_per_pixel	= 4,
		.address_align		= 4,
	},
	[g2d_clrfmt_argb8888]		= {
		.clrfmt		= 0b100,
		.bytes_per_pixel	= 4,
		.address_align		= 4,
	},
	[g2d_clrfmt_rgb888]		= {
		.clrfmt		= 0b011,
		.bytes_per_pixel	= 3,
		.address_align		= 1,
	},
	[g2d_clrfmt_xrgb8888]		= {
		.clrfmt		= 0b110,
		.bytes_per_pixel	= 4,
		.address_align		= 4,
	},
};

static int g2d_wait_idle(struct mtk_g2d *g2d)
{
	u32 status;

	/*
	 * readl_poll_timeout() does its own deadline arithmetic, so the
	 * microsecond budget needs no jiffies conversion here, and it sleeps
	 * between reads.  A hand-rolled "while (BUSY) cpu_relax()" loop spins
	 * on the APB bus without ever yielding, which is a scheduler-hostile
	 * way to sit on a mutex for a 100 ms budget.
	 */
	return readl_poll_timeout(g2d->regs + G2D_STATUS, status,
				  !(status & G2D_STATUS_BUSY), 20,
				  G2D_TIMEOUT_US);
}

/**
 * g2d_reset - drive the data sheet's warm-reset sequence.
 * @g2d: device
 *
 * G2D_RESET.WRST is the data sheet's "warm reset", and the register
 * description spells out the sequence verbatim, with the reason it must be
 * followed exactly:
 *
 *	G2D_START = 0;
 *	G2D_RESET = 1;
 *	while (G2D_STATUS != 0) { read G2D_STATUS; }
 *	G2D_RESET = 0;
 *
 * "Please follow the correct reset sequence to avoid potential bus hang
 * problem (breaking bus protocol)."  Skipping the STATUS poll, or asserting
 * WRST while START is still high, is precisely what breaks the bus protocol,
 * so the steps below are issued in that order and are not reordered.
 *
 * The data sheet's poll is unbounded; in the kernel it is bounded, so a
 * genuinely stuck engine cannot hang the caller.  The reset is de-asserted
 * either way: leaving WRST asserted would keep the engine permanently in
 * reset for every later operation.
 *
 * G2D_IRQ is deliberately left untouched.  The data sheet scopes the register
 * resets precisely: APB_RESET alone "resets G2D APB registers to initial
 * value", and HRST resets everything "except for APB registers" - so this
 * warm reset is not even documented to clear the APB-side IRQ status, and a
 * pending IRQ_STA may well survive it.  It must not be cleaned up here
 * regardless: IRQ_STA and EN share one register, so any write aimed at the
 * status that did not preserve EN would drop EN as well, and the line is
 * negative level sensitive, so the driver would never see another completion.
 * Leaving the register alone is safe in both cases - if a stale IRQ_STA
 * remains, the handler sees it, clears it and keeps EN, exactly as it does
 * for a genuine completion, and the engine is idle so nothing is missed.
 *
 * Must be called with @g2d->lock held; the caller is the only writer of the
 * control block, so this serialises against the next operation's programming.
 */
static void g2d_reset(struct mtk_g2d *g2d)
{
	u32 status;

	lockdep_assert_held(&g2d->lock);

	/* Step 1: G2D_START = 0. */
	writel(0, g2d->regs + G2D_START);

	/* Step 2: G2D_RESET = 1, i.e. WRST. */
	writel(G2D_RESET_WRST, g2d->regs + G2D_RESET);

	/* Step 3: while (G2D_STATUS != 0), bounded. */
	readl_poll_timeout(g2d->regs + G2D_STATUS, status,
			   !(status & G2D_STATUS_BUSY), 20,
			   G2D_RESET_TIMEOUT_US);

	if (status & G2D_STATUS_BUSY) {
		/*
		 * Out of reset, but the engine never went idle.  That is the
		 * one case recovery cannot fix on its own, so report it: the
		 * caller still gets -ETIMEDOUT, but a blit that will keep
		 * failing needs to be traceable rather than looking like an
		 * ordinary slow one.
		 */
		dev_err(g2d->dev,
			"G2D still busy after warm reset, engine may be wedged\n");
	}

	/* Step 4: G2D_RESET = 0, de-assert. */
	writel(0, g2d->regs + G2D_RESET);
}

/**
 * g2d_recover - put the engine back into a usable state after a timeout.
 * @g2d: device
 *
 * g2d_start() has already given the engine G2D_TIMEOUT_US to finish and it
 * has not, so G2D_STATUS.BUSY may still be 1.  Clearing the software busy
 * flag alone would leave that hardware state in place, and the next blit
 * would reprogram the engine and re-issue START while the previous operation
 * was possibly still running - which is how a single stall turns into
 * permanently corrupted output.  So reset the hardware before returning.
 *
 * Must be called with @g2d->lock held.
 */
static void g2d_recover(struct mtk_g2d *g2d)
{
	lockdep_assert_held(&g2d->lock);
	g2d_reset(g2d);
}

static irqreturn_t g2d_irq_handler(int irq, void *data)
{
	struct mtk_g2d *g2d = data;
	u32 irq_reg;

	/*
	 * The interrupt predicate is IRQ_STA, not STATUS.BUSY: by the time the
	 * engine raises the interrupt it has normally finished the operation
	 * and BUSY already reads 0, so gating on BUSY would make the handler
	 * claim the interrupt is not ours.
	 */
	irq_reg = readl(g2d->regs + G2D_IRQ);
	if (!(irq_reg & G2D_IRQ_IRQ_STA))
		return IRQ_NONE;

	/*
	 * Clear IRQ_STA without touching EN: the two fields share G2D_IRQ,
	 * and a write of 0 would leave EN at 0 and mute the line for good.
	 */
	writel(irq_reg & ~G2D_IRQ_IRQ_STA, g2d->regs + G2D_IRQ);

	return IRQ_HANDLED;
}

/**
 * g2d_start - fire one operation and wait for it to finish.
 * @g2d: device
 *
 * Writes G2D_START = 0 then 1, which is what the data sheet asks for, and
 * then waits for G2D_STATUS to read 0.
 *
 * The completion wait is a poll, not an interrupt wait: the interrupt only
 * retires the level-sensitive line, so nothing here depends on it and a late
 * or lost interrupt cannot turn into a hang.
 */
static int g2d_start(struct mtk_g2d *g2d)
{
	int ret;

	writel(0, g2d->regs + G2D_START);
	writel(G2D_START_START, g2d->regs + G2D_START);

	ret = g2d_wait_idle(g2d);
	if (ret) {
		/*
		 * The engine did not finish in time and BUSY may still be set.
		 * Recover the hardware *before* handing the timeout back, so
		 * the next caller does not reprogram a still-running engine and
		 * re-issue START on top of it.
		 */
		g2d_recover(g2d);
		return ret;
	}

	return 0;
}

static int g2d_check_fmt(u32 format)
{
	if (format >= ARRAY_SIZE(g2d_formats))
		return -EINVAL;
	if (!g2d_formats[format].bytes_per_pixel)
		return -EINVAL;

	return 0;
}

u32 mtk_g2d_max_pitch(void)
{
	return G2D_PITCH_MAX;
}

u32 mtk_g2d_max_width(void)
{
	return G2D_MAX_WIDTH;
}

u32 mtk_g2d_max_height(void)
{
	return G2D_MAX_HEIGHT;
}

/**
 * mtk_g2d_addr_align - required start-address alignment of one format.
 * @format: an &enum g2d_format
 * @align: alignment in bytes, returned to the caller
 *
 * The public form of g2d_check_align(): the same table, the same
 * g2d_check_fmt() test, so a caller that validates a request against this cannot
 * accept a format the engine would then reject - or the reverse, which would be
 * worse, since a format the engine does not have is one whose encoding the
 * caller has no way to predict.
 *
 * @align is only written on success.
 */
int mtk_g2d_addr_align(u32 format, u32 *align)
{
	int ret;

	ret = g2d_check_fmt(format);
	if (ret)
		return ret;

	*align = g2d_formats[format].address_align;

	return 0;
}

/**
 * g2d_check_rect - validate one surface before any register is programmed.
 * @pitch: pitch of that surface, in bytes
 * @bpp: bytes per pixel of that surface
 * @x: x origin of the rectangle on this surface, in pixels
 * @y: y origin of the rectangle on this surface, in pixels
 * @width: scan window width, in pixels
 * @height: scan window height, in pixels
 *
 * Everything rejected here is a hard error rather than something to clamp:
 * the pitch registers only hold 14 bits with 0x2000 as the usable maximum,
 * so an over-large pitch would be silently truncated to a different (and
 * possibly zero) pitch, and W2M_SIZE holds WIDTH and HEIGHT as 12 bits
 * documented as 1..2048.
 *
 * The rectangle must fit within a single row, origin included: (x + width) *
 * bpp must not exceed the pitch.  Checking only width * bpp <= pitch would
 * leave a rectangle that starts partway along a row free to run off the end of
 * it, and for the destination that is a write into memory the caller never
 * handed over.  The origin is baked into the address the engine is given, so
 * the bound has to be checked against it as well as against the pitch.
 *
 * @height is deliberately not bounded here.  The engine is only ever told a
 * starting address and a pitch, so it cannot walk more rows than the caller
 * actually owns; bounding the height is the caller's job, and
 * mtk_g2d_clip_rect() does it against the framebuffer's own height.
 */
static int g2d_check_rect(u32 pitch, u32 bpp, u32 x, u32 y,
			  u32 width, u32 height)
{
	if (!width || width > G2D_MAX_WIDTH)
		return -EINVAL;
	if (!height || height > G2D_MAX_HEIGHT)
		return -EINVAL;

	/*
	 * Pitch is in bytes: the entire rectangle, origin included, must fit in
	 * one row, and the pitch must be a whole number of pixels.  The sum is
	 * evaluated in u64 so a large x cannot wrap past the check on a
	 * configuration where dma_addr_t is 32 bits wide.
	 */
	if (pitch > G2D_PITCH_MAX)
		return -EINVAL;
	if ((u64)(x + width) * bpp > pitch || pitch % bpp)
		return -EINVAL;

	/*
	 * The origin is capped at the same scan-window bound the size is
	 * checked against, so that the whole request is describable in the
	 * register set without further reasoning.
	 */
	if (x > G2D_MAX_WIDTH || y > G2D_MAX_HEIGHT)
		return -EINVAL;

	return 0;
}

/**
 * g2d_check_align - validate the computed start address of one surface.
 * @addr: start address, already offset by x/y
 *
 * The data sheet requires 2-byte alignment for RGB565 and 4-byte alignment
 * for the 8888 formats; RGB888 output may start at any address.
 */
static int g2d_check_align(dma_addr_t addr,
			   const struct g2d_format_info *fmt)
{
	if (addr % fmt->address_align)
		return -EINVAL;

	return 0;
}

/**
 * g2d_check_offset - validate a pixel origin against one surface's pitch.
 * @base: surface base address, as handed in by the caller
 * @pitch: pitch of that surface, in bytes
 * @bpp: bytes per pixel of that surface
 * @x: x origin, in pixels
 * @y: y origin, in pixels
 * @addr: computed start address, returned to the caller
 *
 * x and y arrive from the caller as plain u32s, so "base + y * pitch +
 * x * bpp" is computed in 32-bit arithmetic and wraps silently on a
 * configuration where dma_addr_t is 32 bits wide (this one: LPAE and HIGHMEM
 * are off, so CONFIG_ARCH_DMA_ADDR_T_64BIT is not set).  A wrapped address
 * is exactly the sort of value that still passes the alignment test, and it
 * would send the engine to write somewhere else entirely.  So reject any
 * origin whose byte offset does not fit rather than programming a wrapped
 * one.
 *
 * The caller must already have run g2d_check_rect() on this surface, which
 * bounds pitch and keeps x * bpp well clear of any overflow.
 *
 * There is no hardware offset register in this block, so the origin is folded
 * into the base address here, in software, before it reaches the engine.  That
 * is also why the source and destination origins are independent of each
 * other: they are two separate base-address registers.
 */
static int g2d_check_offset(dma_addr_t base, u32 pitch, u32 bpp,
			     u32 x, u32 y, dma_addr_t *addr)
{
	u64 offset;

	offset = (u64)y * pitch + (u64)x * bpp;
	if (offset + base > (u64)(dma_addr_t)~0ULL)
		return -EINVAL;

	*addr = base + offset;

	return 0;
}

/**
 * g2d_program_blt - program one bitblt, with both origins already applied.
 * @g2d: device
 * @src_addr: source start address, already advanced past the source origin
 * @src_pitch: source pitch, in bytes
 * @src_fmt: source CLRFMT
 * @dst_addr: destination start address, already advanced past the destination
 *	origin
 * @dst_pitch: destination pitch, in bytes
 * @dst_fmt: destination CLRFMT
 * @width: scan window width, in pixels
 * @height: scan window height, in pixels
 *
 * This is the register-programming half of a bitblt, split out so that the
 * legacy one-origin form and the full two-origin form share exactly one
 * sequence.  Both entry points funnel through here, so the two cannot drift.
 *
 * Must be called with @g2d->lock held.
 */
static int g2d_program_blt(struct mtk_g2d *g2d,
			    dma_addr_t src_addr, u32 src_pitch,
			    enum g2d_format src_fmt,
			    dma_addr_t dst_addr, u32 dst_pitch,
			    enum g2d_format dst_fmt,
			    u32 width, u32 height)
{
	/*
	 * The address registers are 32 bits wide, which is also the width of
	 * dma_addr_t on this configuration: LPAE and HIGHMEM are both off, and
	 * the part tops out at 2 GB of LPDDR2.  So no truncation can occur.
	 * g2d_check_offset() has already refused anything that would not fit.
	 */
	writel((u32)src_addr, g2d->regs + G2D_SRC_ADDR);
	writel(src_pitch & G2D_PITCH_MASK, g2d->regs + G2D_SRC_PITCH);
	writel(g2d_formats[src_fmt].clrfmt, g2d->regs + G2D_SRC_CON);

	/*
	 * The write target is the W2M (write-to-memory) engine.  The data
	 * sheet's note on W2M_CON.DST_NEQ says that when the destination read
	 * buffer is the same as the write buffer - which is what a plain
	 * bitblt is - the bit is 0 and the driver then does not need to set
	 * G2D_DST_CON, G2D_DST_ADDR or G2D_DST_PITCH at all.  DST_NEQ is 0 out
	 * of reset, so only the W2M side is programmed here.
	 *
	 * Note that G2D_DST_* is the destination *read* port, used for
	 * read-modify-write blending, not an alternative write target: the only
	 * writable surface is W2M_ADDR.  That is why a blit programs two
	 * addresses here and why G2D_DST_ADDR cannot be used as a second
	 * destination origin.
	 */
	writel((u32)dst_addr, g2d->regs + G2D_W2M_ADDR);
	writel(dst_pitch & G2D_PITCH_MASK, g2d->regs + G2D_W2M_PITCH);
	writel(g2d_formats[dst_fmt].clrfmt, g2d->regs + G2D_W2M_CON);

	/*
	 * G2D_W2M_SIZE is the width and height of the destination scan window,
	 * and it is the only geometry register in the block: there is no
	 * G2D_SRC_SIZE, no ROI and no clip register.  Both ports therefore
	 * always move the same number of pixels, which is what makes one shared
	 * window unavoidable and is the one real limitation of this engine.
	 */
	writel((width << 16) | height, g2d->regs + G2D_W2M_SIZE);

	/* ENG_MODE 0 selects bitblt. */
	writel(0, g2d->regs + G2D_MODE_CON);

	return g2d_start(g2d);
}

/**
 * g2d_rects_overlap - test whether a blit would read and write the same bytes.
 * @src_bpp: bytes per pixel of the source
 * @src_x, @src_y: origin of the source rectangle within @src, in pixels
 * @dst_bpp: bytes per pixel of the destination
 * @dst_x, @dst_y: origin of the destination rectangle within @dst, in pixels
 * @width: rectangle width, in pixels
 * @height: rectangle height, in pixels
 *
 * Both rectangles are the same size, because G2D_W2M_SIZE sizes the single scan
 * window that both ports share.  A blit from a buffer onto itself is only
 * meaningful when the two regions are disjoint: the engine reads a row and
 * writes a row, and with overlapping regions the outcome depends on the order
 * it does that in, which nothing in this driver specifies or can observe.  So
 * an in-place blit is reported as an overlap and rejected, rather than
 * producing the usual undefined result.
 *
 * Two distinct base addresses cannot alias, whatever the origins - they are
 * two different allocations - so the arithmetic only runs for the same-surface
 * case.  There the comparison is done in bytes, because the two ports may have
 * different bytes-per-pixel, but only over the rows where the two rectangles
 * actually meet: the y ranges are compared first, and a row range that does not
 * intersect cannot overlap at all.  The pitch is deliberately not used - within
 * one row each rectangle is a contiguous run of pixels, so the byte offsets of
 * the two origins decide it.
 */
static bool g2d_rects_overlap(dma_addr_t src, u32 src_bpp,
			       u32 src_x, u32 src_y,
			       dma_addr_t dst, u32 dst_bpp,
			       u32 dst_x, u32 dst_y,
			       u32 width, u32 height)
{
	u32 y_start, y_end;
	u64 s, d;

	if (src != dst)
		return false;

	/* Rows the two rectangles have in common. */
	y_start = max(src_y, dst_y);
	y_end = min(src_y + height, dst_y + height);
	if (y_start >= y_end)
		return false;

	/*
	 * Within any shared row, the source span and the destination span are
	 * each a contiguous byte range, so one interval intersection decides
	 * every shared row at once.
	 */
	s = (u64)src_x * src_bpp;
	d = (u64)dst_x * dst_bpp;

	return min(s + (u64)width * src_bpp, d + (u64)width * dst_bpp) >
	       max(s, d);
}

/**
 * mtk_g2d_blt_rect - copy one rectangle between two surfaces at independent
 *			origins.
 * @g2d: device
 * @src: source surface base address
 * @src_pitch: source pitch, in bytes
 * @src_fmt: source CLRFMT
 * @src_x: x origin within the source, in pixels
 * @src_y: y origin within the source, in pixels
 * @dst: destination surface base address
 * @dst_pitch: destination pitch, in bytes
 * @dst_fmt: destination CLRFMT
 * @dst_x: x origin within the destination, in pixels
 * @dst_y: y origin within the destination, in pixels
 * @width: rectangle width, in pixels
 * @height: rectangle height, in pixels
 *
 * The source and destination origins are independent, so this expresses a true
 * move as well as a same-coordinate copy.
 *
 * What is shared is the *size*: G2D_W2M_SIZE describes the single scan window
 * that both ports read and write, so a source rectangle and a destination
 * rectangle can never differ in size.  The engine has no per-surface size
 * register to express one.
 *
 * The origins themselves are not hardware offsets - the block has no offset
 * register at all - they are folded into the two base addresses in software
 * here, which is why each surface's origin is validated against *that*
 * surface's pitch and alignment.
 *
 * @src and @dst may name the same surface only if the two rectangles do not
 * overlap; an overlapping in-place blit is rejected with -EINVAL rather than
 * producing the usual undefined result, because the engine reads and writes in
 * an order this driver cannot describe.
 *
 * Every argument is validated before any register is programmed, so a request
 * rejected with -EINVAL leaves the engine untouched.  Blocks until the engine
 * is idle; -ETIMEDOUT means it did not stop, after a warm reset.
 */
int mtk_g2d_blt_rect(struct mtk_g2d *g2d,
		      dma_addr_t src, u32 src_pitch, enum g2d_format src_fmt,
		      u32 src_x, u32 src_y,
		      dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		      u32 dst_x, u32 dst_y,
		      u32 width, u32 height)
{
	u32 src_bpp, dst_bpp;
	dma_addr_t src_addr, dst_addr;
	int ret;

	if (!g2d)
		return -EINVAL;

	ret = g2d_check_fmt(src_fmt);
	if (ret)
		return ret;
	ret = g2d_check_fmt(dst_fmt);
	if (ret)
		return ret;

	src_bpp = g2d_formats[src_fmt].bytes_per_pixel;
	dst_bpp = g2d_formats[dst_fmt].bytes_per_pixel;

	/*
	 * Validate both surfaces up front: the registers must not be touched at
	 * all unless the whole request is programmable.  Each surface is
	 * checked against its own origin, because the origins differ.
	 */
	ret = g2d_check_rect(src_pitch, src_bpp, src_x, src_y, width, height);
	if (ret)
		return ret;
	ret = g2d_check_rect(dst_pitch, dst_bpp, dst_x, dst_y, width, height);
	if (ret)
		return ret;

	ret = g2d_check_offset(src, src_pitch, src_bpp, src_x, src_y, &src_addr);
	if (ret)
		return ret;
	ret = g2d_check_offset(dst, dst_pitch, dst_bpp, dst_x, dst_y, &dst_addr);
	if (ret)
		return ret;

	/* Each origin must carry its own format's start-address alignment. */
	ret = g2d_check_align(src_addr, &g2d_formats[src_fmt]);
	if (ret)
		return ret;
	ret = g2d_check_align(dst_addr, &g2d_formats[dst_fmt]);
	if (ret)
		return ret;

	if (g2d_rects_overlap(src, src_bpp, src_x, src_y,
			      dst, dst_bpp, dst_x, dst_y,
			      width, height))
		return -EINVAL;

	mutex_lock(&g2d->lock);

	ret = g2d_program_blt(g2d, src_addr, src_pitch, src_fmt,
			      dst_addr, dst_pitch, dst_fmt, width, height);

	mutex_unlock(&g2d->lock);

	return ret;
}

/**
 * mtk_g2d_blt - copy one rectangular region between two surfaces at a shared
 *		 origin.
 * @x: x offset, in pixels, applied to both the source and the destination
 * @y: y offset, in pixels, applied to both the source and the destination
 *
 * The legacy one-origin form, kept because it is what a same-coordinate copy
 * means and it is the smaller call.  It is exactly
 * mtk_g2d_blt_rect() with @src_x == @dst_x == @x and @src_y == @dst_y == @y;
 * it shares that function's programming sequence verbatim.
 */
int mtk_g2d_blt(struct mtk_g2d *g2d,
		dma_addr_t src, u32 src_pitch, enum g2d_format src_fmt,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height)
{
	if (!g2d)
		return -EINVAL;

	return mtk_g2d_blt_rect(g2d,
				src, src_pitch, src_fmt, x, y,
				dst, dst_pitch, dst_fmt, x, y,
				width, height);
}

int mtk_g2d_fill(struct mtk_g2d *g2d,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height, u32 color)
{
	u32 con;
	u32 bpp;
	dma_addr_t dst_addr;
	int ret;

	if (!g2d)
		return -EINVAL;

	ret = g2d_check_fmt(dst_fmt);
	if (ret)
		return ret;

	bpp = g2d_formats[dst_fmt].bytes_per_pixel;
	ret = g2d_check_rect(dst_pitch, bpp, x, y, width, height);
	if (ret)
		return ret;

	ret = g2d_check_offset(dst, dst_pitch, bpp, x, y, &dst_addr);
	if (ret)
		return ret;
	ret = g2d_check_align(dst_addr, &g2d_formats[dst_fmt]);
	if (ret)
		return ret;

	mutex_lock(&g2d->lock);

	/*
	 * Same shape as the bitblt destination: the write target is the W2M
	 * engine, and COLOR_EN (bit 9, named DST_COLOR_EN on both control
	 * registers) selects the constant colour instead of a buffer.
	 */
	writel((u32)dst_addr, g2d->regs + G2D_W2M_ADDR);
	writel(dst_pitch & G2D_PITCH_MASK, g2d->regs + G2D_W2M_PITCH);

	con = g2d_formats[dst_fmt].clrfmt | G2D_CON_COLOR_EN;
	writel(con, g2d->regs + G2D_W2M_CON);

	/* No source surface: the colour comes from W2M's constant colour. */
	writel(color, g2d->regs + G2D_DST_COLOR);

	writel((width << 16) | height, g2d->regs + G2D_W2M_SIZE);

	writel(0, g2d->regs + G2D_MODE_CON);

	ret = g2d_start(g2d);

	mutex_unlock(&g2d->lock);

	return ret;
}

static int mtk_g2d_probe(struct platform_device *pdev)
{
	struct mtk_g2d *g2d;
	struct device *dev = &pdev->dev;
	int irq, ret;

	g2d = devm_kzalloc(dev, sizeof(*g2d), GFP_KERNEL);
	if (!g2d)
		return -ENOMEM;
	g2d->dev = dev;

	g2d->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(g2d->regs))
		return PTR_ERR(g2d->regs);

	g2d->clk_engine = devm_clk_get(dev, "g2d-engine");
	if (IS_ERR(g2d->clk_engine))
		return dev_err_probe(dev, PTR_ERR(g2d->clk_engine),
				     "failed to get engine clock\n");

	g2d->clk_smi = devm_clk_get(dev, "g2d-smi");
	if (IS_ERR(g2d->clk_smi))
		return dev_err_probe(dev, PTR_ERR(g2d->clk_smi),
				     "failed to get smi clock\n");

	ret = clk_prepare_enable(g2d->clk_engine);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable engine clock\n");

	ret = clk_prepare_enable(g2d->clk_smi);
	if (ret) {
		clk_disable_unprepare(g2d->clk_engine);
		return dev_err_probe(dev, ret, "failed to enable smi clock\n");
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = dev_err_probe(dev, irq, "failed to get irq\n");
		goto disable_clocks;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL, g2d_irq_handler,
					IRQF_ONESHOT, dev_name(dev), g2d);
	if (ret) {
		ret = dev_err_probe(dev, ret, "failed to request irq\n");
		goto disable_clocks;
	}
	g2d->irq = irq;

	mutex_init(&g2d->lock);

	/* G2D_IRQ EN, bit 0: enables the 2D engine interrupt. */
	writel(G2D_IRQ_EN, g2d->regs + G2D_IRQ);

	platform_set_drvdata(pdev, g2d);

	return 0;

disable_clocks:
	/*
	 * The clocks were taken with clk_prepare_enable() above, not with
	 * devm_clk_*_enable(), so unwinding is explicit: every path that leaves
	 * probe after a clock was enabled must come through here.  Order is the
	 * reverse of acquisition - smi first, then engine - and each clock is
	 * disabled exactly once, on the one path that took it.
	 */
	clk_disable_unprepare(g2d->clk_smi);
	clk_disable_unprepare(g2d->clk_engine);

	return ret;
}

/**
 * mtk_g2d_remove - give back what probe took.
 *
 * The clocks were enabled with clk_prepare_enable(), which devm does not
 * undo, so without this the engine and SMI clock gates stay enabled for the
 * rest of the boot after the device is unbound.  devm frees the register
 * mapping, the IRQ and the allocation after this returns, so the clocks must
 * go first and in reverse acquisition order.  Mutex destruction is not
 * needed - the memory is about to be freed.
 */
static void mtk_g2d_remove(struct platform_device *pdev)
{
	struct mtk_g2d *g2d = dev_get_drvdata(&pdev->dev);

	clk_disable_unprepare(g2d->clk_smi);
	clk_disable_unprepare(g2d->clk_engine);
}

#ifdef CONFIG_PM_SLEEP
static int mtk_g2d_suspend(struct platform_device *pdev,
			   pm_message_t state)
{
	struct mtk_g2d *g2d = dev_get_drvdata(&pdev->dev);

	/*
	 * The line is negative level sensitive, so leaving the IRQ enabled
	 * across a suspend means a completion that lands while the clocks are
	 * off calls the handler against register reads that no longer have a
	 * clock behind them.  Mask it first; unmask on resume.
	 */
	disable_irq(g2d->irq);

	clk_disable_unprepare(g2d->clk_smi);
	clk_disable_unprepare(g2d->clk_engine);

	return 0;
}

static int mtk_g2d_resume(struct platform_device *pdev)
{
	struct mtk_g2d *g2d = dev_get_drvdata(&pdev->dev);
	int ret;

	ret = clk_prepare_enable(g2d->clk_engine);
	if (ret)
		return ret;

	ret = clk_prepare_enable(g2d->clk_smi);
	if (ret) {
		clk_disable_unprepare(g2d->clk_engine);
		return ret;
	}

	enable_irq(g2d->irq);

	return 0;
}
#endif

static const struct of_device_id mtk_g2d_of_match[] = {
	{ .compatible = "mediatek,mt6589-g2d" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_g2d_of_match);

/**
 * mtk_g2d_get - find the G2D device belonging to a DRM device.
 * @dev: the mediatek-drm device
 *
 * Returns a &struct mtk_g2d with a reference held, or NULL if this DRM device
 * has no blitter.  NULL is a normal answer, not an error: the MDP DRM device
 * has no G2D, and callers must fall back rather than fail when there is none.
 *
 * The reference is what makes the returned pointer safe to use.  A bare
 * dev_get_drvdata() on a device found by node would be a use-after-free the
 * moment the platform device unbound, and unbinding is not gated on the DRM
 * device going away.
 */
struct mtk_g2d *mtk_g2d_get(struct device *dev)
{
	struct device_node *node;
	struct mtk_g2d *g2d = NULL;
	struct device *pdev;

	if (!dev->parent || !dev->parent->of_node || !dev->parent->of_node->parent)
		return NULL;

	/*
	 * G2D is a sibling of the DRM device under MMSYS, not a child of it,
	 * so this walks MMSYS - the same enumeration mtk_drm_probe() does when
	 * it looks for the DISP blocks.  Being under MMSYS is not enough on
	 * its own: an MDP DRM device sits under the same MMSYS and has no G2D,
	 * which is why this is a lookup and not a shared global.
	 *
	 * The node may be absent, disabled, unbound or bound; each of those is
	 * "no blitter" rather than an error, because none of them is something
	 * the caller can act on.
	 */
	for_each_child_of_node(dev->parent->of_node->parent, node) {
		if (!of_match_node(mtk_g2d_of_match, node))
			continue;
		if (!of_device_is_available(node))
			break;

		/*
		 * The node existing says nothing about the driver: probe can
		 * still fail, or not have run yet.  Only a bound device can be
		 * programmed, so an unbound one is reported as absent rather
		 * than dereferenced.
		 *
		 * bus_find_device_by_of_node() returns a device with a reference
		 * taken, and it is kept - not put here - because that reference
		 * is what keeps both the platform device and its drvdata alive
		 * for as long as the caller holds the &struct mtk_g2d.  The
		 * matching put_device() is in mtk_g2d_put().  Note that
		 * for_each_child_of_node() itself refcounts @node and releases
		 * its own reference on the next iteration, so @node needs no
		 * of_node_put() here and must not get one: doing so would drop
		 * the reference the iteration is holding for us.
		 */
		pdev = bus_find_device_by_of_node(&platform_bus_type, node);
		if (!pdev)
			break;

		g2d = dev_get_drvdata(pdev);
		break;
	}

	return g2d;
}

void mtk_g2d_put(struct mtk_g2d *g2d)
{
	if (!g2d)
		return;

	/*
	 * mtk_g2d_get() kept the reference bus_find_device_by_of_node() took,
	 * so this is put_device() and not anything G2D-specific: it is what
	 * finally allows the platform device to be unbound and its drvdata
	 * freed.  Calling it more than once per get() would be an over-release,
	 * which is why the contract is one put per get().
	 */
	put_device(g2d->dev);
}

struct device *mtk_g2d_device(struct mtk_g2d *g2d)
{
	return g2d ? g2d->dev : NULL;
}

static struct platform_driver mtk_g2d_driver = {
	.probe		= mtk_g2d_probe,
	.remove		= mtk_g2d_remove,
	.driver		= {
		.name		= "mtk-g2d",
		.of_match_table	= mtk_g2d_of_match,
	},
#ifdef CONFIG_PM_SLEEP
	.suspend	= mtk_g2d_suspend,
	.resume		= mtk_g2d_resume,
#endif
};
module_platform_driver(mtk_g2d_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6589 G2D driver");
MODULE_LICENSE("GPL");
