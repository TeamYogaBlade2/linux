// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/property.h>

#include "mtk-devapc-mt6589.h"

#define VIO_MOD_TO_REG_IND(m)	((m) / 32)
#define VIO_MOD_TO_REG_OFF(m)	((m) % 32)

struct mtk_devapc_vio_dbgs {
	union {
		u32 vio_dbg0;
		struct {
			u32 mstid:16;
			u32 dmnid:6;
			u32 vio_w:1;
			u32 vio_r:1;
			u32 addr_h:4;
			u32 resv:4;
		} dbg0_bits;
	};

	u32 vio_dbg1;
};

struct mtk_devapc_regs_ofs {
	/* reg offset */
	u32 vio_mask_offset;
	u32 vio_sta_offset;
	u32 vio_dbg0_offset;
	u32 vio_dbg1_offset;
	u32 apc_con_offset;
	u32 vio_shift_sta_offset;
	u32 vio_shift_sel_offset;
	u32 vio_shift_con_offset;
};

struct mtk_devapc_data {
	/* numbers of violation index */
	u32 vio_idx_num;
	const struct mtk_devapc_regs_ofs *regs_ofs;
	bool mt6589;
	u32 nr_instances;
};

/* One MT6589 DEVAPC instance: an always-on permission window and a
 * power-down violation window.  The single-instance SoCs have neither and
 * use infra_base for both.
 */
struct mtk_devapc_instance {
	void __iomem *ao_base;
	void __iomem *pd_base;
	u32 nr_modules;
	u32 dxs_vio_sta_bit;	/* bit in DEVAPC0_DXS_VIO_STA */
};

/* A slave whose permission the DT asks us to program. */
struct mtk_devapc_forbid {
	u32 instance;
	u32 module;
	u32 dom_mask;	/* bitmask of enum mt6589_devapc_domain */
	u32 perm;	/* enum mt6589_devapc_perm */
};

struct mtk_devapc_context {
	struct device *dev;
	void __iomem *infra_base;
	struct clk *infra_clk;
	const struct mtk_devapc_data *data;
	struct mtk_devapc_instance inst[MT6589_DEVPAPC_INSTANCES];
};

static void clear_vio_status(struct mtk_devapc_context *ctx)
{
	void __iomem *reg;
	int i;

	reg = ctx->infra_base + ctx->data->regs_ofs->vio_sta_offset;

	for (i = 0; i < VIO_MOD_TO_REG_IND(ctx->data->vio_idx_num) - 1; i++)
		writel(GENMASK(31, 0), reg + 4 * i);

	writel(GENMASK(VIO_MOD_TO_REG_OFF(ctx->data->vio_idx_num) - 1, 0),
	       reg + 4 * i);
}

static void mask_module_irq(struct mtk_devapc_context *ctx, bool mask)
{
	void __iomem *reg;
	u32 val;
	int i;

	reg = ctx->infra_base + ctx->data->regs_ofs->vio_mask_offset;

	if (mask)
		val = GENMASK(31, 0);
	else
		val = 0;

	for (i = 0; i < VIO_MOD_TO_REG_IND(ctx->data->vio_idx_num) - 1; i++)
		writel(val, reg + 4 * i);

	val = readl(reg + 4 * i);
	if (mask)
		val |= GENMASK(VIO_MOD_TO_REG_OFF(ctx->data->vio_idx_num) - 1,
			       0);
	else
		val &= ~GENMASK(VIO_MOD_TO_REG_OFF(ctx->data->vio_idx_num) - 1,
				0);

	writel(val, reg + 4 * i);
}

#define PHY_DEVAPC_TIMEOUT	0x10000

