// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017 MediaTek Inc.
 * Copyright (c) 2026 MT6589 adaptation
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

#define DISP_COLOR_CFG_MAIN			0x0400
#define DISP_COLOR_START_MT2701			0x0f00
#define DISP_COLOR_START_MT8167			0x0400
#define DISP_COLOR_START_MT8173			0x0c00
#define DISP_COLOR_START(comp)			((comp)->data->color_offset)
#define DISP_COLOR_WIDTH(comp)			(DISP_COLOR_START(comp) + 0x50)
#define DISP_COLOR_HEIGHT(comp)			(DISP_COLOR_START(comp) + 0x54)

#define COLOR_BYPASS_ALL			BIT(7)
#define COLOR_SEQ_SEL				BIT(13)
/* MT6589 specific: main control bit to enable the color module */
#define COLOR_MAIN_EN				BIT(29)

/* Additional registers needed by MT6589 */
#define DISP_COLOR_R2Y_EN			(DISP_COLOR_START_MT2701 + 0x60)	/* 0xf60 */
#define DISP_COLOR_R2Y_MATRIX_BASE		(DISP_COLOR_START_MT2701 + 0x64)	/* 0xf64 */
#define DISP_COLOR_CCOR_EN			(DISP_COLOR_START_MT2701 + 0xa0)	/* 0xfa0 */

struct mtk_disp_color *color;

struct mtk_disp_color_data {
	unsigned int color_offset;
	bool enable_main_bit29;		/* set BIT(29) in CFG_MAIN */
	void (*init)(struct mtk_disp_color *color);	/* extra init sequence */
};

struct mtk_disp_color {
	struct drm_crtc				*crtc;
	struct clk				*clk;
	void __iomem				*regs;
	struct cmdq_client_reg			cmdq_reg;
	const struct mtk_disp_color_data	*data;
};

int mtk_color_clk_enable(struct device *dev)
{
	struct mtk_disp_color *color = dev_get_drvdata(dev);

	return clk_prepare_enable(color->clk);
}

void mtk_color_clk_disable(struct device *dev)
{
	struct mtk_disp_color *color = dev_get_drvdata(dev);

	clk_disable_unprepare(color->clk);
}

void mtk_color_config(struct device *dev, unsigned int w,
		      unsigned int h, unsigned int vrefresh,
		      unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_color *color = dev_get_drvdata(dev);

	mtk_ddp_write(cmdq_pkt, w, &color->cmdq_reg, color->regs, DISP_COLOR_WIDTH(color));
	mtk_ddp_write(cmdq_pkt, h, &color->cmdq_reg, color->regs, DISP_COLOR_HEIGHT(color));
}

/*
 * MT6589 specific initialization:
 * - Enable the color module via BIT(29) in CFG_MAIN
 * - Setup the color space conversion matrix (BT.601 YUV->RGB)
 * - Enable R2Y and CCOR stages
 */
static void mt6589_color_init(struct mtk_disp_color *color)
{
	void __iomem *regs = color->regs;

	/* Color matrix coefficients (downstream values) */
	writel(306,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x00);
	writel(601,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x04);
	writel(117,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x08);
	writel(-173,  regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x0c);
	writel(-339,  regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x10);
	writel(512,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x14);
	writel(512,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x18);
	writel(-429,  regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x1c);
	writel(-83,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x20);
	/* The rest of the matrix (0x00 at 0x24..0x34) are already zero */

	writel(128,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x34);	/* offset R */
	writel(128,   regs + DISP_COLOR_R2Y_MATRIX_BASE + 0x38);	/* offset G */
	/* 0x00 at 0x3c already zero */

	/* Enable R2Y (YUV to RGB conversion) */
	writel(1, regs + DISP_COLOR_R2Y_EN);

	/* Enable color correction (CCOR) */
	writel(1, regs + DISP_COLOR_CCOR_EN);
}

void mtk_color_start(struct device *dev)
{
	struct mtk_disp_color *color = dev_get_drvdata(dev);
	u32 cfg_main = COLOR_BYPASS_ALL | COLOR_SEQ_SEL;

	if (color->data->enable_main_bit29)
		cfg_main |= COLOR_MAIN_EN;

	writel(cfg_main, color->regs + DISP_COLOR_CFG_MAIN);

	/* SoC-specific extra initialization */
	if (color->data->init)
		color->data->init(color);

	writel(0x1, color->regs + DISP_COLOR_START(color));
}

static int mtk_disp_color_bind(struct device *dev, struct device *master,
			       void *data)
{
	return 0;
}

static void mtk_disp_color_unbind(struct device *dev, struct device *master,
				  void *data)
{
}

static const struct component_ops mtk_disp_color_component_ops = {
	.bind	= mtk_disp_color_bind,
	.unbind = mtk_disp_color_unbind,
};

static int mtk_disp_color_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_color *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "failed to get color clk\n");

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "failed to ioremap color\n");
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "get mediatek,gce-client-reg fail!\n");
#endif

	priv->data = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, priv);

	ret = component_add(dev, &mtk_disp_color_component_ops);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add component\n");

	return 0;
}

static void mtk_disp_color_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_color_component_ops);
}

static const struct mtk_disp_color_data mt2701_color_driver_data = {
	.color_offset = DISP_COLOR_START_MT2701,
};

static const struct mtk_disp_color_data mt8167_color_driver_data = {
	.color_offset = DISP_COLOR_START_MT8167,
};

static const struct mtk_disp_color_data mt8173_color_driver_data = {
	.color_offset = DISP_COLOR_START_MT8173,
};

static const struct mtk_disp_color_data mt6589_color_driver_data = {
	.color_offset = DISP_COLOR_START_MT2701,	/* same as MT2701: 0x0f00 */
	.enable_main_bit29 = true,
	.init = mt6589_color_init,
};

static const struct of_device_id mtk_disp_color_driver_dt_match[] = {
	{ .compatible = "mediatek,mt2701-disp-color",
	  .data = &mt2701_color_driver_data},
	{ .compatible = "mediatek,mt8167-disp-color",
	  .data = &mt8167_color_driver_data},
	{ .compatible = "mediatek,mt8173-disp-color",
	  .data = &mt8173_color_driver_data},
	{ .compatible = "mediatek,mt6589-disp-color",
	  .data = &mt6589_color_driver_data},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_disp_color_driver_dt_match);

struct platform_driver mtk_disp_color_driver = {
	.probe		= mtk_disp_color_probe,
	.remove		= mtk_disp_color_remove,
	.driver		= {
		.name	= "mediatek-disp-color",
		.of_match_table = mtk_disp_color_driver_dt_match,
	},
};
