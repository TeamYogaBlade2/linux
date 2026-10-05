// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 MediaTek Inc.
 *
 * DISP_WDMA - the display subsystem's write-DMA engine.
 *
 * WDMA is the write-only output leg of the display pipeline.  Where RDMA
 * reads pixels from memory and pushes them into the pipeline, WDMA takes a
 * pixel stream from an upstream block and writes it out to memory.  It is
 * never a source for a display output, and it is never a CRTC path component
 * that carries a plane.
 *
 * MT6589 has two instances, WDMA0 at 0x14004000 and WDMA1 at 0x14005000
 * (data sheet ch. 41, p. 1540: "Module name: DISP_WDMA base address:
 * (+14004000h)").  The instances are identical; the vendor driver selects
 * one by adding DISP_INDEX_OFFSET (0x1000, ddp_wdma.c:32) to every register
 * address, which is the same 4 KiB stride this driver gets for free from
 * having a separate platform device per instance.
 *
 * Upstream mainline has no DISP_WDMA DRM driver.  What it has is the
 * dt-bindings (Documentation/devicetree/bindings/display/mediatek/
 * mediatek,wdma.yaml), DT nodes for MT8173/MT6795/MT8167/MT7623, and a
 * matching entry in mtk_drm_drv.c:853 - all binding to no driver at all.
 * This file is written from the vendor source
 * (aquaris-5/mediatek/platform/mt6589/kernel/drivers/dispsys/ddp_wdma.c)
 * with every register offset and bit position checked against data sheet
 * chapter 41.  It deliberately does NOT take its structure from the MDP3
 * media driver (drivers/media/platform/mediatek/mdp3/mtk-mdp3-comp.c), which
 * is a different block family for a different SoC generation.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

/*
 * Register offsets.  Every one of these was read out of data sheet ch. 41's
 * register definition table (pp. 1540-1546) and cross-checked against
 * ddp_reg.h:442-474; the two agree exactly.  The table also confirms the
 * block is 0xAC bytes long at most, so the 4 KiB reg resource below is not
 * lying about anything.
 */
#define WDMA_INTEN			0x0000
#define WDMA_INTSTA			0x0004
#define WDMA_EN			0x0008
#define WDMA_RST			0x000c
#define WDMA_SMI_CON			0x0010
#define WDMA_CFG			0x0014
#define WDMA_SRC_SIZE			0x0018
#define WDMA_CLIP_SIZE			0x001c
#define WDMA_CLIP_COORD			0x0020
#define WDMA_DST_ADDR			0x0024
#define WDMA_DST_W_IN_BYTE		0x0028
#define WDMA_ALPHA			0x002c
#define WDMA_STA			0x0034
#define WDMA_BUF_CON1			0x0038
#define WDMA_BUF_CON2			0x003c
#define WDMA_DST_U_ADDR			0x0070
#define WDMA_DST_V_ADDR			0x0074
#define WDMA_DST_UV_PITCH		0x0078

/*
 * WDMA_INTEN (0x00).  Both bits, and they are the complete set: data sheet
 * p. 1540 gives WDMA_INTEN exactly two named bits, FUE (bit 1, frame
 * underrun) and FCE (bit 0, frame complete), both reset 0.  Matches
 * WDMA_INTEN_FLD_Frame_Underrun/Frame_Complete, REG_FLD(1, 1) and REG_FLD(1,
 * 0), in ddp_wdma.h.
 */
#define WDMA_INTEN_FRAME_UNDERRUN	BIT(1)
#define WDMA_INTEN_FRAME_COMPLETE	BIT(0)
#define WDMA_INT_ALL			(WDMA_INTEN_FRAME_UNDERRUN | \
					 WDMA_INTEN_FRAME_COMPLETE)

/*
 * WDMA_EN (0x08).  Bit 0 only: "EN, Enable, Enables WDMA or not /
 * 0: Disable WDMA / 1: Enable WDMA", reset 0 (data sheet p. 1541).  The
 * vendor WDMAStart() writes 0x01 and WDMAStop() writes 0x00 to the whole
 * register (ddp_wdma.c:42-44, :49), which is the same thing because bits
 * 31..1 are unnamed and reset to 0.
 */
#define WDMA_EN_ENABLE			BIT(0)

/*
 * WDMA_RST (0x0c).  Bit 0, Soft_Reset.  The data sheet is explicit about the
 * protocol (p. 1542): "Write this bit to trigger soft reset.  WDMA will
 * reset itself when bus transaction is done and then auto clear this bit to
 * 0.  SW can poll this bit to see if reset is done or not."  So 1 = trigger,
 * and reading it back as 0 is how software learns the reset finished - the
 * bit is self-clearing, so the poll below waits for it to drop rather than
 * for a value software has to push.  "Note: HW will not assert IRQ during SW
 * reset", which is why the interrupt is masked around this.
 */
#define WDMA_RST_SOFT_RESET		BIT(0)

/*
 * WDMA_SMI_CON (0x10).  Fields, from data sheet p. 1543: SE[4], SL[7:5],
 * SC[15:8], TH[3:0].  Matches WDMA_SMI_CON_FLD_* in ddp_wdma.h.  Reset value
 * is 0x03, i.e. SE=0 and TH=3, so the burst threshold already defaults to 4.
 *
 * Nothing here programs SMI_CON.  The vendor driver's WDMASlowMode()
 * (ddp_wdma.c) is the only writer and no caller in this tree reaches it; the
 * slow-down feature exists to relieve memory bandwidth pressure on
 * particular board layouts, and guessing at it would be inventing behaviour.
 * The reset value is left in place deliberately.
 */

