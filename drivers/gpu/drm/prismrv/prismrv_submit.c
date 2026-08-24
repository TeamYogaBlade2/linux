// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_submit.c — command submission: kernel CCB + TA/3D kicks.
 *
 * Mirrors the vendor flow (sgxutils.c SGXScheduleCCBCommand):
 *
 *   1. acquire a CCB slot (256 entries, 32 bytes each)
 *   2. fill the command; ui32ServiceAddress = uKernel base +
 *      hostkick_instr[type] * 8 (the USE service handler entry)
 *   3. advance ccb->write_offset (mod 256)
 *   4. kick EUR_CR_EVENT_KICK
 *
 * The command payload (ui32Data[6]) is passed through from userspace:
 * the vendor model has userspace (libsrv_um) define the per-command-type
 * data layout, and the kernel only sequences execution.
 */
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/units.h>
#include <linux/delay.h>
#include <linux/sync_file.h>
#include <linux/pm_runtime.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <linux/iosys-map.h>
#include <linux/dma-resv.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

#define HWRTDATA_SIZE		496

struct prismrv_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *prismrv_fence_name(struct dma_fence *f)
{
	return "prismrv";
}

const struct dma_fence_ops prismrv_fence_ops = {
	.get_driver_name = prismrv_fence_name,
	.get_timeline_name = prismrv_fence_name,
};

int prismrv_ccb_init(struct prismrv_device *pv)
{
	spin_lock_init(&pv->ccb_lock);

	pv->ccb = dma_alloc_coherent(pv->drm.dev, sizeof(*pv->ccb),
				     &pv->ccb_dma, GFP_KERNEL);
	if (!pv->ccb)
		return -ENOMEM;
	memset(pv->ccb, 0, sizeof(*pv->ccb));

	/* TA + 3D render contexts (HWRTData), zero-initialised */
	pv->hwrt = dma_alloc_coherent(pv->drm.dev, 2 * HWRTDATA_SIZE,
				      &pv->hwrt_dma, GFP_KERNEL);
	if (!pv->hwrt) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->ccb), pv->ccb,
				  pv->ccb_dma);
		pv->ccb = NULL;
		return -ENOMEM;
	}

	/* expose the shared structures to the uKernel through the MMU */
	prismrv_mmu_map(pv, PRISMRV_HOSTCTL_VADDR,
			pv->hostctl_dma, sizeof(*pv->hostctl));
	prismrv_mmu_map(pv, PRISMRV_CCB_VADDR,
			pv->ccb_dma, sizeof(*pv->ccb));
	prismrv_mmu_map(pv, PRISMRV_HWRTDATA_VADDR,
			pv->hwrt_dma, 2 * HWRTDATA_SIZE);

	return 0;
}

void prismrv_ccb_fini(struct prismrv_device *pv)
{
	if (pv->ccb) {
		dma_free_coherent(pv->drm.dev, sizeof(*pv->ccb), pv->ccb,
				  pv->ccb_dma);
		pv->ccb = NULL;
	}
	if (pv->hwrt) {
		dma_free_coherent(pv->drm.dev, 2 * HWRTDATA_SIZE, pv->hwrt,
				  pv->hwrt_dma);
		pv->hwrt = NULL;
	}
}

/*
 * Vendor SGXAcquireKernelCCBSlot: the CCB is full when advancing the
 * write offset would swallow an unconsumed command.  Wait for the
 * uKernel to drain it instead of corrupting the ring.
 */
/* ring-full test: caller holds ccb_lock or otherwise serialises */
static bool prismrv_ccb_full(struct prismrv_device *pv)
{
	return ((le32_to_cpu(pv->ccb->write_offset) + 1 -
		 le32_to_cpu(pv->ccb->read_offset)) & 255) == 0;
}

static void prismrv_ccb_schedule(struct prismrv_device *pv,
				 enum prismrv_cmd_type type,
				 const __le32 data[6])
{
	struct prismrv_ccb_cmd *cmd;
	u32 slot;

	spin_lock(&pv->ccb_lock);

	while (prismrv_ccb_full(pv)) {
		spin_unlock(&pv->ccb_lock);
		/* nudge the uKernel main loop so it drains the CCB */
		writel(EUR_CR_EVENT_KICK_NOW_MASK,
		       pv->regs + EUR_CR_EVENT_KICK);
		usleep_range(50, 100);
		spin_lock(&pv->ccb_lock);
	}

	slot = le32_to_cpu(pv->ccb->write_offset) & 255;
	cmd = &pv->ccb->commands[slot];

	cmd->service_address =
		cpu_to_le32(PRISMRV_UKERNEL_VADDR +
			    prismrv_hostkick_instr[type] * 8);
	cmd->cache_control = 0;
	memcpy(cmd->data, data, sizeof(cmd->data));

	/*
	 * publish the command before bumping write_offset; the uKernel
	 * polls write_offset on the other side of a coherent mapping
	 */
	wmb();
	pv->ccb->write_offset = cpu_to_le32((slot + 1) & 255);

	spin_unlock(&pv->ccb_lock);

	writel(EUR_CR_EVENT_KICK_NOW_MASK, pv->regs + EUR_CR_EVENT_KICK);
}