static int devapc_sync_vio_dbg(struct mtk_devapc_context *ctx)
{
	void __iomem *pd_vio_shift_sta_reg;
	void __iomem *pd_vio_shift_sel_reg;
	void __iomem *pd_vio_shift_con_reg;
	int min_shift_group;
	int ret;
	u32 val;

	pd_vio_shift_sta_reg = ctx->infra_base +
			       ctx->data->regs_ofs->vio_shift_sta_offset;
	pd_vio_shift_sel_reg = ctx->infra_base +
			       ctx->data->regs_ofs->vio_shift_sel_offset;
	pd_vio_shift_con_reg = ctx->infra_base +
			       ctx->data->regs_ofs->vio_shift_con_offset;

	val = readl(pd_vio_shift_sta_reg);
	if (!val)
		return false;

	min_shift_group = __ffs(val);

	writel(0x1 << min_shift_group, pd_vio_shift_sel_reg);

	writel(0x1, pd_vio_shift_con_reg);

	ret = readl_poll_timeout(pd_vio_shift_con_reg, val, val == 0x3, 0,
				 PHY_DEVAPC_TIMEOUT);
	if (ret) {
		dev_err(ctx->dev, "%s: Shift violation info failed\n", __func__);
		return false;
	}

	writel(0x0, pd_vio_shift_con_reg);

	writel(0x1 << min_shift_group, pd_vio_shift_sta_reg);

	return true;
}

static void devapc_extract_vio_dbg(struct mtk_devapc_context *ctx)
{
	struct mtk_devapc_vio_dbgs vio_dbgs;
	void __iomem *vio_dbg0_reg;
	void __iomem *vio_dbg1_reg;

	vio_dbg0_reg = ctx->infra_base + ctx->data->regs_ofs->vio_dbg0_offset;
	vio_dbg1_reg = ctx->infra_base + ctx->data->regs_ofs->vio_dbg1_offset;

	vio_dbgs.vio_dbg0 = readl(vio_dbg0_reg);
	vio_dbgs.vio_dbg1 = readl(vio_dbg1_reg);

	if (vio_dbgs.dbg0_bits.vio_w)
		dev_info(ctx->dev, "Write Violation\n");
	else if (vio_dbgs.dbg0_bits.vio_r)
		dev_info(ctx->dev, "Read Violation\n");

	dev_info(ctx->dev, "Bus ID:0x%x, Dom ID:0x%x, Vio Addr:0x%x\n",
		 vio_dbgs.dbg0_bits.mstid, vio_dbgs.dbg0_bits.dmnid,
		 vio_dbgs.vio_dbg1);
}

static irqreturn_t devapc_violation_irq(int irq_number, void *data)
{
	struct mtk_devapc_context *ctx = data;

	while (devapc_sync_vio_dbg(ctx))
		devapc_extract_vio_dbg(ctx);

	clear_vio_status(ctx);

	return IRQ_HANDLED;
}


static void mt6589_write_perm(struct mtk_devapc_instance *in,
			      const struct mtk_devapc_forbid *f)
{
	enum mt6589_devapc_domain dom;

	for (dom = MT6589_DOMAIN_AP; dom < MT6589_DOMAIN_COUNT; dom++) {
		u32 shift, reg, val;

		if (!(f->dom_mask & BIT(dom)))
			continue;

		reg = MT6589_DEVPAPC_APC_REG(dom, f->module);
		shift = MT6589_DEVPAPC_PERM_SHIFT(f->module);

		val = readl(in->ao_base + reg);
		val &= ~MT6589_DEVPAPC_PERM_MASK(f->module);
		val |= (u32)f->perm << shift;
		writel(val, in->ao_base + reg);
	}
}

static const u8 mt6589_devapc_nr_modules[MT6589_DEVPAPC_INSTANCES] = {
	24, 19, 30, 21, 24,
};


static void mt6589_clear_vio_status(struct mtk_devapc_instance *in)
{
	enum mt6589_devapc_domain dom;
	u32 valid_mask = GENMASK(in->nr_modules - 1, 0);

	for (dom = MT6589_DOMAIN_AP; dom < MT6589_DOMAIN_COUNT; dom++) {
		void __iomem *reg = in->pd_base +
				    MT6589_DEVPAPC_VIO_STA_REG(dom);
		u32 val = readl(reg);

		if (val)
			writel(val & valid_mask, reg);
	}
}

/*
 * MT6589 has two independent policies that must not be conflated:
 *
 *  - Permission policy: which masters may touch which slave, and at what
 *    level (L0..L3).  Programmed into the per-domain APC registers from the DT
 *    "mediatek,devapc-forbid-slaves" table by mt6589_apply_forbid().
 *
 *  - Interrupt-monitoring policy: which slaves, if any, raise an interrupt on
 *    a violation.  That is D<d>_VIO_MASK, and it is independent of the
 *    permission level - a slave locked to L3 can still be masked out (no
 *    interrupt, violation still logged in the status bit), or left at L0 and
 *    unmasked.  D<d>_VIO_STA and D<d>_VIO_MASK are cleared unconditionally at
 *    probe; only the mask honours mediatek,vio-mask-interrupts.
 */

