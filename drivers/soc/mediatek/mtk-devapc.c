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
#include <linux/of_irq.h>
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
	/*
	 * Number of independent DEVAPC instances.  1 for the monolithic
	 * MT6779/MT8186 blocks, MT6589_DEVPAPC_INSTANCES for MT6589.
	 */
	u32 nr_instances;
};

/*
 * MT6589 supports multiple independent DEVAPC instances, each split across an
 * always-on (permission) window and a power-down (violation) window.  The
 * single-instance SoCs reuse infra_base for both.
 */
struct mtk_devapc_instance {
	void __iomem *ao_base;
	void __iomem *pd_base;
	u32 nr_modules;
	u32 dxs_vio_sta_bit;
};

/* A slave whose permission the DT asks us to program. */
struct mtk_devapc_forbid {
	u8 instance;
	u8 module;
	u8 dom_mask;	/* bitmask of enum mt6589_devapc_domain */
	u8 perm;	/* enum mt6589_devapc_perm */
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

/*
 * devapc_sync_vio_dbg - do "shift" mechansim" to get full violation information.
 *                       shift mechanism is depends on devapc hardware design.
 *                       Mediatek devapc set multiple slaves as a group.
 *                       When violation is triggered, violation info is kept
 *                       inside devapc hardware.
 *                       Driver should do shift mechansim to sync full violation
 *                       info to VIO_DBGs registers.
 *
 */
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

	/* Find the minimum shift group which has violation */
	val = readl(pd_vio_shift_sta_reg);
	if (!val)
		return false;

	min_shift_group = __ffs(val);

	/* Assign the group to sync */
	writel(0x1 << min_shift_group, pd_vio_shift_sel_reg);

	/* Start syncing */
	writel(0x1, pd_vio_shift_con_reg);

	ret = readl_poll_timeout(pd_vio_shift_con_reg, val, val == 0x3, 0,
				 PHY_DEVAPC_TIMEOUT);
	if (ret) {
		dev_err(ctx->dev, "%s: Shift violation info failed\n", __func__);
		return false;
	}

	/* Stop syncing */
	writel(0x0, pd_vio_shift_con_reg);

	/* Write clear */
	writel(0x1 << min_shift_group, pd_vio_shift_sta_reg);

	return true;
}

/*
 * devapc_extract_vio_dbg - extract full violation information after doing
 *                          shift mechanism.
 */
static void devapc_extract_vio_dbg(struct mtk_devapc_context *ctx)
{
	struct mtk_devapc_vio_dbgs vio_dbgs;
	void __iomem *vio_dbg0_reg;
	void __iomem *vio_dbg1_reg;

	vio_dbg0_reg = ctx->infra_base + ctx->data->regs_ofs->vio_dbg0_offset;
	vio_dbg1_reg = ctx->infra_base + ctx->data->regs_ofs->vio_dbg1_offset;

	vio_dbgs.vio_dbg0 = readl(vio_dbg0_reg);
	vio_dbgs.vio_dbg1 = readl(vio_dbg1_reg);

	/* Print violation information */
	if (vio_dbgs.dbg0_bits.vio_w)
		dev_info(ctx->dev, "Write Violation\n");
	else if (vio_dbgs.dbg0_bits.vio_r)
		dev_info(ctx->dev, "Read Violation\n");

	dev_info(ctx->dev, "Bus ID:0x%x, Dom ID:0x%x, Vio Addr:0x%x\n",
		 vio_dbgs.dbg0_bits.mstid, vio_dbgs.dbg0_bits.dmnid,
		 vio_dbgs.vio_dbg1);
}

/*
 * devapc_violation_irq - the devapc Interrupt Service Routine (ISR) will dump
 *                        violation information including which master violates
 *                        access slave.
 */
static irqreturn_t devapc_violation_irq(int irq_number, void *data)
{
	struct mtk_devapc_context *ctx = data;

	while (devapc_sync_vio_dbg(ctx))
		devapc_extract_vio_dbg(ctx);

	clear_vio_status(ctx);

	return IRQ_HANDLED;
}


