/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#ifndef MTK_DRM_DRV_H
#define MTK_DRM_DRV_H

#include <drm/drm_ioctl.h>
#include <linux/io.h>
#include "mtk_ddp_comp.h"

#define MAX_CONNECTOR	2
#define DDP_COMPONENT_DRM_OVL_ADAPTOR (DDP_COMPONENT_ID_MAX + 1)
#define DDP_COMPONENT_DRM_ID_MAX (DDP_COMPONENT_DRM_OVL_ADAPTOR + 1)

enum mtk_crtc_path {
	CRTC_MAIN,
	CRTC_EXT,
	CRTC_THIRD,
	MAX_CRTC,
};

struct device;
struct device_node;
struct drm_crtc;
struct drm_device;
struct drm_fb_helper;
struct drm_framebuffer;
struct drm_property;
struct mtk_g2d;
struct regmap;

struct mtk_drm_route {
	const unsigned int crtc_id;
	const unsigned int route_ddp;
};

struct mtk_mmsys_driver_data {
	const unsigned int *main_path;
	unsigned int main_len;
	const unsigned int *ext_path;
	unsigned int ext_len;
	const unsigned int *third_path;
	unsigned int third_len;
	const struct mtk_drm_route *conn_routes;
	unsigned int num_conn_routes;

	bool shadow_register;
	unsigned int mmsys_id;
	unsigned int mmsys_dev_num;

	u16 max_width;
	u16 min_width;
	u16 min_height;
};

struct mtk_drm_private {
	struct drm_device *drm;
	bool mtk_drm_bound;
	bool drm_master;
	struct device *dev;
	struct device_node *mutex_node;
	struct device *mutex_dev;
	struct device *mmsys_dev;
	struct device_node *comp_node[DDP_COMPONENT_DRM_ID_MAX];
	struct mtk_ddp_comp ddp_comp[DDP_COMPONENT_DRM_ID_MAX];
	struct mtk_mmsys_driver_data *data;
	struct drm_atomic_state *suspend_state;
	unsigned int mbox_index;
	struct mtk_drm_private **all_drm_private;

	/*
	 * The 2D blitter for this MMSYS, with a reference held, or NULL if this
	 * device has none.  Held rather than looked up per use so that the
	 * pointer is valid without a device-tree walk, and so the reference
	 * spans the DRM device's whole lifetime.
	 */
	struct mtk_g2d *g2d;
};

extern struct platform_driver mtk_disp_aal_driver;
extern struct platform_driver mtk_disp_ccorr_driver;
extern struct platform_driver mtk_disp_color_driver;
extern struct platform_driver mtk_disp_gamma_driver;
extern struct platform_driver mtk_disp_merge_driver;
extern struct platform_driver mtk_disp_ovl_adaptor_driver;
extern struct platform_driver mtk_disp_ovl_driver;
extern struct platform_driver mtk_disp_rdma_driver;
extern struct platform_driver mtk_disp_tdshp_driver;
extern struct platform_driver mtk_dpi_driver;
extern struct platform_driver mtk_dsi_driver;
extern struct platform_driver mtk_ethdr_driver;
extern struct platform_driver mtk_mdp_rdma_driver;
extern struct platform_driver mtk_padding_driver;

/*
 * G2D, from the DRM side.
 *
 * mtk-g2d.c owns the engine and takes DMA addresses; these take DRM
 * framebuffers and own the buffers for the duration.  They live here rather
 * than in the G2D object because mtk-g2d.o is deliberately free of DRM
 * dependencies.
 *
 * Contract common to all of them:
 *
 *  - Synchronous.  The engine is idle by the time they return, so there is no
 *    fence to wait on and the buffers can be released the moment they do.
 *
 *  - They may block, and can sleep: they take a reservation lock on each
 *    buffer for the whole operation.
 *
 *  - They refuse rather than guess.  A framebuffer that is not single-plane,
 *    linear and in a format the engine can encode is -EINVAL, and an origin,
 *    pitch or size the registers cannot hold is -EINVAL too.  A rectangle
 *    larger than the buffers hold is clipped down to what both surfaces can
 *    supply.  No argument is rounded into something the caller did not ask
 *    for.  The one error that is not a refusal is -ETIMEDOUT, which means the
 *    engine did not stop in time and its state is unknown.
 *
 *  - @g2d may be NULL, meaning the DRM device has no blitter.  That is an
 *    ordinary configuration - the MDP device has none - so it returns
 *    -ENODEV.  A caller that wants to fall back rather than fail must test
 *    mtk_drm_private::g2d itself before calling.
 */

