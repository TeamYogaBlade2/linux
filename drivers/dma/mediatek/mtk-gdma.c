// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 MediaTek, Inc.
 *
 * Driver for the MT6589 AP DMA "General DMA" (G_DMA) engines.
 *
 * Data sheet chapter 18 ("AP DMA").  The block contains 18 engines; only
 * G_DMA0 and G_DMA1 are generic, they sit at 0x11000080 and 0x11000100, and
 * they are the only two with their own interrupt (GIC SPI 57 and 58).
 *
 * AP_DMA has no descriptor ring and no scatter-gather list: a transfer is
 * programmed as plain SRC/DST/LEN1/LEN2 registers and the completion is
 * reported through a single INT_FLAG bit.  That is why the sibling
 * mtk-cqdma.c is the right model for the transfer side and not fsl-edma or
 * axi-dmac, all of which need a descriptor linked list built in RAM.
 *
 * What this first commit deliberately does NOT contain is any transfer logic.
 * Nothing in the tree consumes these two channels yet, and there is one
 * register question that cannot be answered from the data sheet or from the
 * vendor source: the polarity of the two completion interrupts.  Until that is
 * settled on hardware, the only useful thing a driver can do is prove that
 * the clock, the reset and the interrupt line are wired up, which is what
 * this commit is.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/reset.h>

#include "mtk-gdma.h"

#define MTK_GDMA_RESET_TIMEOUT_US	2000000

/**
 * struct mtk_gdma_channel - one of the two general DMA engines
 * @dev: the parent platform device, used for logging
 * @base: base address of this engine's register block
 * @irq: the completion interrupt for this engine
 */
struct mtk_gdma_channel {
	struct device *dev;
	void __iomem *base;
	int irq;
};

/**
 * struct mtk_gdma - the AP DMA General DMA block
 * @dev: the platform device
 * @regs: the mapped 0x180 register window, global registers at offset 0
 * @chan: the per-engine state, indexed by engine number
 * @clk: the "main" clock gating the whole block
 * @rstc: the "main" software reset for the block
 */
struct mtk_gdma {
	struct device *dev;
	void __iomem *regs;
	struct mtk_gdma_channel chan[MTK_GDMA_NR_CHANNELS];
	struct clk *clk;
	struct reset_control *rstc;
};

/*
 * Perform the global warm reset the data sheet describes in section 18.5.1
 * (p. 928): set WARM_RST, poll the global running status until every engine
 * has gone idle, then clear WARM_RST again.
 *
 * A hard reset is deliberately not used.  The same section warns that it
 * "may break the bus protocol and cause system hang", because it drops
 * transactions that will then never complete.
 *
 * The block's own reset register is only half of the job; the peripheral
 * domain also has a software reset bit (PERICFG PERI_GLOBALCON_RST0 bit 11,
 * data sheet p. 469) which the caller takes care of through the reset
 * framework before we get here.
 */
static void mtk_gdma_reset(struct mtk_gdma *gdma)
{
	struct device *dev = gdma->dev;
	u32 status;

	writel(MTK_GDMA_RST_WARM_RST, gdma->regs + MTK_GDMA_GLOBAL_RST);

	status = readl_poll_timeout(gdma->regs + MTK_GDMA_GLOBAL_RUNNING_STATUS,
				    status, !status, 10,
				    MTK_GDMA_RESET_TIMEOUT_US);
	if (status)
		dev_warn(dev, "timed out waiting for engines to stop, status %#x\n",
			 status);

	/* Clear even on timeout: leaving WARM_RST set would wedge the block. */
	writel(0, gdma->regs + MTK_GDMA_GLOBAL_RST);
}

/*
 * Completion handler.  This does nothing beyond acknowledging the interrupt
 * so that the line can be proven reachable at all; see the note on IRQ
 * polarity in mtk_gdma_probe() and in the commit message.
 */
static irqreturn_t mtk_gdma_irq_handler(int irq, void *data)
{
	struct mtk_gdma_channel *channel = data;

	/* Writing 0 clears FLAG (data sheet p.887). */
	writel(0, channel->base + MTK_GDMA_INT_FLAG);

	/*
	 * Rate limited on purpose.  If the line really is level triggered but
	 * the flag does not clear it, this handler is re-entered immediately
	 * and an unconditional dev_info() per call would flood the console and
	 * hide the very fact this commit wants to demonstrate.
	 */
	dev_info_ratelimited(channel->dev, "interrupt %d taken\n", irq);

	return IRQ_HANDLED;
}

