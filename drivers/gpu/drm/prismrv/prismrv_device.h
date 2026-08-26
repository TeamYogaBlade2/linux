/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * prismrv_device.h — core device state for the PrismRV SGX driver.
 */
#ifndef _PRISMRV_DEVICE_H_
#define _PRISMRV_DEVICE_H_

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/devfreq.h>
#include <linux/reset.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
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

/*
 * Fixed GPU-VA layout for uKernel shared structures.  The base addresses
 * match the values the vendor loader patches into the uKernel image
 * (DEADBEEF slot table): each slot selects the heap base the service
 * routines operate on.
 */
#define PRISMRV_HOSTCTL_VADDR	0x00101000u	/* slot 0x0d */
#define PRISMRV_CCB_VADDR	0x00400000u	/* slot 0x4d */
#define PRISMRV_HWRTDATA_VADDR	0x00a00000u	/* slot 0xad (TA), 3D follows */

/* kernel CCB: array of commands plus control block (sgx_mkif_km.h) */
struct prismrv_ccb_cmd {
	__le32 service_address;	/* USE handler address inside uKernel */
	__le32 cache_control;
	__le32 data[6];
} __packed;

struct prismrv_ccb {
	struct prismrv_ccb_cmd commands[256];
	__le32 write_offset;	/* advanced by the kernel */
	__le32 read_offset;	/* consumed by the uKernel */
};

/* HostKickAddr service entry instruction indices in the uKernel image
 * (analysis/ukernel/SERVICE_ROUTINES.md); service address = index * 8 */
enum prismrv_cmd_type {
	PRISMRV_CMD_TA = 0,
	PRISMRV_CMD_TRANSFER,
	PRISMRV_CMD_2D,
	PRISMRV_CMD_POWER,
	PRISMRV_CMD_CONTEXTSUSPEND,
	PRISMRV_CMD_CLEANUP,
	PRISMRV_CMD_GETMISCINFO,
	PRISMRV_CMD_PROCESS_QUEUES,
	PRISMRV_CMD_DATABREAKPOINT,
	PRISMRV_CMD_SETHWPERFSTATUS,
	PRISMRV_CMD_COUNT
};

static const unsigned int prismrv_hostkick_instr[PRISMRV_CMD_COUNT] = {
	554,  /* TA */
	523,  /* TRANSFER */
	486,  /* 2D */
	397,  /* POWER */
	427,  /* CONTEXTSUSPEND */
	75,   /* CLEANUP */
	1115, /* GETMISCINFO */
	1017, /* PROCESS_QUEUES */
	0,    /* DATABREAKPOINT: not implemented in this uKernel */
	1023, /* SETHWPERFSTATUS */
};

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

/* submission fence, signalled by the IRQ handler on completion */
struct prismrv_fence {
	struct dma_fence base;
	spinlock_t lock;
	struct list_head node;
};

struct prismrv_device {
	struct drm_device drm;		/* must be first */
	struct platform_device *pdev;
	const struct prismrv_chip_info *info;

	void __iomem *regs;
	resource_size_t regs_size;
	struct reset_control *rstc;

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

	/* kernel CCB + HWRTData render contexts */
	dma_addr_t ccb_dma;
	struct prismrv_ccb *ccb;
	spinlock_t ccb_lock;

	dma_addr_t hwrt_dma;
	void *hwrt;			/* 2 x 496 bytes (TA, 3D) */

	/* errata work-around allocations (prismrv_errata_apply) */
#define PRISMRV_ERRATA_BUF_PTLA_WB	0	/* BRN_31780 */
#define PRISMRV_ERRATA_BUF_CC_DM	1	/* BRN_36513 clear-clip DM stream */
#define PRISMRV_ERRATA_BUF_CC_INDEX	2
#define PRISMRV_ERRATA_BUF_CC_PDS	3
#define PRISMRV_ERRATA_BUF_CC_USE	4
#define PRISMRV_ERRATA_BUF_CC_PARAM	5
#define PRISMRV_ERRATA_BUF_COUNT	6
	struct {
		void *cpu;
		dma_addr_t dma;
		size_t size;
		u32 vaddr;
	} errata_buf[PRISMRV_ERRATA_BUF_COUNT];

	struct prismrv_devfreq devfreq;

	spinlock_t event_lock;		/* protects kicker + event masks */
	struct list_head pending_fences; /* completed in order by the IRQ */
	atomic_t missed_completions;
	atomic_t busy_count;
	atomic_t fence_context;		/* dma_fence context id */
	atomic_t fence_seqno;		/* per-context sequence number */
	struct work_struct recovery_work;
	struct mutex init_mutex;	/* serialises prismrv_hw_init */

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
int prismrv_errata_apply(struct prismrv_device *pv);
void prismrv_errata_release(struct prismrv_device *pv);
u32 prismrv_read_revision(struct prismrv_device *pv);

int prismrv_mmu_init(struct prismrv_device *pv);
void prismrv_mmu_fini(struct prismrv_device *pv);
int prismrv_mmu_map(struct prismrv_device *pv, u32 vaddr,
		    dma_addr_t phys, size_t size);
void prismrv_mmu_unmap(struct prismrv_device *pv, u32 vaddr, size_t size);

irqreturn_t prismrv_irq_handler(int irq, void *data);
void prismrv_recovery_work(struct work_struct *work);

int prismrv_submit_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file);
int prismrv_gem_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file);
int prismrv_gem_mmap_offset_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file);
int prismrv_get_param_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file);

struct drm_device;
struct drm_gem_object;
struct drm_gem_object *prismrv_gem_create_object(struct drm_device *dev,
						 size_t size);
int prismrv_gem_populate(struct prismrv_device *pv,
			 struct drm_gem_object **objs, u32 count);
u32 prismrv_bo_gpuva(struct drm_gem_object *obj);

int prismrv_devfreq_init(struct prismrv_device *pv);
void prismrv_devfreq_fini(struct prismrv_device *pv);

int prismrv_ccb_init(struct prismrv_device *pv);
void prismrv_ccb_fini(struct prismrv_device *pv);

#endif /* _PRISMRV_DEVICE_H_ */
