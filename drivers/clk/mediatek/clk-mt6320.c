// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: Flora Fu <flora.fu@mediatek.com>
 *
 * Author: Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6320/registers.h>
#include <linux/regmap.h>
#include <dt-bindings/clock/mediatek,mt6320-clk.h>

#include "clk-mtk.h"
#include "clk-gate.h"

static const struct mtk_gate_regs top_ckpdn_regs = {
	.sta_ofs = MT6320_TOP_CKPDN,
};
static const struct mtk_gate_regs top_ckpdn2_regs = {
	.sta_ofs = MT6320_TOP_CKPDN2,
};
static const struct mtk_gate_regs wrp_ckpdn_regs = {
	.sta_ofs = MT6320_WRP_CKPDN,
};

#define GATE_TOP(_id, _name, _parent, _shift) \
	GATE_MTK(_id, _name, _parent, &top_ckpdn_regs, _shift, &mtk_clk_gate_ops_no_setclr)

#define GATE_TOP2(_id, _name, _parent, _shift) \
	GATE_MTK(_id, _name, _parent, &top_ckpdn2_regs, _shift, &mtk_clk_gate_ops_no_setclr)

#define GATE_WRP(_id, _name, _parent, _shift) \
	GATE_MTK(_id, _name, _parent, &wrp_ckpdn_regs, _shift, &mtk_clk_gate_ops_no_setclr)

static const struct mtk_fixed_clk mt6320_fixed_clks[] = {
	FIXED_CLK(MT6320_CLK_AUD26M, "aud26m", NULL, 26000000),
	FIXED_CLK(MT6320_CLK_SMPS24M, "smps24m", NULL, 24000000),
	FIXED_CLK(MT6320_CLK_PMU75K, "pmu75k", NULL, 75000),
	FIXED_CLK(MT6320_CLK_PMU12M, "pmu12m", NULL, 12000000),
	FIXED_CLK(MT6320_CLK_FG32K, "fg32k", NULL, 32000),
	FIXED_CLK(MT6320_CLK_RTC32K, "rtc32k", NULL, 32000),
	FIXED_CLK(MT6320_CLK_CHR1M, "chr1m", NULL, 1000000),
};

static const struct mtk_fixed_factor mt6320_factors[] = {
	FACTOR(MT6320_CLK_AUD13M, "aud13m", "aud26m", 1, 2),
	FACTOR(MT6320_CLK_AUD26M_DIV64, "aud26m_div64", "aud26m", 1, 64),
	FACTOR(MT6320_CLK_SMPS12M, "smps12m", "smps24m", 1, 2),
	FACTOR(MT6320_CLK_SMPS6M, "smps6m", "smps24m", 1, 4),
	FACTOR(MT6320_CLK_SMPS3M, "smps3m", "smps24m", 1, 8),
	FACTOR(MT6320_CLK_SMPS2M, "smps2m", "smps24m", 1, 12),
	FACTOR(MT6320_CLK_SMPS1M, "smps1m", "smps24m", 1, 24),
};

static const struct mtk_gate mt6320_gates[] = {
	GATE_TOP(MT6320_TOPCKPDN_AUD_26M, "top-aud26m", "aud26m", 0),
	GATE_TOP(MT6320_TOPCKPDN_AUD_13M, "top-aud13m", "aud13m", 1),
	GATE_TOP(MT6320_TOPCKPDN_SPK_CK, "top-spk", "smps1m", 2),
	GATE_TOP(MT6320_TOPCKPDN_PWMOC_CK, "top-pwmoc", "smps2m", 3),
	GATE_TOP(MT6320_TOPCKPDN_EFUSE_CK, "top-efuse", "pmu75k", 4),
	GATE_TOP(MT6320_TOPCKPDN_FGADC_CK, "top-fgadc", "fg32k", 5),
	GATE_TOP(MT6320_TOPCKPDN_BST_DRV_1M_CK,"top-bstdrv1m", "smps1m", 7),
	GATE_TOP(MT6320_TOPCKPDN_SMPS_CK_DIV2, "top-smpsdiv2", "smps12m", 11),
	GATE_TOP(MT6320_TOPCKPDN_SMPS_CK_DIV, "top-smpsdiv", "smps24m", 12),
	GATE_TOP(MT6320_TOPCKPDN_STRUP_6M, "top-strup6m", "smps6m", 15),

	GATE_TOP2(MT6320_TOPCKPDN2_RTC32K_1V8, "top2-rtc32k1v8", "rtc32k", 0),
	GATE_TOP2(MT6320_TOPCKPDN2_STRUP_75K_CK, "top2-starup75k", "pmu75k", 2),
	GATE_TOP2(MT6320_TOPCKPDN2_RTC_32K_CK, "top2-rtc32k", "rtc32k", 3),
	GATE_TOP2(MT6320_TOPCKPDN2_PCHR_32K_CK, "top2-pchr32k", "rtc32k", 4),
	GATE_TOP2(MT6320_TOPCKPDN2_LDOSTB_1M_CK, "top2-ldostb1m", "smps1m", 5),
	GATE_TOP2(MT6320_TOPCKPDN2_INTRP_CK, "top2-intr", "pmu75k", 6),
	GATE_TOP2(MT6320_TOPCKPDN2_DRV_32K_CK, "top2-drv32k", "smps1m", 7),
	GATE_TOP2(MT6320_TOPCKPDN2_CHR1M_CK, "top2-chr1m", "chr1m", 8),
	GATE_TOP2(MT6320_TOPCKPDN2_BUCK_CK, "top2-buck", "pmu12m", 9),
	GATE_TOP2(MT6320_TOPCKPDN2_BUCK_ANA_CK, "top2-buckana", "smps2m", 10),
	GATE_TOP2(MT6320_TOPCKPDN2_BUCK32K, "top2-buck32k", "rtc32k", 11),
	GATE_TOP2(MT6320_TOPCKPDN2_BUCK_1M_CK, "top2-buck1m", "smps1m", 12),
	GATE_TOP2(MT6320_TOPCKPDN2_STRUP_32K_CK, "top2-starup32k", "rtc32k", 13),
	GATE_TOP2(MT6320_TOPCKPDN2_RTC_75K_CK, "top2-rtc75k", "pmu75k", 14),
	GATE_TOP2(MT6320_TOPCKPDN2_RSV_15, "top2-rsv", "smps24m",15),

	GATE_WRP(MT6320_WRPCKPDN_32K, "wrap-32k", "rtc32k", 6),
};