/**
 * mtk_g2d_drm_blt - copy a rectangle at one origin shared by both surfaces.
 * @src_fb, @dst_fb: single-plane linear framebuffers
 * @x, @y: one origin, applied to both surfaces
 * @width, @height: rectangle, clipped to what both buffers contain
 *
 * The shared-origin form of mtk_g2d_drm_blt_rect().  Use that one to move a
 * rectangle to a different position.
 *
 * Returns 0 once the engine is idle, or -ENODEV, -EINVAL or -ETIMEDOUT.
 */
int mtk_g2d_drm_blt(struct mtk_g2d *g2d,
		    struct drm_framebuffer *src_fb,
		    struct drm_framebuffer *dst_fb,
		    u32 x, u32 y, u32 width, u32 height);

/**
 * mtk_g2d_drm_blt_rect - copy a rectangle between two independent origins.
 * @src_x, @src_y: origin within @src_fb, in pixels
 * @dst_x, @dst_y: origin within @dst_fb, in pixels
 * @width, @height: rectangle, clipped to what both buffers contain
 *
 * @width and @height are one size for both surfaces, because the engine has a
 * single scan window and no per-surface size register.  The origins may differ
 * freely; each is validated against its own framebuffer.
 *
 * An in-place copy is allowed when the two rectangles do not overlap; an
 * overlapping one is -EINVAL.
 *
 * Returns 0 once the engine is idle, or -ENODEV, -EINVAL or -ETIMEDOUT.
 */
int mtk_g2d_drm_blt_rect(struct mtk_g2d *g2d,
			 struct drm_framebuffer *src_fb,
			 u32 src_x, u32 src_y,
			 struct drm_framebuffer *dst_fb,
			 u32 dst_x, u32 dst_y,
			 u32 width, u32 height);

/**
 * mtk_g2d_drm_fill - fill a rectangle of a DRM framebuffer with a constant.
 * @color: an ordinary DRM pixel value in the destination's format
 *
 * Returns 0 once the engine is idle, or -ENODEV, -EINVAL or -ETIMEDOUT.
 */
int mtk_g2d_drm_fill(struct mtk_g2d *g2d,
		     struct drm_framebuffer *dst_fb,
		     u32 x, u32 y, u32 width, u32 height, u32 color);

/**
 * mtk_g2d_can_blit - test whether a DRM format pair has CLRFMT encodings.
 *
 * Returns 0 if both formats can be programmed, or -EINVAL if either cannot.
 */
int mtk_g2d_can_blit(u32 src_format, u32 dst_format);

/**
 * mtk_g2d_format_name - CLRFMT name of a DRM fourcc, for diagnostics.
 *
 * Returns a static string, or "unsupported".
 */
const char *mtk_g2d_format_name(u32 format);

/*
 * mtk_g2d_ioctls - the G2D userspace ioctls, in mtk-g2d-uapi.c.
 *
 * Referenced by mtk_drm_driver's .ioctls/.num_ioctls.  It is declared here
 * rather than as a local extern in mtk_drm_drv.c because .num_ioctls applies
 * sizeof() to it, which needs the complete type; mtk_drm_drv.c does not include
 * the header that defines it.
 */
extern const struct drm_ioctl_desc mtk_g2d_ioctls[];

#endif /* MTK_DRM_DRV_H */
