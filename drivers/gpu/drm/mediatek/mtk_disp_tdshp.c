// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek Two-Dimension Sharpness Processor (TDSHP)
 *
 * Copyright (c) 2025 MediaTek Inc.
 * Copyright (c) 2026 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 *
 * Ported to the MT6589 display pipeline.  The MT6589 TDSHP sits
 * between COLOR and BLS/DBI and is kept in relay (bypass) mode: the
 * downstream kernel programs the sharpness engine but ships with
 * sharpGain = 0, effectively passing the frame through unchanged.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

#define DISP_REG_TDSHP_EN			0x0000
#define  DISP_TDSHP_TDS_EN			BIT(31)
/*
 * 0x0350: PBC/misc bypass register.  The downstream kernel clears this
 * to zero ("bypass off") during init; it is NOT DTDS_CONFIG.
 */
#define DISP_REG_TDSHP_PBC_BYPASS		0x0350
/*
 * 0x0f10: DTDS_CONFIG -- enables the RGB->YUV (R2Y) and YUV->RGB (Y2R)
 * colour-matrix wrappers inside the TDSHP block.  bit[2]=R2Y_EN,
 * bit[1]=Y2R_EN; the downstream kernel writes 0x6 = both enabled.
 */
#define DISP_REG_TDSHP_DTDS_CONFIG		0x0f10
#define  DISP_TDSHP_R2Y_EN			BIT(2)
#define  DISP_TDSHP_Y2R_EN			BIT(1)
#define DISP_REG_TDSHP_INPUT_SIZE		0x0f40
#define DISP_REG_TDSHP_OUTPUT_SIZE		0x0f44
#define DISP_REG_TDSHP_START			0x0f00

struct mtk_disp_tdshp {
	void __iomem *regs;
	struct clk *clk;
	struct cmdq_client_reg cmdq_reg;
};

void mtk_tdshp_config(struct device *dev, unsigned int w,
		      unsigned int h, unsigned int vrefresh,
		      unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_tdshp *tdshp = dev_get_drvdata(dev);

	mtk_ddp_write(cmdq_pkt, w, &tdshp->cmdq_reg,
		      tdshp->regs, DISP_REG_TDSHP_INPUT_SIZE);
	mtk_ddp_write(cmdq_pkt, h, &tdshp->cmdq_reg,
		      tdshp->regs, DISP_REG_TDSHP_OUTPUT_SIZE);

	/*
	 * Clear the PBC/misc bypass register (0x350) as the downstream does.
	 * Then enable the R2Y and Y2R colour-matrix wrappers via DTDS_CONFIG.
	 */
	mtk_ddp_write(cmdq_pkt, 0,
		      &tdshp->cmdq_reg, tdshp->regs, DISP_REG_TDSHP_PBC_BYPASS);
	mtk_ddp_write(cmdq_pkt, DISP_TDSHP_R2Y_EN | DISP_TDSHP_Y2R_EN,
		      &tdshp->cmdq_reg, tdshp->regs, DISP_REG_TDSHP_DTDS_CONFIG);
}

static void mtk_tdshp_matrix_init(struct mtk_disp_tdshp *tdshp)
{
	static const u32 in_matrix[] = {
		0x132,  0x259,  0x075,		/* c00..c02 */
		0x1f53, 0x1ead, 0x200,		/* c10..c12 */
		0x200,  0x1e53, 0x1fad,		/* c20..c22 */
		0, 0, 0,			/* input offsets */
		0, 0x80, 0x80,			/* output offsets */
	};
	static const u32 out_matrix[] = {
		0x400,  0x1fff, 0x59c,
		0x400,  0x1e9f, 0x1d25,
		0x400,  0x716,  0x001,
		0, 0x180, 0x180,		/* input offsets */
		0, 0, 0,			/* output offsets */
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(in_matrix); i++)
		writel(in_matrix[i], tdshp->regs + 0xf50 + i * 4);

	for (i = 0; i < ARRAY_SIZE(out_matrix); i++)
		writel(out_matrix[i], tdshp->regs + 0xf90 + i * 4);

	writel(0x40, tdshp->regs + 0xf24);	/* TDS_HSYNC_WIDTH */
	writel(0x40, tdshp->regs + 0xf3c);	/* ACTIVE_WIDTH_IN_VBLANK */
}

int mtk_tdshp_clk_enable(struct device *dev)
{
	struct mtk_disp_tdshp *tdshp = dev_get_drvdata(dev);

	return clk_prepare_enable(tdshp->clk);
}

void mtk_tdshp_clk_disable(struct device *dev)
{
	struct mtk_disp_tdshp *tdshp = dev_get_drvdata(dev);

	clk_disable_unprepare(tdshp->clk);
}

void mtk_tdshp_start(struct device *dev)
{
	struct mtk_disp_tdshp *tdshp = dev_get_drvdata(dev);

	writel(1, tdshp->regs + DISP_REG_TDSHP_START);
}

void mtk_tdshp_stop(struct device *dev)
{
	/* The MT6589 TDSHP has no stop bit; EN is cleared instead. */
}

static int mtk_tdshp_bind(struct device *dev, struct device *master, void *data)
{
	return 0;
}

static void mtk_tdshp_unbind(struct device *dev, struct device *master, void *data)
{
}

static const struct component_ops mtk_disp_tdshp_component_ops = {
	.bind	= mtk_tdshp_bind,
	.unbind = mtk_tdshp_unbind,
};

static int mtk_disp_tdshp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_tdshp *tdshp;
	int ret;

	tdshp = devm_kzalloc(dev, sizeof(*tdshp), GFP_KERNEL);
	if (!tdshp)
		return -ENOMEM;

	tdshp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tdshp->regs))
		return dev_err_probe(dev, PTR_ERR(tdshp->regs),
				     "Cannot get reg resource\n");

	tdshp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(tdshp->clk))
		return dev_err_probe(dev, PTR_ERR(tdshp->clk),
				     "Cannot get clocks\n");

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &tdshp->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "No mediatek,gce-client-reg\n");
#endif

	mtk_tdshp_matrix_init(tdshp);

	platform_set_drvdata(pdev, tdshp);

	ret = component_add(dev, &mtk_disp_tdshp_component_ops);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add component\n");

	return 0;
}

static void mtk_disp_tdshp_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_tdshp_component_ops);
}

static const struct of_device_id mtk_disp_tdshp_driver_dt_match[] = {
	{ .compatible = "mediatek,mt6589-disp-tdshp", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_disp_tdshp_driver_dt_match);

struct platform_driver mtk_disp_tdshp_driver = {
	.probe = mtk_disp_tdshp_probe,
	.remove = mtk_disp_tdshp_remove,
	.driver = {
		.name = "mediatek-disp-tdshp",
		.owner = THIS_MODULE,
		.of_match_table = mtk_disp_tdshp_driver_dt_match,
	},
};

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek Display Controller 2D Sharpness Processor Driver");
MODULE_LICENSE("GPL");