struct mt6320_clk_composite {
	struct clk_hw		hw;
	struct regmap		*regmap;
	u32			enable_reg;
	u32			enable_shift;
	u32			div_reg;
	u32			div_shift;
	u32			div_width;
	u32			div_base;
	u32			div_factor;
	u32			mux_reg;
	u32			mux_shift;
	u32			mux_width;
};

#define to_mt6320_composite(_hw) \
	container_of(_hw, struct mt6320_clk_composite, hw)

static int mt6320_composite_enable(struct clk_hw *hw)
{
	struct mt6320_clk_composite *c = to_mt6320_composite(hw);
	return regmap_clear_bits(c->regmap, c->enable_reg,
				 BIT(c->enable_shift));
}

static void mt6320_composite_disable(struct clk_hw *hw)
{
	struct mt6320_clk_composite *c = to_mt6320_composite(hw);
	regmap_set_bits(c->regmap, c->enable_reg, BIT(c->enable_shift));
}

static int mt6320_composite_is_enabled(struct clk_hw *hw)
{
	struct mt6320_clk_composite *c = to_mt6320_composite(hw);
	u32 val;
	regmap_read(c->regmap, c->enable_reg, &val);
	return !(val & BIT(c->enable_shift));
}

static unsigned long mt6320_composite_recalc_rate(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct mt6320_clk_composite *c = to_mt6320_composite(hw);
	u32 val, div;

	if (c->div_width == 0)
		return parent_rate;

	regmap_read(c->regmap, c->div_reg, &val);
	val >>= c->div_shift;
	val &= GENMASK(c->div_width - 1, 0);

	if (val)
		div = (1 << val) * c->div_base * c->div_factor;
	else
		div = c->div_base;

	return parent_rate / div;
}

static u8 mt6320_composite_get_parent(struct clk_hw *hw)
{
	struct mt6320_clk_composite *c = to_mt6320_composite(hw);
	int num_parents = clk_hw_get_num_parents(hw);
	u32 val;

	if (num_parents == 1)
		return 0;

	regmap_read(c->regmap, c->mux_reg, &val);
	val >>= c->mux_shift;
	val &= GENMASK(c->mux_width - 1, 0);

	if (val >= num_parents)
		return -EINVAL;

	return val;
}

static int mt6320_composite_set_parent(struct clk_hw *hw, u8 index)
{
	struct mt6320_clk_composite *c = to_mt6320_composite(hw);
	u32 mask = GENMASK(c->mux_width - 1, 0);

	return regmap_update_bits(c->regmap, c->mux_reg,
				 mask << c->mux_shift,
				 index << c->mux_shift);
}

static const struct clk_ops mt6320_composite_ops = {
	.enable		= mt6320_composite_enable,
	.disable	= mt6320_composite_disable,
	.is_enabled	= mt6320_composite_is_enabled,
	.recalc_rate	= mt6320_composite_recalc_rate,
	.get_parent	= mt6320_composite_get_parent,
	.set_parent	= mt6320_composite_set_parent,
	.determine_rate	= __clk_mux_determine_rate_closest,
};

