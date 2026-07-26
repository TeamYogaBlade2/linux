// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for MTK architected m4u v1 implementations
 *
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Honghui Zhang <honghui.zhang@mediatek.com>
 *
 * Based on driver/iommu/mtk_iommu.c
 */
#include <linux/array_size.h>
#include <linux/bug.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string_choices.h>
#include <linux/types.h>
#include <asm/barrier.h>
#include <dt-bindings/memory/mtk-memory-port.h>
#include <dt-bindings/memory/mt2701-larb-port.h>
#include <dt-bindings/memory/mt6589-larb-port.h>
#include <soc/mediatek/smi.h>

#if defined(CONFIG_ARM)
#include <asm/dma-iommu.h>
#else
#define arm_iommu_create_mapping(...) NULL
#define arm_iommu_attach_device(...)	-ENODEV
struct dma_iommu_mapping {
	struct iommu_domain *domain;
};
#endif

#define REG_MMU_PT_BASE_ADDR			0x000

#define F_ALL_INVLD				0x2
#define F_MMU_INV_RANGE				0x1
#define F_INVLD_EN0				BIT(0)
#define F_INVLD_EN1				BIT(1)

#define F_MMU_FAULT_VA_MSK			0xfffff000
#define MTK_PROTECT_PA_ALIGN			128

/* -------- Common M4U v1 register definitions (MT2701 and MT6589 core) -------- */
#define REG_MMU_CTRL_REG			0x210
#define F_MMU_CTRL_COHERENT_EN			BIT(8)
#define REG_MMU_IVRP_PADDR			0x214
#define REG_MMU_INT_CONTROL			0x220
#define F_INT_TRANSLATION_FAULT			BIT(0)
#define F_INT_MAIN_MULTI_HIT_FAULT		BIT(1)
#define F_INT_INVALID_PA_FAULT			BIT(2)
#define F_INT_ENTRY_REPLACEMENT_FAULT		BIT(3)
#define F_INT_TABLE_WALK_FAULT			BIT(4)
#define F_INT_TLB_MISS_FAULT			BIT(5)
#define F_INT_PFH_DMA_FIFO_OVERFLOW		BIT(6)
#define F_INT_MISS_DMA_FIFO_OVERFLOW		BIT(7)

#define F_MMU_TF_PROTECT_SEL(prot)		(((prot) & 0x3) << 5)
#define F_INT_CLR_BIT				BIT(12)

#define REG_MMU_FAULT_ST			0x224
#define REG_MMU_FAULT_VA			0x228
#define REG_MMU_INVLD_PA			0x22C
#define REG_MMU_INT_ID				0x388

/* MT2701 specific (core space) */
#define REG_MMU_INVALIDATE			0x5c0
#define REG_MMU_INVLD_START_A			0x5c4
#define REG_MMU_INVLD_END_A			0x5c8

#define REG_MMU_INV_SEL				0x5d8
#define REG_MMU_STANDARD_AXI_MODE		0x5e8

#define REG_MMU_DCM				0x5f0
#define F_MMU_DCM_ON				BIT(1)
#define REG_MMU_CPE_DONE			0x60c

/* MT6589 global space registers */
#define REG_MMUg_CTRL				0x00
#define F_MMUg_CTRL_INV_EN0			BIT(0)
#define F_MMUg_CTRL_INV_EN1			BIT(1)
#define F_MMUg_CTRL_INV_EN2			BIT(2)	/* L2 */
#define F_MMUg_CTRL_PRE_LOCK(en)		((en) ? BIT(3) : 0)
#define F_MMUg_CTRL_PRE_EN			BIT(4)

#define REG_MMUg_INVLD				0x04
#define F_MMUg_INV_ALL				0x2
#define F_MMUg_INV_RANGE			0x1

#define REG_MMUg_INVLD_SA			0x08
#define REG_MMUg_INVLD_EA			0x0C
#define REG_MMUg_PT_BASE			0x10
#define F_MMUg_PT_VA_MSK			0xffff0000

#define REG_MMUg_L2_SEL				0x18
#define F_MMUg_L2_SEL_FLUSH_EN(en)		((en) ? BIT(3) : 0)
#define F_MMUg_L2_SEL_L2_ULTRA(en)		((en) ? BIT(2) : 0)
#define F_MMUg_L2_SEL_L2_SHARE(en)		((en) ? BIT(1) : 0)
#define F_MMUg_L2_SEL_L2_BUS_SEL(go_emi)	((go_emi) ? BIT(0) : 0)

#define REG_MMUg_DCM				0x1C
#define F_MMUg_DCM_ON(on)			((on) ? BIT(0) : 0)

/* L2 cache registers (MT6589) */
#define REG_L2_GDC_STATE			0x00
#define F_L2_GDC_ST_EVENT_MSK			GENMASK(7,6)
#define F_L2_GDC_ST_EVENT_VAL(val)		(((val) & 0x3) << 6)

#define REG_L2_GDC_OP				0x04
#define F_L2_GDC_BYPASS(en)			((en) ? BIT(10) : 0)
#define F_L2_GDC_PERF_MASK(msk)			(((msk) & 0x7) << 7)
#define GDC_PERF_MASK_HIT_MISS			0
#define F_L2_GDC_LOCK_ALERT_DIS(dis)		((dis) ? BIT(6) : 0)
#define F_L2_GDC_PERF_EN(en)			((en) ? BIT(5) : 0)
#define F_L2_GDC_LOCK_TH(th)			(((th) & 0x3) << 2)
#define F_L2_GDC_PAUSE_OP(op)			((op) & 0x3)
#define GDC_NO_PAUSE				0

#define REG_L2_GPE_STATUS			0x18
#define F_L2_GPE_ST_RANGE_INV_DONE		BIT(1)
#define F_L2_GPE_ST_PREFETCH_DONE		BIT(0)

