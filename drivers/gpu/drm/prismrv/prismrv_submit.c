// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_submit.c — command submission and completion fences.
 *
 * Command buffers are opaque to the kernel: userspace builds the CCB
 * stream (the same format the uKernel consumes) and the kernel only
 * sequences execution, manages dependencies via dma_fence/sync_file and
 * kicks the hardware.
 */
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

struct prismrv_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *prismrv_fence_name(struct dma_fence *f)
{
	return "prismrv";
}

static bool prismrv_fence_signaled(struct dma_fence *f)
{
	return false;	/* signalled explicitly on IRQ */
}

const struct dma_fence_ops prismrv_fence_ops = {
	.get_driver_name = prismrv_fence_name,
	.get_timeline_name = prismrv_fence_name,
	.signaled = prismrv_fence_signaled,
};

int prismrv_submit_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_submit *args = data;
	struct prismrv_fence *f;
	struct sync_file *sf;
	int ret = 0, fd;

	if (!pv->hw_ready)
		return -ENODEV;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	spin_lock_init(&f->lock);
	dma_fence_init(&f->base, &prismrv_fence_ops, &f->lock,
		       0 /* context */, 1 /* seqno */);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		kfree(f);
		return fd;
	}

	sf = sync_file_create(&f->base);
	dma_fence_put(&f->base);
	if (!sf) {
		put_unused_fd(fd);
		kfree(f);
		return -ENOMEM;
	}

	args->out_fence_fd = fd;
	fd_install(fd, sf->file);

	/* TODO: program the CCB and kick TA/3D once the uKernel service
	 * interface is wired up.  For now the fence is never signalled
	 * and submissions complete immediately. */
	dma_fence_signal(&f->base);

	return ret;
}