struct mt6320_comp_desc {
	int id;
	const char *name;
	const char * const *parent_names;
	int num_parents;
	u32 enable_reg;
	u32 enable_shift;
	u32 div_reg;
	u32 div_shift;
	u32 div_width;
	u32 div_base;
	u32 div_factor;
	u32 mux_reg;
	u32 mux_shift;
	u32 mux_width;
};

#define COMP_GATE_DIV(_id, _name, _parent, _reg, _shift,		\
		 _div_reg, _div_shift, _div_width,			\
		 _div_base, _div_factor)				\
	{								\
		.id = _id, .name = _name,				\
		.parent_names = (const char * const []){ _parent },	\
		.num_parents = 1,					\
		.enable_reg = _reg, .enable_shift = _shift,		\
		.div_reg = _div_reg, .div_shift = _div_shift,		\
		.div_width = _div_width, .div_base = _div_base,		\
		.div_factor = _div_factor,				\
	}

#define COMP_MUX(_id, _name, _parents, _num_p, _reg, _shift,		\
		 _mux_reg, _mux_shift, _mux_width)			\
	{								\
		.id = _id, .name = _name,				\
		.parent_names = _parents,				\
		.num_parents = _num_p,					\
		.enable_reg = _reg, .enable_shift = _shift,		\
		.mux_reg = _mux_reg, .mux_shift = _mux_shift,		\
		.mux_width = _mux_width,				\
	}

static const char * const accdet_parents[] = {
	"rtc32k", "pmu12m", "top-accdet", "aud26m",
};
static const char * const fgadc_ana_parents[] = {
	"rtc32k", "aud26m_div64",
};
static const char * const fgmtr_parents[] = {
	"rtc32k", "aud26m",
};

static const struct mt6320_comp_desc mt6320_composites[] = {
	COMP_MUX(MT6320_TOPCKPDN_FGADC_ANA_CK, "top-fgadc-ana",
		 fgadc_ana_parents, ARRAY_SIZE(fgadc_ana_parents),
		 MT6320_TOP_CKPDN, 6,
		 MT6320_TOP_CKCON1, 12, 1),

	COMP_GATE_DIV(MT6320_TOPCKPDN_RTC_MCLK, "top-rtc-mclk",
		      "smps24m", MT6320_TOP_CKPDN, 8,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),

	COMP_GATE_DIV(MT6320_TOPCKPDN_SPK_PWM_DIV, "top-spkpwm",
		      "smps1m", MT6320_TOP_CKPDN, 9,
		      MT6320_TOP_CKCON2, 3, 2, 1, 8),

	COMP_GATE_DIV(MT6320_TOPCKPDN_SPK_DIV, "top-spkdiv",
		      "smps1m", MT6320_TOP_CKPDN, 10,
		      MT6320_TOP_CKCON2, 5, 2, 1, 1),

	COMP_GATE_DIV(MT6320_TOPCKPDN_AUXADC_CK, "top-auxadc",
		      "pmu12m", MT6320_TOP_CKPDN, 13,
		      MT6320_TOP_CKCON2, 0, 2, 6, 1),

	COMP_MUX(MT6320_TOPCKPDN_ACCDET_CK, "top-accdet",
		 accdet_parents, ARRAY_SIZE(accdet_parents),
		 MT6320_TOP_CKPDN, 14,
		 MT6320_TOP_CKCON1, 13, 2),

	COMP_MUX(MT6320_TOPCKPDN2_FQMTR, "top2-fqmtr",
		 fgmtr_parents, ARRAY_SIZE(fgmtr_parents),
		 MT6320_TOP_CKPDN2, 1,
		 MT6320_TOP_CKCON1, 15, 1),

	/* WRAP clocks (gate + divider only) */
	COMP_GATE_DIV(MT6320_WRPCKPDN_I2C0, "wrap-i2c0",
		      "smps24m", MT6320_WRP_CKPDN, 0,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),
	COMP_GATE_DIV(MT6320_WRPCKPDN_I2C1, "wrap-i2c1",
		      "smps24m", MT6320_WRP_CKPDN, 1,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),
	COMP_GATE_DIV(MT6320_WRPCKPDN_I2C2, "wrap-i2c2",
		      "smps24m", MT6320_WRP_CKPDN, 2,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),
	COMP_GATE_DIV(MT6320_WRPCKPDN_PWM, "wrap-pwm",
		      "smps24m", MT6320_WRP_CKPDN, 3,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),
	COMP_GATE_DIV(MT6320_WRPCKPDN_KP, "wrap-kp",
		      "smps24m", MT6320_WRP_CKPDN, 4,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),
	COMP_GATE_DIV(MT6320_WRPCKPDN_EINT, "wrap-eint",
		      "smps24m", MT6320_WRP_CKPDN, 5,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),
	COMP_GATE_DIV(MT6320_WRPCKPDN_WRP, "wrap-wrp",
		      "smps24m", MT6320_WRP_CKPDN, 7,
		      MT6320_TOP_CKCON2, 14, 2, 1, 1),
};