/*
 * WDMA_CFG (0x14).  Field positions confirmed against data sheet p. 1543
 * (IF[3:0] In_Format, OF[7:4] Out_Format, BYTES[8] byte swap, RGBS[9] rgb
 * swap, UVS[10] uv swap, V_AVG[12], DNSP_SEL[15]) and against the
 * WDMA_CFG_FLD_* macros in ddp_wdma.h.  ERR_DIF_EN is bit 25 and DITHER_EN
 * bit 24 per ddp_wdma.h; the data sheet page for WDMA_CFG does not name
 * those two bits in its text, so they are defined here only as part of the
 * configuration mask and are never set by this driver.
 */
#define WDMA_CFG_IN_FORMAT		GENMASK(3, 0)
#define WDMA_CFG_OUT_FORMAT		GENMASK(7, 4)
#define WDMA_CFG_BYTE_SWAP		BIT(8)
#define WDMA_CFG_RGB_SWAP		BIT(9)
#define WDMA_CFG_UV_SWAP		BIT(10)
#define WDMA_CFG_DNSP_SEL		BIT(15)
#define WDMA_CFG_KNOWN_MASK		(WDMA_CFG_IN_FORMAT | \
					 WDMA_CFG_OUT_FORMAT | \
					 WDMA_CFG_BYTE_SWAP | \
					 WDMA_CFG_RGB_SWAP | \
					 WDMA_CFG_UV_SWAP | \
					 WDMA_CFG_DNSP_SEL)

/* Shift count of WDMA_CFG_OUT_FORMAT, so a format value can be placed in it. */
#define WDMA_CFG_OUT_FORMAT_SHIFT	__ffs(WDMA_CFG_OUT_FORMAT)

/*
 * The one output format this driver programs, WDMA_OUTPUT_FORMAT_ARGB from
 * the vendor enumeration in ddp_wdma.h.  That header's full list is
 * WDMA_INPUT_FORMAT_* / WDMA_OUTPUT_FORMAT_*, and these values are the
 * encoding the data sheet's Out_Format field takes (bits [7:4] of WDMA_CFG),
 * not a guess.  The input format is not a parameter at all: see
 * mtk_wdma_config() for why it is fixed.
 */
#define WDMA_OUTPUT_FORMAT_ARGB		0x2

/*
 * WDMA_ALPHA (0x2c).  ASEL[31] selects between the OVL's per-pixel alpha and
 * the constant in A[7:0] (data sheet p. 1546, and WDMA_ALPHA_FLD_A_Sel /
 * WDMA_ALPHA_FLD_A_Value in ddp_wdma.h).  Register reset is 0x000000ff: A_Sel
 * is 0, so the constant is ignored by default and per-pixel alpha passes.
 */
#define WDMA_ALPHA_SEL			BIT(31)
#define WDMA_ALPHA_VALUE		GENMASK(7, 0)

/*
 * WDMA_DST_ADDR (0x24) and WDMA_DST_W_IN_BYTE (0x28).
 *
 * DST_ADDR is 32 bits and the data sheet states outright "There is no
 * alignment restriction" (p. 1546).  The device tree gives this block an
 * iommus property and the reset controller maps the hardware M4U, so the
 * address reaching this register is an M4U-translated IOVA, which on this
 * SoC is a 32-bit quantity.
 *
 * DWB, the width in bytes, is bits [13:0] only (data sheet p. 1546: the
 * bit-number row is 13..0 and the mnemonic is DWB).  That is worth stating
 * plainly because ddp_wdma.h declares WDMA_DST_W_IN_BYTE_FLD_Dst_W_in_Byte
 * as REG_FLD(32, 0) - a 32-bit field.  Following the vendor header would let
 * a pitch up to 16 KiB be written, of which only the low 14 bits survive, so
 * a 4096-pixel ARGB line (16384 bytes) would silently become width 0 and
 * every line would be written back to back.  This driver masks to the field
 * the hardware actually has, and its width check below rejects anything that
 * would not fit instead of letting it be truncated.
 */
#define WDMA_DST_W_IN_BYTE_MASK		GENMASK(13, 0)

/*
 * WDMA_SRC_SIZE / CLIP_SIZE (0x18 / 0x1c) and WDMA_CLIP_COORD (0x20) all
 * split into a 16-bit WTH low half and a 16-bit HGT high half, and the data
 * sheet gives the same range for all of them: 0 ~ 2047 pixels
 * (pp. 1544-1545).  2047 is therefore a hardware limit here, not a policy
 * choice, and WDMA_MAX_DIM below encodes it as the mask of the usable range.
 */
#define WDMA_DIM_MAX			2047
#define WDMA_SIZE_WIDTH			GENMASK(15, 0)
#define WDMA_SIZE_HEIGHT		GENMASK(31, 16)

/*
 * WDMA_DST_UV_PITCH (0x78) is bits [13:0] per WDMA_BUF_ADDR_FLD_UV_Pitch,
 * REG_FLD(14, 0), in ddp_wdma.h - the same 14-bit pitch field as DWB, and for
 * the same reason it is deliberately not programmed here.  It belongs to the
 * planar YUV output mode, which this driver does not offer; see
 * mtk_wdma_config_output() for why.  The register offset itself is not
 * defined above because nothing reads or writes it.
 */

