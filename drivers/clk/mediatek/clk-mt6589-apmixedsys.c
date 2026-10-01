// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * Based on clk-mt2712-apmixedsys.c
 * Copyright (c) 2017 MediaTek Inc.
 *                    Weiyi Lu <weiyi.lu@mediatek.com>
 * Copyright (c) 2023 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/container_of.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "clk-pll.h"
#include "clk-pllfh.h"
#include "clk-fhctl.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt6589-clk.h>

#define AP_PLL_CON0	0x0000
#define AP_PLL_CON1	0x0004
#define AP_PLL_CON2	0x0008
#define AP_PLL_CON3	0x000c

#define PLL_HP_CON0	0x0014

#define ARMPLL_CON0	0x0200
#define ARMPLL_CON1	0x0204
#define ARMPLL_CON2	0x0208
#define ARMPLL_PWR_CON0	0x0218

#define MAINPLL_CON0	0x021c
#define MAINPLL_CON1	0x0220
#define MAINPLL_CON2	0x0224
#define MAINPLL_PWR_CON0	0x0234

#define UNIVPLL_CON0	0x0238
#define MMPLL_CON0	0x0240
#define ISPPLL_CON0	0x0248

#define MSDCPLL_CON0	0x0250
#define MSDCPLL_CON1	0x0254
#define MSDCPLL_CON2	0x0258
#define MSDCPLL_PWR_CON0	0x0268

#define TVDPLL_CON0	0x026c
#define TVDPLL_CON1	0x0270
#define TVDPLL_CON2	0x0274
#define TVDPLL_CON3	0x0278
#define TVDPLL_PWR_CON0	0x0284

#define LVDSPLL_CON0	0x0288
#define LVDSPLL_CON1	0x028c
#define LVDSPLL_CON2	0x0290
#define LVDSPLL_CON3	0x0294
#define LVDSPLL_PWR_CON0	0x02a0

#define VOID_REG	0x0

#define CON0_MT6589_RST_BAR	BIT(27)

#define PLL(_id, _name, _reg, _pwr_reg, _en_mask, _flags, _pcwbits,	\
			_pd_reg, _pd_shift, _pcw_reg, _pcw_shift,	\
			_ops, _fmax, _div_table) {			\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = CON0_MT6589_RST_BAR,			\
		.fmax = _fmax,						\
		.pcwbits = _pcwbits,					\
		.pd_reg = _pd_reg,					\
		.pd_mask = 0x3,						\
		.pd_valid_mask = GENMASK(2, 0),			\
		.pd_shift = _pd_shift,					\
		.tuner_reg = VOID_REG,					\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
		.div_table = _div_table,				\
		.ops = _ops,						\
	}

static int mt6589_lc_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct mtk_clk_pll *pll = to_mtk_clk_pll(hw);
	u32 pcw = 0;
	u32 postdiv;
	u32 mask, pcw_mask, rate_mask, old_val, val;
	bool prepared;
	int ret, rollback_ret;

	mtk_pll_calc_values(pll, &pcw, &postdiv, rate, parent_rate);
	prepared = mtk_pll_is_prepared(hw);

	mask = pll->data->pd_mask ?: 0x3;
	pcw_mask = GENMASK(pll->data->pcw_shift +
			   pll->data->pcwbits - 1,
			   pll->data->pcw_shift);
	rate_mask = (mask << pll->data->pd_shift) | pcw_mask;
	old_val = readl(pll->base_addr);

	if (prepared)
		mtk_pll_unprepare(hw);

	/* LC PLL: write directly to CON0, no PCW_CHG trigger */
	val = old_val;
	/* Clear postdiv field (2 bits) */
	val &= ~(mask << pll->data->pd_shift);
	/* Clear FBKDIV field (pcwbits, from pcw_shift) */
	val &= ~pcw_mask;
	/* Set new postdiv and pcw */
	val |= ((ffs(postdiv) - 1) << pll->data->pd_shift);
	val |= (pcw << pll->data->pcw_shift);

	writel(val, pll->base_addr);
	udelay(20); /* stabilize */

	if (prepared) {
		ret = mtk_pll_prepare(hw);
		if (!ret)
			return 0;

		/*
		 * set_rate must not leave the CCF-visible prepared state
		 * different from the hardware state when re-prepare fails.
		 */
		mtk_pll_unprepare(hw);

		/* Restore the old rate while the PLL is known to be off. */
		val = readl(pll->base_addr);
		val &= ~rate_mask;
		val |= old_val & rate_mask;
		writel(val, pll->base_addr);

		/*
		 * Restore the state expected by the caller. If even rollback
		 * preparation fails, leave the PLL unprepared rather than
		 * pretending that the state was recovered.
	 */
		rollback_ret = mtk_pll_prepare(hw);
		if (rollback_ret) {
			mtk_pll_unprepare(hw);
		}

		return ret;
	}

	return 0;
}

