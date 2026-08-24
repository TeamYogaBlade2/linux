// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_gem.c — GEM buffer objects (shmem backed).
 */
#include <linux/dma-mapping.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

struct prismrv_bo {
	struct drm_gem_object base;
	struct page **pages;
	u32 gpu_va;		/* GPU virtual address */
	size_t size;
};

static const struct drm_gem_object_funcs prismrv_gem_funcs;

static inline struct prismrv_bo *to_prbo(struct drm_gem_object *obj)
{
	return container_of(obj, struct prismrv_bo, base);
}

static void prismrv_bo_free(struct drm_gem_object *obj)
{
	struct prismrv_device *pv = to_prismrv(obj->dev);
	struct prismrv_bo *bo = to_prbo(obj);

	if (bo->gpu_va)
		prismrv_mmu_unmap(pv, bo->gpu_va, bo->size);
	drm_gem_object_release(obj);
	kfree(bo);
}

static const struct drm_gem_object_funcs prismrv_gem_funcs = {
	.free = prismrv_bo_free,
};

int prismrv_gem_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_gem_create *args = data;
	struct prismrv_bo *bo;
	int ret;

	if (args->size == 0 || args->size > SZ_256M)
		return -EINVAL;
	args->size = PAGE_ALIGN(args->size);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;

	ret = drm_gem_object_init(dev, &bo->base, args->size);
	if (ret) {
		kfree(bo);
		return ret;
	}
	bo->base.funcs = &prismrv_gem_funcs;
	bo->size = args->size;

	ret = drm_gem_handle_create(file, &bo->base, &args->handle);
	drm_gem_object_put(&bo->base);
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

int prismrv_get_param_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_get_param *args = data;

	switch (args->param) {
	case PRISMRV_PARAM_GPU_ID:
		args->value = pv->info->core_id | (pv->core_rev_major << 8);
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
