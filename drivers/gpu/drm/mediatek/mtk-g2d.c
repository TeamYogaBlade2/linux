// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * G2D - 2D blitter in the MediaTek display subsystem.
 *
 * Register map, bit fields and the programming sequence all come from the
 * MT6589 data sheet, chapter 53 ("2D Acceleration", p. 1914).  What is
 * implemented here is the part that can be expressed without guessing: a
 * bitblt operation between source and destination surfaces, plus a
 * constant-colour fill.
 *
 * Two details from the data sheet are easy to get wrong and are called out
 * where they are used:
 *
 *  - The pitch registers are in BYTES, not pixels, and the pitch divided by
 *    the format's bytes-per-pixel must be at least the ROI width (p. 1923).
 *
 *  - The engine resets itself once it has fired, so no explicit reset is
 *    needed between operations.  G2D_START bit 0 wants a 0 written before
 *    the 1 that triggers the operation (p. 1915).
 */

#include <linux/clk.h>
#include <linux/completion.h>
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
 * Control block - documented 16 bits wide in the data sheet's address map
 * (p. 1914) but nonetheless read and written as 32-bit values on purpose.
 * The vendor HAL does exactly that: ddp_reg.h defines DISP_REG_GET as a
 * "volatile unsigned int" load and DISP_REG_SET as mt65xx_reg_sync_writel(),
 * which sync_write.h expands to writel(), and ddp_drv.c reads G2D_STATUS and
 * G2D_IRQ - both documented 16 bits here - only through those 32-bit macros.
 * MediaTek's APB registers are commonly 32-bit-accessible even when only the
 * low 16 bits are defined, so the documented width is a field width, not an
 * access constraint.
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
 * HEIGHT in bits [11:0], both 12-bit unsigned, range 1..2048 (p. 1920).
 *
 * DI_MAT_0/DI_MAT_1 are the *dither* matrix, not a rectangle - they are
 * left at their reset values (0x26374051).  There is no ROI register; the
 * source rectangle is implied by the scan window and the source base address.
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

#define G2D_MAX_WIDTH		2048
#define G2D_MAX_HEIGHT		2048

#define G2D_TIMEOUT_US			100000

/* Bounded poll budget for the warm-reset sequence's "while (G2D_STATUS)" loop. */
#define G2D_RESET_TIMEOUT_US		100000

/*
 * How many times g2d_recover() will warm-reset and re-poll G2D_STATUS looking
 * for the engine to go idle.  See g2d_wedge() for why the answer to "it is
 * still busy" has to be "shut the block down" rather than "report the timeout
 * and let the buffers go".
 */
#define G2D_RECOVER_TRIES		3

/*
 * How long g2d_start() sleeps waiting for the completion interrupt before it
 * stops waiting and relies on the STATUS poll alone.  It is not a timeout for
 * the operation: g2d_wait_idle() has its own bounded budget and its own
 * recovery, and this only bounds how long the driver sits on the completion
 * before consulting the authoritative register.
 */
#define G2D_IRQ_WAIT_MS			100

struct mtk_g2d {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk_engine;
	struct clk *clk_smi;
	struct mutex lock;
	int irq;
	/*
	 * Set once the engine has been driven through every recovery this driver
	 * is willing to attempt and G2D_STATUS.BUSY still reads high.  The
	 * hardware may then still be reading or writing whatever address was last
	 * programmed, so no buffer may be released and no new operation may be
	 * submitted: both paths below refuse, and every entry point re-checks
	 * this before it touches a register.
	 */
	bool wedged;
	/*
	 * Completion for one operation: the interrupt handler sets this, and
	 * g2d_start() waits on it.  It is a wakeup, not an authority - see
	 * g2d_irq_handler() - so the waiter re-reads G2D_STATUS afterwards
	 * before it may release anything.
	 */
	struct completion done;
	/*
	 * The handle returned by mtk_g2d_register_drm() for this engine, or
	 * NULL if registration failed and the driver is running without a
	 * node.  It is an opaque pointer rather than a &drm_device because
	 * this file deliberately includes no DRM header; mtk-g2d-uapi.c is
	 * the only object that knows what it points at, and the only one that
	 * dereferences it.
	 */
	void *drm;
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
	 * microsecond budget needs no conversion, and it sleeps between reads.
	 */
	return readl_poll_timeout(g2d->regs + G2D_STATUS, status,
				  !(status & G2D_STATUS_BUSY), 20,
				  G2D_TIMEOUT_US);
}