static const struct clk_ops mt6589_lc_pll_ops = {
	.is_prepared	= mtk_pll_is_prepared,
	.prepare		= mtk_pll_prepare,
	.unprepare		= mtk_pll_unprepare,
	.recalc_rate	= mtk_pll_recalc_rate,
	.determine_rate	= mtk_pll_determine_rate,
	.set_rate		= mt6589_lc_pll_set_rate,
};

static const struct clk_ops mt6589_fixed_lc_pll_ops = {
	.is_prepared	= mtk_pll_is_prepared,
	.prepare		= mtk_pll_prepare,
	.unprepare		= mtk_pll_unprepare,
	.recalc_rate	= mtk_pll_recalc_rate,
	.determine_rate	= mtk_pll_determine_rate,
	/* no .set_rate */
};

/*
 * MT6589 frequency hopping / spread spectrum controller.
 *
 * The FHCTL lives in its own address range (see the "mediatek,
 * mt6589-fhctl" node); it can take over five of the SDM PLLs
 * (ARMPLL, MAINPLL, MSDCPLL, TVDPLL and LVDSPLL).  Hopping is
 * started by writing the target NCPO with bit 31 set into the
 * channel's DDS register, which the common code does through the
 * dvfs register alias.
 */
/*
 * MT6589 FHCTL channel mapping (from mt_freqhopping.h / mt_fhreg.h):
 *
 *   CH0  offset 0x4c  PLL_HP_CON0 bit 0  ARMPLL
 *   CH1  offset 0x5c  PLL_HP_CON0 bit 1  MAINPLL
 *   CH2  offset 0x6c  PLL_HP_CON0 bit 2  MEMPLL  (not an SDM PLL, not registered)
 *   CH3  offset 0x7c  PLL_HP_CON0 bit 3  MSDCPLL
 *   CH4  offset 0x8c  PLL_HP_CON0 bit 4  TVDPLL
 *   CH5  offset 0x9c  PLL_HP_CON0 bit 5  LVDSPLL
 *
 * MEMPLL occupies CH2 but is not registered here, so MSDCPLL/TVDPLL/LVDSPLL
 * start at CH3/CH4/CH5.  fh_id is the PLL_HP_CON0 bit index; fhx_offset is
 * the byte offset of FHCTLn_CFG within the FHCTL MMIO window.
 */
enum fh_pll_id {
	FH_ARMPLL  = 0,
	FH_MAINPLL = 1,
	/* CH2 = MEMPLL, not registered */
	FH_MSDCPLL = 3,
	FH_TVDPLL  = 4,
	FH_LVDSPLL = 5,
};

#define _FH(_pllid, _fhid, _offset) {					\
		.data = {						\
			.pll_id = _pllid,				\
			.fh_id = _fhid,					\
			.fh_ver = FHCTL_PLLFH_V3,			\
			.fhx_offset = _offset,				\
			.dds_mask = GENMASK(20, 0),			\
			.slope0_value = 0x6003c97,			\
			.slope1_value = 0x6003c97,			\
			.sfstrx_en = BIT(2),				\
			.frddsx_en = BIT(1),				\
			.fhctlx_en = BIT(0),				\
			.tgl_org = BIT(31),				\
			.dvfs_tri = BIT(31),				\
			.pcwchg = BIT(31),				\
			.dt_val = 0x0,					\
			.df_val = 0x9,					\
			.updnlmt_shft = 16,				\
			.msk_frddsx_dys = GENMASK(23, 20),		\
			.msk_frddsx_dts = GENMASK(19, 16),		\
		},							\
	}