static void mt6589_prepare_inst(struct mtk_devapc_instance *in,
				bool vio_irq_mask)
{
	enum mt6589_devapc_domain dom;
	u32 sta_clr = GENMASK(in->nr_modules - 1, 0);

	for (dom = MT6589_DOMAIN_AP; dom < MT6589_DOMAIN_COUNT; dom++) {
		void __iomem *mask_reg;
		u32 val;

		/* Status write, not a permission change: drops violations
		 * latched before this driver took over.
		 */
		writel(sta_clr, in->pd_base + MT6589_DEVPAPC_VIO_STA_REG(dom));

		mask_reg = in->pd_base + MT6589_DEVPAPC_VIO_MASK_REG(dom);
		val = readl(mask_reg);

		if (vio_irq_mask)
			val |= sta_clr;
		else
			val &= ~sta_clr;

		/*
		 * Keep reserved/unused mask bits intact.  The instances have
		 * different numbers of implemented slave modules.
		 */
		writel(val, mask_reg);
	}

	/* Do not leave a pre-driver violation descriptor latched. */
	writel(MT6589_DEVPAPC_VIO_DBG0_CLR,
	       in->pd_base + MT6589_DEVPAPC_VIO_DBG0);

	/*
	 * Clear APC_CON bit2 ("stop") in both windows.  The downstream driver
	 * does this in init_devpac() for all five instances
	 * (aquaris-5 .../devapc/devapc.c:737-746); leaving the bit set means
	 * the instance never raises an interrupt.
	 */
	writel(readl(in->ao_base + MT6589_DEVPAPC_APC_CON) &
	       ~MT6589_DEVPAPC_APC_CON_STOP,
	       in->ao_base + MT6589_DEVPAPC_APC_CON);
	writel(readl(in->pd_base + MT6589_DEVPAPC_PD_APC_CON) &
	       ~MT6589_DEVPAPC_APC_CON_STOP,
	       in->pd_base + MT6589_DEVPAPC_PD_APC_CON);
}

static void mt6589_prepare_global(struct mtk_devapc_context *ctx)
{
	void __iomem *dxs_mask;
	void __iomem *dxs_sta;
	u32 val;

	/*
	 * DXS_VIO_* is physically present in every PD window, but the
	 * functional summary consumed by the MT6589 interrupt path is the
	 * DEVAPC0 register.  Vendor code always reads/writes this instance
	 * for the DEVAPC0..4 summary bits.
	 *
	 * This is deliberately handled separately from per-domain VIO_MASK:
	 * the latter selects slave violations, while DXS selects the
	 * instance-level interrupt source.
	 */
	dxs_mask = ctx->inst[0].pd_base + MT6589_DEVPAPC_DXS_VIO_MASK;
	dxs_sta = ctx->inst[0].pd_base + MT6589_DEVPAPC_DXS_VIO_STA;

	/* Drop stale summary bits before enabling the interrupt source. */
	writel(MT6589_DEVPAPC_DXS_INSTANCE_MASK, dxs_sta);

	val = readl(dxs_mask);
	val &= ~MT6589_DEVPAPC_DXS_INSTANCE_MASK;
	writel(val, dxs_mask);
}

struct mtk_devapc_vio_dbg {
	u32 master_id;
	u32 domain_id;
	u32 addr;
	bool is_write;
	bool is_read;
};

static void mt6589_read_vio_dbg(struct mtk_devapc_instance *in,
				struct mtk_devapc_vio_dbg *vio)
{
	u32 dbg0;

	dbg0 = readl(in->pd_base + MT6589_DEVPAPC_VIO_DBG0);

	vio->master_id = (dbg0 & MT6589_DEVPAPC_VIO_DBG0_MSTID) >>
			       __bf_shf(MT6589_DEVPAPC_VIO_DBG0_MSTID);
	vio->domain_id = (dbg0 & MT6589_DEVPAPC_VIO_DBG0_DMNID) >>
			       __bf_shf(MT6589_DEVPAPC_VIO_DBG0_DMNID);
	vio->is_write = !!(dbg0 & MT6589_DEVPAPC_VIO_DBG0_VIO_W);
	vio->is_read = !!(dbg0 & MT6589_DEVPAPC_VIO_DBG0_VIO_R);
	vio->addr = readl(in->pd_base + MT6589_DEVPAPC_VIO_DBG1);
}