/* MT6589 core PFH distance / direction registers */
#define REG_MMU_PFH_DIST(port)			(0x80 + (((port) >> 3) << 2))
#define F_MMU_PFH_DIST_VAL(port, val)		(((val) & 0xf) << (((port) & 0x7) << 2))
#define F_MMU_PFH_DIST_MASK(port)		F_MMU_PFH_DIST_VAL(port, 0xf)

#define REG_MMU_PFH_DIR(port)			(((port) < 32) ? 0xF0 : 0xF4)
#define F_MMU_PFH_DIR(port, val)		((!!(val)) << ((port) & 0x1f))

/* Common page table descriptor bits */
#define F_DESC_VALID				0x2
#define F_DESC_NONSEC				BIT(3)
/* MTK generation one iommu HW only support 4K size mapping */
#define MT2701_IOMMU_PAGE_SHIFT			12
#define MT2701_IOMMU_PAGE_SIZE			(1UL << MT2701_IOMMU_PAGE_SHIFT)

/*
 * MTK m4u support 4GB iova address space, and only support 4K page
 * mapping. So the pagetable size should be exactly as 4M.
 */
#define M2701_IOMMU_PGT_SIZE			SZ_4M

#define MAX_M4U_CORES				2

// static const int mt6589_larb_to_mmu[] = {0, 0, 1, 0, 1, 0};
static const int mt6589_larb_to_mmu[] = {0, 0, 1, 0, 1};

struct mtk_iommu_v1_data;

struct mtk_iommu_v1_soc_data {
	const char *compatible;
	unsigned int num_cores;
	bool has_global_base;
	bool has_l2_cache;

	void (*tlb_flush_all)(struct mtk_iommu_v1_data *data);
	void (*tlb_flush_range)(struct mtk_iommu_v1_data *data,
				unsigned long iova, size_t size);

	void (*get_fault_larb_port)(u32 int_id, unsigned int *larb,
				    unsigned int *port);

	int (*hw_init)(struct mtk_iommu_v1_data *data);

	u32 pt_base_reg_offset;
	bool pt_base_in_global;

	const int *larb_port_offsets;
	unsigned int num_larb;
};

struct mtk_iommu_v1_core {
	void __iomem *base;
	int irq;
	struct mtk_iommu_v1_data *data;
	unsigned int id;
};

struct mtk_iommu_v1_suspend_reg {
	/* MT2701 fields */
	u32			standard_axi_mode;
	u32			dcm_dis;
	u32			ctrl_reg;
	u32			int_control0;

	/* MT6589 additional fields */
	u32			mmug_ctrl;
	u32			mmug_pt_base;
	u32			mmug_l2_sel;
	u32			mmug_dcm;
	u32			l2_gdc_op;
};

struct mtk_iommu_v1_data {
	const struct mtk_iommu_v1_soc_data *soc;
	struct device *dev;

	struct mtk_iommu_v1_core cores[MAX_M4U_CORES];
	void __iomem *global_base;
	void __iomem *l2_base;

	struct clk *bclk;
	phys_addr_t protect_base;
	struct mtk_iommu_v1_domain *m4u_dom;

	struct iommu_device iommu;
	struct dma_iommu_mapping *mapping;
	struct mtk_smi_larb_iommu larb_imu[MTK_LARB_NR_MAX];

	struct mtk_iommu_v1_suspend_reg reg;
	struct page *dummy_page; /* Physical address of guard dummy page */
};

struct mtk_iommu_v1_domain {
	spinlock_t			pgtlock; /* lock for page table */
	struct iommu_domain		domain;
	u32				*pgt_va;
	dma_addr_t			pgt_pa;
	struct mtk_iommu_v1_data	*data;
};

static int mtk_iommu_v1_bind(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);

	return component_bind_all(dev, &data->larb_imu);
}

static void mtk_iommu_v1_unbind(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);

	component_unbind_all(dev, &data->larb_imu);
}

static struct mtk_iommu_v1_domain *to_mtk_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct mtk_iommu_v1_domain, domain);
}

static const int mt2701_m4u_in_larb[] = {
	MT2701_LARB0_PORT_OFFSET, MT2701_LARB1_PORT_OFFSET,
	MT2701_LARB2_PORT_OFFSET, MT2701_LARB3_PORT_OFFSET
};

static const int mt6589_m4u_in_larb[] = {
	MT6589_LARB0_PORT_OFFSET, MT6589_LARB1_PORT_OFFSET,
	MT6589_LARB2_PORT_OFFSET, MT6589_LARB3_PORT_OFFSET,
	MT6589_LARB4_PORT_OFFSET, MT6589_LARB5_PORT_OFFSET
};

static inline int mtk_iommu_v1_to_larb(struct mtk_iommu_v1_data *data, int id)
{
	const int *offsets = data->soc->larb_port_offsets;
	int num = data->soc->num_larb;
	int i;

	for (i = num - 1; i >= 0; i--)
		if (id >= offsets[i])
			return i;

	return 0;
}

static inline int mtk_iommu_v1_to_port(struct mtk_iommu_v1_data *data, int id)
{
	int larb = mtk_iommu_v1_to_larb(data, id);
	return id - data->soc->larb_port_offsets[larb];
}

static inline void m4u_set_field(void __iomem *reg, u32 mask, u32 val)
{
	u32 regval = readl_relaxed(reg);
	regval = (regval & ~mask) | val;
	writel_relaxed(regval, reg);
}

/* MT2701 (single core, no global space) */
static void mt2701_tlb_flush_all(struct mtk_iommu_v1_data *data)
{
	void __iomem *base = data->cores[0].base;
	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0, base + REG_MMU_INV_SEL);
	writel_relaxed(F_ALL_INVLD, base + REG_MMU_INVALIDATE);
	wmb();
}