static struct mtk_pllfh_data pllfhs[] = {
//	_FH(CLK_APMIXED_ARMPLL,  FH_ARMPLL,  0x4c),	/* CH0 */
//	_FH(CLK_APMIXED_MAINPLL, FH_MAINPLL, 0x5c),	/* CH1 */
//	/* CH2 = MEMPLL, not registered */
//	_FH(CLK_APMIXED_MSDCPLL, FH_MSDCPLL, 0x7c),	/* CH3 */
//	_FH(CLK_APMIXED_TVDPLL,  FH_TVDPLL,  0x8c),	/* CH4 */
//	_FH(CLK_APMIXED_LVDSPLL, FH_LVDSPLL, 0x9c),	/* CH5 */
};

static const struct mtk_pll_div_table mt6589_isppll_div_table[] = {
	{ .div = 0, .freq = 1664 * MHZ },
	{ .div = 1, .freq = 1000 * MHZ },
	{ .div = 2, .freq = 500 * MHZ },
	{ }
};

/*
 * TVDPLL_MODE is a dedicated output divider in TVDPLL_CON0[23:22].
 *
 * 00: /2
 * 01: /4
 * 10: /8
 * 11: /16
 */
static const struct clk_div_table mt6589_tvdpll_mode_div_table[] = {
	{ .val = 0, .div = 2 },
	{ .val = 1, .div = 4 },
	{ .val = 2, .div = 8 },
	{ .val = 3, .div = 16 },
	{ }
};

static const struct mtk_pll_data plls[] = {
	/* *_OUT_EN / *_XXXM_EN are modeled as child gates below. */
	PLL(CLK_APMIXED_ARMPLL, "armpll", ARMPLL_CON0, ARMPLL_PWR_CON0, BIT(0),
		PLL_AO, 21, ARMPLL_CON1, 24, ARMPLL_CON1, 0, NULL, 1508 * MHZ, NULL),
	PLL(CLK_APMIXED_MAINPLL, "mainpll", MAINPLL_CON0, MAINPLL_PWR_CON0, BIT(0),
		HAVE_RST_BAR, 21, MAINPLL_CON0, 6, MAINPLL_CON1, 0, NULL, 1768 * MHZ, NULL),
	PLL(CLK_APMIXED_UNIVPLL, "univpll", UNIVPLL_CON0, VOID_REG, BIT(0),
		HAVE_RST_BAR, 7, UNIVPLL_CON0, 6, UNIVPLL_CON0, 8,
		&mt6589_fixed_lc_pll_ops, 1248 * MHZ, NULL),
	PLL(CLK_APMIXED_MMPLL, "mmpll", MMPLL_CON0, VOID_REG, BIT(0),
		HAVE_RST_BAR, 7, MMPLL_CON0, 6, MMPLL_CON0, 8,
		&mt6589_fixed_lc_pll_ops, 1690 * MHZ, NULL),
	PLL(CLK_APMIXED_ISPPLL, "isppll", ISPPLL_CON0, VOID_REG, BIT(0),
		0, 7, ISPPLL_CON0, 6, ISPPLL_CON0, 8,
		&mt6589_lc_pll_ops, 1664 * MHZ, mt6589_isppll_div_table),
	PLL(CLK_APMIXED_MSDCPLL, "msdcpll", MSDCPLL_CON0, MSDCPLL_PWR_CON0, BIT(0),
		0, 21, MSDCPLL_CON0, 6, MSDCPLL_CON1, 0, NULL, 1664 * MHZ, NULL),
	PLL(CLK_APMIXED_TVDPLL,  "tvdpll",  TVDPLL_CON0, TVDPLL_PWR_CON0, BIT(0),
		0, 21, TVDPLL_CON0, 6, TVDPLL_CON1, 0, NULL, 2376UL * MHZ, NULL),
	PLL(CLK_APMIXED_LVDSPLL, "lvdspll", LVDSPLL_CON0, LVDSPLL_PWR_CON0, BIT(0),
		0, 21, LVDSPLL_CON0, 6, LVDSPLL_CON1, 0, NULL, 1440 * MHZ, NULL),
};

struct mt6589_apmixed_output {
	int id;
	const char *name;
	const char *parent_name;
	u32 div;
	u32 reg;
	u8 div_shift;
	u8 gate_shift;
	const struct clk_div_table *div_table;
	u8 width;
};