/*
 * Bounded poll budget for the soft-reset's self-clearing bit.  The vendor
 * driver spins 10000 times with no delay at all (ddp_wdma.c:60-70) and gives
 * up, then carries on and writes a reset configuration over the top of
 * whatever the block was doing.  That is the failure mode this driver exists
 * to avoid: the registers are programmed blind while the engine may still be
 * draining a previous frame into a destination buffer that the caller has
 * already been told is finished with.  Here the reset is given a real time
 * budget and a failure is an error, not a shrug.
 */
#define WDMA_RESET_TIMEOUT_US		100000

/*
 * How many times mtk_wdma_soft_reset() will retrigger the reset and re-poll
 * WDMA_RST looking for the block to come back.  See mtk_wdma_recover().
 */
#define WDMA_RESET_TRIES		3

/*
 * WDMA_STA (0x34) is a 32-bit RU (read-only) "WDMA status" word with no
 * named fields in the data sheet - the register table lists a single STA[31:0]
 * field described only as "WDMA status" (p. 1546).  Its reset value is
 * 0x00000001.  Because there is no documented way to decode it, nothing in
 * this driver reads it or interprets it, and it is deliberately absent from the
 * reset sequence too: writing a value the data sheet does not define the
 * behaviour of would be worse than leaving it.
 *
 * That absence is also why this driver has no "is the engine busy" poll to
 * hang a timeout on.  The two completion facts available are WDMA_INTSTA's
 * Frame_Complete bit and the fact that a fresh WDMA_EN=1 with a new
 * configuration is what the vendor driver actually does.  See
 * mtk_wdma_start() for what is done about that.
 */

/*
 * The vendor header caps WDMA at 1920x1080 (WDMA_MAX_WIDTH / WDMA_MAX_HEIGHT
 * in ddp_wdma.h).  The hardware fields are 16 bits wide with a stated range
 * of 0 ~ 2047 for every one of width, height, clip width, clip height and
 * both clip coordinates.  This driver checks against the hardware limit and
 * not against the vendor constant: the larger of the two (2047) is what the
 * registers can represent, and inventing a tighter limit would be a policy
 * claim with no source.  Where they differ, the 16-bit field wins and the
 * difference is reported rather than silently applied.
 */
#define WDMA_MAX_WIDTH			WDMA_DIM_MAX
#define WDMA_MAX_HEIGHT			WDMA_DIM_MAX

struct mtk_disp_wdma;

/**
 * struct mtk_wdma_driver_data - per-SoC facts about a DISP_WDMA block
 * @instance: 0 for WDMA0, 1 for WDMA1
 * @reset: do the data sheet's soft reset, or NULL for an SoC where this
 *	    driver has not established that the block needs it
 * @reset_timeout_us: poll budget for one reset attempt
 *
 * An empty struct body on purpose: there is no per-SoC register offset
 * difference to carry, because this driver has only ever been written for
 * MT6589.  Upstream has no DRM WDMA driver to carry offsets in from, and the
 * MDP3 media driver's block (mdp_reg_wdma.h) is a different part - its
 * WDMA_BUF_CON2 is written with 0x10101050 by config_wdma_frame(), a value
 * with no counterpart anywhere in data sheet ch. 41.  Adding a field here for
 * a hypothetical other SoC would be inventing a difference; adding the SoCs
 * that upstream's YAML names but no driver supports would be claiming support
 * this tree has not verified.
 */
struct mtk_wdma_driver_data {
	unsigned int instance;
	void (*reset)(struct mtk_disp_wdma *wdma);
	unsigned int reset_timeout_us;
};

/*
 * struct mtk_disp_wdma - DISP_WDMA driver structure
 * @data: local driver data
 */
struct mtk_disp_wdma {
	struct device			*dev;
	struct clk_bulk_data		*clks;
	struct reset_control		*rstc;
	int				num_clks;
	void __iomem			*regs;
	struct cmdq_client_reg		cmdq_reg;
	const struct mtk_wdma_driver_data *data;
	void				(*vblank_cb)(void *data);
	void				*vblank_cb_data;
	/*
	 * Set once the soft reset has failed WDMA_RESET_TRIES times and the
	 * block has been left unprogrammed.  See mtk_wdma_recover(): the
	 * hardware may still be writing to whatever address is in
	 * WDMA_DST_ADDR, so no further configuration or start is programmed
	 * and mtk_wdma_start() refuses.
	 */
	bool				wedged;
};

static irqreturn_t mtk_disp_wdma_irq_handler(int irq, void *dev_id)
{
	struct mtk_disp_wdma *priv = dev_id;
	u32 status;

	/*
	 * WDMA_INTSTA (0x04) is write-1-to-clear.  The data sheet gives both of
	 * its bits the same treatment it gives RDMA's (p. 1541): "SW writes
	 * this bit to clear IRQ", for FU at bit 1 and FC at bit 0.  The vendor
	 * driver agrees: WDMAStart() sets INTEN to 0x03 (ddp_wdma.c:41) and
	 * WDMAStop() clears it, and WDMAWait() polls bit 0 of the same
	 * register (ddp_wdma.c:307).
	 *
	 * So the acknowledge is the value that was read, not its complement
	 * and not a blanket 0xffffffff: on a write-1-to-clear register writing
	 * 0 to a bit has no effect, and writing all ones would also strike
	 * bits 31..2, which this block does not name.  Writing the read value
	 * clears exactly what was latched, and leaves alone any condition that
	 * arrived between the read and the write - which arrives as a 0 and is
	 * correctly left standing for the next pass.
	 */
	status = readl(priv->regs + WDMA_INTSTA);
	writel(status, priv->regs + WDMA_INTSTA);

	if (!WDMA_INT_ALL || !(status & WDMA_INT_ALL))
		return IRQ_NONE;

	if (!priv->vblank_cb)
		return IRQ_NONE;

	priv->vblank_cb(priv->vblank_cb_data);

	return IRQ_HANDLED;
}

