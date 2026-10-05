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
#include <linux/pm_runtime.h>
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
#define F_MMU_CTRL_PFH_DIS(dis)			((!!(dis)) << 0)
#define F_MMU_CTRL_TLB_WALK_DIS(dis)		((!!(dis)) << 1)
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
/*
 * REG_MMUg_PT_BASE is documented by its own field mask: both this tree and
 * the vendor tree define F_MMUg_PT_VA_MSK as 0xffff0000 for this register
 * group, i.e. only bits [31:16] of the address are carried.  The base must
 * therefore be 64 KiB aligned, or the low 16 bits are dropped and the M4U
 * walks memory that is not the page table.
 *
 * The vendor driver does not assume this; in
 * aquaris-5 .../mt6589/kernel/drivers/m4u/m4u.c:
 *
 *	#define M4U_PAGE_TABLE_ALIGN (PT_TOTAL_ENTRY_NUM*sizeof(unsigned int) - 1)
 *	// page table addr should (2^16)x align
 *
 * and m4u_struct_init() frees and re-allocates the table whenever
 * dma_alloc_coherent() returns one that violates it.
 *
 * M2701_IOMMU_PGT_SIZE (4 MiB) is a multiple of 64 KiB and dma_alloc_coherent()
 * returns page aligned memory, so the requirement holds in practice.  Assert
 * it instead of trusting the allocator to keep meeting it.
 */
#define MTK_IOMMU_PT_BASE_ALIGN		SZ_64K

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

struct mtk_iommu_v1_data;

struct mtk_iommu_v1_soc_data {
	const char *compatible;
	unsigned int num_cores;
	bool has_global_base;
	bool has_l2_cache;
	u32 int_en_mask;

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
	u32			ctrl_reg[MAX_M4U_CORES];
	u32			int_control0[MAX_M4U_CORES];

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

	/*
	 * Guards @mapping, which is shared by *every* client of this M4U: the
	 * hardware has a single page table and a single IOVA space, so one
	 * mapping is created and then attached to each device in turn.  That
	 * makes the lazy "create it if nobody has yet" step a read-modify-write
	 * on state every client races for, and the clients are probed from
	 * different contexts (the bus notifier, the iommu_device_register()
	 * replay over the bus, and an explicit iommu_probe_device() from a
	 * driver binding late), so the check and the creation have to be one
	 * critical section.  Sleeping is fine and required: the creation
	 * allocates a 4 MiB page table bitmap under GFP_KERNEL.
	 */
	struct mutex mapping_lock;
};

struct mtk_iommu_v1_domain {
	spinlock_t			pgtlock; /* lock for page table */
	struct iommu_domain		domain;
	u32				*pgt_va;
	dma_addr_t			pgt_pa;
	struct mtk_iommu_v1_data	*data;
};

static struct dma_iommu_mapping *mtk_iommu_v1_get_mapping(struct device *dev);

static void mt6589_enable_translation(struct mtk_iommu_v1_data *data)
{
	int i;

	for (i = 0; i < data->soc->num_cores; i++) {
		void __iomem *base = data->cores[i].base;

		u32 ctrl = F_MMU_CTRL_PFH_DIS(0) |
			   F_MMU_CTRL_TLB_WALK_DIS(0) |
			   F_MMU_TF_PROTECT_SEL(2);
		writel_relaxed(ctrl, base + REG_MMU_CTRL_REG);
	}

	/* Invalidate any stale TLB entries */
	data->soc->tlb_flush_all(data);
}

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
	LARB0_PORT_OFFSET, LARB1_PORT_OFFSET,
	LARB2_PORT_OFFSET, LARB3_PORT_OFFSET
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