static void mt2701_tlb_flush_range(struct mtk_iommu_v1_data *data,
				   unsigned long iova, size_t size)
{
	void __iomem *base = data->cores[0].base;
	u32 tmp;
	int ret;

	writel_relaxed(F_INVLD_EN1 | F_INVLD_EN0, base + REG_MMU_INV_SEL);
	writel_relaxed(iova & F_MMU_FAULT_VA_MSK, base + REG_MMU_INVLD_START_A);
	writel_relaxed((iova + size - 1) & F_MMU_FAULT_VA_MSK,
		   base + REG_MMU_INVLD_END_A);
	writel_relaxed(F_MMU_INV_RANGE, base + REG_MMU_INVALIDATE);

	ret = readl_poll_timeout_atomic(base + REG_MMU_CPE_DONE,
					tmp, tmp != 0, 10, 100000);
	if (ret) {
		dev_warn(data->dev,
			 "Partial TLB flush timed out, falling back to full flush\n");
		mt2701_tlb_flush_all(data);
	}
	writel_relaxed(0, base + REG_MMU_CPE_DONE);
}

/* MT6589 (global control, L2) */
static void mt6589_tlb_flush_all(struct mtk_iommu_v1_data *data)
{
	u32 reg = F_MMUg_CTRL_INV_EN0 | F_MMUg_CTRL_INV_EN1;
	if (data->l2_base)
		reg |= F_MMUg_CTRL_INV_EN2;

	writel_relaxed(reg, data->global_base + REG_MMUg_CTRL);
	writel_relaxed(F_MMUg_INV_ALL, data->global_base + REG_MMUg_INVLD);

	if (data->l2_base) {
		u32 event;
		readl_poll_timeout_atomic(data->l2_base + REG_L2_GDC_STATE,
					 event,
					 event & F_L2_GDC_ST_EVENT_MSK,
					 10, 100000);
		writel_relaxed(0, data->l2_base + REG_L2_GDC_STATE);
	}
}

static void mt6589_tlb_flush_range(struct mtk_iommu_v1_data *data,
				   unsigned long iova, size_t size)
{
	u32 reg = F_MMUg_CTRL_INV_EN0 | F_MMUg_CTRL_INV_EN1;
	if (data->l2_base)
		reg |= F_MMUg_CTRL_INV_EN2;

	writel_relaxed(reg, data->global_base + REG_MMUg_CTRL);
	writel_relaxed(iova & F_MMU_FAULT_VA_MSK,
		   data->global_base + REG_MMUg_INVLD_SA);
	writel_relaxed((iova + size - 1) & F_MMU_FAULT_VA_MSK,
		   data->global_base + REG_MMUg_INVLD_EA);
	writel_relaxed(F_MMUg_INV_RANGE, data->global_base + REG_MMUg_INVLD);

	if (data->l2_base) {
		u32 status;
		readl_poll_timeout_atomic(data->l2_base + REG_L2_GPE_STATUS,
					  status,
					  status & F_L2_GPE_ST_RANGE_INV_DONE,
					  10, 100000);
		writel_relaxed(0, data->l2_base + REG_L2_GPE_STATUS);
	}
}

static void mt2701_get_fault_larb_port(u32 int_id, unsigned int *larb,
				       unsigned int *port)
{
	*larb = 6 - ((int_id >> 13) & 0x7);
	*port = (int_id >> 8) & 0xF;
}

static void mt6589_get_fault_larb_port(u32 int_id, unsigned int *larb,
				       unsigned int *port)
{
	*larb = 6 - ((int_id >> 12) & 0x7);
	*port = (int_id >> 8) & 0xF;
}

static irqreturn_t mtk_iommu_v1_isr(int irq, void *dev_id)
{
	struct mtk_iommu_v1_core *core = dev_id;
	struct mtk_iommu_v1_data *data = core->data;
	struct mtk_iommu_v1_domain *dom = data->m4u_dom;
	u32 int_state, regval, fault_iova, fault_pa;
	unsigned int fault_larb, fault_port;

	/* Read error information from registers */
	int_state = readl_relaxed(core->base + REG_MMU_FAULT_ST);
	fault_iova = readl_relaxed(core->base + REG_MMU_FAULT_VA) & F_MMU_FAULT_VA_MSK;
	fault_pa = readl_relaxed(core->base + REG_MMU_INVLD_PA);
	regval = readl_relaxed(core->base + REG_MMU_INT_ID);

	data->soc->get_fault_larb_port(regval, &fault_larb, &fault_port);

	/*
	 * MTK v1 iommu HW could not determine whether the fault is read or
	 * write fault, report as read fault.
	 */
	if (report_iommu_fault(&dom->domain, data->dev, fault_iova,
			IOMMU_FAULT_READ))
		dev_err_ratelimited(data->dev,
			"fault type=0x%x iova=0x%x pa=0x%x larb=%d port=%d core=%d\n",
			int_state, fault_iova, fault_pa,
			fault_larb, fault_port, core->id);

	/* Interrupt clear */
	regval = readl_relaxed(core->base + REG_MMU_INT_CONTROL);
	regval |= F_INT_CLR_BIT;
	writel_relaxed(regval, core->base + REG_MMU_INT_CONTROL);

	data->soc->tlb_flush_all(data);

	return IRQ_HANDLED;
}

static void mtk_iommu_v1_config(struct mtk_iommu_v1_data *data,
				struct device *dev, bool enable)
{
	struct mtk_smi_larb_iommu *larb_mmu;
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	unsigned int larbid, portid, i, mmu_id;
	void __iomem *base;