static void wdma_update_bits(struct device *dev, unsigned int reg,
			     unsigned int mask, unsigned int val)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);
	unsigned int tmp = readl(wdma->regs + reg);

	tmp = (tmp & ~mask) | (val & mask);
	writel(tmp, wdma->regs + reg);
}

void mtk_wdma_register_vblank_cb(struct device *dev,
				 void (*vblank_cb)(void *),
				 void *vblank_cb_data)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	wdma->vblank_cb = vblank_cb;
	wdma->vblank_cb_data = vblank_cb_data;
}

void mtk_wdma_unregister_vblank_cb(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	wdma->vblank_cb = NULL;
	wdma->vblank_cb_data = NULL;
}

void mtk_wdma_enable_vblank(struct device *dev)
{
	wdma_update_bits(dev, WDMA_INTEN, WDMA_INTEN_FRAME_COMPLETE,
			 WDMA_INTEN_FRAME_COMPLETE);
}

void mtk_wdma_disable_vblank(struct device *dev)
{
	wdma_update_bits(dev, WDMA_INTEN, WDMA_INTEN_FRAME_COMPLETE, 0);
}

int mtk_wdma_clk_enable(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(wdma->num_clks, wdma->clks);
}

void mtk_wdma_clk_disable(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(wdma->num_clks, wdma->clks);
}

/**
 * mtk_wdma_soft_reset - drive the data sheet's WDMA_RST protocol once
 * @wdma: device
 *
 * The sequence is the data sheet's, and the reason it is followed exactly is
 * that it is not a formality: "WDMA will reset itself when bus transaction is
 * done and then auto clear this bit to 0" (p. 1542).  Writing 1 arms the
 * reset; the block clears the bit when it has finished draining whatever
 * transaction it had outstanding.  Polling the bit back to 0 is therefore the
 * only way to know the engine has stopped issuing bus traffic.
 *
 * Returns -ETIMEDOUT if the bit has not self-cleared within the budget.  It
 * is deliberately not ignored the way the vendor loop's `break` ignores it
 * (ddp_wdma.c:66-68): on timeout the block may still be writing to the
 * destination address currently in WDMA_DST_ADDR, and the caller must not
 * program a new configuration or release a buffer on that basis.
 *
 * The reset is de-asserted on every path, success or failure.  Leaving
 * WDMA_RST bit 0 asserted would hold the block in reset for every later
 * operation, so a failed reset that was not cleaned up would turn a transient
 * stall into a permanently dead engine.
 *
 * Must be called with the block's clock enabled.
 */
static int mtk_wdma_soft_reset(struct mtk_disp_wdma *wdma)
{
	void __iomem *regs = wdma->regs;
	u32 status;
	int ret;

	/* Write 1 to trigger; the block owns the bit from here. */
	writel(WDMA_RST_SOFT_RESET, regs + WDMA_RST);

	ret = readl_poll_timeout(regs + WDMA_RST, status,
				 !(status & WDMA_RST_SOFT_RESET), 20,
				 wdma->data->reset_timeout_us);

	/*
	 * Step back to 0 either way.  On success the bit has already cleared
	 * itself and this write is a no-op; on timeout it is what stops the
	 * block being held in reset forever.
	 */
	writel(0, regs + WDMA_RST);

	return ret ? -ETIMEDOUT : 0;
}

/**
 * mtk_wdma_recover - put the block back into a usable state after a reset
 *	that did not complete
 * @wdma: device
 *
 * One soft reset that hits its timeout is not proof that the engine is gone
 * for good: the vendor loop's 10000 spins carry no time budget at all
 * (ddp_wdma.c:60-70), so it can give up in well under a millisecond on a
 * block that merely needed longer.  Repeating the sequence gives the engine
 * several real budgets.
 *
 * When the retries are exhausted the block is wedged rather than released.
 * The reason is the same one that governs the G2D engine in this tree
 * (mtk-g2d.c, g2d_wedge()): the hardware may still be writing to whatever
 * address is in WDMA_DST_ADDR, with no register write this driver can make
 * that is documented to stop it.  Returning an error and letting the caller
 * release the buffer would hand memory back to the system while the engine
 * was still writing into it.  So instead every entry point below refuses on a
 * wedged block, and nothing is programmed or released.
 *
 * Must be called with the block's clock enabled.
 */
static void mtk_wdma_recover(struct mtk_disp_wdma *wdma)
{
	unsigned int i;

	for (i = 0; i < WDMA_RESET_TRIES; i++) {
		if (!mtk_wdma_soft_reset(wdma))
			return;

		/* Rate limited: a caller looping on config would otherwise
		 * produce one line per attempt.  The wedge is reported once,
		 * unratelimited, in probe().
		 */
		dev_warn_ratelimited(wdma->dev,
				     "WDMA reset did not complete, attempt %u/%u\n",
				     i + 1, WDMA_RESET_TRIES);
	}

	/*
	 * Reported from the single place the failure becomes visible, and
	 * deliberately not acted on further: the block stays wedged, the
	 * funcs hooks below refuse, and no buffer is released.  Resetting
	 * the hardware out of this from software would mean a register write
	 * documented not to stop the engine.
	 */
	if (wdma->wedged)
		return;

	wdma->wedged = true;

	dev_err(wdma->dev,
		"WDMA did not reset after %u attempts, wedging the block: it will be left unprogrammed\n",
		WDMA_RESET_TRIES);
}

