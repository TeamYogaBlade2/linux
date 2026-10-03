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
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include "mtk-g2d.h"

/* Control block - 16 bit wide. */
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

/* G2D_STATUS - reads 0 once the engine is idle. */
#define G2D_STATUS_BUSY		BIT(0)

/* G2D_IRQ */
#define G2D_IRQ_ENABLE		BIT(0)

/* *_CON format and attribute fields, shared by SRC_CON and DST_CON. */
#define G2D_CON_DI_ALP_MUL		BIT(13)
#define G2D_CON_DITHER_EN		BIT(12)
#define G2D_CON_FLIP			GENMASK(11, 10)
#define G2D_CON_COLOR_EN		BIT(9)
#define G2D_CON_RB_SWP			BIT(4)
#define G2D_CON_BYTE_SWP		BIT(3)
#define G2D_CON_CLRFMT			GENMASK(2, 0)

/* G2D_ALP_CON */
#define G2D_ALP_CON_MODE		GENMASK(1, 0)

#define G2D_TIMEOUT_US			100000

struct mtk_g2d {
	void __iomem *regs;
	struct clk *clk_engine;
	struct clk *clk_smi;
	struct iommu_domain *iommu;
	struct mutex lock;
	spinlock_t busy_lock;
	bool busy;
};

/* Formats the CLRFMT field encodes. */
static const struct g2d_format_info g2d_formats[] = {
	[g2d_clrfmt_rgb565]		= {
		.clrfmt		= 0b001,
		.bytes_per_pixel	= 2,
	},
	[g2d_clrfmt_pargb8888]		= {
		.clrfmt		= 0b101,
		.bytes_per_pixel	= 4,
	},
	[g2d_clrfmt_argb8888]		= {
		.clrfmt		= 0b100,
		.bytes_per_pixel	= 4,
	},
	[g2d_clrfmt_rgb888]		= {
		.clrfmt		= 0b011,
		.bytes_per_pixel	= 3,
	},
	[g2d_clrfmt_xrgb8888]		= {
		.clrfmt		= 0b110,
		.bytes_per_pixel	= 4,
	},
};