	for (i = 0; i < fwspec->num_ids; ++i) {
		larbid = mtk_iommu_v1_to_larb(data, fwspec->ids[i]);
		portid = mtk_iommu_v1_to_port(data, fwspec->ids[i]);
		larb_mmu = &data->larb_imu[larbid];

		dev_dbg(dev, "%s iommu port: %d\n",
			str_enable_disable(enable), portid);

		if (enable)
			larb_mmu->mmu |= MTK_SMI_MMU_EN(portid);
		else
			larb_mmu->mmu &= ~MTK_SMI_MMU_EN(portid);
	}

	/* MT6589 specific: set default prefetch distance & direction */
	if (data->soc->has_global_base) {
		for (i = 0; i < fwspec->num_ids; i++) {
			portid = mtk_iommu_v1_to_port(data, fwspec->ids[i]);
			larbid = mtk_iommu_v1_to_larb(data, fwspec->ids[i]);
			mmu_id = mt6589_larb_to_mmu[larbid];
			base = data->cores[mmu_id].base;

			/* Set distance = 1 */
			m4u_set_field(base + REG_MMU_PFH_DIST(portid),
				      F_MMU_PFH_DIST_MASK(portid),
				      F_MMU_PFH_DIST_VAL(portid, 1));
			/* Set direction = 0 */
			m4u_set_field(base + REG_MMU_PFH_DIR(portid),
				      1 << (portid & 0x1f), 0);
		}
	}
}

static int mtk_iommu_v1_domain_finalise(struct mtk_iommu_v1_data *data)
{
	struct mtk_iommu_v1_domain *dom = data->m4u_dom;

	spin_lock_init(&dom->pgtlock);

	dom->pgt_va = dma_alloc_coherent(data->dev, M2701_IOMMU_PGT_SIZE,
					 &dom->pgt_pa, GFP_KERNEL);
	if (!dom->pgt_va)
		return -ENOMEM;

	if (data->soc->pt_base_in_global)
		writel(dom->pgt_pa, data->global_base + data->soc->pt_base_reg_offset);
	else
		writel(dom->pgt_pa, data->cores[0].base + data->soc->pt_base_reg_offset);

	dom->data = data;

	return 0;
}

static struct iommu_domain *mtk_iommu_v1_domain_alloc_paging(struct device *dev)
{
	struct mtk_iommu_v1_domain *dom;

	dom = kzalloc_obj(*dom);
	if (!dom)
		return NULL;

	dom->domain.pgsize_bitmap = MT2701_IOMMU_PAGE_SIZE;

	return &dom->domain;
}

static void mtk_iommu_v1_domain_free(struct iommu_domain *domain)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	struct mtk_iommu_v1_data *data = dom->data;

	dma_free_coherent(data->dev, M2701_IOMMU_PGT_SIZE,
			dom->pgt_va, dom->pgt_pa);
	kfree(to_mtk_domain(domain));
}

static int mtk_iommu_v1_attach_device(struct iommu_domain *domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	struct dma_iommu_mapping *mtk_mapping;
	int ret;

	/* Only allow the domain created internally. */
	mtk_mapping = data->mapping;
	if (mtk_mapping->domain != domain)
		return 0;

	if (!data->m4u_dom) {
		data->m4u_dom = dom;
		ret = mtk_iommu_v1_domain_finalise(data);
		if (ret) {
			data->m4u_dom = NULL;
			return ret;
		}
	}

	mtk_iommu_v1_config(data, dev, true);
	return 0;
}

static int mtk_iommu_v1_identity_attach(struct iommu_domain *identity_domain,
					struct device *dev,
					struct iommu_domain *old)
{
	struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);

	mtk_iommu_v1_config(data, dev, false);
	return 0;
}

static struct iommu_domain_ops mtk_iommu_v1_identity_ops = {
	.attach_dev = mtk_iommu_v1_identity_attach,
};

static struct iommu_domain mtk_iommu_v1_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &mtk_iommu_v1_identity_ops,
};

static int mtk_iommu_v1_map(struct iommu_domain *domain, unsigned long iova,
			    phys_addr_t paddr, size_t pgsize, size_t pgcount,
			    int prot, gfp_t gfp, size_t *mapped)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	struct mtk_iommu_v1_data *data = dom->data;
	unsigned long flags;
	unsigned int i;
	u32 *pgt_base_iova = dom->pgt_va + (iova >> MT2701_IOMMU_PAGE_SHIFT);
	u32 pabase = (u32)paddr;
	phys_addr_t dummy_pa = page_to_phys(data->dummy_page);
	unsigned int guard_pages = 0;

	if (data->soc->has_global_base) {
		/* Align to 4-entry boundary then add 4 more for PFH prefetch */
		unsigned int mod = pgcount & 0x3;
		guard_pages = (mod ? (4 - mod) : 0) + 4;
	}

	spin_lock_irqsave(&dom->pgtlock, flags);
	for (i = 0; i < pgcount; i++) {
		if (pgt_base_iova[i])
			break;
		pgt_base_iova[i] = pabase | F_DESC_VALID | F_DESC_NONSEC;
		pabase += MT2701_IOMMU_PAGE_SIZE;
	}

	if (guard_pages && i == pgcount) {
		for (i = 0; i < guard_pages; i++) {
			unsigned int idx = pgcount + i;
			if ((iova >> MT2701_IOMMU_PAGE_SHIFT) + idx >=
			    (M2701_IOMMU_PGT_SIZE / sizeof(u32)))
				break;
			if (!pgt_base_iova[idx])
				pgt_base_iova[idx] = dummy_pa | F_DESC_VALID | F_DESC_NONSEC;
		}
		i = pgcount; /* for *mapped calculation */
	}

	spin_unlock_irqrestore(&dom->pgtlock, flags);

	*mapped = i * MT2701_IOMMU_PAGE_SIZE;
	data->soc->tlb_flush_range(data, iova, *mapped);

	return i == pgcount ? 0 : -EEXIST;
}