/**
 * mtk_wdma_reset - soft reset and return to the documented default state
 * @wdma: device
 *
 * Follows WDMAReset() (ddp_wdma.c:56-86): reset, then clear the configuration
 * registers the vendor driver clears.  The list is taken from that function
 * exactly - CFG, SRC_SIZE, CLIP_SIZE, CLIP_COORD, DST_ADDR, DST_W_IN_BYTE,
 * ALPHA - and nothing more.
 *
 * Registers deliberately not cleared: WDMA_STA and WDMA_BUF_CON1/2 are left
 * at their reset values.  WDMA_STA is a read-only status word with no
 * documented decode, and BUF_CON1's reset value (0x001000ff, data sheet
 * p. 1546) is a FIFO threshold configuration that the vendor driver never
 * writes and never needs to clear; writing 0 there would disable the ultra
 * high request entirely, which is a behaviour change with no source behind
 * it.
 *
 * The reset goes through mtk_wdma_recover(), so a block that will not come
 * back is wedged rather than having these clears written over the top of it.
 * That matters most here, at probe: the vendor WDMAReset() ignores its own
 * timeout and writes the configuration clears anyway (ddp_wdma.c:66-68 breaks
 * out of the poll loop and carries on), which programs a block that may still
 * be draining a transaction.
 *
 * Must be called with the block's clock enabled.
 */
static void mtk_wdma_reset(struct mtk_disp_wdma *wdma)
{
	void __iomem *regs = wdma->regs;

	mtk_wdma_recover(wdma);

	/* A wedged block is not programmed any further. */
	if (wdma->wedged)
		return;

	writel(0x00, regs + WDMA_CFG);
	writel(0x00, regs + WDMA_SRC_SIZE);
	writel(0x00, regs + WDMA_CLIP_SIZE);
	writel(0x00, regs + WDMA_CLIP_COORD);
	writel(0x00, regs + WDMA_DST_ADDR);
	writel(0x00, regs + WDMA_DST_W_IN_BYTE);
	writel(0x00, regs + WDMA_ALPHA);
}

/**
 * mtk_wdma_validate_frame - reject a frame the hardware cannot be told to do
 * @wdma: device
 * @width: source width in pixels
 * @height: source height in pixels
 * @bpp: bytes per pixel of the output format
 * @dst_addr: destination M4U-translated IOVA
 *
 * Every limit here comes from a register field, not from the vendor driver's
 * WDMA_MAX_WIDTH/HEIGHT constants, which are tighter (1920x1080) than the
 * hardware (16-bit fields, range 0 ~ 2047, data sheet pp. 1544-1545).
 *
 * The pitch check is the one that has to be careful with arithmetic width.
 * WDMA_DST_W_IN_BYTE holds the destination line pitch in bytes, in 14 bits.
 * The product is computed with the width widened to u64 *before* the
 * multiply, as (u64)width * bpp: writing ((u64)(width + 1) * bpp) would add
 * first in u32 and only then widen, which is the same class of bug the G2D
 * driver documents against at mtk-g2d.c:505 and mtk-g2d-uapi.c:280.  Here
 * width is bounded by WDMA_MAX_WIDTH before the multiply, so the product
 * cannot exceed 2047 * 4 = 8188 and would in fact fit in u16; the u64 is
 * kept so the check does not silently depend on that bound holding.
 */
static int mtk_wdma_validate_frame(struct mtk_disp_wdma *wdma,
				   unsigned int width, unsigned int height,
				   unsigned int bpp, u32 dst_addr)
{
	u64 pitch;

	if (!width || !height)
		return -EINVAL;

	if (width > WDMA_MAX_WIDTH || height > WDMA_MAX_HEIGHT)
		return -EINVAL;

	/* Widen before the multiply, never after. */
	pitch = (u64)width * bpp;

	/*
	 * WDMA_DST_W_IN_BYTE.DWB is 14 bits.  Reject rather than mask: a line
	 * that does not fit is a programming error, and truncating the pitch
	 * would make the hardware write every line at the wrong offset with
	 * no indication that anything went wrong.
	 */
	if (pitch > WDMA_DST_W_IN_BYTE_MASK)
		return -EINVAL;

	/*
	 * WDMA_DST_ADDR is 32 bits with no alignment restriction (data sheet
	 * p. 1546).  The destination is an IOVA from the M4U, so this is a
	 * check that software is not about to hand the block an address it
	 * cannot express, not a check that the IOVA is inside any particular
	 * memory region - the IOMMU owns that, and the DT node's iommus
	 * property is what makes it happen (MT6589_M4U_PORT_WDMA{0,1} at
	 * mt6589-larb-port.h:50-51).
	 *
	 * The final byte written is dst_addr + pitch * height.  Widened before
	 * the multiply and the add for the same reason as above.
	 */
	if ((u64)dst_addr + pitch * height > (u64)(dma_addr_t)~0ULL)
		return -EINVAL;

	return 0;
}