static const struct of_device_id mt6320_clk_match_table[] = {
	{ .compatible = "mediatek,mt6320-clk" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6320_clk_match_table);

static int mt6320_clk_probe(struct platform_device *pdev)
{
	int i, ret;
	struct device *dev = &pdev->dev;
	struct mt6397_chip *mt6320 = dev_get_drvdata(pdev->dev.parent);
	struct clk_hw_onecell_data *clk_data;

	clk_data = mtk_alloc_clk_data(MT6320_CLK_MAX);
	if (!clk_data)
		return -ENOMEM;

	ret = mtk_clk_register_fixed_clks(mt6320_fixed_clks,
					  ARRAY_SIZE(mt6320_fixed_clks),
					  clk_data);
	if (ret)
		goto free_data;

	ret = mtk_clk_register_factors(mt6320_factors,
				       ARRAY_SIZE(mt6320_factors),
				       clk_data);
	if (ret)
		goto unreg_fixed;

	ret = mtk_clk_register_gates(dev, dev->of_node,
				     mt6320_gates,
				     ARRAY_SIZE(mt6320_gates),
				     clk_data);
	if (ret)
		goto unreg_factors;

	for (i = 0; i < ARRAY_SIZE(mt6320_composites); i++) {
		const struct mt6320_comp_desc *d = &mt6320_composites[i];
		struct mt6320_clk_composite *c;
		struct clk_init_data init = {};

		c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
		if (!c) {
			ret = -ENOMEM;
			goto unreg_composites;
		}

		init.name = d->name;
		init.ops = &mt6320_composite_ops;
		init.parent_names = d->parent_names;
		init.num_parents = d->num_parents;
		init.flags = CLK_IGNORE_UNUSED;

		c->regmap = mt6320->regmap;
		c->enable_reg = d->enable_reg;
		c->enable_shift = d->enable_shift;
		c->div_reg = d->div_reg;
		c->div_shift = d->div_shift;
		c->div_width = d->div_width;
		c->div_base = d->div_base;
		c->div_factor = d->div_factor;
		c->mux_reg = d->mux_reg;
		c->mux_shift = d->mux_shift;
		c->mux_width = d->mux_width;
		c->hw.init = &init;

		ret = clk_hw_register(dev, &c->hw);
		if (ret) {
			dev_err(dev, "failed to register composite %s\n", d->name);
			goto unreg_composites;
		}
		clk_data->hws[d->id] = &c->hw;
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
	if (ret) {
		dev_err(dev, "failed to add clock provider\n");
		goto unreg_composites;
	}

	platform_set_drvdata(pdev, clk_data);
	return 0;

unreg_composites:
	while (--i >= 0)
		clk_hw_unregister(clk_data->hws[mt6320_composites[i].id]);
	mtk_clk_unregister_gates(mt6320_gates, ARRAY_SIZE(mt6320_gates), clk_data);
unreg_factors:
	mtk_clk_unregister_factors(mt6320_factors, ARRAY_SIZE(mt6320_factors), clk_data);
unreg_fixed:
	mtk_clk_unregister_fixed_clks(mt6320_fixed_clks, ARRAY_SIZE(mt6320_fixed_clks), clk_data);
free_data:
	mtk_free_clk_data(clk_data);
	return ret;
}

static void mt6320_clk_remove(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *clk_data = platform_get_drvdata(pdev);
	int i;

	of_clk_del_provider(pdev->dev.of_node);

	for (i = ARRAY_SIZE(mt6320_composites) - 1; i >= 0; i--)
		clk_hw_unregister(clk_data->hws[mt6320_composites[i].id]);

	mtk_clk_unregister_gates(mt6320_gates, ARRAY_SIZE(mt6320_gates), clk_data);
	mtk_clk_unregister_factors(mt6320_factors, ARRAY_SIZE(mt6320_factors), clk_data);
	mtk_clk_unregister_fixed_clks(mt6320_fixed_clks, ARRAY_SIZE(mt6320_fixed_clks), clk_data);
	mtk_free_clk_data(clk_data);
}

static struct platform_driver mt6320_clk_driver = {
	.probe = mt6320_clk_probe,
	.remove = mt6320_clk_remove,
	.driver = {
		.name = "mt6320-clk",
		.of_match_table = mt6320_clk_match_table,
	},
};
module_platform_driver(mt6320_clk_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("Clock Driver for MediaTek MT6320 PMIC");
MODULE_LICENSE("GPL");