static size_t mtk_iommu_v1_unmap(struct iommu_domain *domain, unsigned long iova,
				 size_t pgsize, size_t pgcount,
				 struct iommu_iotlb_gather *gather)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	u32 *pgt_base_iova = dom->pgt_va + (iova  >> MT2701_IOMMU_PAGE_SHIFT);
	size_t size = pgcount * MT2701_IOMMU_PAGE_SIZE;

	spin_lock_irqsave(&dom->pgtlock, flags);
	memset(pgt_base_iova, 0, pgcount * sizeof(u32));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	dom->data->soc->tlb_flush_range(dom->data, iova, size);

	return size;
}

static phys_addr_t mtk_iommu_v1_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	unsigned long flags;
	phys_addr_t pa;

	spin_lock_irqsave(&dom->pgtlock, flags);
	pa = *(dom->pgt_va + (iova >> MT2701_IOMMU_PAGE_SHIFT));
	pa = pa & (~(MT2701_IOMMU_PAGE_SIZE - 1));
	spin_unlock_irqrestore(&dom->pgtlock, flags);

	return pa;
}

static const struct iommu_ops mtk_iommu_v1_ops;

/*
 * MTK generation one iommu HW only support one iommu domain, and all the client
 * sharing the same iova address space.
 */
static int mtk_iommu_v1_create_mapping(struct device *dev,
				       const struct of_phandle_args *args)
{
	struct mtk_iommu_v1_data *data;
	struct platform_device *m4updev;
	struct dma_iommu_mapping *mtk_mapping;
	int ret;

	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",
			args->args_count);
		return -EINVAL;
	}

	ret = iommu_fwspec_init(dev, of_fwnode_handle(args->np));
	if (ret)
		return ret;

	if (!dev_iommu_priv_get(dev)) {
		/* Get the m4u device */
		m4updev = of_find_device_by_node(args->np);
		if (WARN_ON(!m4updev))
			return -EINVAL;

		dev_iommu_priv_set(dev, platform_get_drvdata(m4updev));

		put_device(&m4updev->dev);
	}

	ret = iommu_fwspec_add_ids(dev, args->args, 1);
	if (ret)
		return ret;

	data = dev_iommu_priv_get(dev);
	mtk_mapping = data->mapping;
	if (!mtk_mapping) {
		/* MTK iommu support 4GB iova address space. */
		mtk_mapping = arm_iommu_create_mapping(dev, 0, 1ULL << 32);
		if (IS_ERR(mtk_mapping))
			return PTR_ERR(mtk_mapping);

		data->mapping = mtk_mapping;
	}

	return 0;
}

static struct iommu_device *mtk_iommu_v1_probe_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = NULL;
	struct of_phandle_args iommu_spec;
	struct mtk_iommu_v1_data *data;
	int err, idx = 0, larbid, larbidx;
	struct device_link *link;
	struct device *larbdev;

	while (!of_parse_phandle_with_args(dev->of_node, "iommus",
					   "#iommu-cells",
					   idx, &iommu_spec)) {

		err = mtk_iommu_v1_create_mapping(dev, &iommu_spec);
		of_node_put(iommu_spec.np);
		if (err)
			return ERR_PTR(err);

		/* dev->iommu_fwspec might have changed */
		fwspec = dev_iommu_fwspec_get(dev);
		idx++;
	}

	if (!fwspec)
		return ERR_PTR(-ENODEV);

	data = dev_iommu_priv_get(dev);

	/* Link the consumer device with the smi-larb device(supplier) */
	larbid = mtk_iommu_v1_to_larb(data, fwspec->ids[0]);
	if (larbid >= MTK_LARB_NR_MAX)
		return ERR_PTR(-EINVAL);

	for (idx = 1; idx < fwspec->num_ids; idx++) {
		larbidx = mtk_iommu_v1_to_larb(data, fwspec->ids[idx]);
		if (larbid != larbidx) {
			dev_err(dev, "Can only use one larb. Fail@larb%d-%d.\n",
				larbid, larbidx);
			return ERR_PTR(-EINVAL);
		}
	}

	larbdev = data->larb_imu[larbid].dev;
	if (!larbdev)
		return ERR_PTR(-EINVAL);

	link = device_link_add(dev, larbdev,
			       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
	if (!link)
		dev_err(dev, "Unable to link %s\n", dev_name(larbdev));

	return &data->iommu;
}

static void mtk_iommu_v1_probe_finalize(struct device *dev)
{
	__maybe_unused struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);
	int err;

	err = arm_iommu_attach_device(dev, data->mapping);
	if (err)
		dev_err(dev, "Can't create IOMMU mapping - DMA-OPS will not work\n");
}

static void mtk_iommu_v1_release_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct mtk_iommu_v1_data *data;
	struct device *larbdev;
	unsigned int larbid;

	data = dev_iommu_priv_get(dev);
	larbid = mtk_iommu_v1_to_larb(data, fwspec->ids[0]);
	larbdev = data->larb_imu[larbid].dev;
	device_link_remove(dev, larbdev);
}

