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
#include <linux/list.h>
#include <linux/sizes.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_file.h>
#include <drm/drm_device.h>
#include <drm/drm_mm.h>

#include <uapi/drm/prismrv_drm.h>
#include "prismrv_device.h"

/*
 * GPU virtual address heap: a drm_mm range in the 32-bit BIF space, above
 * the uKernel region and the fixed firmware carve-outs.  A BO's GPU VA is
 * assigned once at creation and stays fixed for the BO's lifetime (it is
 * returned to userspace by GEM_CREATE so command streams can embed it);
 * only the MMU *mapping* is torn down and rebuilt across runtime suspend
 * and GPU recovery.
 */
#define PRISMRV_VA_BASE		0x10000000u
#define PRISMRV_VA_SIZE		0x30000000u

struct prismrv_bo {
	struct drm_gem_shmem_object base;
	struct drm_mm_node va_node;	/* GPU VA range, fixed for BO life */
	bool va_allocated;
	bool mapped;			/* PTEs present in the current MMU */
	struct list_head bo_node;	/* prismrv_device.bo_list */
};

int prismrv_va_init(struct prismrv_device *pv)
{
	mutex_init(&pv->va_lock);
	drm_mm_init(&pv->va_mm, PRISMRV_VA_BASE, PRISMRV_VA_SIZE);
	return 0;
}

void prismrv_va_fini(struct prismrv_device *pv)
{
	drm_mm_takedown(&pv->va_mm);
}

static inline struct prismrv_bo *to_prbo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct prismrv_bo, base);
}

static void prismrv_bo_free(struct drm_gem_object *obj)
{
	struct prismrv_device *pv = to_prismrv(obj->dev);
	struct prismrv_bo *bo = to_prbo(obj);

	/*
	 * Lock order (device-wide): mmu_lock -> bo_list_lock.
	 * prismrv_mmu_invalidate_all_bos() is called with mmu_lock held and
	 * then takes bo_list_lock; taking them in the opposite order here
	 * (as an earlier revision did) is an ABBA deadlock against runtime
	 * suspend / GPU recovery.  The last GEM reference can be dropped at
	 * any time, so nothing else serialises this against them.
	 */
	mutex_lock(&pv->mmu_lock);
	spin_lock(&pv->bo_list_lock);
	list_del(&bo->bo_node);
	spin_unlock(&pv->bo_list_lock);

	/*
	 * If recovery/runtime suspend already tore the MMU down, ->mapped
	 * was cleared by prismrv_mmu_invalidate_all_bos() and there is
	 * nothing to unmap (unmap_locked also tolerates pd_pts == NULL).
	 */
	if (bo->mapped) {
		prismrv_mmu_unmap_locked(pv, bo->va_node.start, obj->size);
		bo->mapped = false;
	}
	mutex_unlock(&pv->mmu_lock);

	if (bo->va_allocated) {
		mutex_lock(&pv->va_lock);
		drm_mm_remove_node(&bo->va_node);
		mutex_unlock(&pv->va_lock);
		bo->va_allocated = false;
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
	size_t va_off = 0;
	int ret;

	/*
	 * Fast path: mapped by a previous submit and the mapping is still
	 * live.  Re-check under mmu_lock below to close the window between
	 * concurrent submits referencing the same BO.
	 */
	if (READ_ONCE(bo->mapped))
		return 0;

	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	mutex_lock(&pv->mmu_lock);
	if (bo->mapped) {
		mutex_unlock(&pv->mmu_lock);
		return 0;
	}

	for_each_sgtable_dma_sg(sgt, sg, i) {
		size_t len = sg_dma_len(sg);

		ret = prismrv_mmu_map_locked(pv, bo->va_node.start + va_off,
					     sg_dma_address(sg), len);
		if (ret)
			goto err_unmap;
		va_off += len;
	}

	/* PTEs must be globally visible before ->mapped is published */
	smp_wmb();
	WRITE_ONCE(bo->mapped, true);
	mutex_unlock(&pv->mmu_lock);
	return 0;

err_unmap:
	prismrv_mmu_unmap_locked(pv, bo->va_node.start, va_off);
	mutex_unlock(&pv->mmu_lock);
	return ret;
}

/*
 * shmem helper callback: allocate the driver BO wrapper so that the
 * helper initialises its state inside prismrv_bo (container_of layout).
 * This replaces a previous open-coded drm_gem_object_init() call that
 * left the drm_gem_shmem_object state uninitialised.
 */
struct drm_gem_object *
prismrv_gem_create_object(struct drm_device *dev, size_t size)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct prismrv_bo *bo;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	bo->base.base.funcs = &prismrv_gem_funcs;
	INIT_LIST_HEAD(&bo->bo_node);

	spin_lock(&pv->bo_list_lock);
	list_add(&bo->bo_node, &pv->bo_list);
	spin_unlock(&pv->bo_list_lock);

	return &bo->base.base;
}

