// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Akari Tsuyukusa <akkun11.open@gmail.com>
 */
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/math64.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6589-clk.h>

#define TOP_CKMUXSEL	0x0000
#define TOP_CKDIV1	0x0008

struct mt6589_armdiv_ratio {
	u8 val;
	u8 num;
	u8 den;
};

static const struct mt6589_armdiv_ratio mt6589_armdiv_ratios[] = {
	{ 0x00, 1, 1 },
	{ 0x08, 4, 4 },
	{ 0x09, 3, 4 },
	{ 0x0a, 2, 4 },
	{ 0x0b, 1, 4 },
	{ 0x10, 5, 5 },
	{ 0x11, 4, 5 },
	{ 0x12, 3, 5 },
	{ 0x13, 2, 5 },
	{ 0x14, 1, 5 },
	{ 0x18, 6, 6 },
	{ 0x19, 5, 6 },
	{ 0x1a, 4, 6 },
	{ 0x1b, 3, 6 },
	{ 0x1c, 2, 6 },
	{ 0x1d, 1, 6 },
};

static const struct mt6589_armdiv_ratio *
mt6589_armdiv_find_val(unsigned int val)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6589_armdiv_ratios); i++)
		if (mt6589_armdiv_ratios[i].val == val)
			return &mt6589_armdiv_ratios[i];

	return NULL;
}

static const struct mt6589_armdiv_ratio *
mt6589_armdiv_find_rate(unsigned long rate, unsigned long parent_rate)
{
	const struct mt6589_armdiv_ratio *best = NULL;
	u64 best_diff = ~0ULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6589_armdiv_ratios); i++) {
		const struct mt6589_armdiv_ratio *ratio =
			&mt6589_armdiv_ratios[i];
		u64 candidate;
		u64 diff;

		candidate = div_u64((u64)parent_rate * ratio->num,
				    ratio->den);
		diff = candidate > rate ? candidate - rate : rate - candidate;

		if (diff < best_diff) {
			best_diff = diff;
			best = ratio;
		}
	}

	return best;
}

static unsigned long mt6589_armdiv_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct clk_divider *div = to_clk_divider(hw);
	const struct mt6589_armdiv_ratio *ratio;
	u32 val;

	val = readl(div->reg) >> div->shift;
	val &= GENMASK(div->width - 1, 0);

	ratio = mt6589_armdiv_find_val(val);
	if (!ratio)
		return parent_rate;

	return div_u64((u64)parent_rate * ratio->num, ratio->den);
}

static int mt6589_armdiv_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	const struct mt6589_armdiv_ratio *ratio;

	ratio = mt6589_armdiv_find_rate(req->rate, req->best_parent_rate);
	if (!ratio)
		return -EINVAL;

	req->rate = div_u64((u64)req->best_parent_rate * ratio->num,
			    ratio->den);

	return 0;
}

static int mt6589_armdiv_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct clk_divider *div = to_clk_divider(hw);
	const struct mt6589_armdiv_ratio *ratio;
	unsigned long flags;
	u32 val;
	u32 mask;

	ratio = mt6589_armdiv_find_rate(rate, parent_rate);
	if (!ratio)
		return -EINVAL;

	mask = GENMASK(div->width - 1, 0);

	if (div->lock)
		spin_lock_irqsave(div->lock, flags);

	val = readl(div->reg);
	val &= ~(mask << div->shift);
	val |= (u32)ratio->val << div->shift;
	writel(val, div->reg);

	if (div->lock)
		spin_unlock_irqrestore(div->lock, flags);

	return 0;
}

static const struct clk_ops mt6589_armdiv_ops = {
	.recalc_rate = mt6589_armdiv_recalc_rate,
	.determine_rate = mt6589_armdiv_determine_rate,
	.set_rate = mt6589_armdiv_set_rate,
};
#define INFRA_RST0	0x0030
#define INFRA_RST1	0x0034
#define INFRA_PDN_SET	0x0040
#define INFRA_PDN_CLR	0x0044
#define INFRA_PDN_STA	0x0048

static DEFINE_SPINLOCK(mt6589_infra_clk_lock);

static const char * const infra_mux1_parents[] = {
	"clk26m",
	"armpll",
	"mainpll",
	"mmpll_d2",
};

static const struct mtk_composite cpu_muxes[] = {
	MUX(CLK_INFRA_MUX1, "infra_mux1_sel", infra_mux1_parents, TOP_CKMUXSEL, 2, 2),
};

static const struct mtk_gate_regs infra_cg_regs = {
	.set_ofs = INFRA_PDN_SET,
	.clr_ofs = INFRA_PDN_CLR,
	.sta_ofs = INFRA_PDN_STA,
};

#define GATE_INFRA(_id, _name, _parent, _shift)				\
	GATE_MTK(_id, _name, _parent, &infra_cg_regs, _shift, &mtk_clk_gate_ops_setclr)