static int mt2701_hw_init(struct mtk_iommu_v1_data *data)
{
	u32 regval;
	int ret;

	ret = clk_prepare_enable(data->bclk);
	if (ret) {
		dev_err(data->dev, "Failed to enable iommu bclk(%d)\n", ret);
		return ret;
	}

	regval = F_MMU_CTRL_COHERENT_EN | F_MMU_TF_PROTECT_SEL(2);
	writel_relaxed(regval, data->cores[0].base + REG_MMU_CTRL_REG);

	regval = F_INT_TRANSLATION_FAULT |
		F_INT_MAIN_MULTI_HIT_FAULT |
		F_INT_INVALID_PA_FAULT |
		F_INT_ENTRY_REPLACEMENT_FAULT |
		F_INT_TABLE_WALK_FAULT |
		F_INT_TLB_MISS_FAULT |
		F_INT_PFH_DMA_FIFO_OVERFLOW |
		F_INT_MISS_DMA_FIFO_OVERFLOW;
	writel_relaxed(regval, data->cores[0].base + REG_MMU_INT_CONTROL);

	writel_relaxed(data->protect_base, data->cores[0].base + REG_MMU_IVRP_PADDR);
	writel_relaxed(F_MMU_DCM_ON, data->cores[0].base + REG_MMU_DCM);

	return 0;
}

static int mt6589_hw_init(struct mtk_iommu_v1_data *data)
{
	u32 regval;
	int i, ret;

	ret = clk_prepare_enable(data->bclk);
	if (ret) {
		dev_err(data->dev, "Failed to enable bclk\n");
		goto err_clk;
	}

	/* ---- Global registers ---- */
	writel_relaxed(F_MMUg_L2_SEL_FLUSH_EN(1) | F_MMUg_L2_SEL_L2_ULTRA(1) |
		   F_MMUg_L2_SEL_L2_SHARE(0) | F_MMUg_L2_SEL_L2_BUS_SEL(1),
		   data->global_base + REG_MMUg_L2_SEL);
	writel_relaxed(F_MMUg_DCM_ON(1), data->global_base + REG_MMUg_DCM);

	/*
	 * Before any TLB operations, set a safe dummy page table base address.
	 * The real page table will be set later in domain_finalise().
	 * Use the protect buffer as a dummy, as downstream does.
	 */
	writel_relaxed(data->protect_base, data->global_base + REG_MMUg_PT_BASE);

	/* ---- L2 cache ---- */
	if (data->l2_base) {
		regval = F_L2_GDC_BYPASS(0) |
			 F_L2_GDC_PERF_MASK(GDC_PERF_MASK_HIT_MISS) |
			 F_L2_GDC_LOCK_ALERT_DIS(0) |
			 F_L2_GDC_LOCK_TH(3) |
			 F_L2_GDC_PAUSE_OP(GDC_NO_PAUSE);
		writel_relaxed(regval, data->l2_base + REG_L2_GDC_OP);
	}

	/* ---- Per-core setup ---- */
	for (i = 0; i < data->soc->num_cores; i++) {
		void __iomem *base = data->cores[i].base;

		regval = 0;  /* PFH enabled, walk enabled, cohere disabled */
		regval |= F_MMU_TF_PROTECT_SEL(2);
		writel_relaxed(regval, base + REG_MMU_CTRL_REG);

		regval = F_INT_TRANSLATION_FAULT |
			 F_INT_MAIN_MULTI_HIT_FAULT |
			 F_INT_INVALID_PA_FAULT |
			 F_INT_ENTRY_REPLACEMENT_FAULT |
			 F_INT_TABLE_WALK_FAULT |
			 F_INT_TLB_MISS_FAULT |
			 F_INT_PFH_DMA_FIFO_OVERFLOW |
			 F_INT_MISS_DMA_FIFO_OVERFLOW;
		writel_relaxed(regval, base + REG_MMU_INT_CONTROL);
		writel_relaxed(0xff, base + REG_MMU_FAULT_ST);
		writel_relaxed(data->protect_base, base + REG_MMU_IVRP_PADDR);
	}

	return 0;

err_clk:
	return ret;
}

static const struct iommu_ops mtk_iommu_v1_ops = {
	.identity_domain = &mtk_iommu_v1_identity_domain,
	.domain_alloc_paging = mtk_iommu_v1_domain_alloc_paging,
	.probe_device	= mtk_iommu_v1_probe_device,
	.probe_finalize = mtk_iommu_v1_probe_finalize,
	.release_device	= mtk_iommu_v1_release_device,
	.device_group	= generic_device_group,
	.owner          = THIS_MODULE,
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= mtk_iommu_v1_attach_device,
		.map_pages	= mtk_iommu_v1_map,
		.unmap_pages	= mtk_iommu_v1_unmap,
		.iova_to_phys	= mtk_iommu_v1_iova_to_phys,
		.free		= mtk_iommu_v1_domain_free,
	}
};

static const struct mtk_iommu_v1_soc_data mt2701_soc_data = {
	.compatible = "mediatek,mt2701-m4u",
	.num_cores = 1,
	.has_global_base = false,
	.has_l2_cache = false,
	.tlb_flush_all = mt2701_tlb_flush_all,
	.tlb_flush_range = mt2701_tlb_flush_range,
	.get_fault_larb_port = mt2701_get_fault_larb_port,
	.hw_init = mt2701_hw_init,
	.pt_base_reg_offset = REG_MMU_PT_BASE_ADDR,
	.pt_base_in_global = false,
	.larb_port_offsets = mt2701_m4u_in_larb,
	.num_larb = ARRAY_SIZE(mt2701_m4u_in_larb),
};

static const struct mtk_iommu_v1_soc_data mt6589_soc_data = {
	.compatible = "mediatek,mt6589-m4u",
	.num_cores = 2,
	.has_global_base = true,
	.has_l2_cache = true,
	.tlb_flush_all = mt6589_tlb_flush_all,
	.tlb_flush_range = mt6589_tlb_flush_range,
	.get_fault_larb_port = mt6589_get_fault_larb_port,
	.hw_init = mt6589_hw_init,
	.pt_base_reg_offset = REG_MMUg_PT_BASE,
	.pt_base_in_global = true,
	.larb_port_offsets = mt6589_m4u_in_larb,
	.num_larb = ARRAY_SIZE(mt6589_m4u_in_larb),
};