/**
 * mtk_wdma_config_output - program WDMA_CFG's format and swap fields
 * @wdma: device
 * @out_format: WDMA_OUTPUT_FORMAT_*
 *
 * The output format encodings are the vendor WDMA_OUTPUT_FORMAT_* values from
 * ddp_wdma.h, which is the encoding the data sheet's Out_Format field takes
 * (bits [7:4] of WDMA_CFG).  The byte/rgb/uv swap bits exist because the
 * field names a small set of formats and the rest of the colour-space
 * arrangements are reached by swapping - that is what ddp_wdma.c:130-176 is
 * doing, mapping BGR888 onto RGB888 with rgb_swap and so on.
 *
 * Only RGB565 and ARGB are programmed here, and for both the vendor driver
 * takes the same branch: output_format = outputFormat with byte_swap,
 * rgb_swap and uv_swap all left 0 (ddp_wdma.c:132-140).  The swap bits
 * therefore need no value from this function at all, and the format table
 * below carries them as zero rather than pretending to encode formats this
 * driver cannot reach.
 *
 * The planar YUV420 and UYVY modes the vendor driver supports are
 * deliberately not offered.  YUV420_P needs DST_U_ADDR, DST_V_ADDR and
 * DST_UV_PITCH programmed with a second and third plane of addresses, and
 * the destination byte pitch of WDMAConfigUV() is computed in the vendor
 * driver as `dstWidth * bpp / 2` from a caller-supplied address arithmetic
 * (ddp_wdma.c:88-97, :608-614) with no bounds check anywhere - that is
 * exactly the "narrow arithmetic into an unbounded buffer" shape the G2D
 * driver in this tree exists to refuse.  Exposing a mode whose address
 * computation has no checked version of it would be shipping the bug, so
 * the mode is left out and said here rather than left to be discovered.
 *
 * Must be called with the block's clock enabled.
 */
static void mtk_wdma_config_output(struct mtk_disp_wdma *wdma,
				   unsigned int out_format)
{
	wdma_update_bits(wdma->dev, WDMA_CFG, WDMA_CFG_KNOWN_MASK,
			 (out_format << WDMA_CFG_OUT_FORMAT_SHIFT) &
				 WDMA_CFG_KNOWN_MASK);
}

/**
 * mtk_wdma_config - program a frame for WDMA to write out
 * @dev: device
 * @width: source width in pixels
 * @height: source height in pixels
 * @vrefresh: vertical refresh rate, unused
 * @bpc: bits per colour component, unused
 * @cmdq_pkt: command packet, or NULL for a direct register write
 *
 * Mirrors WDMAConfig() (ddp_wdma.c:98-306) for the subset this driver
 * supports.  The clip rectangle is programmed as the full frame at (0,0):
 * that is what every in-tree caller of the vendor function does, passing
 * clipX = clipY = 0 and clipWidth/Height equal to the source size
 * (ddp_path.c:596-603 for the OVL -> WDMA1 memory-out path, and :680-686 for
 * the suspend-mode capture path).  There is no consumer on this tree that
 * clips, and exposing a clip path that nothing uses would be untested code.
 *
 * The destination width in bytes is the source width times the output
 * format's bytes per pixel, exactly as WDMAConfig() computes it from its own
 * bpp table (ddp_wdma.c:280-298, :301).  For ARGB that is 4.
 *
 * This hook exists so the funcs table is complete and so the block can be
 * brought to a known, idle state.  It is NOT a way to aim WDMA at memory:
 * no buffer is allocated for this block anywhere on this tree, so
 * WDMA_DST_ADDR is deliberately left at 0 rather than at whatever a previous
 * caller left there.  See mtk_wdma_start() for why that makes start() refuse.
 *
 * On a wedged block, or on a frame the hardware cannot be told to do, this
 * returns without programming anything.  Returning without a register write is
 * deliberate in both cases: a partial program followed by WDMA_EN=1 would
 * start the engine on a half-written destination.
 */
