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

/* free-list of (base, size) VA ranges reclaimed on BO destroy */
struct prismrv_va_range {
	u32 base;
	size_t size;
	struct list_head node;
};
static LIST_HEAD(va_free_list);
static u32 va_next = PRISMRV_VA_BASE;

/* return a range to the free list, merging adjacent neighbours.
 * callers hold va_lock. */
static void prismrv_va_free_locked(u32 base, size_t size)
{
	struct prismrv_va_range *range, *next, *new_range;
	struct list_head *insert_before = &va_free_list; /* sentinel = tail */

	/* merge into a lower neighbour that ends exactly at our base */
	list_for_each_entry(range, &va_free_list, node) {
		if (range->base + range->size == base) {
			range->size += size;

			/* and possibly chain into the next free range */
			if (!list_is_last(&range->node, &va_free_list)) {
				next = list_next_entry(range, node);

				if (range->base + range->size == next->base) {
					range->size += next->size;
					list_del(&next->node);
					kfree(next);
				}
			}
			return;
		}
		if (range->base > base) {
			/*
			 * 'range' is the first existing entry that starts
			 * after our new range.  We want to insert before it.
			 * list_add_tail(new, &range->node) inserts before
			 * range->node — exactly what we want.
			 */
			insert_before = &range->node;
			break;
		}
		/*
		 * 'range' starts before 'base' and does not end at 'base'
		 * (merge above didn't fire).  Continue scanning.
		 * If we exhaust the loop, insert_before stays as the
		 * sentinel (va_free_list head), which list_add_tail treats
		 * as "append to tail" — the correct behaviour for a new
		 * range that is higher than all existing ones.
		 */
	}

	new_range = kmalloc(sizeof(*new_range), GFP_KERNEL);
	if (!new_range)
		return; /* VA leak is non-fatal; the space is large */
	new_range->base = base;
	new_range->size = size;

	/* list_add_tail(new, head) inserts new BEFORE head */
	list_add_tail(&new_range->node, insert_before);
}

struct prismrv_bo {
	struct drm_gem_shmem_object base;
	u32 gpu_va;
	struct list_head bo_node;	/* prismrv_device.bo_list */
};

static inline struct prismrv_bo *to_prbo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct prismrv_bo, base);
}

