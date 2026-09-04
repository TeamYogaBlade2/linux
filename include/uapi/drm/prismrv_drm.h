/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * prismrv_drm.h — user API for the PrismRV DRM driver
 * (Imagination PowerVR SGX series GPUs).
 */
#ifndef _UAPI_PRISMRV_DRM_H_
#define _UAPI_PRISMRV_DRM_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define PRISMRV_UAPI_VERSION		1

/* GPU virtual addresses are 32-bit (BIF MMU, 4 GiB space). */
typedef __u32 prismrv_dev_addr_t;

struct drm_prismrv_gem_create {
	__u64 size;		/* in bytes, page aligned by the kernel */
	__u32 flags;		/* PRISMRV_BO_* */
	__u32 handle;		/* out: GEM handle */
};

#define PRISMRV_BO_CACHED	0x0	/* normal cached mapping (default) */
#define PRISMRV_BO_UNCACHED	(1U << 31) /* write-combine */

struct drm_prismrv_gem_mmap_offset {
	__u32 handle;
	__u32 flags;
	__u64 offset;		/* out: fake mmap offset for drm_mmap() */
};

struct drm_prismrv_submit {
	__u32 cmd_handle;	/* GEM handle holding the command stream */
	__u32 cmd_size;		/* valid bytes in the command buffer */

	__u32 num_in_fences;	/* sync_file fds to wait on before start */
	__u64 in_fences;	/* pointer to __s32 array */

	__u32 num_bos;		/* BOs referenced by this submit */
	__u64 bos;		/* pointer to __u32 GEM handle array */

	__u32 out_fence_fd;	/* out: sync_file fd signalling completion */
	__u32 cmd_type;		/* PRISMRV_CMD_* service type */
};

struct drm_prismrv_get_param {
	__u32 param;		/* PRISMRV_PARAM_* */
	__u32 pad;
	__u64 value;		/* out */
};

#define PRISMRV_PARAM_GPU_ID		1 /* core id + revision, e.g. 0x05440073 */
#define PRISMRV_PARAM_CORE_COUNT	2 /* number of SGX MP cores */
#define PRISMRV_PARAM_UKERNEL_SIZE	3 /* size of the loaded uKernel image */
#define PRISMRV_PARAM_ERRATA		4 /* bitmask of active BRN workarounds */

#define DRM_PRISMRV_GEM_CREATE		0x00
#define DRM_PRISMRV_GEM_MMAP_OFFSET	0x01
#define DRM_PRISMRV_SUBMIT		0x02
#define DRM_PRISMRV_GET_PARAM		0x03

#define DRM_IOCTL_PRISMRV_GEM_CREATE \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_PRISMRV_GEM_CREATE, struct drm_prismrv_gem_create)
#define DRM_IOCTL_PRISMRV_GEM_MMAP_OFFSET \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_PRISMRV_GEM_MMAP_OFFSET, struct drm_prismrv_gem_mmap_offset)
#define DRM_IOCTL_PRISMRV_SUBMIT \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_PRISMRV_SUBMIT, struct drm_prismrv_submit)
#define DRM_IOCTL_PRISMRV_GET_PARAM \
	_IOWR(DRM_IOCTL_BASE, DRM_COMMAND_BASE + DRM_PRISMRV_GET_PARAM, struct drm_prismrv_get_param)

#endif /* _UAPI_PRISMRV_DRM_H_ */