static const struct mtk_gate infra_clks[] = {
	GATE_INFRA(CLK_INFRA_DBGCLK, "infra_dbgclk", "axi_sel", 0), /* mt8135 */
	GATE_INFRA(CLK_INFRA_SMI, "infra_smi", "smi_sel", 1), /* mt8135 */
	GATE_INFRA(CLK_INFRA_SPI0, "infra_spi0", "spi_sel", 2), /* maybe, is it infra_mfg_bus? */
	GATE_INFRA(CLK_INFRA_AUDIO, "infra_audio", "audintbus_sel", 5),
	GATE_MTK(CLK_INFRA_CEC, "infra_cec", "axi_sel", &infra_cg_regs, 6, &mtk_clk_gate_ops_setclr_inv), /* or devapc */
	GATE_INFRA(CLK_INFRA_MFGAXI, "infra_mfgaxi", "axi_sel", 7), /* mt8135 */
	/*
	 * The M4U clock is critical: stopping it while the IOMMU is
	 * attached hangs the whole system (any outstanding translation
	 * never completes).  Keep it on from boot.
	 */
	GATE_MTK_FLAGS(CLK_INFRA_M4U, "infra_m4u", "mem_sel",
		       &infra_cg_regs, 8, &mtk_clk_gate_ops_setclr,
		       CLK_IS_CRITICAL),
	GATE_INFRA(CLK_INFRA_MD1MCUAXI, "infra_md1mcuaxi", "axi_sel", 9), /* maybe */
	GATE_INFRA(CLK_INFRA_MD1HWMIXAXI, "infra_md1hwmixaxi", "axi_sel", 10), /* maybe */
	GATE_INFRA(CLK_INFRA_MD1AHB, "infra_md1ahb", "axi_sel", 11), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2MCUAXI, "infra_md2mcuaxi", "axi_sel", 12), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2HWMIXAXI, "infra_md2hwmixaxi", "axi_sel", 13), /* maybe */
	GATE_INFRA(CLK_INFRA_MD2AHB, "infra_md2ahb", "axi_sel", 14), /* maybe */
	GATE_INFRA(CLK_INFRA_CPUM, "infra_cpum", "cpum_tck_in", 15), /* mt8135 */
	GATE_INFRA(CLK_INFRA_KP, "infra_kp", "axi_sel", 16), /* mt8135 */
	GATE_INFRA(CLK_INFRA_CCIF0, "infra_ccif0", "axi_sel", 20), /* mt8135 */
	GATE_INFRA(CLK_INFRA_CCIF1, "infra_ccif1", "axi_sel", 21), /* mt8135 */
	GATE_INFRA(CLK_INFRA_PMICSPI, "infra_pmicspi", "pmicspi_sel", 22), /* mt8135 */
	GATE_INFRA(CLK_INFRA_PMICWRAP, "infra_pmicwrap", "axi_sel", 23), /* mt8135 */
};

static u16 infrasys_rst_ofs[] = { INFRA_RST0, INFRA_RST1 };

static const struct mtk_clk_rst_desc infra_clk_rst_desc = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = infrasys_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(infrasys_rst_ofs),
};

/*
 * The CPU DVFS path divides ARMPLL through this field while
 * reprogramming the PLL; exposing it as a clock lets cpufreq switch
 * through it instead of poking the register directly.
 */
static const struct mtk_clk_divider infra_dividers[] = {
	{
		.id = CLK_INFRA_ARMDIV1,
		.name = "armdiv1",
		.parent_name = "infra_mux1_sel",
		.flags = CLK_SET_RATE_PARENT,
		.div_reg = TOP_CKDIV1,
		.div_shift = 0,
		.div_width = 5,
		.ops = &mt6589_armdiv_ops,
	},
};

static const struct mtk_clk_desc infra_desc = {
	.clks = infra_clks,
	.num_clks = ARRAY_SIZE(infra_clks),
	.cpumuxes = cpu_muxes,
	.num_cpumuxes = ARRAY_SIZE(cpu_muxes),
	.divider_clks = infra_dividers,
	.num_divider_clks = ARRAY_SIZE(infra_dividers),
	.rst_desc = &infra_clk_rst_desc,
	.clk_lock = &mt6589_infra_clk_lock,
};

static const struct of_device_id of_match_clk_mt6589_infracfg[] = {
	{ .compatible = "mediatek,mt6589-infracfg", .data = &infra_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6589_infracfg);

static struct platform_driver clk_mt6589_infracfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6589-infracfg",
		.of_match_table = of_match_clk_mt6589_infracfg,
	},
};
module_platform_driver(clk_mt6589_infracfg_drv);

MODULE_DESCRIPTION("MediaTek MT6589 infracfg clocks driver");
MODULE_LICENSE("GPL");