static void prismrv_bo_free(struct drm_gem_object *obj)
{
	struct prismrv_device *pv = to_prismrv(obj->dev);
	struct prismrv_bo *bo = to_prbo(obj);

	/* remove from the device-wide BO list before touching MMU */
	spin_lock(&pv->bo_list_lock);
	list_del(&bo->bo_node);
	spin_unlock(&pv->bo_list_lock);

	if (bo->gpu_va) {
		mutex_lock(&pv->mmu_lock);
		/*
		 * Guard against the case where recovery/runtime-resume
		 * has already torn down the MMU (pd_pts == NULL).  In
		 * that case gpu_va was cleared by
		 * prismrv_mmu_invalidate_all_bos() and there is nothing
		 * to unmap; skip silently.
		 */
		if (pv->pd_pts)
			prismrv_mmu_unmap_locked(pv, bo->gpu_va, obj->size);
		mutex_lock(&va_lock);
		prismrv_va_free_locked(bo->gpu_va, PAGE_ALIGN(obj->size));
		mutex_unlock(&va_lock);
		bo->gpu_va = 0;
		mutex_unlock(&pv->mmu_lock);
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
	size_t want = PAGE_ALIGN(shmem->base.size);
	size_t va_off = 0;
	int ret;

	/*
	 * Fast-path check outside the lock: if gpu_va is already set the
	 * BO was mapped in a previous submit and the mapping is still live.
	 * Re-check under mmu_lock below to close the TOCTOU window between
	 * concurrent submits referencing the same BO.
	 */
	if (READ_ONCE(bo->gpu_va))
		return 0;

	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	mutex_lock(&pv->mmu_lock);

	/* re-check under lock: another thread may have mapped it first */
	if (bo->gpu_va) {
		mutex_unlock(&pv->mmu_lock);
		return 0;
	}

	mutex_lock(&va_lock);
	/* reuse the first freed range large enough, else bump-allocate */
	{
		struct prismrv_va_range *range;

		list_for_each_entry(range, &va_free_list, node) {
			if (range->size >= want) {
				bo->gpu_va = range->base;
				if (range->size > want) {
					range->base += want;
					range->size -= want;
				} else {
					list_del(&range->node);
					kfree(range);
				}
				break;
			}
		}
		if (!bo->gpu_va) {
			if (want > PRISMRV_VA_SIZE ||
			    va_next > PRISMRV_VA_BASE + PRISMRV_VA_SIZE - want) {
				mutex_unlock(&va_lock);
				mutex_unlock(&pv->mmu_lock);
				return -ENOSPC;
			}
			bo->gpu_va = va_next;
			va_next += want;
		}
	}
	mutex_unlock(&va_lock);

	va_off = 0;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		size_t len = sg_dma_len(sg);

		ret = prismrv_mmu_map_locked(pv, bo->gpu_va + va_off,
				      sg_dma_address(sg), len);
		if (ret)
			goto err_unmap;
		va_off += len;
	}

	mutex_unlock(&pv->mmu_lock);
	return 0;

err_unmap:
	prismrv_mmu_unmap_locked(pv, bo->gpu_va, va_off);
	mutex_lock(&va_lock);
	prismrv_va_free_locked(bo->gpu_va, want);
	mutex_unlock(&va_lock);
	bo->gpu_va = 0;
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

int prismrv_gem_create_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	struct drm_prismrv_gem_create *args = data;
	struct drm_gem_shmem_object *shmem;
	int ret;

	if (args->size == 0 || args->size > SZ_256M)
		return -EINVAL;
	args->size = PAGE_ALIGN(args->size);

	shmem = drm_gem_shmem_create(dev, args->size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	/*
	 * Write-combine mapping for streaming buffers (vertex arrays,
	 * command streams): CPU writes bypass the cache so the GPU sees
	 * them without an explicit flush.  Must be set before the first
	 * vmap (drm_gem_shmem_vmap honours it when creating the pgprot).
	 */
	if (args->flags & PRISMRV_BO_UNCACHED)
		shmem->map_wc = true;

	ret = drm_gem_handle_create(file, &shmem->base, &args->handle);
	drm_gem_object_put(&shmem->base);
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
 * prismrv_mmu_invalidate_all_bos() - clear gpu_va on every live BO.
 *
 * Called from prismrv_hw_fini() before tearing down the MMU page
 * tables.  Clearing bo->gpu_va ensures that the next submit re-maps
 * each BO into the fresh MMU context rather than skipping the map
 * (because pin_and_map() early-returns when gpu_va != 0).
 *
 * The VA space is also returned to the free list so the new mapping
 * can reuse the same addresses if convenient.
 *
 * Must be called with pv->mmu_lock held by the caller (i.e. from
 * hw_fini which already serialises via init_mutex + submit_rwsem).
 */
void prismrv_mmu_invalidate_all_bos(struct prismrv_device *pv)
{
	struct prismrv_bo *bo;
	LIST_HEAD(snapshot);

	/*
	 * Build a snapshot of gpu_va values under the spinlock, then
	 * release the lock before calling mutex_lock(&va_lock).
	 *
	 * The previous implementation called mutex_lock() while holding
	 * bo_list_lock (a spinlock), which is illegal: mutexes are
	 * sleeping locks and cannot be acquired in atomic/spinlock context
	 * (triggers "BUG: sleeping function called from invalid context").
	 *
	 * Correctness: callers hold mmu_lock + init_mutex + submit_rwsem,
	 * so no concurrent pin_and_map() or bo_free() can run.  The
	 * bo_list itself is protected by bo_list_lock but its entries are
	 * stable here, so reading gpu_va under the spinlock and clearing
	 * it after is safe — the two stores are separated by the acquire
	 * of mmu_lock, which pin_and_map() also holds before modifying
	 * gpu_va.
	 */
	mutex_lock(&va_lock);
	spin_lock(&pv->bo_list_lock);
	list_for_each_entry(bo, &pv->bo_list, bo_node) {
		if (bo->gpu_va) {
			prismrv_va_free_locked(bo->gpu_va,
					       PAGE_ALIGN(bo->base.base.size));
			bo->gpu_va = 0;
		}
	}
	spin_unlock(&pv->bo_list_lock);
	mutex_unlock(&va_lock);
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
	default:
		return -EINVAL;
	}
	return 0;
}
