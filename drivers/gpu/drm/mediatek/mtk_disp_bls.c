// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 BLS (Brightness Level Scaling / backlight scaling) driver
 *
 * The MT6589 main path is OVL -> COLOR -> BLS -> RDMA0 -> DSI0, so BLS sits
 * in the middle of the pipeline and its output is what RDMA0 fetches.  It
 * is driven from the BLS node at 14008000, whose compatible is
 * "mediatek,mt6589-disp-pwm" because the block also carries the panel
 * backlight PWM.
 *
 * This is not the same block as the one in mtk_disp_merge.c: that is the
 * MT8195 10-bit merge unit with a MERGE_CFG_* register map, whereas MT6589
 * BLS is the block below, so the merge driver cannot be reused here.
 *
 * Copyright (c) 2026 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/reset.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"

/* BLS registers (offsets within the 0x1000 block at 14008000). */
#define DISP_BLS_EN		0x0000
#define DISP_BLS_RST		0x0004
#define DISP_BLS_BLS_SETTING	0x0008
#define DISP_BLS_INTEN		0x0010
#define DISP_BLS_INTSTA		0x0014
#define DISP_BLS_SRC_SIZE	0x0018

/*
 * The value the stock driver waits for after enabling BLS, both when it
 * starts and when it resumes from the AAL path (ddp_drv.c).  The low 0x1 is
 * the block-enable bit; the high 0x80010000 are status/latch bits that the
 * hardware sets once it has caught up.  Writing anything else leaves BLS
 * not actually running, which on the main path means RDMA0 fetches a
 * picture that has not been scaled.
 */
#define DISP_BLS_EN_TARGET	0x80010001

/*
 * Cap the poll so a BLS that never reaches the target state cannot stall
 * the display pipeline forever.  The stock driver waits up to one second;
 * use a shorter bound with a warning, since a failure here is fatal to the
 * panel but not to the system.
 */
#define DISP_BLS_ENABLE_TIMEOUT_US	100000

struct mtk_disp_bls {
	void __iomem			*regs;
	struct cmdq_client_reg		cmdq_reg;
	struct reset_control		*rstc;
	struct device			*dev;
};

void mtk_bls_start(struct device *dev)
{
	struct mtk_disp_bls *bls = dev_get_drvdata(dev);
	unsigned int timeout = DISP_BLS_ENABLE_TIMEOUT_US;
	u32 val;

	writel(DISP_BLS_EN_TARGET, bls->regs + DISP_BLS_EN);

	/*
	 * Wait for the enable to take effect rather than assuming it did.
	 * The stock driver polls this exact value and prints a diagnostic on
	 * timeout; without the wait, programming RDMA0 immediately afterwards
	 * races the block still coming up.
	 */
	while (timeout) {
		val = readl(bls->regs + DISP_BLS_EN);
		if (val == DISP_BLS_EN_TARGET)
			return;
		udelay(1);
		timeout--;
	}

	dev_err(bls->dev,
		"BLS did not reach the enable value, BLS_EN = 0x%08x\n",
		readl(bls->regs + DISP_BLS_EN));
}

void mtk_bls_stop(struct device *dev)
{
	struct mtk_disp_bls *bls = dev_get_drvdata(dev);

	writel(0, bls->regs + DISP_BLS_EN);
}

void mtk_bls_config(struct device *dev, unsigned int w,
			   unsigned int h, unsigned int vrefresh,
			   unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_bls *bls = dev_get_drvdata(dev);

	mtk_ddp_write(cmdq_pkt, w, &bls->cmdq_reg, bls->regs, DISP_BLS_SRC_SIZE);
}

static void mtk_disp_bls_probe(struct mtk_disp_bls *bls)
{
	/* Nothing to do until the pipeline starts. */
}

static int mtk_disp_bls_dev_probe(struct platform_device *pdev)
{
	struct mtk_disp_bls *bls;
	int ret;

	bls = devm_kzalloc(&pdev->dev, sizeof(*bls), GFP_KERNEL);
	if (!bls)
		return -ENOMEM;

	bls->dev = &pdev->dev;
	bls->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bls->regs))
		return PTR_ERR(bls->regs);

	/*
	 * Release the block's reset before anything touches its registers.
	 * BLS sits in the middle of the OVL -> COLOR -> BLS -> RDMA0 -> DSI0
	 * path, so while its reset bit is asserted RDMA0 fetches from a block
	 * that is not running.  Look the reset up by index: the node carries a
	 * bare "resets = <&dispsys MT6589_DISP_BLS_RST>" with no reset-names,
	 * and a named lookup fails with -ENOENT before any reset controller is
	 * consulted.
	 */
	bls->rstc = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(bls->rstc)) {
		ret = PTR_ERR(bls->rstc);
		return dev_err_probe(&pdev->dev, ret,
				     "failed to get reset control\n");
	}

	ret = reset_control_reset(bls->rstc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to reset BLS\n");

	platform_set_drvdata(pdev, bls);

	ret = cmdq_dev_get_client_reg(&pdev->dev, &bls->cmdq_reg, 0);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to get cmdq reg\n");

	mtk_disp_bls_probe(bls);

	return 0;
}

static const struct of_device_id mtk_disp_bls_dt_match[] = {
	{ .compatible = "mediatek,mt6589-disp-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_disp_bls_dt_match);

static struct platform_driver mtk_disp_bls_driver = {
	.probe = mtk_disp_bls_dev_probe,
	.driver = {
		.name = "mtk-disp-bls",
		.of_match_table = mtk_disp_bls_dt_match,
	},
};
module_platform_driver(mtk_disp_bls_driver);

MODULE_DESCRIPTION("MediaTek MT6589 BLS driver");
MODULE_LICENSE("GPL");