static void mt6589_l2_clear_status(struct mtk_iommu_v1_data *data,
				   unsigned int reg_offset, u32 mask)
{
	u32 reg;

	reg = readl_relaxed(data->l2_base + reg_offset);
	reg &= ~mask;
	writel_relaxed(reg, data->l2_base + reg_offset);
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
		int ret;

		ret = readl_poll_timeout_atomic(data->l2_base + REG_L2_GDC_STATE,
						event,
						event & F_L2_GDC_ST_EVENT_MSK,
						10, 100000);
		if (ret)
			dev_warn_ratelimited(data->dev,
					     "MT6589 L2 full TLB invalidation timed out\n");

		mt6589_l2_clear_status(data, REG_L2_GDC_STATE,
				       F_L2_GDC_ST_EVENT_MSK);
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
		int ret;

		ret = readl_poll_timeout_atomic(
			data->l2_base + REG_L2_GPE_STATUS, status,
			status & F_L2_GPE_ST_RANGE_INV_DONE,
			10, 100000);
		if (ret) {
			dev_warn_ratelimited(data->dev,
					     "MT6589 L2 range TLB invalidation timed out; falling back to full flush\n");
			mt6589_l2_clear_status(data, REG_L2_GPE_STATUS,
					       F_L2_GPE_ST_RANGE_INV_DONE);
			mt6589_tlb_flush_all(data);
			return;
		}

		mt6589_l2_clear_status(data, REG_L2_GPE_STATUS,
				       F_L2_GPE_ST_RANGE_INV_DONE);
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
	struct mtk_iommu_v1_domain *dom;
	u32 int_state, regval, fault_iova, fault_pa;
	unsigned int fault_larb, fault_port;

	/* The domain may not be ready yet; just clear the interrupt */
	if (!data->m4u_dom) {
		regval = readl_relaxed(core->base + REG_MMU_INT_CONTROL);
		regval |= F_INT_CLR_BIT;
		writel_relaxed(regval, core->base + REG_MMU_INT_CONTROL);
		return IRQ_HANDLED;
	}
	dom = data->m4u_dom;

	/* Read error information from registers */
	int_state = readl_relaxed(core->base + REG_MMU_FAULT_ST);
	fault_iova = readl_relaxed(core->base + REG_MMU_FAULT_VA) & F_MMU_FAULT_VA_MSK;
	fault_pa = readl_relaxed(core->base + REG_MMU_INVLD_PA);
	regval = readl_relaxed(core->base + REG_MMU_INT_ID);

	data->soc->get_fault_larb_port(regval, &fault_larb, &fault_port);

	/*
	 * M4U v1 hardware does not record the direction of the faulting
	 * access: the MT6589 fault status register (REG_MMU_FAULT_ST) only
	 * carries the fault type bits, and REG_MMU_INT_ID only the LARB and
	 * port.  There is no read/write bit to report, so the direction is
	 * genuinely unknown here rather than known to be a read.
	 *
	 * report_iommu_fault() takes one of the two directions the API
	 * defines (IOMMU_FAULT_READ / IOMMU_FAULT_WRITE -- there is no
	 * "unknown" value in this tree), so keep passing the read value and
	 * say so explicitly in the message below rather than letting the
	 * tracepoint alone imply that the access was a read.
	 */
	if (report_iommu_fault(&dom->domain, data->dev, fault_iova,
			IOMMU_FAULT_READ))
		dev_err_ratelimited(data->dev,
			"fault type=0x%x iova=0x%x pa=0x%x larb=%d port=%d core=%d dir=unknown\n",
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
	unsigned int larbid, portid, i;

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

}

static int mtk_iommu_v1_write_pt_base(struct mtk_iommu_v1_data *data,
				      dma_addr_t pgt_pa)
{
	void __iomem *base;

	/*
	 * Only the MT6589 register format is known to drop the low bits
	 * (see MTK_IOMMU_PT_BASE_ALIGN above); the MT2701 core register is
	 * left alone rather than guessing its field width.
	 */
	if (WARN_ON_ONCE(data->soc->pt_base_in_global &&
			 !IS_ALIGNED(pgt_pa, MTK_IOMMU_PT_BASE_ALIGN)))
		return -EINVAL;

	base = data->soc->pt_base_in_global ? data->global_base :
					      data->cores[0].base;
	writel(pgt_pa, base + data->soc->pt_base_reg_offset);

	return 0;
}

static int mtk_iommu_v1_domain_finalise(struct mtk_iommu_v1_data *data)
{
	struct mtk_iommu_v1_domain *dom = data->m4u_dom;

	spin_lock_init(&dom->pgtlock);

	dom->pgt_va = dma_alloc_coherent(data->dev, M2701_IOMMU_PGT_SIZE,
					 &dom->pgt_pa, GFP_KERNEL);
	if (!dom->pgt_va)
		return -ENOMEM;

	/*
	 * A misaligned base cannot be worked around here: the register
	 * simply has no bits to store it in.  Fail the attach rather than
	 * enable translation on top of a base the M4U will misread.
	 */
	if (mtk_iommu_v1_write_pt_base(data, dom->pgt_pa))
		return -EINVAL;

	/*
	 * Now that a valid page table is in place and all ports are in
	 * physical mode, it is safe to enable translation.
	 */
	if (data->soc->has_global_base)
		mt6589_enable_translation(data);

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
	dom->data = dev_iommu_priv_get(dev);

	return &dom->domain;
}

static void mtk_iommu_v1_domain_free(struct iommu_domain *domain)
{
	struct mtk_iommu_v1_domain *dom = to_mtk_domain(domain);
	struct mtk_iommu_v1_data *data = dom->data;

	/* The page table only exists once the domain got attached. */
	if (dom->pgt_va)
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

	/*
	 * Only allow the domain created internally.  The mapping is fetched
	 * rather than read straight out of @data so that a missing mapping is
	 * reported as an error instead of being dereferenced; it was built once
	 * in .of_xlate() before the core ever got here, so by construction this
	 * cannot be the first client of the M4U.
	 */
	mtk_mapping = mtk_iommu_v1_get_mapping(dev);
	if (IS_ERR(mtk_mapping))
		return PTR_ERR(mtk_mapping);

	if (mtk_mapping->domain != domain) {
		dev_warn(dev, "Ignoring attach request for foreign domain\n");
		return 0;
	}

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

	spin_lock_irqsave(&dom->pgtlock, flags);
	for (i = 0; i < pgcount; i++) {
		if (pgt_base_iova[i]) {
			memset(pgt_base_iova, 0, i * sizeof(u32));
			break;
		}
		pgt_base_iova[i] = pabase | F_DESC_VALID | F_DESC_NONSEC;
		pabase += MT2701_IOMMU_PAGE_SIZE;
	}

	spin_unlock_irqrestore(&dom->pgtlock, flags);

	*mapped = i * MT2701_IOMMU_PAGE_SIZE;
	if (*mapped)
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
 * sharing the same iova address space.  That is why the mapping is created
 * once and then attached to each client in turn, rather than built per device:
 * a second mapping would be a second page-table bitmap over the same IOVA range
 * and a second domain the hardware cannot translate with.
 *
 * The creation is under data->mapping_lock and the NULL test is inside that
 * lock, because the clients are probed concurrently - from the bus notifier,
 * from the replay bus_iommu_probe() runs at iommu_device_register(), and from
 * any late iommu_probe_device() a driver binding late triggers - and they all
 * share this one @data.  An unsynchronised "if (!data->mapping)" lets two
 * clients each build a mapping: one wins and the other is silently orphaned,
 * leaking its bitmap and its 4 MiB page table, while the loser still attaches
 * the winner's.  Check-and-create in one critical section makes the second
 * caller wait and then find the mapping already there, so exactly one mapping
 * exists for the lifetime of the M4U and every client attaches that same
 * pointer.
 *
 * Returns the shared mapping, or an error pointer.
 */
/*
 * mtk_iommu_v1_get_mapping - return the M4U's one shared mapping.
 * @dev: any client of this M4U
 *
 * Returns the shared mapping, or an error pointer.  This NEVER creates: the
 * mapping is built once in .of_xlate and every later caller only reads it.
 *
 * The creation deliberately does not happen here.  .attach_dev is called from
 * iommu_setup_default_domain() -> __iommu_group_set_domain() inside
 * __iommu_probe_device() (drivers/iommu/iommu.c), which is strictly before
 * .probe_finalize() (iommu.c:722).  So a lazy create in either of those
 * callbacks runs too early for the first client of an M4U, and doing it there
 * also means the first attach - rather than the first fwspec - decides when
 * hardware state is programmed.
 *
 * Instead .of_xlate() creates it.  That runs once per client, early, with no
 * group and no domain involved, and it is exactly where the M4U's private data
 * is first resolved from the phandle - so this is the first point at which we
 * can know which M4U we belong to.  arm_iommu_create_mapping() ending in
 * iommu_paging_domain_alloc() is a problem for .attach_dev and .probe_finalize
 * (no group yet) but not here, because it allocates the domain that the later
 * attach will find and reuse.
 */
static struct dma_iommu_mapping *
mtk_iommu_v1_get_mapping(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_iommu_priv_get(dev);
	struct dma_iommu_mapping *mtk_mapping;

	mutex_lock(&data->mapping_lock);
	mtk_mapping = data->mapping;
	mutex_unlock(&data->mapping_lock);

	if (!mtk_mapping)
		return ERR_PTR(-ENODEV);

	return mtk_mapping;
}

/**
 * mtk_iommu_v1_of_xlate - translate one "iommus" phandle into a master id.
 * @dev: the client device
 * @args: the phandle arguments: one LARB port number
 *
 * This is the entry point the core uses, and there is no substitute for it:
 * of_iommu_xlate() looks the ops up by fwnode and returns -ENODEV if they have
 * no .of_xlate, so a driver without this callback never gets its fwspec built
 * at all.  A client that declares "iommus" then is accepted, parses cleanly
 * and silently gets no IOMMU - no group, no domain, no DMA ops.  That is what
 * this was doing on this SoC for jpgdec, jpgenc, ovl, rdma0 and rdma1.
 *
 * The core has already called iommu_fwspec_init() by the time this runs, so the
 * only jobs here are to remember which M4U this client belongs to and to record
 * the port.  @data is shared by every client of the M4U - it is the M4U
 * platform device's drvdata - so it is fetched once here and every later
 * callback, including the ones the core runs from another context, can find it.
 *
 * Called once per phandle, so a client with four ports contributes four ids and
 * probe_device() can check they all belong to one LARB.
 */
static int mtk_iommu_v1_of_xlate(struct device *dev,
				const struct of_phandle_args *args)
{
	struct mtk_iommu_v1_data *data;
	struct dma_iommu_mapping *mtk_mapping;
	struct platform_device *m4updev;
	int ret;

	if (args->args_count != 1) {
		dev_err(dev, "invalid #iommu-cells(%d) property for IOMMU\n",
			args->args_count);
		return -EINVAL;
	}

	if (!dev_iommu_priv_get(dev)) {
		/* Get the m4u device */
		m4updev = of_find_device_by_node(args->np);
		if (WARN_ON(!m4updev))
			return -EINVAL;

		dev_iommu_priv_set(dev, platform_get_drvdata(m4updev));

		put_device(&m4updev->dev);
	}

	data = dev_iommu_priv_get(dev);

	/*
	 * Create the M4U's single mapping here, once, if the first client of
	 * this M4U got here first.  See mtk_iommu_v1_get_mapping() for why this
	 * is the only place it may happen: .attach_dev and .probe_finalize are
	 * both too late to be the first, and .probe_device is too early to know
	 * the M4U.  Later clients of the same M4U find it already set.
	 */
	mutex_lock(&data->mapping_lock);
	if (!data->mapping) {
		/* MTK iommu support 4GB iova address space. */
		mtk_mapping = arm_iommu_create_mapping(dev, 0, 1ULL << 32);
		if (IS_ERR(mtk_mapping) || !mtk_mapping) {
			/*
			 * A NULL is not an error pointer, so IS_ERR() alone would
			 * let it through and store NULL in @mapping - which both
			 * .attach_dev and .probe_finalize would then hand to
			 * arm_iommu_attach_device().  The !CONFIG_ARM stub above
			 * expands to a plain NULL, so both spellings are refused.
			 */
			ret = IS_ERR(mtk_mapping) ? PTR_ERR(mtk_mapping) :
						     -ENODEV;
			mutex_unlock(&data->mapping_lock);
			return ret;
		}

		data->mapping = mtk_mapping;
	}
	mutex_unlock(&data->mapping_lock);

	return iommu_fwspec_add_ids(dev, args->args, 1);
}

static struct iommu_device *mtk_iommu_v1_probe_device(struct device *dev)
{
	struct iommu_fwspec *fwspec;
	struct mtk_iommu_v1_data *data;
	int idx, larbid, larbidx;
	struct device_link *link;
	struct device *larbdev;

	/*
	 * The fwspec and its ids were built by mtk_iommu_v1_of_xlate() before the
	 * core got here; this only validates what it collected.  No client can
	 * reach this point without one, because the core found no ops otherwise.
	 */
	fwspec = dev_iommu_fwspec_get(dev);
	if (!fwspec || !fwspec->num_ids)
		return ERR_PTR(-EINVAL);

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

/**
 * mtk_iommu_v1_probe_finalize - attach the shared mapping to one client.
 * @dev: the client, now in a group and attached to the group's domain
 *
 * The mapping is created in .of_xlate(), which is the only callback that runs
 * early enough to be the first for a given M4U: .attach_dev and this both run
 * from inside __iommu_probe_device() and its call to
 * iommu_setup_default_domain(), whereas .of_xlate runs before the device has a
 * group or a domain at all.
 *
 * So the shape is: of_xlate collects the master ids and builds the one mapping
 * the M4U has, probe_device validates the ids and links the consumer to its
 * LARB, and this attaches that mapping to this device.  Every client ends up
 * attached to the same mapping, which is what "one page table, one IOVA space"
 * requires.
 */
static void mtk_iommu_v1_probe_finalize(struct device *dev)
{
	struct dma_iommu_mapping *mtk_mapping;
	int ret;

	mtk_mapping = mtk_iommu_v1_get_mapping(dev);
	if (IS_ERR(mtk_mapping)) {
		dev_err(dev, "Can't create IOMMU mapping - DMA-OPS will not work\n");
		return;
	}

	ret = arm_iommu_attach_device(dev, mtk_mapping);
	if (ret)
		dev_err(dev, "Can't create IOMMU mapping - DMA-OPS will not work\n");
}

/**
 * mtk_iommu_v1_release_device - undo what .probe_device did to the LARB link.
 *
 * The mapping is deliberately not released here: it belongs to the M4U, not
 * to any one client, and the other clients of it are still using it.  It is
 * freed with the M4U's private data.
 */
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

	regval = data->soc->int_en_mask;
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
		return ret;
	}

	/*
	 * Program the MT6589 core and L2 defaults. The page-table base and
	 * final TLB invalidation are installed during domain finalization,
	 * while individual SMI ports are switched to IOMMU mode separately.
	 */
	writel_relaxed(F_MMUg_L2_SEL_FLUSH_EN(1) | F_MMUg_L2_SEL_L2_ULTRA(1) |
		   F_MMUg_L2_SEL_L2_SHARE(0) | F_MMUg_L2_SEL_L2_BUS_SEL(1),
		   data->global_base + REG_MMUg_L2_SEL);
	writel_relaxed(F_MMUg_DCM_ON(1), data->global_base + REG_MMUg_DCM);

	/* ---- L2 cache ---- */
	if (data->l2_base) {
		regval = F_L2_GDC_BYPASS(0) |
			 F_L2_GDC_PERF_MASK(GDC_PERF_MASK_HIT_MISS) |
			 F_L2_GDC_LOCK_ALERT_DIS(0) |
			 F_L2_GDC_LOCK_TH(3) |
			 F_L2_GDC_PAUSE_OP(GDC_NO_PAUSE);
		writel_relaxed(regval, data->l2_base + REG_L2_GDC_OP);
	}

	/* Match the MT6589 hardware defaults used by the downstream driver. */
	for (i = 0; i < data->soc->num_cores; i++) {
		void __iomem *base = data->cores[i].base;

		regval = F_MMU_CTRL_PFH_DIS(0) |
			 F_MMU_CTRL_TLB_WALK_DIS(0) |
			 F_MMU_TF_PROTECT_SEL(2);
		writel_relaxed(regval, base + REG_MMU_CTRL_REG);

		regval = data->soc->int_en_mask;
		writel_relaxed(regval, base + REG_MMU_INT_CONTROL);
		writel_relaxed(data->protect_base, base + REG_MMU_IVRP_PADDR);
	}

	return 0;
}

static const struct iommu_ops mtk_iommu_v1_ops = {
	.identity_domain = &mtk_iommu_v1_identity_domain,
	.domain_alloc_paging = mtk_iommu_v1_domain_alloc_paging,
	.of_xlate	= mtk_iommu_v1_of_xlate,
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
	.int_en_mask = F_INT_TRANSLATION_FAULT |
		       F_INT_MAIN_MULTI_HIT_FAULT |
		       F_INT_INVALID_PA_FAULT |
		       F_INT_ENTRY_REPLACEMENT_FAULT |
		       F_INT_TABLE_WALK_FAULT |
		       F_INT_TLB_MISS_FAULT |
		       F_INT_PFH_DMA_FIFO_OVERFLOW |
		       F_INT_MISS_DMA_FIFO_OVERFLOW,
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
	/*
	 * The downstream kernel enables the first seven interrupt sources
	 * only; F_INT_MISS_DMA_FIFO_OVERFLOW is left masked there as well.
	 */
	.int_en_mask = F_INT_TRANSLATION_FAULT |
		       F_INT_MAIN_MULTI_HIT_FAULT |
		       F_INT_INVALID_PA_FAULT |
		       F_INT_ENTRY_REPLACEMENT_FAULT |
		       F_INT_TABLE_WALK_FAULT |
		       F_INT_TLB_MISS_FAULT |
		       F_INT_PFH_DMA_FIFO_OVERFLOW,
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

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	soc = of_device_get_match_data(dev);
	data->soc = soc;

	mutex_init(&data->mapping_lock);

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

	ret = soc->hw_init(data);
	if (ret) {
		return ret;
	}

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
			/*
			 * Silently deferring here is what makes an M4U that
			 * never comes up hard to diagnose: the boot log only
			 * ever says "deferred probe pending: (reason
			 * unknown)".  Name the LARB that is missing its
			 * driver.
			 */
			dev_info(dev, "deferring: larb %d has no driver yet\n",
				 i);
			ret = -EPROBE_DEFER;
			goto out_put_larbs;
		}
		data->larb_imu[i].dev = &plarbdev->dev;

		component_match_add_release(dev, &match, component_release_of,
					    component_compare_of, larbnode);
	}

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
out_clk_unprepare:
	/* Before disabling clock, mask all interrupts to avoid spurious faults */
	for (i = 0; i < data->soc->num_cores; i++) {
		if (data->cores[i].base) {
			writel_relaxed(0, data->cores[i].base + REG_MMU_INT_CONTROL);
			mb(); /* ensure mask write is completed */
		}
	}

	/*
	 * The bclk is marked CLK_IS_CRITICAL, so the clock core keeps it
	 * running even when this (possibly deferred) probe drops its
	 * reference here -- stopping the M4U clock while the larbs are
	 * still being brought up hangs the system.  The disable only
	 * takes effect once the hardware has really gone away (remove).
	 */
	clk_disable_unprepare(data->bclk);
	return ret;
}

