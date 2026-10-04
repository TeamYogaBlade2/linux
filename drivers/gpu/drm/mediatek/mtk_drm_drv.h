/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#ifndef MTK_DRM_DRV_H
#define MTK_DRM_DRV_H

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
 * All of them are synchronous: the engine is idle by the time they return.
 */

/**
 * mtk_g2d_drm_blt - copy a rectangle between two DRM framebuffers.
 * @g2d: the engine, from mtk_drm_private::g2d
 * @src_fb, @dst_fb: single-plane linear framebuffers
 * @x, @y: one origin, applied to both surfaces
 * @width, @height: rectangle, clipped to what both buffers contain
 *
 * The engine has no independent source and destination origins, so this can
 * only express a same-coordinate copy.  Returns 0 once the engine is idle.
 */
int mtk_g2d_drm_blt(struct mtk_g2d *g2d,
		    struct drm_framebuffer *src_fb,
		    struct drm_framebuffer *dst_fb,
		    u32 x, u32 y, u32 width, u32 height);

/**
 * mtk_g2d_drm_fill - fill a rectangle of a DRM framebuffer with a constant.
 * @color: an ordinary DRM pixel value in the destination's format
 *
 * Returns 0 once the engine is idle.
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

#endif /* MTK_DRM_DRV_H */