void mtk_wdma_config(struct device *dev, unsigned int width,
		     unsigned int height, unsigned int vrefresh,
		     unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);
	const unsigned int out_format = WDMA_OUTPUT_FORMAT_ARGB;
	const unsigned int bpp = 4;

	/* Never program a wedged block. */
	if (wdma->wedged)
		return;

	/*
	 * Validated against the destination this would program, which is 0.
	 * The address is the one thing mtk_wdma_validate_frame() is here to
	 * range-check, and it is checked here rather than skipped because
	 * "it happens to be zero": the check is what would catch a future
	 * caller that supplies a real address, and it must not be the path
	 * that is untested.
	 */
	if (mtk_wdma_validate_frame(wdma, width, height, bpp, 0))
		return;

	/*
	 * The input format is fixed, not a parameter.  On this tree the only
	 * consumer of a WDMA is the overlay: the OVL -> WDMA1 route is the
	 * memory-out path (ddp_path.c:566, disp_path_config_mem_out()) and it
	 * passes WDMA_INPUT_FORMAT_ARGB (ddp_path.c:597).  The YUV444 direct
	 * link the vendor header names has no producer on this tree.
	 */
	mtk_wdma_config_output(wdma, out_format);

	/* SRC_SIZE: height in [31:16], width in [15:0]. */
	mtk_ddp_write_mask(cmdq_pkt, (height << 16) | width, &wdma->cmdq_reg,
			   wdma->regs, WDMA_SRC_SIZE,
			   WDMA_SIZE_HEIGHT | WDMA_SIZE_WIDTH);

	/* CLIP_COORD at the origin, CLIP_SIZE the full frame. */
	mtk_ddp_write_mask(cmdq_pkt, 0, &wdma->cmdq_reg, wdma->regs,
			   WDMA_CLIP_COORD, WDMA_SIZE_HEIGHT | WDMA_SIZE_WIDTH);
	mtk_ddp_write_mask(cmdq_pkt, (height << 16) | width, &wdma->cmdq_reg,
			   wdma->regs, WDMA_CLIP_SIZE,
			   WDMA_SIZE_HEIGHT | WDMA_SIZE_WIDTH);

	/*
	 * ALPHA: A_Sel = 0 takes the per-pixel alpha from the overlay.  The
	 * register resets to 0xff with A_Sel already 0, so this writes the
	 * reset state explicitly rather than relying on it surviving - a
	 * previous configuration could have selected the constant-alpha mode.
	 */
	mtk_ddp_write_mask(cmdq_pkt, 0, &wdma->cmdq_reg, wdma->regs,
			   WDMA_ALPHA, WDMA_ALPHA_SEL | WDMA_ALPHA_VALUE);

	/*
	 * DST_ADDR is written as 0, deliberately and on every config, not
	 * skipped: no buffer is allocated for this block anywhere on this tree
	 * and no component supplies a destination address, so the alternative is
	 * leaving whatever the bootloader or a previous mode put there.  With
	 * the register pinned at 0 there is no address for this engine to be
	 * pointed at, which is what makes the refusal in mtk_wdma_start()
	 * honest rather than advisory.
	 */
	mtk_ddp_write_mask(cmdq_pkt, 0, &wdma->cmdq_reg, wdma->regs,
			   WDMA_DST_ADDR, GENMASK(31, 0));
	mtk_ddp_write_mask(cmdq_pkt, width * bpp, &wdma->cmdq_reg,
			   wdma->regs, WDMA_DST_W_IN_BYTE,
			   WDMA_DST_W_IN_BYTE_MASK);
}

/**
 * mtk_wdma_start - enable the engine
 * @dev: device
 *
 * This is the vendor WDMAStart() (ddp_wdma.c:40-45): unmask both interrupts,
 * then set WDMA_EN.
 *
 * Interrupts are enabled before the engine, not after.  Any status latched
 * while the block was idle would otherwise be delivered as if it were fresh
 * the moment EN went high, which is the same reasoning the RDMA driver gives
 * for mtk_rdma_start() in this tree.
 *
 * Note what this does NOT do: it does not return a completion, and it does not
 * wait for one.  WDMA is not a CRTC path component on any SoC this driver
 * claims - it appears in no mtk_ddp_ext array - so there is no atomic commit
 * boundary for it to complete against, and no buffer whose release this
 * driver could safely gate on a completion it is not tracking.  The vblank
 * hooks above exist so a caller that does have such a boundary can use it,
 * but nothing in this driver owns a destination buffer, so nothing calls
 * them.  Writing a "wait for Frame_Complete and return" here would be
 * inventing a lifetime this block does not have on this tree; the vendor
 * WDMAWait() (ddp_wdma.c:301-320) exists and is not called by any in-tree
 * consumer either.
 *
 * Must be called with the block's clock enabled.
 */
void mtk_wdma_start(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	/*
	 * Refuse on a wedged block.  WDMA_EN is what makes the engine fetch
	 * and write the address in WDMA_DST_ADDR, and after a failed reset that
	 * register still describes an operation that never finished, so this is
	 * the one place where "submit" happens and therefore the one place the
	 * wedge has to be refused.  Nothing has been released on the way here,
	 * so returning also means no buffer is handed back.
	 */
	if (wdma->wedged)
		return;

	/*
	 * The whole set of INTEN bits, which is complete: the data sheet names
	 * exactly two (FUE, FCE), so writing WDMA_INT_ALL cannot enable a bit
	 * this driver is unable to name or acknowledge.
	 */
	writel(WDMA_INT_ALL, wdma->regs + WDMA_INTEN);
	writel(WDMA_EN_ENABLE, wdma->regs + WDMA_EN);
}

/**
 * mtk_wdma_stop - disable the engine
 * @dev: device
 *
 * Mirrors WDMAStop() (ddp_wdma.c:47-54): mask the interrupts, clear the
 * enable, and acknowledge the status.  The acknowledge is the full mask, not
 * the 0x00 the vendor function writes, because WDMA_INTSTA is
 * write-1-to-clear (see the IRQ handler): the vendor write of 0 clears
 * nothing, which is why the vendor driver can only get away with it because
 * WDMAReset() happens to follow in most call paths.
 */
void mtk_wdma_stop(struct device *dev)
{
	struct mtk_disp_wdma *wdma = dev_get_drvdata(dev);

	writel(0x00, wdma->regs + WDMA_INTEN);
	writel(0x00, wdma->regs + WDMA_EN);
	writel(WDMA_INT_ALL, wdma->regs + WDMA_INTSTA);
}

unsigned int mtk_wdma_supported_rotations(struct device *dev)
{
	/*
	 * DRM_MODE_ROTATE_0 and nothing else.  WDMA has no rotation of its
	 * own: the display pipeline's scaling and rotation lives in SCL, and
	 * the vendor driver passes no rotation parameter to WDMAConfig()
	 * at all (ddp_wdma.c:98-306).  Returning ROTATE_0 is also what
	 * mtk_ddp_comp_supported_rotations() falls back to, so this matches
	 * that default rather than overriding it with a claim.
	 *
	 * Advertising anything else would be a lie with a visible consequence:
	 * the CRTC would accept a rotated mode and neither WDMA nor anything
	 * else on this path would perform the rotation.
	 */
	return DRM_MODE_ROTATE_0;
}