#define APMIXED_OUTPUT(_id, _name, _parent, _div, _reg, _gate_shift) \
	{ \
		.id = _id, .name = _name, .parent_name = _parent, .div = _div, \
		.reg = _reg, .div_shift = 0, .gate_shift = _gate_shift, \
		.div_table = NULL, .width = 0, \
	}

#define APMIXED_OUTPUT_DIVIDER(_id, _name, _parent, \
		       _reg, _div_shift, _gate_shift, _width, _table) \
	{ \
		.id = _id, .name = _name, .parent_name = _parent, .div = 1, \
		.reg = _reg, .div_shift = _div_shift, \
		.gate_shift = _gate_shift, \
		.div_table = _table, \
		.width = _width, \
	}

static const struct mt6589_apmixed_output apmixed_outputs[] = {
	APMIXED_OUTPUT(CLK_APMIXED_ARMPLL_1300M, "armpll_1300m",
		       "armpll", 1, ARMPLL_CON0, 31),

	APMIXED_OUTPUT(CLK_APMIXED_MAINPLL_806M, "mainpll_806m",
		       "mainpll", 2, MAINPLL_CON0, 31),
	APMIXED_OUTPUT(CLK_APMIXED_MAINPLL_537P3M, "mainpll_537p3m",
		       "mainpll", 3, MAINPLL_CON0, 30),
	APMIXED_OUTPUT(CLK_APMIXED_MAINPLL_322P4M, "mainpll_322p4m",
		       "mainpll", 5, MAINPLL_CON0, 29),
	APMIXED_OUTPUT(CLK_APMIXED_MAINPLL_230P3M, "mainpll_230p3m",
		       "mainpll", 7, MAINPLL_CON0, 28),

	APMIXED_OUTPUT(CLK_APMIXED_UNIVPLL_624M, "univpll_624m",
		       "univpll", 2, UNIVPLL_CON0, 31),
	APMIXED_OUTPUT(CLK_APMIXED_UNIVPLL_416M, "univpll_416m",
		       "univpll", 3, UNIVPLL_CON0, 30),
	APMIXED_OUTPUT(CLK_APMIXED_UNIVPLL_249P6M, "univpll_249p6m",
		       "univpll", 5, UNIVPLL_CON0, 29),
	APMIXED_OUTPUT(CLK_APMIXED_UNIVPLL_178P3M, "univpll_178p3m",
		       "univpll", 7, UNIVPLL_CON0, 28),
	APMIXED_OUTPUT(CLK_APMIXED_UNIVPLL_48M, "univpll_48m",
		       "univpll", 26, UNIVPLL_CON0, 25),
	APMIXED_OUTPUT(CLK_APMIXED_UNIVPLL_USB_48M, "univpll_usb_48m",
		       "univpll", 26, UNIVPLL_CON0, 24),

	APMIXED_OUTPUT(CLK_APMIXED_MMPLL_D2, "mmpll_d2",
		       "mmpll", 2, MMPLL_CON0, 31),
	APMIXED_OUTPUT(CLK_APMIXED_MMPLL_D3, "mmpll_d3",
		       "mmpll", 3, MMPLL_CON0, 30),
	APMIXED_OUTPUT(CLK_APMIXED_MMPLL_D5, "mmpll_d5",
		       "mmpll", 5, MMPLL_CON0, 29),
	APMIXED_OUTPUT(CLK_APMIXED_MMPLL_D7, "mmpll_d7",
		       "mmpll", 7, MMPLL_CON0, 28),

	APMIXED_OUTPUT(CLK_APMIXED_ISPPLL_208M, "isppll_208m",
		       "isppll", 2, ISPPLL_CON0, 31),

	APMIXED_OUTPUT(CLK_APMIXED_MSDCPLL_208M, "msdcpll_208m",
		       "msdcpll", 2, MSDCPLL_CON0, 31),

	APMIXED_OUTPUT_DIVIDER(CLK_APMIXED_TVDPLL_148P5M, "tvdpll_148p5m",
			       "tvdpll",
			       TVDPLL_CON0, 22, 31, 2,
			       mt6589_tvdpll_mode_div_table),

	APMIXED_OUTPUT(CLK_APMIXED_LVDSPLL_180M, "lvdspll_180m",
		       "lvdspll", 2, LVDSPLL_CON0, 31),
};