/*
 * Import the user-passed sync_file fds and wait for every dependency
 * before the CCB command is published (implicit-sync semantics).
 */
static int prismrv_wait_in_fences(u32 num_fds, const u32 __user *user_fds)
{
	u32 *fds;
	unsigned int i;
	long ret = 0;

	if (!num_fds)
		return 0;

	fds = kmemdup_array(user_fds, num_fds, sizeof(u32), GFP_KERNEL);
	if (!fds)
		return -ENOMEM;

	for (i = 0; i < num_fds && ret == 0; i++) {
		struct dma_fence *fence;

		fence = sync_file_get_fence(fds[i]);
		if (!fence) {
			ret = -EINVAL;
			break;
		}
		ret = dma_fence_wait_timeout(fence, true, MAX_SCHEDULE_TIMEOUT);
		dma_fence_put(fence);
		if (ret < 0)
			break;
	}
	kfree(fds);

	if (ret > 0)
		return 0;
	return (int)ret ?: -ETIMEDOUT;
}

int prismrv_submit_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_submit *args = data;
	struct drm_gem_object **objs = NULL;
	struct prismrv_fence *f;
	struct sync_file *sf;
	u32 __user *in_fds;
	__le32 cmd_data[6] = {};
	int ret = 0, fd;
	unsigned int i;

	ret = pm_runtime_resume_and_get(pv->drm.dev);
	if (ret)
		return ret;

	if (!pv->hw_ready) {
		pm_runtime_put_sync(pv->drm.dev);
		return -ENODEV;
	}
	if (args->cmd_type >= PRISMRV_CMD_COUNT) {
		pm_runtime_put_sync(pv->drm.dev);
		return -EINVAL;
	}

	/* slot 0 = command BO, 1..num_bos = user bos */
	objs = kvcalloc(args->num_bos + 1, sizeof(*objs), GFP_KERNEL);
	if (!objs)
		return -ENOMEM;

	/* block on explicit dependencies first */
	in_fds = u64_to_user_ptr(args->in_fences);
	ret = prismrv_wait_in_fences(args->num_in_fences, in_fds);
	if (ret)
		goto out_put;

	/* the command BO is always referenced object 0 so it is pinned,
	 * DMA-mapped and MMU-mapped along with the rest */
	objs[0] = drm_gem_object_lookup(file, args->cmd_handle);
	if (!objs[0]) {
		ret = -ENOENT;
		goto out_put;
	}

	/* pin + DMA-map + MMU-map every referenced BO */
	{
		struct drm_gem_object **lut = objs + 1;

		ret = drm_gem_objects_lookup(file,
			u64_to_user_ptr(args->bos), args->num_bos, &lut);
	}
	if (ret)
		goto out_put;
	ret = prismrv_gem_populate(pv, objs, args->num_bos + 1);
	if (ret)
		goto out_put;

	/* pass the command stream by reference: data[0] = GPU VA of the
	 * command BO, data[1] = valid byte count.  The uKernel-side client
	 * CCB handler (and the emulator's HostCtl shim) reads the packet
	 * stream from there; the vendor model also keeps payload bodies
	 * out of the 24-byte kernel CCB slot. */
	cmd_data[0] = cpu_to_le32(prismrv_bo_gpuva(objs[0]));
	cmd_data[1] = cpu_to_le32(args->cmd_size);
	if (args->num_bos >= 1 && objs[2])
		/* bos array convention: user bos[0] (objs[2]) is the TA
		 * packet-stream BO for draw submissions */
		cmd_data[2] = cpu_to_le32(prismrv_bo_gpuva(objs[2]));

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		ret = -ENOMEM;
		goto out_put;
	}
	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &prismrv_fence_ops, &f->lock,
		       0 /* context */, 1 /* seqno */);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		kfree(f);
		ret = fd;
		goto out_put;
	}

	sf = sync_file_create(&f->base);
	dma_fence_put(&f->base);
	if (!sf) {
		put_unused_fd(fd);
		kfree(f);
		ret = -ENOMEM;
		goto out_put;
	}

	args->out_fence_fd = fd;
	fd_install(fd, sf->file);

	/* record the fence so the IRQ handler can signal it on completion */
	dma_fence_get(&f->base);
	spin_lock(&pv->event_lock);
	pv->pending_fence = &f->base;
	spin_unlock(&pv->event_lock);

	atomic_inc(&pv->busy_count);
	prismrv_ccb_schedule(pv, args->cmd_type, cmd_data);
	pm_runtime_mark_last_busy(pv->drm.dev);
	pm_runtime_put_autosuspend(pv->drm.dev);

	/* record the fence so devfreq can see the busy window */
out_put:
	/* objects 0..num_bos are referenced (0 = cmd BO) */
	for (i = 0; i <= args->num_bos && objs; i++)
		if (objs[i])
			drm_gem_object_put(objs[i]);
	kvfree(objs);
	return ret;
}