static void mt6589_report_vio_dbg(struct mtk_devapc_context *ctx,
				  unsigned int idx,
				  const struct mtk_devapc_vio_dbg *vio)
{
	const char *rw;

	if (vio->is_write && vio->is_read)
		rw = "RW";
	else if (vio->is_write)
		rw = "W";
	else if (vio->is_read)
		rw = "R";
	else
		rw = "?";

	dev_info(ctx->dev,
		 "DEVAPC%u violation: %s addr 0x%08x, master ID 0x%03x, domain ID 0x%x\n",
		 idx, rw, vio->addr,
		 vio->master_id, vio->domain_id);
}

static void mt6589_extract_vio_dbg(struct mtk_devapc_context *ctx,
				   struct mtk_devapc_instance *in,
				   unsigned int idx)
{
	struct mtk_devapc_vio_dbg vio;

	mt6589_read_vio_dbg(in, &vio);
	mt6589_report_vio_dbg(ctx, idx, &vio);
}

/*
 * mt6589_violation_irq - MT6589 has no VIO_SHIFT_* registers: VIO_DBG0 and
 *			 VIO_DBG1 are latched directly, and writing bit31 of
 *			 VIO_DBG0 clears them.  The MT6779 shift handshake would
 *			 read an unrelated register and time out here, so walk the
 *			 instances directly instead.
 *
 * Order matters and must not be rearranged: read the latch, decode and
 * report it, and only then clear the latch and acknowledge the instance
 * status.  Clearing first destroys the evidence the diagnostic exists to
 * provide, so a violation would be reported as all zeroes.  VIO_DBG0 is not
 * cleared by a read, so reading it here is safe.
 */
static irqreturn_t mt6589_violation_irq(int irq, void *data)
{
	struct mtk_devapc_context *ctx = data;
	void __iomem *dxs_sta;
	u32 pending;
	unsigned int i;

	dxs_sta = ctx->inst[0].pd_base + MT6589_DEVPAPC_DXS_VIO_STA;
	pending = readl(dxs_sta) & MT6589_DEVPAPC_DXS_INSTANCE_MASK;

	/*
	 * MT6589 shares this line with other bus-protection blocks.
	 * Do not claim an unrelated interrupt.
	 */
	if (!pending)
		return IRQ_NONE;

	for (i = 0; i < ctx->data->nr_instances; i++) {
		struct mtk_devapc_instance *in = &ctx->inst[i];

		if (!(pending & in->dxs_vio_sta_bit))
			continue;

		/*
		 * Read the instance-local debug latch selected by the global
		 * DXS summary, not its local DXS_VIO_STA.
		 */
		mt6589_extract_vio_dbg(ctx, in, i);

		/*
		 * We don't have the violating slave number in VIO_DBG0.
		 * Clear every currently latched slave status in the instance
		 * after reporting the debug latch, otherwise the global DXS bit
		 * can remain asserted and cause an IRQ storm.
		 */
		mt6589_clear_vio_status(in);

		/* Release the debug latch. */
		writel(MT6589_DEVPAPC_VIO_DBG0_CLR,
		       in->pd_base + MT6589_DEVPAPC_VIO_DBG0);

		/* Acknowledge the shared instance-level status. */
		writel(in->dxs_vio_sta_bit, dxs_sta);
	}

	return IRQ_HANDLED;
}

/*
 * mt6589_apply_forbid - parse "mediatek,devapc-forbid-slaves" and program the
 *		       named slaves.  The property is a flat list of 4-tuples:
 *
 *   <instance> <module> <domain-mask> <perm>
 *
 * where domain-mask is a bitmask of the domain indices (1 = AP, 2 = MD1,
 * 4 = MD2, 8 = MM) and perm is 0..3.  Slaves not listed stay at L0.
 */
