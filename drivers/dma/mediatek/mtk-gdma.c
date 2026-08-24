// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6589 GDMA (general-purpose DMA) driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6589 has two non-linked-list general purpose DMA channels in the
 * APDMA block.  Each channel is a flat register file at
 * APDMA + 0x80 * (n + 1):
 *
 *   0x00 INT_FLAG   0x04 INT_EN    0x08 START     0x0c RESET
 *   0x10 STOP       0x14 FLUSH     0x18 CON       0x1c SRC
 *   0x20 DST        0x24 LEN1      0x28 LEN2
 *
 * A transfer is programmed with direction/size bits in CON, source and
 * destination addresses, and the byte count in LEN1; completion raises the
 * channel interrupt (INT_FLAG bit 0 must be cleared by writing 0).
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "../virt-dma.h"

#define MTK_GDMA_IRQ_CLEAR		0x0
#define MTK_GDMA_START			0x1

/* Channel register offsets */
#define MTK_GDMA_INT_FLAG		0x00
#define MTK_GDMA_INT_EN			0x04
#define MTK_GDMA_START_REG		0x08
#define MTK_GDMA_RESET			0x0c
#define MTK_GDMA_STOP			0x10
#define MTK_GDMA_FLUSH			0x14
#define MTK_GDMA_CON			0x18
#define MTK_GDMA_SRC			0x1c
#define MTK_GDMA_DST			0x20
#define MTK_GDMA_LEN1			0x24

/* CON fields */
#define GDMA_CON_DIR_MEM_TO_MEM		BIT(0)		/* 1: mem->mem */
#define GDMA_CON_DFIX			BIT(3)
#define GDMA_CON_SFIX			BIT(4)
#define GDMA_CON_WSIZE_4BYTE		(0x2 << 24)
#define GDMA_CON_RSIZE_4BYTE		(0x2 << 28)
#define GDMA_CON_BURST_SINGLE		(0x0 << 16)

#define GDMA_LEN_MAX			0x000fffff	/* LEN1[19:0] */
#define GDMA_INT_EN_BIT			BIT(0)
#define GDMA_START_BIT			BIT(0)
#define GDMA_FLUSH_BIT			BIT(0)

struct mtk_gdma_chan_cfg {
	u32				src_addr;
	u32				dst_addr;
	u32				len;
};

struct mtk_gdma_desc {
	struct virt_dma_desc		vd;
	struct mtk_gdma_chan_cfg	cfg;
};

struct mtk_gdma_chan {
	struct virt_dma_chan		vc;
	void __iomem *base;
	struct mtk_gdma_desc *desc;
	unsigned int id;
};

struct mtk_gdma {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	spinlock_t lock;
	struct dma_device dmadev;
	struct mtk_gdma_chan chan[2];
};

static inline struct mtk_gdma_chan *to_mtk_gdma_chan(struct dma_chan *c)
{
	return container_of(c, struct mtk_gdma_chan, vc.chan);
}

static inline struct mtk_gdma_desc *to_mtk_gdma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct mtk_gdma_desc, vd);
}

static void mtk_gdma_chan_write(struct mtk_gdma_chan *chan,
				unsigned int reg, u32 val)
{
	writel(val, chan->base + reg);
}

static u32 mtk_gdma_chan_read(struct mtk_gdma_chan *chan, unsigned int reg)
{
	return readl(chan->base + reg);
}

static int mtk_gdma_chan_start(struct mtk_gdma_chan *chan)
{
	struct mtk_gdma_desc *desc = chan->desc;

	if (!desc)
		return -EINVAL;

	mtk_gdma_chan_write(chan, MTK_GDMA_INT_FLAG, 0);

	mtk_gdma_chan_write(chan, MTK_GDMA_CON,
			    GDMA_CON_DIR_MEM_TO_MEM |
			    GDMA_CON_SFIX | GDMA_CON_DFIX |
			    GDMA_CON_RSIZE_4BYTE | GDMA_CON_WSIZE_4BYTE |
			    GDMA_CON_BURST_SINGLE);
	mtk_gdma_chan_write(chan, MTK_GDMA_SRC, desc->cfg.src_addr);
	mtk_gdma_chan_write(chan, MTK_GDMA_DST, desc->cfg.dst_addr);
	mtk_gdma_chan_write(chan, MTK_GDMA_LEN1, desc->cfg.len);

	mtk_gdma_chan_write(chan, MTK_GDMA_INT_EN, GDMA_INT_EN_BIT);
	mtk_gdma_chan_write(chan, MTK_GDMA_START_REG, GDMA_START_BIT);

	return 0;
}