static void mtk_iommu_v1_remove(struct platform_device *pdev)
{
	struct mtk_iommu_v1_data *data = platform_get_drvdata(pdev);
	int i;

	iommu_device_sysfs_remove(&data->iommu);
	iommu_device_unregister(&data->iommu);

	/*
	 * The bclk is marked CLK_IS_CRITICAL so it stays enabled for as
	 * long as the IOMMU is registered; dropping it here is safe
	 * because the hardware has just been taken away from the system.
	 */
	clk_disable_unprepare(data->bclk);
	for (i = 0; i < data->soc->num_cores; i++)
		devm_free_irq(&pdev->dev, data->cores[i].irq, &data->cores[i]);
	component_master_del(&pdev->dev, &mtk_iommu_v1_com_ops);

	for (i = 0; i < MTK_LARB_NR_MAX; i++)
		if (data->larb_imu[i].dev)
			put_device(data->larb_imu[i].dev);
}

static int __maybe_unused mtk_iommu_v1_suspend(struct device *dev)
{
	struct mtk_iommu_v1_data *data = dev_get_drvdata(dev);
	struct mtk_iommu_v1_suspend_reg *reg = &data->reg;
	void __iomem *base;
	int i;

	/* Common core registers */
	for (i = 0; i < data->soc->num_cores; i++) {
		base = data->cores[i].base;
		reg->ctrl_reg[i] = readl_relaxed(base + REG_MMU_CTRL_REG);
		reg->int_control0[i] =
			readl_relaxed(base + REG_MMU_INT_CONTROL);
	}

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
		writel_relaxed(reg->ctrl_reg[i], base + REG_MMU_CTRL_REG);
		writel_relaxed(reg->int_control0[i],
			       base + REG_MMU_INT_CONTROL);
		writel_relaxed(data->protect_base, base + REG_MMU_IVRP_PADDR);
	}

	if (!data->soc->has_global_base) {
		/* MT2701 extra */
		base = data->cores[0].base;
		writel_relaxed(reg->standard_axi_mode, base + REG_MMU_STANDARD_AXI_MODE);
		writel_relaxed(reg->dcm_dis, base + REG_MMU_DCM);
	}

	/*
	 * The saved mmug_pt_base was asserted to be aligned when it was
	 * programmed, but restore the live domain base through the same
	 * helper so the assertion covers this programming site too.
	 */
	if (data->m4u_dom && data->m4u_dom->pgt_pa)
		mtk_iommu_v1_write_pt_base(data, data->m4u_dom->pgt_pa);

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