static void mt6589_apply_forbid(struct mtk_devapc_context *ctx)
{
	struct device_node *node = ctx->dev->of_node;
	struct mtk_devapc_forbid f;
	u32 val;
	int count, n;
	unsigned int applied = 0;

	count = of_property_count_elems_of_size(
		node, "mediatek,devapc-forbid-slaves", sizeof(u32));
	if (count < 0)
		return;

	if (count % 4) {
		dev_err(ctx->dev,
			"forbid: property has %d cells, expected a multiple of 4\n",
			count);
		return;
	}

	for (n = 0; n < count; n += 4) {
		if (of_property_read_u32_index(node,
					"mediatek,devapc-forbid-slaves",
					n, &f.instance))
			break;

		if (of_property_read_u32_index(node,
					       "mediatek,devapc-forbid-slaves",
					       n + 1, &f.module))
			break;

		if (of_property_read_u32_index(node,
					       "mediatek,devapc-forbid-slaves",
					       n + 2, &f.dom_mask))
			break;

		if (of_property_read_u32_index(node,
					       "mediatek,devapc-forbid-slaves",
					       n + 3, &f.perm))
			break;

		if (f.instance >= ctx->data->nr_instances) {
			dev_warn(ctx->dev, "forbid: instance %u out of range\n",
				 f.instance);
			continue;
		}

		if (f.module >= ctx->inst[f.instance].nr_modules) {
			dev_warn(ctx->dev,
				 "forbid: module %u out of range for instance %u\n",
				 f.module, f.instance);
			continue;
		}

		if (!f.dom_mask ||
		    f.dom_mask & ~GENMASK(MT6589_DOMAIN_COUNT - 1, 0)) {
			dev_warn(ctx->dev,
				 "forbid: invalid domain mask 0x%x\n",
				 f.dom_mask);
			continue;
		}

		if (f.perm > MT6589_APC_L3) {
			dev_warn(ctx->dev, "forbid: bad perm %u\n", f.perm);
			continue;
		}

		mt6589_write_perm(&ctx->inst[f.instance], &f);
		applied++;
	}

	if (applied)
		dev_info(ctx->dev, "programmed %u DEVAPC permission entrie(s)\n",
			 applied);
}

static void mt6589_set_dxs_irq_mask(struct mtk_devapc_context *ctx,
				    bool mask)
{
	void __iomem *reg;
	u32 val;

	/*
	 * DXS_VIO_MASK bit 0..4 controls the five DEVAPC instance
	 * interrupt sources.  The remaining bits are shared with other
	 * protection logic, so preserve them.
	 */
	reg = ctx->inst[0].pd_base + MT6589_DEVPAPC_DXS_VIO_MASK;
	val = readl(reg);

	if (mask)
		val |= MT6589_DEVPAPC_DXS_INSTANCE_MASK;
	else
		val &= ~MT6589_DEVPAPC_DXS_INSTANCE_MASK;

	writel(val, reg);
}

/*
 * mt6589_start - prepare every instance and apply the DT permission table.
 *
 * This runs exactly once, from probe.  There is deliberately no PM runtime
 * suspend/resume callback:
 *
 *  - The permission windows are in the always-on (AO) window and the violation
 *    windows are in a separately clocked PD window, but neither is in a
 *    switchable SPM power domain (see the devapc node in mt6589.dtsi), so
 *    there is nothing that would drop the registers across a suspend.  The
 *    only gate is the infracfg devapc clock, which the driver holds enabled
 *    for its whole lifetime via devm_clk_get_enabled().
 *
 *  - Re-programming the permission windows from a resume callback would be
 *    actively wrong while MT6589_DEVPAPC_APC_LOCK is unused: if that register
 *    is ever found to lock the windows one way at first write, a resume that
 *    rewrites them would silently stop restoring the policy it thought it was
 *    restoring.  See the header for the evidence that its polarity is unknown.
 */