/*
 * mt6589_write_perm - program one slave's permission for each requested domain
 *		       master of one instance.
 */
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

/*
 * MT6589 has two independent policies that must not be conflated:
 *
 *  - Permission policy: which masters may touch which slave, and at what
 *    level (L0..L3).  That is programmed into the per-domain APC registers
 *    from the DT "mediatek,devapc-forbid-slaves" table by
 *    mt6589_apply_forbid().
 *
 *  - Interrupt-monitoring policy: which slaves, if any, raise an interrupt
 *    when a violation occurs.  That is the D<d>_VIO_MASK register, and it is
 *    completely independent of the permission level - a slave can be locked
 *    to L3 and still be masked out (no interrupt, violation still logged in
 *    the status bit), or left at L0 and unmasked.
 *
 * By default we unmask everything so bring-up logs violations on any slave.
 * Set "mediatek,vio-mask-interrupts" in DT to mask all interrupts instead
 * (permissions still apply; violations are still latched in D<d>_VIO_STA but
 * no interrupt is raised).
 */

/*
 * mt6589_prepare_inst - clear stale violation status and apply the
 *			 interrupt-monitoring policy for one instance.
 *
 * Clearing D<d>_VIO_STA is unconditional and is a status-register write, not
 * a permission change: it just drops violations latched before this driver
 * took over.  The VIO_MASK write below is the *only* thing that decides
 * whether violations generate interrupts, and it is driven by the DT policy
 * rather than being hardcoded.
 */
static void mt6589_prepare_inst(struct mtk_devapc_instance *in,
				bool vio_irq_mask)
{
	enum mt6589_devapc_domain dom;
	u32 sta_clr = GENMASK(in->nr_modules - 1, 0);
	u32 irq_mask = vio_irq_mask ? GENMASK(in->nr_modules - 1, 0) : 0;

	for (dom = MT6589_DOMAIN_AP; dom < MT6589_DOMAIN_COUNT; dom++) {
		/* Drop stale latched violations (status, not permission). */
		writel(sta_clr, in->pd_base + MT6589_DEVPAPC_VIO_STA_REG(dom));

		/* Interrupt-monitoring policy, independent of permission. */
		writel(irq_mask, in->pd_base + MT6589_DEVPAPC_VIO_MASK_REG(dom));
	}

	/*
	 * Clear APC_CON bit2 ("stop") in both windows.  The downstream driver
	 * does this in init_devpac() for all five instances; leaving the bit set
	 * means the instance never raises an interrupt.
	 */
	writel(readl(in->ao_base + MT6589_DEVPAPC_APC_CON) &
	       ~MT6589_DEVPAPC_APC_CON_STOP,
	       in->ao_base + MT6589_DEVPAPC_APC_CON);
	writel(readl(in->pd_base + MT6589_DEVPAPC_PD_APC_CON) &
	       ~MT6589_DEVPAPC_APC_CON_STOP,
	       in->pd_base + MT6589_DEVPAPC_PD_APC_CON);
}

/*
 * mt6589_vio_dbg - decoded VIO_DBG0/VIO_DBG1 pair from one instance.
 *
 * The values are captured into this struct so they can be reported before
 * the caller clears the debug latch; see MT6589_DEVPAPC_VIO_DBG0_CLR.
 */
struct mtk_devapc_vio_dbg {
	u32 master_id;
	u32 domain_id;
	u32 addr;
	bool is_write;
};

/*
 * mt6589_read_vio_dbg - latch and decode the violation debug registers.
 *
 * VIO_DBG0 is read only - it is NOT cleared by a read, so this is safe to
 * call before the clear.  Decode first, clear later: writing bit31 releases
 * the latch and the information cannot be recovered afterwards.
 */
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
	vio->addr = readl(in->pd_base + MT6589_DEVPAPC_VIO_DBG1);
}

