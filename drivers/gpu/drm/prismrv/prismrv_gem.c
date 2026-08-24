// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_gem.c — GEM buffer objects (drm_gem_shmem_helper backed).
 *
 * Buffers are shmem backed; pages are pinned and DMA-mapped through the
 * shmem helper's sg_table when the object is first mapped into the GPU
 * MMU (at submit time).  Userspace accesses pages via the standard GEM
 * mmap path (drm_gem_shmem_vm_ops).
 */
#include <linux/dma-mapping.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

/* GPU virtual address heap: bump allocator in the 32-bit BIF space,
 * above the uKernel region and the fixed firmware carve-outs */
#define PRISMRV_VA_BASE		0x10000000u
#define PRISMRV_VA_SIZE		0x30000000u

static DEFINE_MUTEX(va_lock);
static u32 va_next = PRISMRV_VA_BASE;

struct prismrv_bo {
	struct drm_gem_shmem_object base;
	u32 gpu_va;
};

static inline struct prismrv_bo *to_prbo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct prismrv_bo, base);
}

static void prismrv_bo_free(struct drm_gem_object *obj)
{
	struct prismrv_device *pv = to_prismrv(obj->dev);
	struct prismrv_bo *bo = to_prbo(obj);

	if (bo->gpu_va) {
		prismrv_mmu_unmap(pv, bo->gpu_va, obj->size);
		mutex_lock(&va_lock);
		/* simple bump allocator: no reclaim (VA space is 768 MiB) */
		mutex_unlock(&va_lock);
	}
	drm_gem_shmem_free(&bo->base);
}

static const struct drm_gem_object_funcs prismrv_gem_funcs = {
	.free = prismrv_bo_free,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static int prismrv_bo_pin_and_map(struct prismrv_device *pv,
				  struct prismrv_bo *bo)
{
	struct drm_gem_shmem_object *shmem = &bo->base;
	struct sg_table *sgt;
	struct scatterlist *sg;
	unsigned int i;
	dma_addr_t addr;
	int ret;

	if (bo->gpu_va)
		return 0;

	ret = drm_gem_shmem_pin(shmem);
	if (ret)
		return ret;

	sgt = drm_gem_shmem_get_sg_table(shmem);
	if (IS_ERR(sgt)) {
		drm_gem_shmem_unpin(shmem);
		return PTR_ERR(sgt);
	}

	ret = dma_map_sgtable(pv->drm.dev, sgt, DMA_BIDIRECTIONAL, 0);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		drm_gem_shmem_unpin(shmem);
		return ret;
	}

	mutex_lock(&va_lock);
	bo->gpu_va = va_next;
	va_next += PAGE_ALIGN(shmem->base.size);
	mutex_unlock(&va_lock);

	for_each_sgtable_dma_sg(sgt, sg, i) {
		addr = sg_dma_address(sg);
		prismrv_mmu_map(pv, bo->gpu_va + i * PAGE_SIZE,
				addr, sg_dma_len(sg));
	}

	return 0;
}

int prismrv_gem_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	struct drm_prismrv_gem_create *args = data;
	struct prismrv_bo *bo;
	int ret;

	if (args->size == 0 || args->size > SZ_256M)
		return -EINVAL;
	args->size = PAGE_ALIGN(args->size);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;

	ret = drm_gem_object_init(dev, &bo->base.base, args->size);
	if (ret) {
		kfree(bo);
		return ret;
	}
	bo->base.base.funcs = &prismrv_gem_funcs;

	ret = drm_gem_handle_create(file, &bo->base.base, &args->handle);
	drm_gem_object_put(&bo->base.base);
	return ret;
}

int prismrv_gem_mmap_offset_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct drm_prismrv_gem_mmap_offset *args = data;
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(obj);
	if (ret == 0)
		args->offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	return ret;
}

/**
 * prismrv_gem_populate() — pin, DMA-map and MMU-map a BO before submit.
 * Called on the list of BOs referenced by a submission.
 */
int prismrv_gem_populate(struct prismrv_device *pv, struct drm_gem_object **objs,
			 u32 count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		struct prismrv_bo *bo = to_prbo(objs[i]);

		ret = prismrv_bo_pin_and_map(pv, bo);
		if (ret)
			return ret;
	}
	return 0;
}

u32 prismrv_bo_gpuva(struct drm_gem_object *obj)
{
	return to_prbo(obj)->gpu_va;
}

int prismrv_get_param_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_get_param *args = data;

	switch (args->param) {
	case PRISMRV_PARAM_GPU_ID:
		/* raw EUR_CR_CORE_REVISION: [23:16] major (RTL head rev),
		 * [15:8] minor, [7:0] maintenance */
		args->value = pv->core_revision;
		break;
	case PRISMRV_PARAM_CORE_COUNT:
		args->value = pv->info->num_cores;
		break;
	case PRISMRV_PARAM_UKERNEL_SIZE:
		args->value = pv->ukernel_size;
		break;
	case PRISMRV_PARAM_ERRATA:
		args->value = pv->errata;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