static void mt6589_start(struct mtk_devapc_context *ctx)
{
	unsigned int i;
	bool mask_irqs;

	/*
	 * Interrupt-monitoring policy, read from DT.  A boolean, not a bitmask:
	 * VIO_MASK is per-slave and per-domain, so a scalar would imply a
	 * precision this driver does not have.
	 *
	 * Unmasked is the bring-up default: every violation on every slave
	 * raises an interrupt, so an unexpected access shows up in the log
	 * immediately.  That is wrong for production, where a denied access is
	 * expected traffic and would become interrupt load; boards that have
	 * settled their mediatek,devapc-forbid-slaves table should set
	 * mediatek,vio-mask-interrupts.
	 */
	mask_irqs = of_property_read_bool(ctx->dev->of_node,
					  "mediatek,vio-mask-interrupts");

	/*
	 * Keep the shared DEVAPC summary masked until all instance-local
	 * state and permission registers have been initialized.
	 */
	mt6589_set_dxs_irq_mask(ctx, true);

	dev_info(ctx->dev, "violation interrupts %s%s\n",
		 mask_irqs ? "masked for all slaves" :
			     "unmasked for all slaves",
		 mask_irqs ? "" : " (bring-up default)");

	for (i = 0; i < ctx->data->nr_instances; i++)
		mt6589_prepare_inst(&ctx->inst[i], mask_irqs);

	mt6589_apply_forbid(ctx);

	/*
	 * Drop violations that raced with the initialization sequence before
	 * finally exposing the DEVAPC summary to the interrupt controller.
	 */
	writel(MT6589_DEVPAPC_DXS_INSTANCE_MASK,
	       ctx->inst[0].pd_base + MT6589_DEVPAPC_DXS_VIO_STA);

	mt6589_set_dxs_irq_mask(ctx, mask_irqs);

	/*
	 * This is where a policy lock would have to go, after
	 * mt6589_apply_forbid() has programmed every permission window.  It is
	 * deliberately not done: the polarity of APC_LOCK is undocumented, and
	 * guessing it risks permanently freezing or widening the policy just
	 * programmed.  See MT6589_DEVPAPC_APC_LOCK in mtk-devapc-mt6589.h.
	 */
}

/*
 * start_devapc - unmask slave's irq to start receiving devapc violation.
 */
static void start_devapc(struct mtk_devapc_context *ctx)
{
	if (ctx->data->mt6589) {
		mt6589_start(ctx);
		return;
	}

	writel(BIT(31), ctx->infra_base + ctx->data->regs_ofs->apc_con_offset);

	mask_module_irq(ctx, false);
}

/*
 * stop_devapc - mask slave's irq to stop service.
 */
static void stop_devapc(struct mtk_devapc_context *ctx)
{
	if (ctx->data->mt6589) {
		unsigned int i;

		mt6589_set_dxs_irq_mask(ctx, true);

		for (i = 0; i < ctx->data->nr_instances; i++) {
			struct mtk_devapc_instance *in = &ctx->inst[i];
			enum mt6589_devapc_domain dom;
			u32 module_mask;
			u32 val;

			module_mask = GENMASK(in->nr_modules - 1, 0);

			for (dom = MT6589_DOMAIN_AP;
			     dom < MT6589_DOMAIN_COUNT; dom++) {
				void __iomem *reg;

				reg = in->pd_base +
				      MT6589_DEVPAPC_VIO_MASK_REG(dom);
				val = readl(reg);
				val |= module_mask;
				writel(val, reg);
			}

			val = readl(in->ao_base + MT6589_DEVPAPC_APC_CON);
			val |= MT6589_DEVPAPC_APC_CON_STOP;
			writel(val, in->ao_base + MT6589_DEVPAPC_APC_CON);

			val = readl(in->pd_base + MT6589_DEVPAPC_PD_APC_CON);
			val |= MT6589_DEVPAPC_APC_CON_STOP;
			writel(val, in->pd_base + MT6589_DEVPAPC_PD_APC_CON);
		}

		return;
	}

	mask_module_irq(ctx, true);

	writel(BIT(2), ctx->infra_base + ctx->data->regs_ofs->apc_con_offset);
}

static const struct mtk_devapc_regs_ofs devapc_regs_ofs_mt6779 = {
	.vio_mask_offset = 0x0,
	.vio_sta_offset = 0x400,
	.vio_dbg0_offset = 0x900,
	.vio_dbg1_offset = 0x904,
	.apc_con_offset = 0xF00,
	.vio_shift_sta_offset = 0xF10,
	.vio_shift_sel_offset = 0xF14,
	.vio_shift_con_offset = 0xF20,
};

static const struct mtk_devapc_data devapc_mt6779 = {
	.vio_idx_num = 511,
	.regs_ofs = &devapc_regs_ofs_mt6779,
	.nr_instances = 1,
};

static const struct mtk_devapc_data devapc_mt8186 = {
	.vio_idx_num = 519,
	.regs_ofs = &devapc_regs_ofs_mt6779,
	.nr_instances = 1,
};