static int mtk_gdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_gdma *gdma;
	unsigned int i;
	int ret;

	gdma = devm_kzalloc(dev, sizeof(*gdma), GFP_KERNEL);
	if (!gdma)
		return -ENOMEM;

	gdma->dev = dev;
	platform_set_drvdata(pdev, gdma);

	gdma->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gdma->regs))
		return PTR_ERR(gdma->regs);

	gdma->clk = devm_clk_get(dev, "main");
	if (IS_ERR(gdma->clk))
		return dev_err_probe(dev, PTR_ERR(gdma->clk),
				     "failed to get main clock\n");

	ret = clk_prepare_enable(gdma->clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable main clock\n");

	gdma->rstc = devm_reset_control_get(dev, "main");
	if (IS_ERR(gdma->rstc))
		return dev_err_probe(dev, PTR_ERR(gdma->rstc),
				     "failed to get main reset\n");

	ret = reset_control_assert(gdma->rstc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to assert main reset\n");

	ret = reset_control_deassert(gdma->rstc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert main reset\n");

	mtk_gdma_reset(gdma);

	/*
	 * Channel N lives 0x80 bytes apart, G_DMA0 first (data sheet p.887).
	 */
	for (i = 0; i < MTK_GDMA_NR_CHANNELS; i++) {
		struct mtk_gdma_channel *channel = &gdma->chan[i];
		int irq;

		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(dev, irq,
					     "failed to get irq for channel %u\n", i);

		channel->dev = dev;
		channel->irq = irq;
		channel->base = gdma->regs + 0x80 * (i + 1);

		/*
		 * Data sheet Table 7-1 lists SPI 57 (gdma0) and 58 (gdma1) as
		 * "Positive", and that is what the DT node asks for.  The
		 * vendor driver asks for the opposite: it passes
		 * IRQF_TRIGGER_LOW to request_irq() (mt_dma.c:502,511) while
		 * the lines immediately above that call, which set
		 * MT65xx_POLARITY_LOW through MT6577_* helpers, are
		 * commented out.  Those helpers name MT6577, not MT6589, so
		 * this looks like a copy from a SoC whose polarity differs and
		 * the polarity half was dropped.
		 *
		 * Going with the data sheet for now.  The handler above does
		 * nothing but acknowledge the interrupt precisely so that one
		 * hardware run settles the question; commit 2 (transfers)
		 * depends on the answer.
		 */
		ret = devm_request_threaded_irq(dev, irq, mtk_gdma_irq_handler,
						NULL, IRQF_ONESHOT,
						dev_name(dev), channel);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request irq %d for channel %u\n",
					     irq, i);

		dev_info(dev, "channel %u at %pa, irq %d\n", i, &channel->base,
			 irq);
	}

	return 0;
}

static void mtk_gdma_remove(struct platform_device *pdev)
{
	struct mtk_gdma *gdma = platform_get_drvdata(pdev);

	/*
	 * devm unhooks the interrupts, the mapping and the reset control when
	 * we return, but not the clock: devm_clk_get() only drops the
	 * reference, and clk_prepare_enable() has to be undone by hand.
	 *
	 * There is deliberately no per-engine flush here.  This commit starts
	 * no transfer, so no engine can be running when we get called; the
	 * flush-and-wait-for-idle dance that a real remove() needs arrives
	 * with the transfer code in commit 2, where it can be written against
	 * a channel that is known to be doing something.
	 */
	clk_disable_unprepare(gdma->clk);
}

static const struct of_device_id mtk_gdma_of_match[] = {
	{ .compatible = "mediatek,mt6589-gdma" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_gdma_of_match);

static struct platform_driver mtk_gdma_driver = {
	.probe = mtk_gdma_probe,
	.remove = mtk_gdma_remove,
	.driver = {
		.name = "mtk-gdma",
		.of_match_table = mtk_gdma_of_match,
	},
};
module_platform_driver(mtk_gdma_driver);

MODULE_DESCRIPTION("MediaTek MT6589 AP DMA General DMA controller driver");
MODULE_LICENSE("GPL");
