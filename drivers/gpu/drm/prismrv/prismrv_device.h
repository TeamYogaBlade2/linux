/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * prismrv_device.h — core device state for the PrismRV SGX driver.
 */
#ifndef _PRISMRV_DEVICE_H_
#define _PRISMRV_DEVICE_H_

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/devfreq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <drm/drm_device.h>
#include <drm/gpu_scheduler.h>
#include "prismrv_regs.h"

/* SGX core types (matches EUR_CR_CORE_REVISION designer/major fields) */
#define PRISMRV_CORE_SGX530	0x0130
#define PRISMRV_CORE_SGX531	0x0131
#define PRISMRV_CORE_SGX535	0x0135
#define PRISMRV_CORE_SGX540	0x0140
#define PRISMRV_CORE_SGX543	0x0143
#define PRISMRV_CORE_SGX544	0x0144
#define PRISMRV_CORE_SGX545	0x0145

/* uKernel upload address inside GPU virtual space */
#define PRISMRV_UKERNEL_VADDR	0x0c000000u

/* HostCtl block shared with the uKernel (SGXMKIF_HOST_CTL subset) */
struct prismrv_host_ctl {
	__le32 ui32InterruptFlags;
	__le32 ui32ClearFlags;
	__le32 ui32InitStatus;
	__le32 ui32HostClock;
	/* remaining fields opaque to the kernel; sized to 256 bytes total */
	u8 pad[240];
} __packed;
#define PRISMRV_EDM_INIT_COMPLETE	BIT(0)

/* firmware-loaded init script record */
enum {
	PRISMRV_INIT_OP_WRITE = 1,
	PRISMRV_INIT_OP_READ  = 2,
	PRISMRV_INIT_OP_HALT  = 3,
};
struct prismrv_init_rec {
	__le32 op;
	__le32 offset;
	__le32 value;
};

/* per-core feature/errata description, selected by compatible + runtime rev */
struct prismrv_chip_info {
	const char *name;		/* e.g. "sgx544" */
	u32 core_id;			/* PRISMRV_CORE_* */
	unsigned int num_cores;		/* MP cores */
	bool has_isp2;			/* second ISP pipe present */
	bool has_multi_event_kick;	/* EVENT_KICK2 style kick */
	u32 clkgate_defaults;
};

struct prismrv_devfreq {
	struct devfreq *devfreq;
	ktime_t time_last_update;
	u64 busy_time;
	u64 idle_time;
	unsigned long busy_count;
	spinlock_t lock;
};

struct prismrv_device {
	struct drm_device drm;		/* must be first */
	struct platform_device *pdev;
	const struct prismrv_chip_info *info;

	void __iomem *regs;
	resource_size_t regs_size;

	struct clk_bulk_data *clocks;	/* from devm_clk_bulk_get_all_enabled */
	int nr_clocks;

	/* runtime-detected hardware revision */
	u32 core_revision;	/* raw EUR_CR_CORE_REVISION */
	u32 core_rev_major;
	u32 core_rev_minor;

	/* active BRN workaround bitmask (prismrv_errata.c) */
	u32 errata;

	/* MMU state */
	u32 pd_gpu_addr;		/* page directory in device space */
	u32 *pd_cpu;			/* page directory (kernel shadow) */
	u32 **pd_pts;			/* per-PDE page table cpu pointers */
	dma_addr_t *pd_pt_dma;		/* matching dma addresses */
	dma_addr_t pt_dma_addr;		/* scratch dma addr (PD alloc / latest PT) */

	/* uKernel */
	size_t ukernel_size;
	dma_addr_t ukernel_dma;
	void *ukernel_cpu;
	u32 ukernel_vaddr;

	/* shared HostCtl block */
	dma_addr_t hostctl_dma;
	struct prismrv_host_ctl *hostctl;

	struct prismrv_devfreq devfreq;

	spinlock_t event_lock;		/* protects kicker + event masks */

	wait_queue_head_t init_wq;
	bool hw_ready;
};

static inline struct prismrv_device *to_prismrv(struct drm_device *d)
{
	return container_of(d, struct prismrv_device, drm);
}

int prismrv_hw_init(struct prismrv_device *pv);
void prismrv_hw_fini(struct prismrv_device *pv);
int prismrv_fw_load(struct prismrv_device *pv);
void prismrv_errata_init(struct prismrv_device *pv);
u32 prismrv_read_revision(struct prismrv_device *pv);

int prismrv_mmu_init(struct prismrv_device *pv);
void prismrv_mmu_fini(struct prismrv_device *pv);
int prismrv_mmu_map(struct prismrv_device *pv, u32 vaddr,
		    dma_addr_t phys, size_t size);
void prismrv_mmu_unmap(struct prismrv_device *pv, u32 vaddr, size_t size);

irqreturn_t prismrv_irq_handler(int irq, void *data);

int prismrv_gem_init(struct prismrv_device *pv);
int prismrv_submit_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file);
int prismrv_gem_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file);
int prismrv_gem_mmap_offset_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file);
int prismrv_get_param_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file);

int prismrv_devfreq_init(struct prismrv_device *pv);
void prismrv_devfreq_fini(struct prismrv_device *pv);

#endif /* _PRISMRV_DEVICE_H_ */
