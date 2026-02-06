// SPDX-License-Identifier: GPL-2.0+

#include "clk-mux.h"
#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-pll.h"

#include <dt-bindings/clock/mediatek,mt6572-clk.h>

#define ARMPLL_OFFSET		0x100
#define MAINPLL_OFFSET		0x120
#define UNIVPLL_OFFSET		0x140
#define WHPLL_OFFSET		0x240

#define WHPLL_PATHSEL_CON 	0x254
#define RSV_RW0_CON1		0xf04

#define REG_CON0	0x0
#define REG_CON1	0x4
#define REG_PWR_CON0	0x10

#define CON0_RST_BAR	BIT(27)

#define PLL_DIV(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, _fmin, \
		_fmax, _div)                                               \
	{ .id = _id,                                                       \
	  .name = _name,                                                   \
	  .parent_name = "clk26m",                                         \
	  .reg = (_base) + REG_CON0,                                       \
	  .pwr_reg = (_base) + REG_PWR_CON0,                               \
	  .en_mask = _en_mask,                                             \
	  .rst_bar_mask = _rst_bar_mask,                                   \
	  .pd_reg = (_base) + REG_CON1,                                    \
	  .pd_shift = 24,                                                  \
	  .pcw_reg = (_base) + REG_CON1,                                   \
	  .pcw_chg_reg = (_base) + REG_CON1,                               \
	  .pcwbits = 21,                                                   \
	  .flags = _flags,                                                 \
	  .fmin = _fmin,                                                   \
	  .fmax = _fmax,                                                   \
	  .div_table = _div }

#define PLL_FREQ(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, _fmin, _fmax) \
	PLL_DIV(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, _fmin, _fmax, NULL)

#define PLL(_id, _name, _base, _en_mask, _rst_bar_mask, _flags) \
	PLL_DIV(_id, _name, _base, _en_mask, _rst_bar_mask, _flags, 0, 0, NULL)

static const struct mtk_pll_div_table armpll_div_table[] = {
    { .div = 0, .freq = 1989 * MHZ },
    { .div = 1, .freq = 1001 * MHZ },
    { .div = 2, .freq = 520 * MHZ },
    { .div = 3, .freq = 260 * MHZ },
    { .div = 4, .freq = 130 * MHZ },
    { /* sentinel */ }
};

static const struct mtk_pll_data apmixedsys_plls[] = {
	PLL_DIV(CLK_APMIXED_ARMPLL, "armpll", ARMPLL_OFFSET, 0x00000011, 0,
		PLL_AO, 1000 * MHZ, 1800 * MHZ, armpll_div_table),

	PLL_FREQ(CLK_APMIXED_MAINPLL, "mainpll", MAINPLL_OFFSET, 0x00000011,
	    CON0_RST_BAR, PLL_AO | HAVE_RST_BAR, 1000 * MHZ, 1800 * MHZ),

	//PLL_FREQ(CLK_APMIXED_UNIVPLL, "univpll", UNIVPLL_OFFSET, 0x30000011,
	//    CON0_RST_BAR, HAVE_RST_BAR, 1248 * MHZ, 1248 * MHZ),

	PLL_DIV(CLK_APMIXED_WHPLL, "whpll", WHPLL_OFFSET, 0x00000011, 0, 0,
		754 * MHZ, 1508 * MHZ, armpll_div_table),
};

static int clk_mt6572_apmixed_probe(struct platform_device *pdev)
{
	void __iomem *base;
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct clk_hw_onecell_data *clk_data;
	int ret;

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	/*
	 * WHPLL requires WHPLL_PATHSEL_CON and RSV_RW0_CON1 setup to enable
	 * output. Otherwise the GPU will get 0 Hz and hang.
	 */
	writel(1, base + WHPLL_PATHSEL_CON);
	writel(0xC0000000, base + RSV_RW0_CON1);

	clk_data = mtk_devm_alloc_clk_data(&pdev->dev, ARRAY_SIZE(apmixedsys_plls));
	if (!clk_data)
		return -ENOMEM;
	platform_set_drvdata(pdev, clk_data);

	ret = mtk_clk_register_plls(&pdev->dev, apmixedsys_plls,
				   ARRAY_SIZE(apmixedsys_plls), clk_data);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register PLLs: %d\n", ret);
		return ret;
	}

	ret = devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get,
					  clk_data);
	if (ret)
		dev_err(&pdev->dev,
			"Failed to register clock provider: %d\n", ret);

	return ret;
}

static void clk_mt6572_apmixed_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);

	mtk_clk_unregister_plls(apmixedsys_plls, ARRAY_SIZE(apmixedsys_plls), clk_data);
}

static const struct of_device_id of_match_mt6572_apmixedsys[] = {
	{ .compatible = "mediatek,mt6572-apmixedsys" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt6572_apmixedsys);

static struct platform_driver clk_mt6572_apmixedsys = {
	.probe = clk_mt6572_apmixed_probe,
	.remove = clk_mt6572_apmixed_remove,
	.driver = {
		.name = "clk-mt6572-apmixedsys",
		.of_match_table = of_match_mt6572_apmixedsys,
	},
};
module_platform_driver(clk_mt6572_apmixedsys);

MODULE_DESCRIPTION("MediaTek MT6572 apmixedsys clock driver");
MODULE_LICENSE("GPL");
