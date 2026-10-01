/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * prismrv_drm.h — user API for the PrismRV DRM driver
 * (Imagination PowerVR SGX series GPUs).
 */
#ifndef _UAPI_PRISMRV_DRM_H_
#define _UAPI_PRISMRV_DRM_H_

#include "drm.h"

#include <linux/ioctl.h>
#include <linux/types.h>

/* Incremented when ABI-incompatible changes are made. */
#define PRISMRV_UAPI_VERSION		3

/* GPU virtual addresses are 32-bit (BIF MMU, 4 GiB space). */
typedef __u32 prismrv_dev_addr_t;

struct drm_prismrv_gem_create {
	__u64 size;		/* in bytes, page aligned by the kernel */
	__u32 flags;		/* PRISMRV_BO_* */
	__u32 handle;		/* out: GEM handle */
	__u32 gpu_va;		/* out: fixed GPU virtual address of the BO */
	__u32 pad;		/* must be zero on input, zeroed on output */
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

/*
 * History: UAPI v1 had a combined PRISMRV_PARAM_GPU_ID (value 1) that
 * returned the raw EUR_CR_CORE_REVISION value.  Since UAPI v2 value 1 is
 * PRISMRV_PARAM_CORE_ID (EUR_CR_CORE_ID) and the revision is a separate
 * parameter, because the two are separate hardware registers.  v1
 * userspace that queries "GPU_ID" now silently receives CORE_ID, so it
 * must check PRISMRV_PARAM_UAPI_VERSION (v3+) before trusting values.
 * v3 added GEM_CREATE.gpu_va, PARAM_UAPI_VERSION and PARAM_CMD_ABI.
 */
#define PRISMRV_PARAM_CORE_ID		1 /* raw EUR_CR_CORE_ID register */
#define PRISMRV_PARAM_CORE_REVISION	5 /* raw EUR_CR_CORE_REVISION register */
#define PRISMRV_PARAM_CORE_COUNT	2 /* number of SGX MP cores */
#define PRISMRV_PARAM_UKERNEL_SIZE	3 /* size of the loaded uKernel image */
#define PRISMRV_PARAM_ERRATA		4 /* bitmask of active BRN workarounds */

/*
 * PRISMRV_PARAM_UAPI_VERSION returns PRISMRV_UAPI_VERSION so userspace can
 * detect the kernel ABI with a query instead of a failing ioctl.
 *
 * PRISMRV_PARAM_CMD_ABI describes how the kernel presents a submit to the
 * uKernel:
 *   PRISMRV_CMD_ABI_STREAM_V1: the command BO is opaque to the kernel.
 *     The kernel publishes one CCB entry with
 *       data[0] = GPU VA of the command BO, data[1] = cmd_size,
 *       data[2] = GPU VA of bos[0] (if num_bos >= 1).
 *     Interpreting the stream is the job of the loaded uKernel image
 *     (or the PrismRV emulator).  The stock vendor uKernel does NOT
 *     understand it: real-hardware rendering needs a uKernel that does,
 *     or a userspace that emits vendor TA/3D CCB commands.
 *   All GPU addresses embedded in a command stream must be taken from
 *   drm_prismrv_gem_create.gpu_va; GEM handles mean nothing to the GPU.
 */
#define PRISMRV_PARAM_UAPI_VERSION	6
#define PRISMRV_PARAM_CMD_ABI		7
#define PRISMRV_CMD_ABI_STREAM_V1	1

/*
 * Surface formats used by SET_RT / SET_TEXTURE packets of STREAM_V1
 * (driver-independent codes, not DRM fourccs or Gallium pipe_format).
 */
#define PRISMRV_FMT_RGBA8_UNORM	0
#define PRISMRV_FMT_BGRA8_UNORM	1
#define PRISMRV_FMT_RGBA32F	2

/*
 * STREAM_V1 security contract: the kernel copies the stream into a
 * kernel-owned buffer, checks every packet (unknown opcodes and any GPU
 * address range outside the BOs listed in drm_prismrv_submit.bos[] are
 * rejected with -EINVAL) and executes only the copy.  Streams may only
 * reference BOs the caller listed.  Packet layout and opcodes are
 * documented in Mesa's src/gallium/drivers/prismrv/prismrv_context.c.
 */
#define PRISMRV_STREAM_MAX_BYTES	(1024 * 1024)

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