static DEFINE_SPINLOCK(mt6589_apmixed_clk_lock);

struct mt6589_apmixed_output_hw {
	struct clk_gate gate;
	const struct mt6589_apmixed_output *data;
};

static inline struct mt6589_apmixed_output_hw *
to_mt6589_apmixed_output_hw(struct clk_hw *hw)
{
	return container_of(hw, struct mt6589_apmixed_output_hw, gate.hw);
}

static int mt6589_apmixed_output_enable(struct clk_hw *hw)
{
	return clk_gate_ops.enable(hw);
}

static void mt6589_apmixed_output_disable(struct clk_hw *hw)
{
	clk_gate_ops.disable(hw);
}

static int mt6589_apmixed_output_is_enabled(struct clk_hw *hw)
{
	return clk_gate_ops.is_enabled(hw);
}

static unsigned long
mt6589_apmixed_output_recalc_rate(struct clk_hw *hw,
				  unsigned long parent_rate)
{
	struct mt6589_apmixed_output_hw *output_hw =
		to_mt6589_apmixed_output_hw(hw);
	const struct mt6589_apmixed_output *output = output_hw->data;
	u32 val;

	if (!output->div_table)
		return parent_rate / output->div;

	val = readl(output_hw->gate.reg) >> output->div_shift;
	val &= clk_div_mask(output->width);

	return divider_recalc_rate(hw, parent_rate, val, output->div_table,
				   0, output->width);
}

static int
mt6589_apmixed_output_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	struct mt6589_apmixed_output_hw *output_hw =
		to_mt6589_apmixed_output_hw(hw);
	const struct mt6589_apmixed_output *output = output_hw->data;

	if (output->div_table)
		return divider_determine_rate(hw, req, output->div_table,
					     output->width, 0);

	if (clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT) {
		unsigned long best_parent;

		best_parent = req->rate * output->div;
		req->best_parent_rate =
			clk_hw_round_rate(req->best_parent_hw, best_parent);
	}

	req->rate = req->best_parent_rate / output->div;

	return 0;
}

static int
mt6589_apmixed_output_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct mt6589_apmixed_output_hw *output_hw =
		to_mt6589_apmixed_output_hw(hw);
	const struct mt6589_apmixed_output *output = output_hw->data;
	unsigned long flags;
	int value;
	u32 val;

	if (!output->div_table)
		return 0;

	value = divider_get_val(rate, parent_rate, output->div_table,
				output->width, 0);
	if (value < 0)
		return value;

	spin_lock_irqsave(output_hw->gate.lock, flags);

	val = readl(output_hw->gate.reg);
	val &= ~(clk_div_mask(output->width) << output->div_shift);
	val |= (u32)value << output->div_shift;
	writel(val, output_hw->gate.reg);

	spin_unlock_irqrestore(output_hw->gate.lock, flags);

	return 0;
}

static unsigned long
mt6589_apmixed_output_recalc_accuracy(struct clk_hw *hw,
				      unsigned long parent_accuracy)
{
	return parent_accuracy;
}

static const struct clk_ops mt6589_apmixed_output_ops = {
	.enable		= mt6589_apmixed_output_enable,
	.disable	= mt6589_apmixed_output_disable,
	.is_enabled	= mt6589_apmixed_output_is_enabled,
	.recalc_rate	= mt6589_apmixed_output_recalc_rate,
	.determine_rate	= mt6589_apmixed_output_determine_rate,
	.set_rate	= mt6589_apmixed_output_set_rate,
	.recalc_accuracy = mt6589_apmixed_output_recalc_accuracy,
};

struct mt6589_apmixed_priv {
	struct clk_hw_onecell_data *clk_data;
	struct mt6589_apmixed_output_hw *output_hws[ARRAY_SIZE(apmixed_outputs)];
};

static void mt6589_apmixed_unregister_outputs(struct mt6589_apmixed_priv *priv)
{
	int i;

	for (i = ARRAY_SIZE(apmixed_outputs) - 1; i >= 0; i--) {
		const struct mt6589_apmixed_output *output = &apmixed_outputs[i];
		struct mt6589_apmixed_output_hw *output_hw =
			priv->output_hws[i];

		if (!output_hw)
			continue;

		clk_hw_unregister(&output_hw->gate.hw);
		kfree(output_hw);
		priv->output_hws[i] = NULL;
		priv->clk_data->hws[output->id] = ERR_PTR(-ENOENT);
	}
}