/*
 * mt6589_report_vio_dbg - hand a decoded violation to the log.
 */
static void mt6589_report_vio_dbg(struct mtk_devapc_context *ctx,
				  unsigned int idx,
				  const struct mtk_devapc_vio_dbg *vio)
{
	dev_info(ctx->dev,
		 "DEVAPC%u violation: %s addr 0x%08x, master ID 0x%03x, domain ID 0x%x\n",
		 idx, vio->is_write ? "W" : "R", vio->addr,
		 vio->master_id, vio->domain_id);
}

/*
 * mt6589_extract_vio_dbg - decode VIO_DBG0/VIO_DBG1 from one instance and
 *			   report it.  Everything is reported here, before the
 *			   caller clears the latch.
 */
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
 * provide, so a violation would be reported as all zeroes.
 */
static irqreturn_t mt6589_violation_irq(int irq, void *data)
{
	struct mtk_devapc_context *ctx = data;
	unsigned int i;

	for (i = 0; i < ctx->data->nr_instances; i++) {
		struct mtk_devapc_instance *in = &ctx->inst[i];

		/* Does this instance have a pending violation? */
		if (!(readl(in->pd_base + MT6589_DEVPAPC_DXS_VIO_STA) &
		      in->dxs_vio_sta_bit))
			continue;

		/* 1. Read and decode, 2. report, in that order. */
		mt6589_extract_vio_dbg(ctx, in, i);

		/* 3. Only now release the debug latch (write-1-to-clear). */
		writel(MT6589_DEVPAPC_VIO_DBG0_CLR,
		       in->pd_base + MT6589_DEVPAPC_VIO_DBG0);

		/* 4. Acknowledge the instance-level status. */
		writel(in->dxs_vio_sta_bit,
		       in->pd_base + MT6589_DEVPAPC_DXS_VIO_STA);
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
	int n = 0;

	while (n < 64 &&
	       !of_property_read_u32_index(node,
					"mediatek,devapc-forbid-slaves",
					n * 4, &val)) {
		f.instance = val;

		if (of_property_read_u32_index(node,
					       "mediatek,devapc-forbid-slaves",
					       n * 4 + 1, &val))
			break;
		f.module = val;

		if (of_property_read_u32_index(node,
					       "mediatek,devapc-forbid-slaves",
					       n * 4 + 2, &val))
			break;
		f.dom_mask = val;

		if (of_property_read_u32_index(node,
					       "mediatek,devapc-forbid-slaves",
					       n * 4 + 3, &val))
			break;
		f.perm = val;

		n++;

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

		if (f.perm > MT6589_APC_L3) {
			dev_warn(ctx->dev, "forbid: bad perm %u\n", f.perm);
			continue;
		}

		mt6589_write_perm(&ctx->inst[f.instance], &f);
	}

	if (n)
		dev_info(ctx->dev, "forbidding %d slave permission field(s)\n", n);
}

/*
 * mt6589_start - prepare every instance and apply the DT permission table.
 */
static void mt6589_start(struct mtk_devapc_context *ctx)
{
	unsigned int i;
	bool mask_irqs;

	/*
	 * Interrupt-monitoring policy, read from DT.  This is a boolean, not a
	 * bitmask: VIO_MASK is per-slave and per-domain, so a scalar would
	 * imply a precision this driver does not have.  Absent (the default)
	 * interrupts stay unmasked so bring-up logs every violation.
	 */
	mask_irqs = of_property_read_bool(ctx->dev->of_node,
					  "mediatek,vio-mask-interrupts");

	dev_info(ctx->dev, "violation interrupts %s\n",
		 mask_irqs ? "masked for all slaves" :
			     "unmasked for all slaves");

	for (i = 0; i < ctx->data->nr_instances; i++)
		mt6589_prepare_inst(&ctx->inst[i], mask_irqs);

	mt6589_apply_forbid(ctx);
}

/*
 * start_devapc - unmask slave's irq to start receiving devapc violation.
 */
static void start_devapc(struct mtk_devapc_context *ctx)
{
	if (ctx->data->nr_instances > 1) {
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
	if (ctx->data->nr_instances > 1) {
		unsigned int i;

		for (i = 0; i < ctx->data->nr_instances; i++) {
			struct mtk_devapc_instance *in = &ctx->inst[i];
			enum mt6589_devapc_domain dom;

			for (dom = MT6589_DOMAIN_AP; dom < MT6589_DOMAIN_COUNT; dom++)
				writel(GENMASK(31, 0),
				       in->pd_base +
				       MT6589_DEVPAPC_VIO_MASK_REG(dom));
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
};

static const struct mtk_devapc_data devapc_mt8186 = {
	.vio_idx_num = 519,
	.regs_ofs = &devapc_regs_ofs_mt6779,
};

static const struct mtk_devapc_data devapc_mt6589 = {
	/* MT6589 has no flat violation index space; modules are per instance. */
	.vio_idx_num = 0,
	.regs_ofs = NULL,
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
	struct of_phandle_args oirq;
	unsigned long devapc_irq_flags;
	u32 devapc_irq;
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
		 * MT6589 describes each instance as a pair of reg entries: the
		 * always-on permission window followed by the power-down
		 * violation window, so instance n uses reg 2n and 2n+1.
		 */
		for (i = 0; i < ctx->data->nr_instances; i++) {
			struct mtk_devapc_instance *in = &ctx->inst[i];

			/*
			 * devm_platform_ioremap_resource() validates each window
			 * against its own "reg" entry, so a missing or malformed
			 * region is reported here instead of faulting later.
			 */
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

			in->nr_modules = MT6589_DEVPAPC_MAX_MODULES;
			in->dxs_vio_sta_bit = BIT(i);
		}
	} else {
		ctx->infra_base = of_iomap(node, 0);
		if (!ctx->infra_base)
			return -EINVAL;
	}

	devapc_irq = irq_of_parse_and_map(node, 0);
	if (!devapc_irq) {
		ret = -EINVAL;
		goto err_unmap_inst;
	}

	ctx->infra_clk = devm_clk_get_enabled(&pdev->dev, "devapc-infra-clock");
	if (IS_ERR(ctx->infra_clk)) {
		ret = -EINVAL;
		goto err_unmap_inst;
	}

	/*
	 * The violation interrupt is level sensitive: the MT6589 DTS declares
	 * IRQ_TYPE_LEVEL_LOW, and the downstream driver requests it with
	 * IRQF_TRIGGER_LOW | IRQF_SHARED.  Derive the trigger from the DT
	 * specifier rather than hardcoding one, so a board that wires it
	 * edge triggered still works.
	 *
	 * IRQF_SHARED is deliberately NOT set.  The vendor shares the line
	 * because its driver is paired with a userspace cdev control
	 * interface that also claims it; in mainline nothing else claims
	 * GIC SPI 94 on this platform, and claiming it shared would require
	 * every other owner to be equally correct.
	 */
	ret = of_irq_parse_one(node, 0, &oirq);
	if (!ret && oirq.args_count > 1)
		switch (oirq.args[1]) {
		case IRQ_TYPE_EDGE_FALLING:
			devapc_irq_flags = IRQF_TRIGGER_FALLING;
			break;
		case IRQ_TYPE_EDGE_RISING:
			devapc_irq_flags = IRQF_TRIGGER_RISING;
			break;
		default:
			/* Level sensitive, the MT6589 default. */
			devapc_irq_flags = IRQF_TRIGGER_LOW;
			break;
		}
	else
		devapc_irq_flags = IRQF_TRIGGER_LOW;

	ret = devm_request_irq(&pdev->dev, devapc_irq,
			       ctx->data->nr_instances > 1 ?
			       mt6589_violation_irq : devapc_violation_irq,
			       devapc_irq_flags, "devapc", ctx);
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