static int g2d_wait_idle(struct mtk_g2d *g2d)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(G2D_TIMEOUT_US / 1000);

	while (readl(g2d->regs + G2D_STATUS) & G2D_STATUS_BUSY) {
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

static irqreturn_t g2d_irq_handler(int irq, void *data)
{
	struct mtk_g2d *g2d = data;
	u32 status;

	status = readl(g2d->regs + G2D_STATUS);
	if (!(status & G2D_STATUS_BUSY))
		return IRQ_NONE;

	/*
	 * Clear by writing 0, per the data sheet's description of the
	 * status register, then acknowledge the interrupt.
	 */
	writel(0, g2d->regs + G2D_STATUS);
	writel(0, g2d->regs + G2D_IRQ);

	spin_lock_irq(&g2d->busy_lock);
	g2d->busy = false;
	spin_unlock_irq(&g2d->busy_lock);

	return IRQ_HANDLED;
}

/**
 * g2d_start - fire one operation and wait for it to finish.
 * @g2d: device
 *
 * Writes G2D_START = 0 then 1, which is what the data sheet asks for, and
 * then either waits for G2D_STATUS to read 0 or for the interrupt.
 */
static int g2d_start(struct mtk_g2d *g2d)
{
	int ret;

	spin_lock_irq(&g2d->busy_lock);
	g2d->busy = true;
	spin_unlock_irq(&g2d->busy_lock);

	writel(0, g2d->regs + G2D_START);
	writel(G2D_START_START, g2d->regs + G2D_START);

	ret = g2d_wait_idle(g2d);
	if (ret) {
		spin_lock_irq(&g2d->busy_lock);
		g2d->busy = false;
		spin_unlock_irq(&g2d->busy_lock);
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

int mtk_g2d_blt(struct mtk_g2d *g2d,
		dma_addr_t src, u32 src_pitch, enum g2d_format src_fmt,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height)
{
	u32 con, flip = 0;
	u32 src_bpp, dst_bpp;
	int ret;

	if (!width || !height)
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
	 * The pitch registers are in bytes and must satisfy
	 * pitch / bytes_per_pixel >= width, and be a multiple of it.
	 */
	if (src_pitch < width * src_bpp || src_pitch % src_bpp)
		return -EINVAL;
	if (dst_pitch < width * dst_bpp || dst_pitch % dst_bpp)
		return -EINVAL;

	mutex_lock(&g2d->lock);

	/*
	 * FLIP is bits [11:10]: 0 none, 1 horizontal, 2 vertical, 3 both.
	 * x/y here is the source rectangle origin, not a flip request - the
	 * data sheet drives flips from SRC_CON, not from the rectangle.
	 */
	/*
	 * x/y are byte offsets into the two surfaces: src_pitch counts bytes
	 * per line, so a row is a pitch, and a column is bytes-per-pixel.
	 */
	/*
	 * The address registers are 32 bits wide, which is also the width of
	 * dma_addr_t on this configuration: LPAE and HIGHMEM are both off, and
	 * the part tops out at 2 GB of LPDDR2.  So no truncation can occur.
	 */
	writel((u32)(src + y * src_pitch + x * src_bpp),
	       g2d->regs + G2D_SRC_ADDR);
	writel(src_pitch & 0x3fff, g2d->regs + G2D_SRC_PITCH);
	con = g2d_formats[src_fmt].clrfmt | flip;
	writel(con, g2d->regs + G2D_SRC_CON);

	/*
	 * The write target is the W2M (write-to-memory) engine.  The data
	 * sheet's note on W2M_CON.DST_NEQ says that when the destination read
	 * buffer is the same as the write buffer - which is what a plain
	 * bitblt is - the bit is 0 and the driver then does not need to set
	 * G2D_DST_CON, G2D_DST_ADDR or G2D_DST_PITCH at all.  DST_NEQ is 0
	 * out of reset, so only the W2M side is programmed here.
	 */
	/*
	 * W2M_SIZE is the width/height of the destination *scan window*, so
	 * x/y position the window in the destination and the source origin
	 * follows from it.  Offsetting both surfaces independently would make
	 * only the x=0,y=0 case behave.  So: window origin in the
	 * destination, matching source offset in the source.
	 */
	writel((u32)(dst + y * dst_pitch + x * dst_bpp),
	       g2d->regs + G2D_W2M_ADDR);
	writel(dst_pitch & 0x3fff, g2d->regs + G2D_W2M_PITCH);
	writel(g2d_formats[dst_fmt].clrfmt, g2d->regs + G2D_W2M_CON);

	/*
	 * Destination scan window.  There is no separate ROI register: the
	 * source rectangle is implied by this window plus SRC_ADDR.
	 */
	writel((width << 16) | height, g2d->regs + G2D_W2M_SIZE);

	/* ENG_MODE 0 selects bitblt. */
	writel(0, g2d->regs + G2D_MODE_CON);

	ret = g2d_start(g2d);

	mutex_unlock(&g2d->lock);

	return ret;
}

int mtk_g2d_fill(struct mtk_g2d *g2d,
		dma_addr_t dst, u32 dst_pitch, enum g2d_format dst_fmt,
		u32 x, u32 y, u32 width, u32 height, u32 color)
{
	u32 con;
	u32 bpp;
	int ret;

	if (!width || !height)
		return -EINVAL;

	ret = g2d_check_fmt(dst_fmt);
	if (ret)
		return ret;

	bpp = g2d_formats[dst_fmt].bytes_per_pixel;
	if (dst_pitch < width * bpp || dst_pitch % bpp)
		return -EINVAL;

	mutex_lock(&g2d->lock);

	/*
	 * Same shape as the bitblt destination: the write target is the W2M
	 * engine, and COLOR_EN (bit 9, named DST_COLOR_EN on both control
	 * registers) selects the constant colour instead of a buffer.
	 */
	writel((u32)(dst + y * dst_pitch + x * bpp),
	       g2d->regs + G2D_W2M_ADDR);
	writel(dst_pitch & 0x3fff, g2d->regs + G2D_W2M_PITCH);

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
	if (irq < 0)
		return dev_err_probe(dev, irq, "failed to get irq\n");

	ret = devm_request_threaded_irq(dev, irq, NULL, g2d_irq_handler,
					IRQF_ONESHOT, dev_name(dev), g2d);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");

	mutex_init(&g2d->lock);
	spin_lock_init(&g2d->busy_lock);

	/* ENABLE in G2D_IRQ, bit 0: enables the 2D engine interrupt. */
	writel(G2D_IRQ_ENABLE, g2d->regs + G2D_IRQ);

	platform_set_drvdata(pdev, g2d);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mtk_g2d_suspend(struct platform_device *pdev,
			   pm_message_t state)
{
	struct mtk_g2d *g2d = dev_get_drvdata(&pdev->dev);

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

	return 0;
}
#endif

static const struct of_device_id mtk_g2d_of_match[] = {
	{ .compatible = "mediatek,mt6589-g2d" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_g2d_of_match);

static struct platform_driver mtk_g2d_driver = {
	.probe		= mtk_g2d_probe,
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