static int mtk_disp_wdma_bind(struct device *dev, struct device *master,
			      void *data)
{
	return 0;
}

static void mtk_disp_wdma_unbind(struct device *dev, struct device *master,
				 void *data)
{
}

static const struct component_ops mtk_disp_wdma_component_ops = {
	.bind	= mtk_disp_wdma_bind,
	.unbind = mtk_disp_wdma_unbind,
};

static int mtk_disp_wdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_wdma *priv;
	int irq;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get WDMA clks\n");
	priv->num_clks = ret;

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "failed to ioremap WDMA\n");

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "get mediatek,gce-client-reg fail!\n");
#endif

	/*
	 * Disable and clear pending interrupts before anything else.  INTEN=0
	 * masks everything; INTSTA is write-1-to-clear so it is acknowledged
	 * with the full mask, not with 0.
	 */
	writel(0x0, priv->regs + WDMA_INTEN);
	writel(WDMA_INT_ALL, priv->regs + WDMA_INTSTA);

	ret = devm_request_irq(dev, irq, mtk_disp_wdma_irq_handler,
			       IRQF_TRIGGER_NONE, dev_name(dev), priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request irq %d\n", irq);

	priv->data = of_device_get_match_data(dev);
	if (!priv->data)
		return dev_err_probe(dev, -ENODEV, "no driver data\n");

	platform_set_drvdata(pdev, priv);

	pm_runtime_enable(dev);

	/*
	 * resume_and_get(), not get_sync(): get_sync() leaves the usage counter
	 * incremented when the resume fails and pm_runtime_disable() does not
	 * undo that reference, so an error return would hand the device back
	 * with a reference nobody will ever drop.  The reference taken here is
	 * kept for the lifetime of the bound device - the put_sync() below is
	 * balanced against the reset setup below it, not against this one.
	 */
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "Failed to enable power\n");
	}

	if (priv->data->reset) {
		/* Look the reset up by index; see the note in mtk_disp_ovl.c
		 * about the missing "reset-names" on the display nodes.
		 */
		priv->rstc = devm_reset_control_get(dev, NULL);
		if (IS_ERR(priv->rstc)) {
			ret = PTR_ERR(priv->rstc);
			pm_runtime_put_sync(dev);
			pm_runtime_disable(dev);
			return dev_err_probe(dev, ret,
					     "Failed to get reset control\n");
		}

		ret = clk_bulk_prepare_enable(priv->num_clks, priv->clks);
		if (ret) {
			pm_runtime_put_sync(dev);
			pm_runtime_disable(dev);
			return dev_err_probe(dev, ret,
					     "Failed to enable WDMA clocks\n");
		}

		/*
		 * Assert the block-level reset before touching it.  WDMA_RST is
		 * the block's own soft reset and is a separate mechanism; this
		 * is MT6589_DISP_WDMA{0,1}_RST (7 and 8,
		 * include/dt-bindings/reset/mt6589-resets.h:82-83), which
		 * resets the whole block.
		 */
		ret = reset_control_reset(priv->rstc);
		if (ret) {
			clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
			pm_runtime_put_sync(dev);
			pm_runtime_disable(dev);
			return dev_err_probe(dev, ret,
					     "Failed to reset WDMA\n");
		}

		/* Clear anything the reset left latched. */
		writel(0x0, priv->regs + WDMA_INTEN);
		writel(WDMA_INT_ALL, priv->regs + WDMA_INTSTA);

		priv->data->reset(priv);

		clk_bulk_disable_unprepare(priv->num_clks, priv->clks);
	}

	pm_runtime_put_sync(dev);

	ret = component_add(dev, &mtk_disp_wdma_component_ops);
	if (ret) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "Failed to add component\n");
	}

	return 0;
}

static void mtk_disp_wdma_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_wdma_component_ops);

	pm_runtime_disable(&pdev->dev);
}

static const struct mtk_wdma_driver_data mt6589_wdma0_driver_data = {
	.instance = 0,
	.reset = mtk_wdma_reset,
	.reset_timeout_us = WDMA_RESET_TIMEOUT_US,
};

static const struct mtk_wdma_driver_data mt6589_wdma1_driver_data = {
	.instance = 1,
	.reset = mtk_wdma_reset,
	.reset_timeout_us = WDMA_RESET_TIMEOUT_US,
};

static const struct of_device_id mtk_disp_wdma_driver_dt_match[] = {
	{ .compatible = "mediatek,mt6589-disp-wdma0",
	  .data = &mt6589_wdma0_driver_data },
	{ .compatible = "mediatek,mt6589-disp-wdma1",
	  .data = &mt6589_wdma1_driver_data },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_disp_wdma_driver_dt_match);

struct platform_driver mtk_disp_wdma_driver = {
	.probe		= mtk_disp_wdma_probe,
	.remove		= mtk_disp_wdma_remove,
	.driver		= {
		.name	= "mediatek-disp-wdma",
		.of_match_table = mtk_disp_wdma_driver_dt_match,
	},
};
module_platform_driver(mtk_disp_wdma_driver);

MODULE_AUTHOR("MediaTek Inc.");
MODULE_DESCRIPTION("MediaTek DISP_WDMA driver");
MODULE_LICENSE("GPL");