static const struct mtk_devapc_data devapc_mt6589 = {
	/* MT6589 has no flat violation index space; modules are per instance. */
	.vio_idx_num = 0,
	.regs_ofs = NULL,
	.mt6589 = true,
	.nr_instances = MT6589_DEVPAPC_INSTANCES,
};

static const struct of_device_id mtk_devapc_dt_match[] = {
	{
		.compatible = "mediatek,mt6779-devapc",
		.data = &devapc_mt6779,
	}, {
		.compatible = "mediatek,mt8186-devapc",
		.data = &devapc_mt8186,
	}, {
		.compatible = "mediatek,mt6589-devapc",
		.data = &devapc_mt6589,
	}, {
	},
};
MODULE_DEVICE_TABLE(of, mtk_devapc_dt_match);

static int mtk_devapc_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct mtk_devapc_context *ctx;
	unsigned long irq_flags = 0;
	int devapc_irq;
	unsigned int i;
	int ret;

	if (!node)
		return -ENODEV;

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->data = of_device_get_match_data(&pdev->dev);
	ctx->dev = &pdev->dev;

	if (ctx->data->nr_instances > 1) {
		/*
		 * Instance n uses reg entries 2n (permission window) and 2n+1
		 * (violation window).  devm_platform_ioremap_resource()
		 * validates each against its own entry, so a missing or
		 * malformed region is reported here instead of faulting later.
		 */
		for (i = 0; i < ctx->data->nr_instances; i++) {
			struct mtk_devapc_instance *in = &ctx->inst[i];

			in->ao_base = devm_platform_ioremap_resource(pdev,
								      i * 2);
			if (IS_ERR(in->ao_base)) {
				ret = PTR_ERR(in->ao_base);
				in->ao_base = NULL;
				goto err_unmap_inst;
			}

			in->pd_base = devm_platform_ioremap_resource(pdev,
								      i * 2 + 1);
			if (IS_ERR(in->pd_base)) {
				ret = PTR_ERR(in->pd_base);
				in->pd_base = NULL;
				goto err_unmap_inst;
			}

			in->nr_modules = mt6589_devapc_nr_modules[i];
			in->dxs_vio_sta_bit = BIT(i);
		}
	} else {
		ctx->infra_base = of_iomap(node, 0);
		if (!ctx->infra_base)
			return -EINVAL;
	}

	devapc_irq = platform_get_irq(pdev, 0);
	if (devapc_irq < 0) {
		ret = devapc_irq;
		goto err_unmap_inst;
	}

	ctx->infra_clk = devm_clk_get_enabled(&pdev->dev, "devapc-infra-clock");
	if (IS_ERR(ctx->infra_clk)) {
		ret = PTR_ERR(ctx->infra_clk);
		goto err_unmap_inst;
	}

	/*
	 * The IRQ domain has already applied the trigger type from DT.
	 * MT6589 additionally shares this line with the other bus-protection
	 * blocks, so only the shared flag is required here.
	 */
	if (ctx->data->mt6589)
		irq_flags = IRQF_SHARED;

	ret = devm_request_irq(&pdev->dev, devapc_irq,
			       ctx->data->mt6589 ?
			       mt6589_violation_irq : devapc_violation_irq,
			       irq_flags, "devapc", ctx);
	if (ret)
		goto err_unmap_inst;

	platform_set_drvdata(pdev, ctx);

	start_devapc(ctx);

	return 0;

err_unmap_inst:
	if (ctx->data->nr_instances <= 1 && ctx->infra_base)
		iounmap(ctx->infra_base);
	return ret;
}

static void mtk_devapc_remove(struct platform_device *pdev)
{
	struct mtk_devapc_context *ctx = platform_get_drvdata(pdev);

	stop_devapc(ctx);
}

static struct platform_driver mtk_devapc_driver = {
	.probe = mtk_devapc_probe,
	.remove = mtk_devapc_remove,
	.driver = {
		.name = "mtk-devapc",
		.of_match_table = mtk_devapc_dt_match,
	},
};

module_platform_driver(mtk_devapc_driver);

MODULE_DESCRIPTION("Mediatek Device APC Driver");
MODULE_AUTHOR("Neal Liu <neal.liu@mediatek.com>");
MODULE_LICENSE("GPL");