static irqreturn_t mtk_gdma_irq(int irq, void *dev_id)
{
	struct mtk_gdma *gdma = dev_id;
	struct mtk_gdma_desc *done = NULL;
	unsigned long flags;
	int i;

	for (i = 0; i < ARRAY_SIZE(gdma->chan); i++) {
		struct mtk_gdma_chan *chan = &gdma->chan[i];
		struct virt_dma_desc *vd;

		if (!(mtk_gdma_chan_read(chan, MTK_GDMA_INT_FLAG) & BIT(0)))
			continue;

		mtk_gdma_chan_write(chan, MTK_GDMA_INT_FLAG, MTK_GDMA_IRQ_CLEAR);

		spin_lock_irqsave(&chan->vc.lock, flags);
		vd = vchan_next_desc(&chan->vc);
		if (vd) {
			list_del(&vd->node);
			chan->desc = to_mtk_gdma_desc(vd);
			done = chan->desc;
			chan->desc = NULL;
		}
		spin_unlock_irqrestore(&chan->vc.lock, flags);
	}

	if (done) {
		vchan_cookie_complete(&done->vd);
		kfree(done);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static struct dma_async_tx_descriptor *
mtk_gdma_prep_memcpy(struct dma_chan *c, dma_addr_t dest, dma_addr_t src,
		     size_t len, unsigned long flags)
{
	struct mtk_gdma_chan *chan = to_mtk_gdma_chan(c);
	struct mtk_gdma_desc *desc;

	if (len > GDMA_LEN_MAX)
		return NULL;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->cfg.src_addr = src;
	desc->cfg.dst_addr = dest;
	desc->cfg.len = len;

	return vchan_tx_prep(&chan->vc, &desc->vd, flags);
}

static size_t mtk_gdma_desc_residue(struct mtk_gdma_chan *chan,
				    struct mtk_gdma_desc *desc)
{
	u32 remaining = mtk_gdma_chan_read(chan, MTK_GDMA_LEN1);

	/* LEN1 counts down during the transfer */
	if (remaining > desc->cfg.len)
		return 0;

	return desc->cfg.len - remaining;
}

static enum dma_status mtk_gdma_tx_status(struct dma_chan *c,
					  dma_cookie_t cookie,
					  struct dma_tx_state *txstate)
{
	struct mtk_gdma_chan *chan = to_mtk_gdma_chan(c);
	enum dma_status status;

	status = dma_cookie_status(c, cookie, txstate);
	if (status == DMA_COMPLETE)
		return status;

	if (chan->desc)
		txstate->residue = mtk_gdma_desc_residue(chan, chan->desc);
	else
		txstate->residue = 0;

	return status;
}

static void mtk_gdma_issue_pending(struct dma_chan *c)
{
	struct mtk_gdma_chan *chan = to_mtk_gdma_chan(c);
	struct virt_dma_desc *vd;
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);
	vd = vchan_next_desc(&chan->vc);
	if (vd) {
		list_del(&vd->node);
		chan->desc = to_mtk_gdma_desc(vd);
		mtk_gdma_chan_start(chan);
	}
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static int mtk_gdma_terminate_all(struct dma_chan *c)
{
	struct mtk_gdma_chan *chan = to_mtk_gdma_chan(c);
	unsigned long flags;

	LIST_HEAD(head);

	spin_lock_irqsave(&chan->vc.lock, flags);
	vchan_get_all_descriptors(&chan->vc, &head);
	chan->desc = NULL;
	spin_unlock_irqrestore(&chan->vc.lock, flags);

	/* stop and flush the channel */
	mtk_gdma_chan_write(chan, MTK_GDMA_FLUSH, GDMA_FLUSH_BIT);
	while (mtk_gdma_chan_read(chan, MTK_GDMA_START_REG))
		cpu_relax();
	mtk_gdma_chan_write(chan, MTK_GDMA_FLUSH, 0);
	mtk_gdma_chan_write(chan, MTK_GDMA_INT_FLAG, MTK_GDMA_IRQ_CLEAR);

	vchan_dma_desc_free_list(&chan->vc, &head);

	return 0;
}

static void mtk_gdma_free_chan_resources(struct dma_chan *c)
{
	vchan_free_chan_resources(to_virt_chan(c));
}

static int mtk_gdma_alloc_chan_resources(struct dma_chan *c)
{
	/* no per-channel resources beyond the virt-chan descriptors */
	return 0;
}

static void mtk_gdma_device_synchronize(struct dma_chan *c)
{
	struct mtk_gdma_chan *chan = to_mtk_gdma_chan(c);

	/*
	 * Ensure the channel has stopped: FLUSH waits for the current burst,
	 * START returns to 0 when the transfer completes or is flushed.
	 */
	if (mtk_gdma_chan_read(chan, MTK_GDMA_START_REG)) {
		mtk_gdma_chan_write(chan, MTK_GDMA_FLUSH, GDMA_FLUSH_BIT);
		while (mtk_gdma_chan_read(chan, MTK_GDMA_START_REG))
			cpu_relax();
		mtk_gdma_chan_write(chan, MTK_GDMA_FLUSH, 0);
	}
}

static int mtk_gdma_chan_init(struct mtk_gdma *gdma, int i)
{
	struct mtk_gdma_chan *chan = &gdma->chan[i];

	chan->id = i;
	chan->base = gdma->base + 0x80 * (i + 1);
	chan->vc.chan.device = &gdma->dmadev;
	dma_cookie_init(&chan->vc.chan);
	INIT_LIST_HEAD(&chan->vc.desc_allocated);

	/* reset the channel into a known state */
	mtk_gdma_chan_write(chan, MTK_GDMA_RESET, 1);
	mtk_gdma_chan_write(chan, MTK_GDMA_RESET, 0);

	return 0;
}

static int mtk_gdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dma_device *dmadev;
	struct mtk_gdma *gdma;
	unsigned int i;
	int ret, irq;

	gdma = devm_kzalloc(dev, sizeof(*gdma), GFP_KERNEL);
	if (!gdma)
		return -ENOMEM;

	gdma->dev = dev;
	platform_set_drvdata(pdev, gdma);

	gdma->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gdma->base))
		return PTR_ERR(gdma->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, mtk_gdma_irq, 0, dev_name(dev), gdma);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	gdma->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(gdma->clk))
		return dev_err_probe(dev, PTR_ERR(gdma->clk),
				     "failed to get clock\n");

	dmadev = &gdma->dmadev;
	dma_cap_zero(dmadev->cap_mask);
	dma_cap_set(DMA_MEMCPY, dmadev->cap_mask);
	dmadev->dev = dev;
	dmadev->device_alloc_chan_resources = mtk_gdma_alloc_chan_resources;
	dmadev->device_free_chan_resources = mtk_gdma_free_chan_resources;
	dmadev->device_prep_dma_memcpy = mtk_gdma_prep_memcpy;
	dmadev->device_issue_pending = mtk_gdma_issue_pending;
	dmadev->device_tx_status = mtk_gdma_tx_status;
	dmadev->device_terminate_all = mtk_gdma_terminate_all;
	dmadev->device_synchronize = mtk_gdma_device_synchronize;
	dmadev->copy_align = 4;
	INIT_LIST_HEAD(&dmadev->channels);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(gdma->chan); i++)
		mtk_gdma_chan_init(gdma, i);

	ret = dma_async_device_register(dmadev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register dma device\n");

	ret = of_dma_controller_register(dev->of_node,
					 of_dma_xlate_by_chan_id, dmadev);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	dma_async_device_unregister(dmadev);
	return ret;
}

static const struct of_device_id mtk_gdma_match[] = {
	{ .compatible = "mediatek,mt6589-gdma" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_gdma_match);

static struct platform_driver mtk_gdma_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = mtk_gdma_match,
	},
	.probe = mtk_gdma_probe,
};
module_platform_driver(mtk_gdma_driver);

MODULE_DESCRIPTION("MediaTek MT6589 General Purpose DMA driver");
MODULE_LICENSE("GPL");