static int mt6589_apmixed_register_outputs(struct device *dev,
					   void __iomem *base,
					   struct mt6589_apmixed_priv *priv)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(apmixed_outputs); i++) {
		const struct mt6589_apmixed_output *output = &apmixed_outputs[i];
		struct mt6589_apmixed_output_hw *output_hw;
		struct clk_init_data init = {};
		int ret;

		output_hw = kzalloc_obj(*output_hw);
		if (!output_hw) {
			mt6589_apmixed_unregister_outputs(priv);
			return -ENOMEM;
		}

		output_hw->data = output;
		output_hw->gate.reg = base + output->reg;
		output_hw->gate.bit_idx = output->gate_shift;
		output_hw->gate.flags = 0;
		output_hw->gate.lock = &mt6589_apmixed_clk_lock;
		output_hw->gate.hw.init = &init;

		init.name = output->name;
		init.ops = &mt6589_apmixed_output_ops;
		init.flags = CLK_SET_RATE_PARENT;
		init.parent_names = &output->parent_name;
		init.num_parents = 1;

		ret = clk_hw_register(dev, &output_hw->gate.hw);
		if (ret) {
			kfree(output_hw);
			mt6589_apmixed_unregister_outputs(priv);
			return ret;
		}

		priv->output_hws[i] = output_hw;
		priv->clk_data->hws[output->id] = &output_hw->gate.hw;
	}

	return 0;
}

static int clk_mt6589_apmixed_probe(struct platform_device *pdev)
{
	const u8 *fhctl_node = "mediatek,mt6589-fhctl";
	struct mt6589_apmixed_priv *priv;
	struct clk_hw_onecell_data *clk_data;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	int r;

	clk_data = mtk_alloc_clk_data(CLK_APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		r = -ENOMEM;
		goto free_clk_data;
	}
	priv->clk_data = clk_data;

	fhctl_parse_dt(fhctl_node, pllfhs, ARRAY_SIZE(pllfhs));
	r = mtk_clk_register_pllfhs(dev, plls, ARRAY_SIZE(plls), pllfhs,
				    ARRAY_SIZE(pllfhs), clk_data);
	if (r)
		goto free_clk_data;

	base = mtk_clk_pll_get_base(clk_data->hws[CLK_APMIXED_ARMPLL],
				    &plls[CLK_APMIXED_ARMPLL]);
	r = mt6589_apmixed_register_outputs(dev, base, priv);
	if (r)
		goto unregister_plls;

	r = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get,
				   clk_data);
	if (r)
		goto unregister_outputs;

	platform_set_drvdata(pdev, priv);

	return 0;

unregister_outputs:
	mt6589_apmixed_unregister_outputs(priv);
unregister_plls:
	mtk_clk_unregister_pllfhs(plls, ARRAY_SIZE(plls), pllfhs,
				  ARRAY_SIZE(pllfhs), clk_data);
free_clk_data:
	mtk_free_clk_data(clk_data);
	return r;
}

static void clk_mt6589_apmixed_remove(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct mt6589_apmixed_priv *priv = platform_get_drvdata(pdev);
	struct clk_hw_onecell_data *clk_data = priv->clk_data;

	of_clk_del_provider(node);
	mt6589_apmixed_unregister_outputs(priv);
	mtk_clk_unregister_pllfhs(plls, ARRAY_SIZE(plls), pllfhs,
				  ARRAY_SIZE(pllfhs), clk_data);
	mtk_free_clk_data(clk_data);
}

static const struct of_device_id of_match_clk_mt6589_apmixed[] = {
	{ .compatible = "mediatek,mt6589-apmixedsys" },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt6589_apmixed_drv = {
	.probe = clk_mt6589_apmixed_probe,
	.remove = clk_mt6589_apmixed_remove,
	.driver = {
		.name = "clk-mt6589-apmixed",
		.of_match_table = of_match_clk_mt6589_apmixed,
	},
};
module_platform_driver(clk_mt6589_apmixed_drv);
MODULE_DESCRIPTION("MediaTek MT6589 apmixedsys clocks driver");
MODULE_LICENSE("GPL");