/**
 * g2d_wedge - declare the engine unrecoverable and shut it out.
 * @g2d: device
 *
 * Called when G2D_STATUS.BUSY still reads high after every recovery attempt.
 * From here the driver cannot promise anything about what the block is doing:
 * the last programmed G2D_SRC_ADDR / G2D_W2M_ADDR may still be the addresses
 * the hardware is moving data to, indefinitely, with no register write this
 * driver can make that is documented to stop it.
 *
 * That is what makes returning -ETIMEDOUT here and releasing the buffers
 * unsafe rather than merely untidy: the caller would be entitled to unmap and
 * reuse that memory, and the engine would keep writing into it.  So the driver
 * does not release them.  Every later entry point sees @wedged and returns
 * before programming a register or waiting on the hardware, which makes the
 * block inert from the driver's point of view; the cost is that G2D stops
 * working until the device is unbound, and that is the right trade for an
 * accelerator that can no longer guarantee it has stopped.
 *
 * Must be called with @g2d->lock held.
 */
static void g2d_wedge(struct mtk_g2d *g2d)
{
	lockdep_assert_held(&g2d->lock);

	if (g2d->wedged)
		return;

	g2d->wedged = true;

	/*
	 * One line, once, not rate limited: this is a state change userspace
	 * observes through the errno it gets from now on, so it must not be
	 * swallowed by an earlier identical message.
	 */
	dev_err(g2d->dev,
		"G2D did not go idle after %u warm resets, wedging the engine: all further G2D operations fail\n",
		G2D_RECOVER_TRIES);
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
 * problem (breaking bus protocol)." (p. 1916)  Skipping the STATUS poll, or
 * asserting WRST while START is still high, is precisely what breaks the bus
 * protocol, so the steps below are issued in that order and are not reordered.
 *
 * The data sheet's poll is unbounded; in the kernel it is bounded, so a
 * genuinely stuck engine cannot hang the caller.  A bound that expires is
 * reported rather than swallowed, because "out of reset but still busy" is
 * the one state this driver cannot recover from - see g2d_recover().
 *
 * G2D_IRQ is deliberately left untouched.  The data sheet scopes the register
 * resets precisely: APB_RESET alone "resets G2D APB registers to initial
 * value", and HRST resets everything "except for APB registers" - so this
 * warm reset is not even documented to clear the APB-side IRQ status, and a
 * pending IRQ_STA may well survive it.  It must not be cleaned up here
 * regardless: IRQ_STA and EN share one register (p. 1917), so any write aimed
 * at the status that did not preserve EN would drop EN as well, and the line
 * is negative level sensitive, so the driver would never see another
 * completion.  If a stale IRQ_STA does remain, the handler sees it, clears it
 * and keeps EN, exactly as it does for a genuine completion.
 *
 * Must be called with @g2d->lock held; the caller is the only writer of the
 * control block, so this serialises against the next operation's programming.
 */
static int g2d_reset(struct mtk_g2d *g2d)
{
	u32 status;

	lockdep_assert_held(&g2d->lock);

	/* Step 1: G2D_START = 0. */
	writel(0, g2d->regs + G2D_START);

	/* Step 2: G2D_RESET = 1, i.e. WRST. */
	writel(G2D_RESET_WRST, g2d->regs + G2D_RESET);

	/* Step 3: while (G2D_STATUS != 0), bounded. */
	status = readl_poll_timeout(g2d->regs + G2D_STATUS, status,
				   !(status & G2D_STATUS_BUSY), 20,
				   G2D_RESET_TIMEOUT_US);

	/* Step 4: G2D_RESET = 0, de-assert. */
	writel(0, g2d->regs + G2D_RESET);

	/*
	 * -ETIMEDOUT, not -EIO: the engine did not stop, which is the same
	 * condition g2d_wait_idle() reports and the same one the caller already
	 * has a documented meaning for.  Note that the reset is de-asserted
	 * either way - leaving WRST asserted would brick the block for every
	 * later operation - so a failure here does not stop the retries in
	 * g2d_recover() from being attempted.
	 */
	return status ? -ETIMEDOUT : 0;
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
 * permanently corrupted output.
 *
 * One warm reset is not treated as sufficient.  The reset sequence's own poll
 * is bounded here, so an engine that needs longer than that budget to settle
 * reads as still busy on the first attempt; repeating the sequence gives it
 * several such budgets rather than one.  When the retries are exhausted the
 * engine is wedged rather than released: the hardware may still be moving
 * data to the address last programmed, and there is no further register write
 * documented to stop it, so the only safe answer is to refuse every later
 * operation and never hand those buffers back.  See g2d_wedge().
 *
 * Must be called with @g2d->lock held.
 */
static void g2d_recover(struct mtk_g2d *g2d)
{
	unsigned int i;

	lockdep_assert_held(&g2d->lock);

	for (i = 0; i < G2D_RECOVER_TRIES; i++) {
		if (!g2d_reset(g2d))
			return;

		/*
		 * Rate limited: the retries are a known-bad state, and a caller
		 * looping on the ioctl would otherwise produce one line per
		 * attempt.  The wedge below is reported once and unratelimited.
		 */
		dev_warn_ratelimited(g2d->dev,
			"G2D still busy after warm reset %u/%u\n",
			i + 1, G2D_RECOVER_TRIES);
	}

	g2d_wedge(g2d);
}

/**
 * g2d_irq_handler - retire the G2D interrupt line.
 * @irq: interrupt number
 * @data: the &struct mtk_g2d
 *
 * This handler wakes the waiter and is NOT trusted as to whether the engine
 * has stopped.  G2D_START is followed by this interrupt and the wait is
 * wait_for_completion_timeout(), but G2D_STATUS decides the outcome:
 *
 *  - G2D_IRQ.IRQ_STA is documented as RW, "Write 0 to clear IRQ", and the
 *    same entry adds that for debugging "software can write 1 to force G2D
 *    to issue IRQ" (data sheet ch. 53, p. 1917).  So the bit is
 *    software-writable, and nothing in the data sheet says when the hardware
 *    sets it or guarantees it is set *only* when an operation ends.
 *  - There is no documented ordering between IRQ_STA and STATUS.BUSY, so the
 *    interrupt alone cannot be read as "the copy is finished".
 *  - The vendor HAL for this SoC is no help.  It registers MT_G2D_IRQ_ID
 *    and its handler logs "G2D done!" when IRQ_STA is set, but it never
 *    writes DISP_REG_G2D_START anywhere in the tree - the only G2D register
 *    it ever writes is G2D_IRQ, from inside that handler.  The vendor never
 *    runs the engine, so its interrupt path is no more proven than one written
 *    here.
 *
 * Getting this wrong is not a hang or a lost interrupt, it is worse: a stale
 * or software-forced IRQ_STA would be believed, the call would return
 * success, and the caller would release a destination the engine may still be
 * writing.  Polling STATUS.BUSY cannot have that failure mode - it can only
 * ever be too slow, never wrong about whether the engine stopped.
 *
 * So the split is: this wakes the waiter, and G2D_STATUS decides.  g2d_start()
 * wakes from wait_for_completion_timeout() and then still polls STATUS until
 * BUSY is clear.  A spurious or forced interrupt therefore costs a re-poll
 * and cannot return success early - which is the failure mode that would hand
 * a caller back a destination the engine is still writing.  Keeping STATUS as
 * the authority preserves the one property this driver is built on: it can
 * only ever be too slow about the engine stopping, never wrong about it.
 *
 * Returning IRQ_HANDLED regardless of the status bit is deliberate.  The line
 * is enabled by software and can be forced from software, so a level that
 * arrives with IRQ_STA already cleared is the same event arriving late, not
 * an interrupt for some other device.
 */
static irqreturn_t g2d_irq_handler(int irq, void *data)
{
	struct mtk_g2d *g2d = data;
	u32 irq_reg;

	/*
	 * Clear IRQ_STA without touching EN: the two fields share G2D_IRQ, and
	 * writing 0 through EN would mute the line for good.  Write the read
	 * value back with only IRQ_STA dropped, so an unnamed bit in the same
	 * word is never disturbed.
	 */
	irq_reg = readl(g2d->regs + G2D_IRQ);
	if (!(irq_reg & G2D_IRQ_IRQ_STA))
		return IRQ_NONE;

	/* Clear IRQ_STA without touching EN: the two fields share G2D_IRQ, and
	 * writing 0 through EN would mute the line for good.  Write the read
	 * value back with only IRQ_STA dropped, so an unnamed bit in the same
	 * word is never disturbed.
	 */
	writel(irq_reg & ~G2D_IRQ_IRQ_STA, g2d->regs + G2D_IRQ);

	complete(&g2d->done);

	return IRQ_HANDLED;
}

/**
 * g2d_start - fire one operation and wait for it to finish.
 * @g2d: device
 *
 * Completion is interrupt-driven, but the interrupt only wakes this waiter:
 * G2D_STATUS is still consulted before success is reported, for the reasons
 * in g2d_irq_handler() - IRQ_STA is software-writable and its ordering
 * against BUSY is undocumented, so believing it alone could hand a caller
 * back a destination the engine is still writing.
 *
 * The two-step keeps the property the rest of this driver is built on.  The
 * STATUS poll can only ever report "still running", never "finished" on false
 * evidence: being too slow is survivable, because the caller's memory is
 * still owned, whereas a premature completion is not.  A spurious or forced
 * interrupt therefore just costs a re-poll.
 *
 * The completion is re-armed before START is written, so an interrupt that
 * lands between the two cannot be missed, and so a leftover completion from a
 * previous operation cannot be taken for this one.
 */
static int g2d_start(struct mtk_g2d *g2d)
{
	int ret;

	/*
	 * Never re-program a wedged engine.  START is what makes it fetch and
	 * write the addresses in the registers, and those still describe the
	 * operation that never finished, so this is the single place where
	 * "submit" happens and therefore the single place the wedge has to be
	 * refused.  The caller has already released nothing yet, so returning
	 * here also means no buffer is handed back on this path.
	 */
	if (g2d->wedged)
		return -ETIMEDOUT;

		/*
		 * Re-arm before START, so neither a late interrupt from the previous
		 * operation nor one racing this write can be missed or misread.
		 */
	reinit_completion(&g2d->done);

	writel(0, g2d->regs + G2D_START);
	writel(G2D_START_START, g2d->regs + G2D_START);

	/* Wake on the interrupt.  Either way the STATUS poll below decides the
	 * outcome, so a missing or spurious interrupt changes nothing but how
	 * long this takes.
	 */
	wait_for_completion_timeout(&g2d->done,
				    msecs_to_jiffies(G2D_IRQ_WAIT_MS));

	ret = g2d_wait_idle(g2d);
	if (ret) {
		/* Recover the hardware *before* handing the timeout back, so
		 * the next caller does not reprogram a still-running engine
		 * and re-issue START on top of it.
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
 * g2d_check_fmt() test, so a caller validating a request against this cannot
 * disagree with the engine about which formats exist.
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
 * the pitch registers only hold 14 bits with 0x2000 as the usable maximum, so
 * an over-large pitch would be silently truncated to a different (and possibly
 * zero) pitch.
 *
 * The rectangle must fit within a single row, origin included: (x + width) *
 * bpp must not exceed the pitch.  Checking only width * bpp <= pitch would
 * leave a rectangle that starts partway along a row free to run off the end of
 * it, and for the destination that is a write into memory the caller never
 * handed over.
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

	/* The sum is widened before the add, not after: (u64)(x + width) would
	 * evaluate x + width in u32 first and wrap there, so a large x passes a
	 * check that exists to keep the rectangle inside the row.  Both are u32
	 * from the caller, so that is reachable.
	 */
	if (pitch > G2D_PITCH_MAX)
		return -EINVAL;
	if (((u64)x + width) * bpp > pitch || pitch % bpp)
		return -EINVAL;

	/* Cap the origin at the same scan-window bound the size is checked
	 * against, so the whole request is describable in the register set.
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
 * for the 8888 formats; RGB888 output may start at any address (p. 1918).
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
 * would send the engine to write somewhere else entirely.
 *
 * The caller must already have run g2d_check_rect() on this surface, which
 * bounds pitch and keeps x * bpp well clear of any overflow.
 *
 * There is no hardware offset register in this block, so the origin is folded
 * into the base address here.  That is also why the source and destination
 * origins are independent: they are two separate base-address registers.
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
	 * dma_addr_t on this configuration (LPAE and HIGHMEM are both off), and
	 * g2d_check_offset() has already refused anything that would not fit.
	 * dma_set_mask_and_coherent(DMA_BIT_MASK(32)) in probe states the same
	 * bound to the DMA API, so nothing allocated for this device can
	 * produce an address the register cannot hold.
	 *
	 * The addresses written here are IOVAs, and they belong to this engine.
	 * They come from a drm_gem_dma_object owned by G2D's own DRM device,
	 * whose dma_dev is this platform device (drm_dev_set_dma_dev() in
	 * mtk_g2d_register_drm()), so with the M4U port attached to this node
	 * each one is a range in the page table this engine reads through.
	 *
	 * That is precisely why the ioctls resolve the handle against G2D's
	 * device rather than the display device's: dma_obj->dma_addr is only
	 * meaningful to the engine that asked for the mapping.  When there was
	 * no IOMMU here the two spaces coincided, which is why borrowing an
	 * address used to look correct.  See the G2D node comment in
	 * mt6589.dtsi.
	 */
	writel((u32)src_addr, g2d->regs + G2D_SRC_ADDR);
	writel(src_pitch & G2D_PITCH_MASK, g2d->regs + G2D_SRC_PITCH);
	writel(g2d_formats[src_fmt].clrfmt, g2d->regs + G2D_SRC_CON);

	/*
	 * The write target is the W2M (write-to-memory) engine.  W2M_CON.DST_NEQ
	 * (p. 1918) says that when the destination read buffer is the same as
	 * the write buffer - which is what a plain bitblt is - the bit is 0 and
	 * G2D_DST_CON, G2D_DST_ADDR and G2D_DST_PITCH need not be set at all.
	 * DST_NEQ is 0 out of reset, so only the W2M side is programmed here.
	 *
	 * G2D_DST_* is the destination *read* port, for read-modify-write
	 * blending, not an alternative write target: the only writable surface
	 * is W2M_ADDR.
	 */
	writel((u32)dst_addr, g2d->regs + G2D_W2M_ADDR);
	writel(dst_pitch & G2D_PITCH_MASK, g2d->regs + G2D_W2M_PITCH);
	writel(g2d_formats[dst_fmt].clrfmt, g2d->regs + G2D_W2M_CON);

	/* The only geometry register in the block: there is no G2D_SRC_SIZE, no
	 * ROI and no clip register, so both ports always move the same number
	 * of pixels.
	 */
	writel((width << 16) | height, g2d->regs + G2D_W2M_SIZE);

	/* ENG_MODE 0 selects bitblt. */
	writel(0, g2d->regs + G2D_MODE_CON);

	return g2d_start(g2d);
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
 * move as well as a same-coordinate copy.  What is shared is the *size*:
 * G2D_W2M_SIZE describes the single scan window that both ports read and write,
 * so a source rectangle and a destination rectangle can never differ in size.
 *
 * @src and @dst must be two different surfaces.  A blit of one buffer onto
 * itself is refused outright, even when the rectangles are far apart, and this
 * is the interesting restriction in this function so it is worth saying why
 * there is no overlap test here.
 *
 * The engine reads a row and writes a row, and nothing in its programming
 * interface says in which order, so an overlapping in-place copy has no
 * defined result - that much only rules out the overlapping case.  Deciding
 * whether the rectangles overlap at all is the harder half, and the arguments
 * do not carry enough to answer it correctly: with different pitches, and with
 * the two origins at different y, row n of the source lives at
 * src_base + (src_y + n) * src_pitch + src_x * src_bpp while row n of the
 * destination lives at dst_base + (dst_y + n) * dst_pitch + dst_x * dst_bpp -
 * two different strides, so the byte ranges intersect only in some rows and
 * the rows on which they do are not the rows whose y ranges overlap.  A test
 * that compares the origins within a row - the only thing that can be computed
 * without walking every row - answers a question about a row that may not
 * intersect at all, and a false "disjoint" is a silently corrupted buffer
 * rather than an error.
 *
 * So v1 requires two distinct surfaces, and the callers upstream (the ioctl
 * layer) refuse a same-GEM-object request by comparing the handles, which is
 * the same rule stated in the one place that knows what "the same surface"
 * means.  Supporting in-place blits later needs a real per-row address walk;
 * that is a feature to add once it is correct, not a shortcut to take here.
 *
 * Every argument is validated before any register is programmed, so a request
 * rejected with -EINVAL leaves the engine untouched.  Blocks until the engine
 * is idle; -ETIMEDOUT means it did not stop, after which the engine is either
 * back in a known idle state or has been wedged and will refuse every further
 * operation.
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

	/* Both surfaces are validated up front, each against its own origin. */
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

	ret = g2d_check_align(src_addr, &g2d_formats[src_fmt]);
	if (ret)
		return ret;
	ret = g2d_check_align(dst_addr, &g2d_formats[dst_fmt]);
	if (ret)
		return ret;

	/* See the comment above: same-surface blits are not expressible safely
	 * here, so the rule is stated as "the caller already ensured two
	 * different surfaces" and this only guards the direct entry point.
	 */
	if (src == dst)
		return -EINVAL;

	mutex_lock(&g2d->lock);

	/*
	 * Re-check under the lock, before anything is programmed.  g2d_start()
	 * would refuse a wedged engine too, but by then g2d_program_blt() has
	 * already written the addresses into SRC_ADDR and W2M_ADDR - which are
	 * the very registers describing what the stuck engine may still be
	 * moving data to.  Refusing before the first write is what keeps a
	 * wedged block from being reprogrammed.
	 */
	if (g2d->wedged) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	ret = g2d_program_blt(g2d, src_addr, src_pitch, src_fmt,
			      dst_addr, dst_pitch, dst_fmt, width, height);

out_unlock:
	mutex_unlock(&g2d->lock);

	return ret;
}

/**
 * mtk_g2d_blt - copy one rectangular region between two surfaces at a shared
 *		 origin.
 * @x: x offset, in pixels, applied to both the source and the destination
 * @y: y offset, in pixels, applied to both the source and the destination
 *
 * Exactly mtk_g2d_blt_rect() with @src_x == @dst_x == @x and @src_y == @dst_y
 * == @y; it shares that function's programming sequence verbatim.
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

	/* As in mtk_g2d_blt_rect(): nothing is programmed on a wedged engine. */
	if (g2d->wedged) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	/* Same shape as the bitblt destination: COLOR_EN (bit 9, named
	 * DST_COLOR_EN) selects the constant colour instead of a buffer.
	 */
	writel((u32)dst_addr, g2d->regs + G2D_W2M_ADDR);
	writel(dst_pitch & G2D_PITCH_MASK, g2d->regs + G2D_W2M_PITCH);

	con = g2d_formats[dst_fmt].clrfmt | G2D_CON_COLOR_EN;
	writel(con, g2d->regs + G2D_W2M_CON);

	writel(color, g2d->regs + G2D_DST_COLOR);

	writel((width << 16) | height, g2d->regs + G2D_W2M_SIZE);

	writel(0, g2d->regs + G2D_MODE_CON);

	ret = g2d_start(g2d);

out_unlock:
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

	/*
	 * This engine does DMA of its own now, and the window is a real 32-bit
	 * one.  The data sheet defines G2D_W2M_ADDR, G2D_SRC_ADDR and
	 * G2D_DST_ADDR as full [31:0] registers (ch. 53, p. 1914), so the
	 * (u32) casts in g2d_program_blt() are not narrowing anything, and
	 * anything wider than 32 bits could not be programmed at all.  What is
	 * narrow besides that is the pitch - [13:0], maximum 0x2000 - and the
	 * scan window, 12 bits, maximum 2,048x2,048.
	 *
	 * dma_set_mask_and_coherent() states that to the DMA API, so a
	 * mapping allocated for this device cannot be handed back an address
	 * the register cannot hold.  With the M4U attached (iommus in the node
	 * comment), the binding constraint is the same 32 bits: the mapping
	 * this driver creates is 1ULL << 32 of IOVA, so an IOVA above 4G is not
	 * reachable by this engine whatever the physical address behind it is.
	 *
	 * Failure is fatal for the probe rather than warned about.  A device
	 * whose DMA mask is 64-bit here is a configuration in which this
	 * driver's addressing is wrong, and continuing would mean accepting
	 * buffers whose addresses get silently truncated by the (u32) casts.
	 */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set 32-bit DMA mask\n");

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
	init_completion(&g2d->done);

	/* G2D_IRQ EN, bit 0: enables the 2D engine interrupt. */
	writel(G2D_IRQ_EN, g2d->regs + G2D_IRQ);

	platform_set_drvdata(pdev, g2d);

	/*
	 * Published last, once the hardware is usable.  The DRM device must
	 * never exist while the engine could still be programmed by someone
	 * who cannot see that it is not ready: mtk_g2d_register_drm() makes
	 * the render node openable, and from that point a BLT or FILL can
	 * arrive.  Registering before the clocks, the interrupt and the
	 * wedge flag are all in place would open a window where a request is
	 * accepted and programmed into an engine that is not running.
	 *
	 * On failure the DRM device is dropped again (drm_dev_put() inside the
	 * callee) and the driver keeps probing as a bare engine, which is
	 * strictly better than a node that exists and fails every ioctl.
	 */
	g2d->drm = mtk_g2d_register_drm(dev, g2d);
	ret = IS_ERR(g2d->drm) ? PTR_ERR(g2d->drm) : 0;
	if (ret) {
		g2d->drm = NULL;
		ret = dev_err_probe(dev, ret, "failed to register DRM device\n");
		goto disable_clocks;
	}

	return 0;

disable_clocks:
	/* The clocks were taken with clk_prepare_enable() above, not with
	 * devm_clk_*_enable(), so unwinding is explicit: reverse of
	 * acquisition order - smi first, then engine.
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
 * rest of the boot after the device is unbound, so they must go before the
 * devm cleanup.  Mutex destruction is not needed.
 */
static void mtk_g2d_remove(struct platform_device *pdev)
{
	struct mtk_g2d *g2d = dev_get_drvdata(&pdev->dev);

	/*
	 * Before the clocks go.  drm_dev_unregister() closes the render node
	 * and waits for the last open file to finish, so after it returns no
	 * ioctl can be in flight and the engine can be powered down without
	 * a programmed address being left behind on a block whose clock is
	 * about to be gated off.
	 */
	mtk_g2d_unregister_drm(g2d->drm);

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
	 * clock behind them.
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