/**
 * prismrv_bo_create() - allocate a BO with a fixed GPU VA range.
 * @flags: PRISMRV_BO_*
 *
 * Used by the create ioctl and, with no GEM handle, for the kernel-owned
 * command-stream snapshots that userspace can neither map nor modify.
 */
struct drm_gem_object *prismrv_bo_create(struct prismrv_device *pv,
					 size_t size, u32 flags)
{
	struct drm_gem_shmem_object *shmem;
	struct prismrv_bo *bo;
	int ret;

	size = PAGE_ALIGN(size);
	shmem = drm_gem_shmem_create(&pv->drm, size);
	if (IS_ERR(shmem))
		return ERR_CAST(shmem);

	bo = to_prbo(&shmem->base);
	mutex_lock(&pv->va_lock);
	ret = drm_mm_insert_node_generic(&pv->va_mm, &bo->va_node, size,
					 SZ_4K, 0, DRM_MM_INSERT_BEST);
	mutex_unlock(&pv->va_lock);
	if (ret) {
		drm_gem_object_put(&shmem->base);
		return ERR_PTR(ret);
	}
	bo->va_allocated = true;

	/* write-combine must be set before the first vmap */
	if (flags & PRISMRV_BO_UNCACHED)
		shmem->map_wc = true;
	return &shmem->base;
}

int prismrv_gem_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_gem_create *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (args->flags & ~PRISMRV_BO_UNCACHED || args->pad)
		return -EINVAL;
	if (args->size == 0 || args->size > SZ_256M)
		return -EINVAL;
	args->size = PAGE_ALIGN(args->size);

	obj = prismrv_bo_create(pv, args->size, args->flags);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	args->gpu_va = to_prbo(obj)->va_node.start;
	args->pad = 0;
	ret = drm_gem_handle_create(file, obj, &args->handle);
	drm_gem_object_put(obj);
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

/**
 * prismrv_mmu_invalidate_all_bos() - forget every BO's MMU mapping.
 *
 * Called before the MMU page tables are torn down (runtime suspend,
 * recovery).  The GPU VA of a BO is stable, so only ->mapped is cleared;
 * the next submit re-creates the PTEs in the fresh MMU context.
 *
 * Callers hold mmu_lock + init_mutex + submit_rwsem (write).  That
 * excludes pin_and_map(), but NOT the last GEM reference being dropped:
 * bo_free() serialises against this function through mmu_lock (lock
 * order mmu_lock -> bo_list_lock everywhere).
 */
void prismrv_mmu_invalidate_all_bos(struct prismrv_device *pv)
{
	struct prismrv_bo *bo;

	spin_lock(&pv->bo_list_lock);
	list_for_each_entry(bo, &pv->bo_list, bo_node)
		WRITE_ONCE(bo->mapped, false);
	spin_unlock(&pv->bo_list_lock);
}

u32 prismrv_bo_gpuva(struct drm_gem_object *obj)
{
	return to_prbo(obj)->va_node.start;
}

int prismrv_get_param_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct prismrv_device *pv = to_prismrv(dev);
	struct drm_prismrv_get_param *args = data;

	switch (args->param) {
	case PRISMRV_PARAM_CORE_ID:
		/* raw EUR_CR_CORE_ID: [31:16] designer, [15:0] core */
		args->value = pv->core_id;
		break;
	case PRISMRV_PARAM_CORE_REVISION:
		/* raw EUR_CR_CORE_REVISION: [23:16] major, [15:8] minor,
		 *                            [7:0] maintenance */
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
	case PRISMRV_PARAM_UAPI_VERSION:
		args->value = PRISMRV_UAPI_VERSION;
		break;
	case PRISMRV_PARAM_CMD_ABI:
		args->value = PRISMRV_CMD_ABI_STREAM_V1;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