static const struct of_device_id mtk_iommu_v1_of_ids[] = {
	{ .compatible = "mediatek,mt2701-m4u", .data = &mt2701_soc_data },
	{ .compatible = "mediatek,mt6589-m4u", .data = &mt6589_soc_data },
	{}
};
MODULE_DEVICE_TABLE(of, mtk_iommu_v1_of_ids);

static const struct component_master_ops mtk_iommu_v1_com_ops = {
	.bind		= mtk_iommu_v1_bind,
	.unbind		= mtk_iommu_v1_unbind,
};

static int mtk_iommu_v1_probe(struct platform_device *pdev)
{
	struct device			*dev = &pdev->dev;
	struct mtk_iommu_v1_data	*data;
	const struct mtk_iommu_v1_soc_data *soc;
	struct component_match		*match = NULL;
	void				*protect;
	int				larb_nr, ret, i;
	struct page			*dummy_page;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	soc = of_device_get_match_data(dev);
	data->soc = soc;

	/* Protect memory. HW will access here while translation fault.*/
	protect = devm_kcalloc(dev, 2, MTK_PROTECT_PA_ALIGN,
			       GFP_KERNEL | GFP_DMA);
	if (!protect)
		return -ENOMEM;
	data->protect_base = ALIGN(virt_to_phys(protect), MTK_PROTECT_PA_ALIGN);

	if (soc->has_global_base) {
		data->global_base = devm_platform_ioremap_resource_byname(pdev,
									  "global");
		if (IS_ERR(data->global_base))
			return PTR_ERR(data->global_base);
	}
	for (i = 0; i < soc->num_cores; i++) {
		char name[8];
		snprintf(name, sizeof(name), "m4u%d", i);
		data->cores[i].base = devm_platform_ioremap_resource_byname(pdev,
									    name);
		if (IS_ERR(data->cores[i].base))
			return PTR_ERR(data->cores[i].base);
		data->cores[i].data = data;
		data->cores[i].id = i;
	}
	if (soc->has_l2_cache) {
		data->l2_base = devm_platform_ioremap_resource_byname(pdev,
								      "l2cache");
		if (IS_ERR(data->l2_base))
			return PTR_ERR(data->l2_base);
	}

	data->bclk = devm_clk_get(dev, "bclk");
	if (IS_ERR(data->bclk))
		return PTR_ERR(data->bclk);

	/* Interrupts - request after hw_init to avoid spurious IRQs? but
	   we need the core IRQs ready before registering ISR. We'll request
	   them after hw_init for simplicity. */
	ret = soc->hw_init(data);
	if (ret)
		return ret;

	for (i = 0; i < soc->num_cores; i++) {
		struct mtk_iommu_v1_core *core = &data->cores[i];
		char irqname[8];
		snprintf(irqname, sizeof(irqname), "m4u%d", i);
		core->irq = platform_get_irq_byname(pdev, irqname);
		if (core->irq < 0) {
			ret = core->irq;
			goto out_clk_unprepare;
		}
		ret = devm_request_irq(dev, core->irq, mtk_iommu_v1_isr, 0,
				       dev_name(dev), core);
		if (ret) {
			dev_err(dev, "Failed to request IRQ %d for core%d\n",
				core->irq, i);
			goto out_clk_unprepare;
		}
	}

	larb_nr = of_count_phandle_with_args(dev->of_node,
					     "mediatek,larbs", NULL);
	if (larb_nr < 0) {
		ret = larb_nr;
		goto out_clk_unprepare;
	}

	if (larb_nr > MTK_LARB_NR_MAX) {
		ret = -EINVAL;
		goto out_clk_unprepare;
	}

	for (i = 0; i < larb_nr; i++) {
		struct device_node *larbnode;
		struct platform_device *plarbdev;

		larbnode = of_parse_phandle(dev->of_node, "mediatek,larbs", i);
		if (!larbnode) {
			ret = -EINVAL;
			goto out_put_larbs;
		}

		if (!of_device_is_available(larbnode)) {
			of_node_put(larbnode);
			continue;
		}

		plarbdev = of_find_device_by_node(larbnode);
		if (!plarbdev) {
			of_node_put(larbnode);
			ret = -ENODEV;
			goto out_put_larbs;
		}
		if (!plarbdev->dev.driver) {
			of_node_put(larbnode);
			put_device(&plarbdev->dev);
			ret = -EPROBE_DEFER;
			goto out_put_larbs;
		}
		data->larb_imu[i].dev = &plarbdev->dev;

		component_match_add_release(dev, &match, component_release_of,
					    component_compare_of, larbnode);
	}

	dummy_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!dummy_page) {
		ret = -ENOMEM;
		goto out_put_larbs;
	}
	data->dummy_page = dummy_page;

	platform_set_drvdata(pdev, data);

	ret = iommu_device_sysfs_add(&data->iommu, dev, NULL,
				     dev_name(dev));
	if (ret)
		goto out_put_larbs;

	ret = iommu_device_register(&data->iommu, &mtk_iommu_v1_ops, dev);
	if (ret)
		goto out_sysfs_remove;

	ret = component_master_add_with_match(dev, &mtk_iommu_v1_com_ops, match);
	if (ret)
		goto out_dev_unreg;
	return ret;

out_dev_unreg:
	iommu_device_unregister(&data->iommu);
out_sysfs_remove:
	iommu_device_sysfs_remove(&data->iommu);
out_put_larbs:
	for (i = 0; i < MTK_LARB_NR_MAX; i++)
		if (data->larb_imu[i].dev)
			put_device(data->larb_imu[i].dev);
	if (data->dummy_page)
		__free_page(data->dummy_page);
out_clk_unprepare:
	clk_disable_unprepare(data->bclk);
	return ret;
}

static void mtk_iommu_v1_remove(struct platform_device *pdev)
{
	struct mtk_iommu_v1_data *data = platform_get_drvdata(pdev);
	int i;

	iommu_device_sysfs_remove(&data->iommu);
	iommu_device_unregister(&data->iommu);

	clk_disable_unprepare(data->bclk);
	for (i = 0; i < data->soc->num_cores; i++)
		devm_free_irq(&pdev->dev, data->cores[i].irq, &data->cores[i]);
	component_master_del(&pdev->dev, &mtk_iommu_v1_com_ops);
	__free_page(data->dummy_page);

	for (i = 0; i < MTK_LARB_NR_MAX; i++)
		if (data->larb_imu[i].dev)
			put_device(data->larb_imu[i].dev);
}

static int __maybe_unused mtk_iommu_v1_suspend(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_v1_suspend_reg *reg = &data->reg;
	void __iomem *base = data->cores[0].base;

	/* Common core registers */
	reg->ctrl_reg = readl_relaxed(base + REG_MMU_CTRL_REG);
	reg->int_control0 = readl_relaxed(base + REG_MMU_INT_CONTROL);

	if (data->soc->has_global_base) {
		reg->mmug_ctrl = readl_relaxed(data->global_base + REG_MMUg_CTRL);
		reg->mmug_pt_base = readl_relaxed(data->global_base + REG_MMUg_PT_BASE);
		reg->mmug_l2_sel = readl_relaxed(data->global_base + REG_MMUg_L2_SEL);
		reg->mmug_dcm = readl_relaxed(data->global_base + REG_MMUg_DCM);
		if (data->l2_base)
			reg->l2_gdc_op = readl_relaxed(data->l2_base + REG_L2_GDC_OP);
	} else {
		/* MT2701 specific */
		base = data->cores[0].base;
		reg->standard_axi_mode = readl_relaxed(base + REG_MMU_STANDARD_AXI_MODE);
		reg->dcm_dis = readl_relaxed(base + REG_MMU_DCM);
	}

	return 0;
}

static void mt6589_restore_pfh_settings(struct mtk_iommu_v1_data *data)
{
	const int *offsets = data->soc->larb_port_offsets;
	int num_larb = data->soc->num_larb;
	int larb, port;

	for (larb = 0; larb < num_larb; larb++) {
		int mmu_id = mt6589_larb_to_mmu[larb];
		void __iomem *base = data->cores[mmu_id].base;
		int first_port = offsets[larb];
		int last_port = (larb == num_larb - 1) ?
				MT6589_M4U_PORT_NR - 1 :
				offsets[larb + 1] - 1;

		for (port = first_port; port <= last_port; port++) {
			m4u_set_field(base + REG_MMU_PFH_DIST(port),
				      F_MMU_PFH_DIST_MASK(port),
				      F_MMU_PFH_DIST_VAL(port, 1));
			m4u_set_field(base + REG_MMU_PFH_DIR(port),
				      1 << (port & 0x1f), 0);
		}
	}
}

static int __maybe_unused mtk_iommu_v1_resume(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_v1_suspend_reg *reg = &data->reg;
	void __iomem *base = data->cores[0].base;
	int i;

	if (data->soc->has_global_base) {
		writel_relaxed(reg->mmug_ctrl, data->global_base + REG_MMUg_CTRL);
		writel_relaxed(reg->mmug_pt_base, data->global_base + REG_MMUg_PT_BASE);
		writel_relaxed(reg->mmug_l2_sel, data->global_base + REG_MMUg_L2_SEL);
		writel_relaxed(reg->mmug_dcm, data->global_base + REG_MMUg_DCM);
		if (data->l2_base)
			writel_relaxed(reg->l2_gdc_op, data->l2_base + REG_L2_GDC_OP);
	}

	/* Per-core restore (common) */
	for (i = 0; i < data->soc->num_cores; i++) {
		base = data->cores[i].base;
		writel_relaxed(reg->ctrl_reg, base + REG_MMU_CTRL_REG);
		writel_relaxed(reg->int_control0, base + REG_MMU_INT_CONTROL);
		writel_relaxed(data->protect_base, base + REG_MMU_IVRP_PADDR);
	}

	if (!data->soc->has_global_base) {
		/* MT2701 extra */
		base = data->cores[0].base;
		writel_relaxed(reg->standard_axi_mode, base + REG_MMU_STANDARD_AXI_MODE);
		writel_relaxed(reg->dcm_dis, base + REG_MMU_DCM);
	}

	if (data->m4u_dom && data->m4u_dom->pgt_pa) {
		if (data->soc->pt_base_in_global)
			writel_relaxed(data->m4u_dom->pgt_pa, data->global_base + data->soc->pt_base_reg_offset);
		else
			writel_relaxed(data->m4u_dom->pgt_pa, data->cores[0].base + data->soc->pt_base_reg_offset);
	}

	if (data->soc->has_global_base)
		mt6589_restore_pfh_settings(data);

	return 0;
}

static const struct dev_pm_ops mtk_iommu_v1_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_iommu_v1_suspend, mtk_iommu_v1_resume)
};

static struct platform_driver mtk_iommu_v1_driver = {
	.probe	= mtk_iommu_v1_probe,
	.remove = mtk_iommu_v1_remove,
	.driver	= {
		.name = "mtk-iommu-v1",
		.of_match_table = mtk_iommu_v1_of_ids,
		.pm = &mtk_iommu_v1_pm_ops,
	}
};
module_platform_driver(mtk_iommu_v1_driver);

MODULE_DESCRIPTION("IOMMU API for MediaTek M4U v1 implementations");
MODULE_LICENSE("GPL v2");
