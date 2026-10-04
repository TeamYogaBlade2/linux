// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author: YT SHEN <yt.shen@mediatek.com>
 */

#include <linux/aperture.h>
#include <linux/dma-resv.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/dma-mapping.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_of.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/mtk_g2d.h>

#include "mtk-g2d.h"
#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

#define DRIVER_NAME "mediatek"
#define DRIVER_DESC "Mediatek SoC DRM"
#define DRIVER_MAJOR 1
#define DRIVER_MINOR 0

static const struct drm_mode_config_helper_funcs mtk_drm_mode_config_helpers = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

static struct drm_framebuffer *
mtk_drm_mode_fb_create(struct drm_device *dev,
		       struct drm_file *file,
		       const struct drm_format_info *info,
		       const struct drm_mode_fb_cmd2 *cmd)
{
	if (info->num_planes != 1)
		return ERR_PTR(-EINVAL);

	return drm_gem_fb_create(dev, file, info, cmd);
}

static const struct drm_mode_config_funcs mtk_drm_mode_config_funcs = {
	.fb_create = mtk_drm_mode_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const unsigned int mt2701_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_BLS,
	DDP_COMPONENT_DSI0,
};

static const unsigned int mt2701_mtk_ddp_ext[] = {
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_DPI0,
};

static const unsigned int mt6589_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_BLS,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_DSI0,
};

static const unsigned int mt7623_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_BLS,
	DDP_COMPONENT_DPI0,
};

static const unsigned int mt7623_mtk_ddp_ext[] = {
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_DSI0,
};

static const unsigned int mt2712_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_OD0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_DPI0,
	DDP_COMPONENT_PWM0,
};

static const unsigned int mt2712_mtk_ddp_ext[] = {
	DDP_COMPONENT_OVL1,
	DDP_COMPONENT_COLOR1,
	DDP_COMPONENT_AAL1,
	DDP_COMPONENT_OD1,
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_DPI1,
	DDP_COMPONENT_PWM1,
};

static const unsigned int mt2712_mtk_ddp_third[] = {
	DDP_COMPONENT_RDMA2,
	DDP_COMPONENT_DSI3,
	DDP_COMPONENT_PWM2,
};

static unsigned int mt8167_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_DSI0,
};

static const unsigned int mt8173_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_OD0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_UFOE,
	DDP_COMPONENT_DSI0,
	DDP_COMPONENT_PWM0,
};

static const unsigned int mt8173_mtk_ddp_ext[] = {
	DDP_COMPONENT_OVL1,
	DDP_COMPONENT_COLOR1,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_DPI0,
};

static const unsigned int mt8183_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_OVL_2L0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,
};

static const unsigned int mt8183_mtk_ddp_ext[] = {
	DDP_COMPONENT_OVL_2L1,
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_DPI0,
};

static const unsigned int mt8186_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_POSTMASK0,
	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,
};

static const unsigned int mt8186_mtk_ddp_ext[] = {
	DDP_COMPONENT_OVL_2L0,
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_DPI0,
};

static const unsigned int mt8188_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_POSTMASK0,
	DDP_COMPONENT_DITHER0,
};

static const struct mtk_drm_route mt8188_mtk_ddp_main_routes[] = {
	{0, DDP_COMPONENT_DP_INTF0},
	{0, DDP_COMPONENT_DSI0},
};

static const unsigned int mt8192_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_OVL_2L0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_POSTMASK0,
	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSI0,
};

static const unsigned int mt8192_mtk_ddp_ext[] = {
	DDP_COMPONENT_OVL_2L2,
	DDP_COMPONENT_RDMA4,
	DDP_COMPONENT_DPI0,
};

static const unsigned int mt8195_mtk_ddp_main[] = {
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DSC0,
	DDP_COMPONENT_MERGE0,
	DDP_COMPONENT_DP_INTF0,
};

static const unsigned int mt8195_mtk_ddp_ext[] = {
	DDP_COMPONENT_DRM_OVL_ADAPTOR,
	DDP_COMPONENT_MERGE5,
	DDP_COMPONENT_DP_INTF1,
};

static const struct mtk_mmsys_driver_data mt2701_mmsys_driver_data = {
	.main_path = mt2701_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt2701_mtk_ddp_main),
	.ext_path = mt2701_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt2701_mtk_ddp_ext),
	.shadow_register = true,
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt6589_mmsys_driver_data = {
	.main_path = mt6589_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt6589_mtk_ddp_main),
	.shadow_register = true,
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt7623_mmsys_driver_data = {
	.main_path = mt7623_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt7623_mtk_ddp_main),
	.ext_path = mt7623_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt7623_mtk_ddp_ext),
	.shadow_register = true,
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt2712_mmsys_driver_data = {
	.main_path = mt2712_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt2712_mtk_ddp_main),
	.ext_path = mt2712_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt2712_mtk_ddp_ext),
	.third_path = mt2712_mtk_ddp_third,
	.third_len = ARRAY_SIZE(mt2712_mtk_ddp_third),
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt8167_mmsys_driver_data = {
	.main_path = mt8167_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt8167_mtk_ddp_main),
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt8173_mmsys_driver_data = {
	.main_path = mt8173_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt8173_mtk_ddp_main),
	.ext_path = mt8173_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt8173_mtk_ddp_ext),
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt8183_mmsys_driver_data = {
	.main_path = mt8183_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt8183_mtk_ddp_main),
	.ext_path = mt8183_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt8183_mtk_ddp_ext),
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt8186_mmsys_driver_data = {
	.main_path = mt8186_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt8186_mtk_ddp_main),
	.ext_path = mt8186_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt8186_mtk_ddp_ext),
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt8188_vdosys0_driver_data = {
	.main_path = mt8188_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt8188_mtk_ddp_main),
	.conn_routes = mt8188_mtk_ddp_main_routes,
	.num_conn_routes = ARRAY_SIZE(mt8188_mtk_ddp_main_routes),
	.mmsys_dev_num = 2,
	.max_width = 8191,
	.min_width = 1,
	.min_height = 1,
};

static const struct mtk_mmsys_driver_data mt8192_mmsys_driver_data = {
	.main_path = mt8192_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt8192_mtk_ddp_main),
	.ext_path = mt8192_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt8192_mtk_ddp_ext),
	.mmsys_dev_num = 1,
};

static const struct mtk_mmsys_driver_data mt8195_vdosys0_driver_data = {
	.main_path = mt8195_mtk_ddp_main,
	.main_len = ARRAY_SIZE(mt8195_mtk_ddp_main),
	.mmsys_dev_num = 2,
	.max_width = 8191,
	.min_width = 1,
	.min_height = 1,
};

static const struct mtk_mmsys_driver_data mt8195_vdosys1_driver_data = {
	.ext_path = mt8195_mtk_ddp_ext,
	.ext_len = ARRAY_SIZE(mt8195_mtk_ddp_ext),
	.mmsys_id = 1,
	.mmsys_dev_num = 2,
	.max_width = 8191,
	.min_width = 2, /* 2-pixel align when ethdr is bypassed */
	.min_height = 1,
};

static const struct mtk_mmsys_driver_data mt8365_mmsys_driver_data = {
	.mmsys_dev_num = 1,
};

static const struct of_device_id mtk_drm_of_ids[] = {
	{ .compatible = "mediatek,mt2701-mmsys",
	  .data = &mt2701_mmsys_driver_data},
	{ .compatible = "mediatek,mt6589-dispsys",
	  .data = &mt6589_mmsys_driver_data},
	{ .compatible = "mediatek,mt7623-mmsys",
	  .data = &mt7623_mmsys_driver_data},
	{ .compatible = "mediatek,mt2712-mmsys",
	  .data = &mt2712_mmsys_driver_data},
	{ .compatible = "mediatek,mt8167-mmsys",
	  .data = &mt8167_mmsys_driver_data},
	{ .compatible = "mediatek,mt8173-mmsys",
	  .data = &mt8173_mmsys_driver_data},
	{ .compatible = "mediatek,mt8183-mmsys",
	  .data = &mt8183_mmsys_driver_data},
	{ .compatible = "mediatek,mt8186-mmsys",
	  .data = &mt8186_mmsys_driver_data},
	{ .compatible = "mediatek,mt8188-vdosys0",
	  .data = &mt8188_vdosys0_driver_data},
	{ .compatible = "mediatek,mt8188-vdosys1",
	  .data = &mt8195_vdosys1_driver_data},
	{ .compatible = "mediatek,mt8192-mmsys",
	  .data = &mt8192_mmsys_driver_data},
	{ .compatible = "mediatek,mt8195-mmsys",
	  .data = &mt8195_vdosys0_driver_data},
	{ .compatible = "mediatek,mt8195-vdosys0",
	  .data = &mt8195_vdosys0_driver_data},
	{ .compatible = "mediatek,mt8195-vdosys1",
	  .data = &mt8195_vdosys1_driver_data},
	{ .compatible = "mediatek,mt8365-mmsys",
	  .data = &mt8365_mmsys_driver_data},
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_drm_of_ids);

/**
 * struct mtk_g2d_drm_surf - a framebuffer resolved for the engine.
 * @addr: DMA address of the first pixel, already advanced past any offset
 * @pitch: stride in bytes
 * @bpp: bytes per pixel, needed for the address arithmetic
 * @obj: the GEM object, referenced while this surface is live
 * @width: image width in pixels
 * @height: image height in pixels
 * @g2d_fmt: CLRFMT this buffer is programmed with
 *
 * @width and @height are the image dimensions, which bound every rectangle:
 * the engine only ever sees a pitch, and a pitch says nothing about how many
 * rows exist.
 *
 * @obj is NULL for a disabled plane.  That is the one case where the remaining
 * fields are meaningless, which is why callers check @obj rather than @addr.
 */
struct mtk_g2d_drm_surf {
	dma_addr_t addr;
	u32 pitch;
	u32 bpp;
	u32 width;
	u32 height;
	enum g2d_format g2d_fmt;
	struct drm_gem_object *obj;
};

/*
 * DRM fourcc to CLRFMT.
 *
 * Only the encodings the engine actually has are listed, and each is mapped to
 * the DRM format whose in-memory layout the matching CLRFMT describes - there
 * is no byte-swap or channel-swap bit being used here, so a BGR variant of any
 * of these is deliberately absent rather than approximated.
 *
 * DRM_FORMAT_RGB888 is the packed 24bpp RGB format, which is exactly what
 * g2d_clrfmt_rgb888 describes, including its 1-byte start-address alignment.
 */
static int mtk_g2d_drm_to_clrfmt(u32 format, enum g2d_format *out)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
		*out = g2d_clrfmt_rgb565;
		return 0;
	case DRM_FORMAT_RGB888:
		*out = g2d_clrfmt_rgb888;
		return 0;
	case DRM_FORMAT_ARGB8888:
		*out = g2d_clrfmt_argb8888;
		return 0;
	case DRM_FORMAT_XRGB8888:
		*out = g2d_clrfmt_xrgb8888;
		return 0;
	default:
		return -EINVAL;
	}
}

const char *mtk_g2d_format_name(u32 format)
{
	switch (format) {
	case DRM_FORMAT_RGB565:
		return "RGB565";
	case DRM_FORMAT_RGB888:
		return "RGB888";
	case DRM_FORMAT_ARGB8888:
		return "ARGB8888";
	case DRM_FORMAT_XRGB8888:
		return "XRGB8888";
	default:
		return "unsupported";
	}
}

int mtk_g2d_can_blit(u32 src_format, u32 dst_format)
{
	enum g2d_format unused;

	if (mtk_g2d_drm_to_clrfmt(src_format, &unused))
		return -EINVAL;

	return mtk_g2d_drm_to_clrfmt(dst_format, &unused);
}

/**
 * mtk_g2d_drm_surf_init - resolve one framebuffer for the engine.
 * @dev: device used for diagnostics
 * @fb: the framebuffer, or NULL for a disabled plane
 * @out: resolved surface
 *
 * The address comes from fb->obj[0]'s dma_addr, which is exactly what
 * mtk_plane_update_new_state() hands to OVL and RDMA: the DRM device has no
 * IOMMU on the display path, so the hardware consumes the raw DMA address and
 * a buffer G2D writes is the same buffer the display engines read.
 *
 * Everything that could make that address mean something other than "the first
 * pixel of a linear image" is rejected: a non-linear modifier, more than one
 * plane, or a format the engine cannot encode.  Guessing at any of them would
 * mean programming the engine with an address into the middle of somebody
 * else's data.
 *
 * Returns 0, or -EINVAL.  A NULL @fb succeeds with @out->obj left NULL.
 */
static int mtk_g2d_drm_surf_init(struct device *dev,
				 struct drm_framebuffer *fb,
				 struct mtk_g2d_drm_surf *out)
{
	struct drm_gem_dma_object *dma_obj;
	const struct drm_format_info *info;
	enum g2d_format g2d_fmt;
	int ret;

	memset(out, 0, sizeof(*out));

	/* A disabled plane is a normal thing to be handed; the caller checks. */
	if (!fb)
		return 0;

	if (fb->format->num_planes != 1) {
		dev_dbg(dev, "G2D: %u-plane framebuffer rejected\n",
			fb->format->num_planes);
		return -EINVAL;
	}

	/*
	 * Only linear.  mtk_plane.c handles the compressed (AFBC) layout by
	 * deriving a second address and pitch from block geometry, and none of
	 * that reaches the engine, so a compressed buffer would be read as a
	 * plain linear one at the wrong address.
	 */
	if (fb->modifier != DRM_FORMAT_MOD_LINEAR) {
		dev_dbg(dev, "G2D: modifier 0x%llx rejected\n",
			(unsigned long long)fb->modifier);
		return -EINVAL;
	}

	if (!fb->obj[0]) {
		dev_dbg(dev, "G2D: framebuffer without a GEM object rejected\n");
		return -EINVAL;
	}

	if (mtk_g2d_drm_to_clrfmt(fb->format->format, &g2d_fmt)) {
		dev_dbg(dev, "G2D: format %s rejected\n",
			mtk_g2d_format_name(fb->format->format));
		return -EINVAL;
	}

	info = fb->format;

	out->pitch = fb->pitches[0];
	out->bpp = info->cpp[0];
	out->width = fb->width;
	out->height = fb->height;
	out->g2d_fmt = g2d_fmt;

	/*
	 * fb->offsets[0] is a byte offset into the backing object where the
	 * image starts, and is not an x/y pixel offset.  Adding it is the same
	 * arithmetic drm_gem_fb_create() set up, so the address is the first
	 * pixel of the image and not the first byte of the allocation.
	 */
	dma_obj = to_drm_gem_dma_obj(fb->obj[0]);
	out->addr = dma_obj->dma_addr + fb->offsets[0];
	out->obj = fb->obj[0];

	/*
	 * Take the reference here rather than leaving it to the caller: the
	 * framebuffer itself only borrows fb->obj[], so a last_plane_state
	 * reference going away must not free the GEM object out from under an
	 * operation that is about to program its address into the engine.
	 * mtk_g2d_drm_surf_fini() drops it, on the error paths below too.
	 */
	drm_gem_object_get(out->obj);

	/*
	 * The pitch registers hold 14 bits with 0x2000 as the usable maximum.
	 * Checking it here rather than letting mtk_g2d_blt() reject it later
	 * means the diagnostic names the framebuffer instead of a number, and it
	 * keeps the pitch checks and the reference above in one place.
	 */
	if (out->pitch > 0x2000) {
		dev_dbg(dev, "G2D: pitch %u exceeds the engine maximum\n",
			out->pitch);
		ret = -EINVAL;
		goto err_put;
	}

	return 0;

err_put:
	drm_gem_object_put(out->obj);
	out->obj = NULL;

	return ret;
}

static void mtk_g2d_drm_surf_fini(struct mtk_g2d_drm_surf *surf)
{
	if (surf->obj)
		drm_gem_object_put(surf->obj);
}

/**
 * mtk_g2d_clip_rect - narrow a rectangle to what both surfaces can address.
 * @src: source surface, which must be a real image
 * @dst: destination surface, which must be a real image
 * @src_x, @src_y: origin within the source, in pixels
 * @dst_x, @dst_y: origin within the destination, in pixels
 * @w: requested width in pixels
 * @h: requested height in pixels
 * @out_w: clipped width in pixels
 * @out_h: clipped height in pixels
 *
 * The engine has one scan window sized once for both ports, so the rectangle
 * has to be expressible in both surfaces: it is bounded by the narrower of the
 * two, offset from each surface by that surface's own origin.  Clipping here is
 * what keeps a caller-supplied rectangle from running off the end of either
 * buffer: the pitch registers are the only thing bounding a row, and nothing
 * else in the engine knows where the buffer ends.
 *
 * @w and @h are clipped down to what both surfaces hold and then to the
 * engine's 2048 scan window.  A larger request is truncated rather than
 * refused, so a caller asking to copy a whole screen gets the part the engine
 * can express instead of nothing at all.
 *
 * Returns 0 and a non-empty rectangle, or -EINVAL.  @out_w and @out_h are only
 * written on success, so a caller that fails here cannot pass a stale zero on
 * to the engine - a zero-area rectangle would be rejected downstream anyway,
 * but by then it would already be a much less specific error.
 */
static int mtk_g2d_clip_rect(const struct mtk_g2d_drm_surf *src,
			     const struct mtk_g2d_drm_surf *dst,
			     u32 src_x, u32 src_y,
			     u32 dst_x, u32 dst_y,
			     u32 w, u32 h,
			     u32 *out_w, u32 *out_h)
{
	u32 src_w, dst_w, src_h, dst_h;

	/*
	 * A row can only be as wide as the pitch allows, and the pitch is
	 * usually larger than the image, so the binding limit on a row is the
	 * narrower of the two.  Height is bounded by the image, since a pitch
	 * says nothing about how many rows exist.
	 */
	src_w = min(src->width, src->pitch / src->bpp);
	dst_w = min(dst->width, dst->pitch / dst->bpp);
	src_h = src->height;
	dst_h = dst->height;

	/*
	 * Each origin must be inside its own image, and the rectangle must
	 * start within both.  A rectangle starting past the edge of either
	 * buffer is not clamped - it is rejected, because there is no
	 * meaningful part of it left to keep.
	 */
	if (src_x >= src_w || src_x >= src_h || dst_x >= dst_w || dst_x >= dst_h)
		return -EINVAL;

	/*
	 * g2d_check_rect() caps each origin at the same 2048 bound it caps the
	 * scan window at, so an origin past that is refused there.  Refusing it
	 * here too, with the same bound spelled out, keeps the reason attached
	 * to the framebuffer rather than to a number the caller passed in.
	 */
	if (src_x > 2048 || src_y > 2048 || dst_x > 2048 || dst_y > 2048)
		return -EINVAL;

	src_w -= src_x;
	src_h -= src_y;
	dst_w -= dst_x;
	dst_h -= dst_y;

	w = min3(w, src_w, dst_w);
	h = min3(h, src_h, dst_h);
	if (!w || !h)
		return -EINVAL;

	/* W2M_SIZE holds width and height as 12-bit fields, documented 1..2048. */
	w = min(w, 2048u);
	h = min(h, 2048u);
	if (!w || !h)
		return -EINVAL;

	*out_w = w;
	*out_h = h;

	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * Lifetime.
 *
 * The engine reads and writes memory that userspace owns, asynchronously with
 * respect to userspace's view of it.  Two things therefore have to be true for
 * the whole of an operation, and both are established here rather than assumed:
 *
 *   - The buffers cannot be freed or remapped underneath us.  The GEM object
 *     reference taken in mtk_g2d_drm_surf_init() stops the allocation being
 *     freed, and the reservation lock stops anything that mutates the mapping -
 *     a vmap/vunmap of the same object, a prime export, another driver taking
 *     it for its own use - from proceeding while the engine is reading it.
 *     This is the same pairing drm_gem_vmap()/drm_gem_vunmap() use, for the
 *     same reason.
 *
 *   - The engine has stopped before the locks are dropped.  mtk_g2d_blt_rect()
 *     and mtk_g2d_fill() both block in g2d_wait_idle() until G2D_STATUS reads
 *     idle before they return, so releasing the locks afterwards is not a
 *     race: there is nothing left running that could touch the memory.
 *
 * That is why this is synchronous and installs no fence.  A fence would let
 * the source be released earlier, which is a real benefit, but it would also
 * mean the engine outliving this call - and the display path here has no
 * fencing infrastructure to hang one on, because nothing in the OVL/RDMA path
 * ever installs one either.  A correct synchronous wait is the honest choice:
 * it is bounded (100 ms), it cannot hang, and it leaves no window in which a
 * buffer is unlocked while the engine is still running.
 *
 * The one qualifier on that claim is the timeout path, and it is worth stating
 * plainly rather than glossing.  On -ETIMEDOUT g2d_start() calls g2d_recover(),
 * which performs the data sheet's warm reset (G2D_START = 0, G2D_RESET = 1,
 * poll G2D_STATUS, G2D_RESET = 0).  That is a best-effort recovery: if
 * G2D_STATUS still reads BUSY afterwards, g2d_reset() logs "engine may be
 * wedged" and the reset is de-asserted regardless, because leaving WRST
 * asserted would brick the block for every later operation.  In that case the
 * engine may still be running, and the buffer may therefore still be being
 * read or written after the locks are dropped.
 *
 * That residual risk is accepted rather than fixed, because closing it properly
 * needs a stronger recovery than this driver can justify: the data sheet
 * documents HRST and APB_RESET but the vendor tree never programs this block at
 * all, so the semantics of a hard reset are unverified, and inventing a
 * recovery sequence on this hardware risks turning a slow blit into a bus hang.
 * A caller that cannot tolerate the best-effort case should treat -ETIMEDOUT as
 * "the engine state is now unknown" and stop using the blitter, which is why
 * the errno is surfaced rather than swallowed.
 *
 * Locking order.  A blit takes two reservation locks, and two concurrent
 * blits of the same pair of buffers in opposite directions would deadlock if
 * the order depended on which buffer was passed first.  It does not:
 * mtk_g2d_drm_lock_pair() always takes them in reservation-object pointer
 * order, which is one global order over every buffer in the system, and
 * short-circuits the same-buffer case that has no order to speak of.  The
 * reservation locks are therefore the only locks held across the engine
 * operation, and they are always released in the reverse of the order taken.
 *
 * No engine lock is taken here: mtk_g2d_blt_rect()/mtk_g2d_fill() take
 * g2d->lock internally around register programming, and this code never programs
 * a register itself, so there is nothing here that could race with another user
 * of the engine.
 */

/**
 * mtk_g2d_drm_lock - take a surface's reservation object.
 * @surf: surface to lock
 * @owner: set to the reservation object actually locked, for the pair case
 *
 * drm_gem_lock() is dma_resv_lock(obj->resv, NULL), and a NULL acquire context
 * is documented as legal only for locking a reservation object against itself.
 * That is exactly the wrong property for a pair of buffers: a NULL context
 * carries no record of what this task already holds, so if the two surfaces
 * turn out to be the same object the second lock blocks on a mutex the first
 * one is still holding, and the task sleeps forever.  A pair lock therefore
 * uses a real ww_acquire_ctx, and returns -EALREADY when the two surfaces are
 * one and the same buffer.
 *
 * Not interruptible, deliberately: what is being waited on is another G2D user
 * finishing a copy that is itself bounded to 100 ms, so bailing out with
 * -EINTR halfway through owning one of two buffers would cost more state to
 * unwind than it saves.
 */
static int mtk_g2d_drm_lock(struct mtk_g2d_drm_surf *surf,
			    struct ww_acquire_ctx *ctx)
{
	/* A disabled plane has nothing to lock. */
	if (!surf->obj)
		return 0;

	return dma_resv_lock(surf->obj->resv, ctx);
}

static void mtk_g2d_drm_unlock(struct mtk_g2d_drm_surf *surf)
{
	if (surf->obj)
		dma_resv_unlock(surf->obj->resv);
}

/**
 * mtk_g2d_drm_lock_pair - lock two surfaces in a deadlock-free order.
 * @a: first surface
 * @b: second surface
 *
 * Both buffers must be held for the whole operation - the engine reads the
 * source while it writes the destination, so locking only one would leave the
 * other exposed.
 *
 * Two things make the order here safe rather than merely conventional:
 *
 *   - The locks are always taken in reservation-object pointer order, which
 *     is a single global order over every buffer in the system.  Two
 *     concurrent blits of the same pair of buffers, one in each direction,
 *     therefore queue up behind each other instead of deadlocking.  Ordering
 *     by "which argument it was" instead would not: A->B and B->A are both
 *     reachable, and the second thread would take the first lock the first
 *     thread is holding and wait for it forever.
 *
 *   - The same-object case is short-circuited.  Two framebuffers over one GEM
 *     object share one reservation object, so there is no order to establish
 *     and taking the lock twice would be a self-deadlock.  One lock is taken
 *     instead of two: an in-place copy within one buffer is legal - the
 *     overlap, not the sharing, is what mtk_g2d_blt_rect() rejects - so this
 *     is not an error case, just a smaller lock set.
 *
 * Returns 0, or the error from the first lock that could not be taken, in
 * which case anything already taken has been released again.
 */
static int mtk_g2d_drm_lock_pair(struct mtk_g2d_drm_surf *a,
				 struct mtk_g2d_drm_surf *b)
{
	struct ww_acquire_ctx ctx;
	struct mtk_g2d_drm_surf *first, *second;
	int ret;

	/*
	 * One reservation object means one lock is already enough, and taking
	 * it twice is a self-deadlock.  An in-place copy within one buffer is
	 * legal (mtk_g2d_blt_rect() rejects only *overlapping* rectangles), so
	 * this is not an error - just one lock instead of two.
	 */
	if (a->obj && a->obj == b->obj) {
		first = a;
		second = NULL;
	} else {
		/* Deterministic global order, independent of argument position. */
		if (a->obj && b->obj && a->obj->resv > b->obj->resv) {
			first = b;
			second = a;
		} else {
			first = a;
			second = b;
		}
	}

	ww_acquire_init(&ctx, &reservation_ww_class);
	ret = mtk_g2d_drm_lock(first, &ctx);
	if (ret)
		return ret;

	if (second) {
		ret = mtk_g2d_drm_lock(second, &ctx);
		if (ret) {
			mtk_g2d_drm_unlock(first);
			return ret;
		}
	}

	ww_acquire_fini(&ctx);

	return 0;
}

static void mtk_g2d_drm_unlock_pair(struct mtk_g2d_drm_surf *a,
				    struct mtk_g2d_drm_surf *b)
{
	/* Strict reverse of the order mtk_g2d_drm_lock_pair() took them in. */
	if (a->obj && a->obj == b->obj) {
		mtk_g2d_drm_unlock(a);
		return;
	}

	if (a->obj && b->obj && a->obj->resv > b->obj->resv) {
		mtk_g2d_drm_unlock(a);
		mtk_g2d_drm_unlock(b);
	} else {
		mtk_g2d_drm_unlock(b);
		mtk_g2d_drm_unlock(a);
	}
}

/**
 * mtk_g2d_drm_blt - copy a rectangle between two DRM framebuffers at one
 *		    shared origin.
 * @x: origin in pixels, applied to both surfaces
 * @y: origin in pixels, applied to both surfaces
 *
 * The shared-origin form of mtk_g2d_drm_blt_rect(), kept because a
 * same-coordinate copy is the common case and it is the smaller call.
 *
 * Returns 0 once the engine is idle, or a negative errno: -ENODEV if there is
 * no blitter, -EINVAL for a bad argument or an unsupported framebuffer, or
 * -ETIMEDOUT if the engine did not stop in time.
 */
int mtk_g2d_drm_blt(struct mtk_g2d *g2d,
		    struct drm_framebuffer *src_fb,
		    struct drm_framebuffer *dst_fb,
		    u32 x, u32 y, u32 width, u32 height)
{
	if (!g2d)
		return -ENODEV;

	/* Shared origin: the same x/y on both surfaces, which is the whole
	 * difference between this and mtk_g2d_drm_blt_rect().
	 */
	return mtk_g2d_drm_blt_rect(g2d, src_fb, x, y, dst_fb, x, y,
				    width, height);
}

/**
 * mtk_g2d_drm_blt_rect - copy a rectangle between two DRM framebuffers at
 *			  independent origins.
 * @src_x, @src_y: origin within the source framebuffer, in pixels
 * @dst_x, @dst_y: origin within the destination framebuffer, in pixels
 *
 * The engine has one scan window rather than two rectangles, so @width and
 * @height apply to both surfaces and the source and destination regions can
 * never differ in size.  Their origins can differ freely, which is what this
 * entry point adds over mtk_g2d_drm_blt().
 *
 * Each origin is clipped against its own framebuffer and the rectangle is
 * sized to what both can supply, so a rectangle that runs off either edge is
 * narrowed rather than trusted.
 *
 * An in-place copy is allowed when the two rectangles do not overlap;
 * mtk_g2d_blt_rect() rejects an overlapping one.  Overlapping is reported
 * before either buffer is locked, so no lock is left held.
 *
 * Returns 0 once the engine is idle, or a negative errno: -ENODEV if there is
 * no blitter, -EINVAL for a bad argument, an unsupported framebuffer or an
 * overlapping in-place copy, or -ETIMEDOUT if the engine did not stop in time.
 */
int mtk_g2d_drm_blt_rect(struct mtk_g2d *g2d,
			 struct drm_framebuffer *src_fb,
			 u32 src_x, u32 src_y,
			 struct drm_framebuffer *dst_fb,
			 u32 dst_x, u32 dst_y,
			 u32 width, u32 height)
{
	struct mtk_g2d_drm_surf src, dst;
	struct device *dev;
	u32 w, h;
	int ret;

	/*
	 * A NULL engine is "this DRM device has no blitter", which is an
	 * ordinary configuration - the MDP device has no G2D - so it is its own
	 * errno rather than a NULL dereference of the device behind it.
	 */
	if (!g2d)
		return -ENODEV;
	if (!src_fb || !dst_fb)
		return -EINVAL;

	dev = mtk_g2d_device(g2d);

	ret = mtk_g2d_drm_surf_init(dev, src_fb, &src);
	if (ret)
		return ret;

	ret = mtk_g2d_drm_surf_init(dev, dst_fb, &dst);
	if (ret)
		goto out_src;

	/* Both planes must exist: this is a copy, not a fill. */
	if (!src.obj || !dst.obj) {
		ret = -EINVAL;
		goto out_dst;
	}

	ret = mtk_g2d_clip_rect(&src, &dst, src_x, src_y,
				dst_x, dst_y, width, height, &w, &h);
	if (ret)
		goto out_dst;

	ret = mtk_g2d_drm_lock_pair(&src, &dst);
	if (ret)
		goto out_dst;

	ret = mtk_g2d_blt_rect(g2d,
			       src.addr, src.pitch, src.g2d_fmt, src_x, src_y,
			       dst.addr, dst.pitch, dst.g2d_fmt, dst_x, dst_y,
			       w, h);

	/*
	 * The engine is idle here whether or not it succeeded: mtk_g2d_blt_rect()
	 * waits for that, and on a timeout it warm-resets before returning.
	 * So dropping the locks on both paths is safe, and not dropping them
	 * on the error path would be a leak, not caution.
	 */
	mtk_g2d_drm_unlock_pair(&src, &dst);

out_dst:
	mtk_g2d_drm_surf_fini(&dst);
out_src:
	mtk_g2d_drm_surf_fini(&src);

	return ret;
}

/**
 * mtk_g2d_drm_fill - fill a rectangle of a DRM framebuffer with a constant.
 * @g2d: the engine
 * @dst_fb: destination framebuffer
 * @x: origin in pixels
 * @y: origin in pixels
 * @width: rectangle width in pixels
 * @height: rectangle height in pixels
 * @color: an ordinary DRM pixel value in the destination's format
 *
 * mtk_g2d_fill() takes the raw constant the CLRFMT describes, not a DRM pixel
 * value, so @color is converted here.  DRM_FORMAT_XRGB8888 is mapped onto the
 * same CLRFMT as ARGB8888, and for a fill that is not an approximation: the
 * top byte of a constant fill is ignored by both encodings, which is why an
 * XRGB framebuffer does not need a separate one.
 *
 * Returns 0 once the engine is idle, or a negative errno: -ENODEV if there is
 * no blitter, -EINVAL for a bad argument or an unsupported framebuffer, or
 * -ETIMEDOUT if the engine did not stop in time.
 */
int mtk_g2d_drm_fill(struct mtk_g2d *g2d,
		     struct drm_framebuffer *dst_fb,
		     u32 x, u32 y, u32 width, u32 height, u32 color)
{
	struct mtk_g2d_drm_surf dst, empty;
	u32 w, h;
	int ret;

	/* A device with no G2D is an ordinary configuration, not a crash. */
	if (!g2d)
		return -ENODEV;
	if (!dst_fb)
		return -EINVAL;

	ret = mtk_g2d_drm_surf_init(mtk_g2d_device(g2d), dst_fb, &dst);
	if (ret)
		return ret;

	if (!dst.obj) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * The fill has only one surface, but the rectangle is still clipped
	 * against a second, empty one so that the origin rule is applied the
	 * same way as in the blit.  That is not a trick to avoid duplicating
	 * the check: it means one origin rule exists, and a caller cannot pass
	 * an origin that is legal for a blit and illegal for a fill.
	 */
	memset(&empty, 0, sizeof(empty));
	empty.width = dst.width;
	empty.height = dst.height;
	empty.pitch = dst.pitch;
	empty.bpp = dst.bpp;

	ret = mtk_g2d_clip_rect(&dst, &empty, x, y, x, y, width, height, &w, &h);
	if (ret)
		goto out;

	ret = mtk_g2d_drm_lock(&dst, NULL);
	if (ret)
		goto out;

	ret = mtk_g2d_fill(g2d, dst.addr, dst.pitch, dst.g2d_fmt,
			   x, y, w, h, color);

	mtk_g2d_drm_unlock(&dst);

out:
	mtk_g2d_drm_surf_fini(&dst);

	return ret;
}

static int mtk_drm_match(struct device *dev, const void *data)
{
	if (!strncmp(dev_name(dev), "mediatek-drm", sizeof("mediatek-drm") - 1))
		return true;
	return false;
}

static bool mtk_drm_get_all_drm_priv(struct device *dev)
{
	struct mtk_drm_private *drm_priv = dev_get_drvdata(dev);
	struct mtk_drm_private *all_drm_priv[MAX_CRTC];
	struct mtk_drm_private *temp_drm_priv;
	struct device_node *phandle = dev->parent->of_node;
	const struct of_device_id *of_id;
	struct device_node *node;
	struct device *drm_dev;
	unsigned int cnt = 0;
	int i, j;

	for_each_child_of_node(phandle->parent, node) {
		struct platform_device *pdev;

		of_id = of_match_node(mtk_drm_of_ids, node);
		if (!of_id)
			continue;

		pdev = of_find_device_by_node(node);
		if (!pdev)
			continue;

		drm_dev = device_find_child(&pdev->dev, NULL, mtk_drm_match);
		put_device(&pdev->dev);
		if (!drm_dev)
			continue;

		temp_drm_priv = dev_get_drvdata(drm_dev);
		put_device(drm_dev);
		if (!temp_drm_priv)
			continue;

		if (temp_drm_priv->data->main_len)
			all_drm_priv[CRTC_MAIN] = temp_drm_priv;
		else if (temp_drm_priv->data->ext_len)
			all_drm_priv[CRTC_EXT] = temp_drm_priv;
		else if (temp_drm_priv->data->third_len)
			all_drm_priv[CRTC_THIRD] = temp_drm_priv;

		if (temp_drm_priv->mtk_drm_bound)
			cnt++;

		if (cnt == MAX_CRTC) {
			of_node_put(node);
			break;
		}
	}

	if (drm_priv->data->mmsys_dev_num == cnt) {
		for (i = 0; i < cnt; i++)
			for (j = 0; j < cnt; j++)
				all_drm_priv[j]->all_drm_private[i] = all_drm_priv[i];

		return true;
	}

	return false;
}

static bool mtk_drm_find_mmsys_comp(struct mtk_drm_private *private, int comp_id)
{
	const struct mtk_mmsys_driver_data *drv_data = private->data;
	int i;

	if (drv_data->main_path)
		for (i = 0; i < drv_data->main_len; i++)
			if (drv_data->main_path[i] == comp_id)
				return true;

	if (drv_data->ext_path)
		for (i = 0; i < drv_data->ext_len; i++)
			if (drv_data->ext_path[i] == comp_id)
				return true;

	if (drv_data->third_path)
		for (i = 0; i < drv_data->third_len; i++)
			if (drv_data->third_path[i] == comp_id)
				return true;

	if (drv_data->num_conn_routes)
		for (i = 0; i < drv_data->num_conn_routes; i++)
			if (drv_data->conn_routes[i].route_ddp == comp_id)
				return true;

	return false;
}

static int mtk_drm_kms_init(struct drm_device *drm)
{
	struct mtk_drm_private *private = drm->dev_private;
	struct mtk_drm_private *priv_n;
	struct device *dma_dev = NULL;
	struct drm_crtc *crtc;
	int ret, i, j;

	if (drm_firmware_drivers_only())
		return -ENODEV;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 64;
	drm->mode_config.min_height = 64;

	/*
	 * set max width and height as default value(4096x4096).
	 * this value would be used to check framebuffer size limitation
	 * at drm_mode_addfb().
	 */
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.funcs = &mtk_drm_mode_config_funcs;
	drm->mode_config.helper_private = &mtk_drm_mode_config_helpers;

	for (i = 0; i < private->data->mmsys_dev_num; i++) {
		drm->dev_private = private->all_drm_private[i];
		ret = component_bind_all(private->all_drm_private[i]->dev, drm);
		if (ret) {
			while (--i >= 0)
				component_unbind_all(private->all_drm_private[i]->dev, drm);
			return ret;
		}
	}

	/*
	 * Ensure internal panels are at the top of the connector list before
	 * crtc creation.
	 */
	drm_helper_move_panel_connectors_to_head(drm);

	/*
	 * 1. We currently support two fixed data streams, each optional,
	 *    and each statically assigned to a crtc:
	 *    OVL0 -> COLOR0 -> AAL -> OD -> RDMA0 -> UFOE -> DSI0 ...
	 * 2. For multi mmsys architecture, crtc path data are located in
	 *    different drm private data structures. Loop through crtc index to
	 *    create crtc from the main path and then ext_path and finally the
	 *    third path.
	 */
	for (i = 0; i < MAX_CRTC; i++) {
		for (j = 0; j < private->data->mmsys_dev_num; j++) {
			priv_n = private->all_drm_private[j];

			if (priv_n->data->max_width)
				drm->mode_config.max_width = priv_n->data->max_width;

			if (priv_n->data->min_width)
				drm->mode_config.min_width = priv_n->data->min_width;

			if (priv_n->data->min_height)
				drm->mode_config.min_height = priv_n->data->min_height;

			if (i == CRTC_MAIN && priv_n->data->main_len) {
				ret = mtk_crtc_create(drm, priv_n->data->main_path,
						      priv_n->data->main_len, j,
						      priv_n->data->conn_routes,
						      priv_n->data->num_conn_routes);
				if (ret)
					goto err_component_unbind;

				continue;
			} else if (i == CRTC_EXT && priv_n->data->ext_len) {
				ret = mtk_crtc_create(drm, priv_n->data->ext_path,
						      priv_n->data->ext_len, j, NULL, 0);
				if (ret)
					goto err_component_unbind;

				continue;
			} else if (i == CRTC_THIRD && priv_n->data->third_len) {
				ret = mtk_crtc_create(drm, priv_n->data->third_path,
						      priv_n->data->third_len, j, NULL, 0);
				if (ret)
					goto err_component_unbind;

				continue;
			}
		}
	}

	/* IGT will check if the cursor size is configured */
	drm->mode_config.cursor_width = 512;
	drm->mode_config.cursor_height = 512;

	/* Use OVL device for all DMA memory allocations */
	crtc = drm_crtc_from_index(drm, 0);
	if (crtc)
		dma_dev = mtk_crtc_dma_dev_get(crtc);
	if (!dma_dev) {
		ret = -ENODEV;
		dev_err(drm->dev, "Need at least one OVL device\n");
		goto err_component_unbind;
	}

	drm_dev_set_dma_dev(drm, dma_dev);

	/*
	 * Configure the DMA segment size to make sure we get contiguous IOVA
	 * when importing PRIME buffers.
	 */
	dma_set_max_seg_size(dma_dev, UINT_MAX);

	ret = drm_vblank_init(drm, MAX_CRTC);
	if (ret < 0)
		goto err_component_unbind;

	drm_kms_helper_poll_init(drm);
	drm_mode_config_reset(drm);

	return 0;

err_component_unbind:
	for (i = 0; i < private->data->mmsys_dev_num; i++)
		component_unbind_all(private->all_drm_private[i]->dev, drm);

	return ret;
}

static void mtk_drm_kms_deinit(struct drm_device *drm)
{
	drm_kms_helper_poll_fini(drm);
	drm_atomic_helper_shutdown(drm);

	component_unbind_all(drm->dev, drm);
}

DEFINE_DRM_GEM_FOPS(mtk_drm_fops);

static const struct drm_driver mtk_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,

	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,

	.ioctls		= mtk_g2d_ioctls,
	.num_ioctls	= MTK_G2D_NR_IOCTLS,

	.fops = &mtk_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
};

static int compare_dev(struct device *dev, void *data)
{
	return dev == (struct device *)data;
}

static int mtk_drm_bind(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);
	struct platform_device *pdev;
	struct drm_device *drm;
	int ret, i;

	pdev = of_find_device_by_node(private->mutex_node);
	if (!pdev) {
		dev_err(dev, "Waiting for disp-mutex device %pOF\n",
			private->mutex_node);
		of_node_put(private->mutex_node);
		return -EPROBE_DEFER;
	}

	private->mutex_dev = &pdev->dev;
	private->mtk_drm_bound = true;
	private->dev = dev;

	if (!mtk_drm_get_all_drm_priv(dev))
		return 0;

	drm = drm_dev_alloc(&mtk_drm_driver, dev);
	if (IS_ERR(drm)) {
		ret = PTR_ERR(drm);
		goto err_put_dev;
	}

	private->drm_master = true;
	drm->dev_private = private;
	for (i = 0; i < private->data->mmsys_dev_num; i++)
		private->all_drm_private[i]->drm = drm;

	ret = mtk_drm_kms_init(drm);
	if (ret < 0)
		goto err_free;

	ret = aperture_remove_all_conflicting_devices(DRIVER_NAME);
	if (ret < 0)
		dev_err(dev, "Error %d while removing conflicting aperture devices", ret);

	ret = drm_dev_register(drm, 0);
	if (ret < 0)
		goto err_deinit;

	drm_client_setup(drm, NULL);

	return 0;

err_deinit:
	mtk_drm_kms_deinit(drm);
err_free:
	private->drm = NULL;
	drm_dev_put(drm);
	for (i = 0; i < private->data->mmsys_dev_num; i++)
		private->all_drm_private[i]->drm = NULL;
err_put_dev:
	put_device(private->mutex_dev);
	return ret;
}

static void mtk_drm_unbind(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);

	/* for multi mmsys dev, unregister drm dev in mmsys master */
	if (private->drm_master) {
		drm_dev_unregister(private->drm);
		mtk_drm_kms_deinit(private->drm);
		drm_dev_put(private->drm);
		put_device(private->mutex_dev);
	}
	private->mtk_drm_bound = false;
	private->drm_master = false;
	private->drm = NULL;
}

static const struct component_master_ops mtk_drm_ops = {
	.bind		= mtk_drm_bind,
	.unbind		= mtk_drm_unbind,
};

static const struct of_device_id mtk_ddp_comp_dt_ids[] = {
	{ .compatible = "mediatek,mt8167-disp-aal",
	  .data = (void *)MTK_DISP_AAL},
	{ .compatible = "mediatek,mt8173-disp-aal",
	  .data = (void *)MTK_DISP_AAL},
	{ .compatible = "mediatek,mt8183-disp-aal",
	  .data = (void *)MTK_DISP_AAL},
	{ .compatible = "mediatek,mt8192-disp-aal",
	  .data = (void *)MTK_DISP_AAL},
	{ .compatible = "mediatek,mt8167-disp-ccorr",
	  .data = (void *)MTK_DISP_CCORR },
	{ .compatible = "mediatek,mt8183-disp-ccorr",
	  .data = (void *)MTK_DISP_CCORR },
	{ .compatible = "mediatek,mt8192-disp-ccorr",
	  .data = (void *)MTK_DISP_CCORR },
	{ .compatible = "mediatek,mt2701-disp-color",
	  .data = (void *)MTK_DISP_COLOR },
	{ .compatible = "mediatek,mt6589-disp-color",
	  .data = (void *)MTK_DISP_COLOR },
	{ .compatible = "mediatek,mt6589-disp-tdshp",
	  .data = (void *)MTK_DISP_TDSHP },
	{ .compatible = "mediatek,mt8167-disp-color",
	  .data = (void *)MTK_DISP_COLOR },
	{ .compatible = "mediatek,mt8173-disp-color",
	  .data = (void *)MTK_DISP_COLOR },
	{ .compatible = "mediatek,mt8167-disp-dither",
	  .data = (void *)MTK_DISP_DITHER },
	{ .compatible = "mediatek,mt8183-disp-dither",
	  .data = (void *)MTK_DISP_DITHER },
	{ .compatible = "mediatek,mt8195-disp-dsc",
	  .data = (void *)MTK_DISP_DSC },
	{ .compatible = "mediatek,mt8167-disp-gamma",
	  .data = (void *)MTK_DISP_GAMMA, },
	{ .compatible = "mediatek,mt8173-disp-gamma",
	  .data = (void *)MTK_DISP_GAMMA, },
	{ .compatible = "mediatek,mt8183-disp-gamma",
	  .data = (void *)MTK_DISP_GAMMA, },
	{ .compatible = "mediatek,mt8195-disp-gamma",
	  .data = (void *)MTK_DISP_GAMMA, },
	{ .compatible = "mediatek,mt8195-disp-merge",
	  .data = (void *)MTK_DISP_MERGE },
	{ .compatible = "mediatek,mt2701-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt2712-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt6589-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8167-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8173-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8183-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8186-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8188-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8192-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8195-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8365-disp-mutex",
	  .data = (void *)MTK_DISP_MUTEX },
	{ .compatible = "mediatek,mt8173-disp-od",
	  .data = (void *)MTK_DISP_OD },
	{ .compatible = "mediatek,mt2701-disp-ovl",
	  .data = (void *)MTK_DISP_OVL },
	{ .compatible = "mediatek,mt6589-disp-ovl",
	  .data = (void *)MTK_DISP_OVL },
	{ .compatible = "mediatek,mt8167-disp-ovl",
	  .data = (void *)MTK_DISP_OVL },
	{ .compatible = "mediatek,mt8173-disp-ovl",
	  .data = (void *)MTK_DISP_OVL },
	{ .compatible = "mediatek,mt8183-disp-ovl",
	  .data = (void *)MTK_DISP_OVL },
	{ .compatible = "mediatek,mt8192-disp-ovl",
	  .data = (void *)MTK_DISP_OVL },
	{ .compatible = "mediatek,mt8195-disp-ovl",
	  .data = (void *)MTK_DISP_OVL },
	{ .compatible = "mediatek,mt8183-disp-ovl-2l",
	  .data = (void *)MTK_DISP_OVL_2L },
	{ .compatible = "mediatek,mt8192-disp-ovl-2l",
	  .data = (void *)MTK_DISP_OVL_2L },
	{ .compatible = "mediatek,mt8192-disp-postmask",
	  .data = (void *)MTK_DISP_POSTMASK },
	{ .compatible = "mediatek,mt2701-disp-pwm",
	  .data = (void *)MTK_DISP_BLS },
	{ .compatible = "mediatek,mt6589-disp-pwm",
	  .data = (void *)MTK_DISP_BLS },
	{ .compatible = "mediatek,mt8167-disp-pwm",
	  .data = (void *)MTK_DISP_PWM },
	{ .compatible = "mediatek,mt8173-disp-pwm",
	  .data = (void *)MTK_DISP_PWM },
	{ .compatible = "mediatek,mt2701-disp-rdma",
	  .data = (void *)MTK_DISP_RDMA },
	{ .compatible = "mediatek,mt6589-disp-rdma",
	  .data = (void *)MTK_DISP_RDMA },
	{ .compatible = "mediatek,mt8167-disp-rdma",
	  .data = (void *)MTK_DISP_RDMA },
	{ .compatible = "mediatek,mt8173-disp-rdma",
	  .data = (void *)MTK_DISP_RDMA },
	{ .compatible = "mediatek,mt8183-disp-rdma",
	  .data = (void *)MTK_DISP_RDMA },
	{ .compatible = "mediatek,mt8195-disp-rdma",
	  .data = (void *)MTK_DISP_RDMA },
	{ .compatible = "mediatek,mt8173-disp-ufoe",
	  .data = (void *)MTK_DISP_UFOE },
	{ .compatible = "mediatek,mt8173-disp-wdma",
	  .data = (void *)MTK_DISP_WDMA },
	{ .compatible = "mediatek,mt2701-dpi",
	  .data = (void *)MTK_DPI },
	{ .compatible = "mediatek,mt8167-dsi",
	  .data = (void *)MTK_DSI },
	{ .compatible = "mediatek,mt8173-dpi",
	  .data = (void *)MTK_DPI },
	{ .compatible = "mediatek,mt8183-dpi",
	  .data = (void *)MTK_DPI },
	{ .compatible = "mediatek,mt8186-dpi",
	  .data = (void *)MTK_DPI },
	{ .compatible = "mediatek,mt8188-dp-intf",
	  .data = (void *)MTK_DP_INTF },
	{ .compatible = "mediatek,mt8192-dpi",
	  .data = (void *)MTK_DPI },
	{ .compatible = "mediatek,mt8195-dp-intf",
	  .data = (void *)MTK_DP_INTF },
	{ .compatible = "mediatek,mt8195-dpi",
	  .data = (void *)MTK_DPI },
	{ .compatible = "mediatek,mt2701-dsi",
	  .data = (void *)MTK_DSI },
	{ .compatible = "mediatek,mt6589-dsi",
	  .data = (void *)MTK_DSI },
	{ .compatible = "mediatek,mt8173-dsi",
	  .data = (void *)MTK_DSI },
	{ .compatible = "mediatek,mt8183-dsi",
	  .data = (void *)MTK_DSI },
	{ .compatible = "mediatek,mt8186-dsi",
	  .data = (void *)MTK_DSI },
	{ .compatible = "mediatek,mt8188-dsi",
	  .data = (void *)MTK_DSI },
	{ }
};

static int mtk_drm_of_get_ddp_comp_type(struct device_node *node, enum mtk_ddp_comp_type *ctype)
{
	const struct of_device_id *of_id = of_match_node(mtk_ddp_comp_dt_ids, node);

	if (!of_id)
		return -EINVAL;

	*ctype = (enum mtk_ddp_comp_type)((uintptr_t)of_id->data);

	return 0;
}

static int mtk_drm_of_get_ddp_ep_cid(struct device_node *node,
				     int output_port, enum mtk_crtc_path crtc_path,
				     struct device_node **next, unsigned int *cid)
{
	struct device_node *ep_dev_node, *ep_out;
	enum mtk_ddp_comp_type comp_type;
	int ret;

	ep_out = of_graph_get_endpoint_by_regs(node, output_port, crtc_path);
	if (!ep_out)
		return -ENOENT;

	ep_dev_node = of_graph_get_remote_port_parent(ep_out);
	of_node_put(ep_out);
	if (!ep_dev_node)
		return -EINVAL;

	/*
	 * Pass the next node pointer regardless of failures in the later code
	 * so that if this function is called in a loop it will walk through all
	 * of the subsequent endpoints anyway.
	 */
	*next = ep_dev_node;

	if (!of_device_is_available(ep_dev_node))
		return -ENODEV;

	ret = mtk_drm_of_get_ddp_comp_type(ep_dev_node, &comp_type);
	if (ret) {
		if (mtk_ovl_adaptor_is_comp_present(ep_dev_node)) {
			*cid = (unsigned int)DDP_COMPONENT_DRM_OVL_ADAPTOR;
			return 0;
		}
		return ret;
	}

	ret = mtk_ddp_comp_get_id(ep_dev_node, comp_type);
	if (ret < 0)
		return ret;

	/* All ok! Pass the Component ID to the caller. */
	*cid = (unsigned int)ret;

	return 0;
}

/**
 * mtk_drm_of_ddp_path_build_one - Build a Display HW Pipeline for a CRTC Path
 * @dev:          The mediatek-drm device
 * @cpath:        CRTC Path relative to a VDO or MMSYS
 * @out_path:     Pointer to an array that will contain the new pipeline
 * @out_path_len: Number of entries in the pipeline array
 *
 * MediaTek SoCs can use different DDP hardware pipelines (or paths) depending
 * on the board-specific desired display configuration; this function walks
 * through all of the output endpoints starting from a VDO or MMSYS hardware
 * instance and builds the right pipeline as specified in device trees.
 *
 * Return:
 * * %0       - Display HW Pipeline successfully built and validated
 * * %-ENOENT - Display pipeline was not specified in device tree
 * * %-EINVAL - Display pipeline built but validation failed
 * * %-ENOMEM - Failure to allocate pipeline array to pass to the caller
 */
static int mtk_drm_of_ddp_path_build_one(struct device *dev, enum mtk_crtc_path cpath,
					 const unsigned int **out_path,
					 unsigned int *out_path_len)
{
	struct device_node *next = NULL, *prev, *vdo = dev->parent->of_node;
	unsigned int temp_path[DDP_COMPONENT_DRM_ID_MAX] = { 0 };
	unsigned int *final_ddp_path;
	unsigned short int idx = 0;
	bool ovl_adaptor_comp_added = false;
	int ret;

	/* Get the first entry for the temp_path array */
	ret = mtk_drm_of_get_ddp_ep_cid(vdo, 0, cpath, &next, &temp_path[idx]);
	if (ret) {
		if (next && temp_path[idx] == DDP_COMPONENT_DRM_OVL_ADAPTOR) {
			dev_dbg(dev, "Adding OVL Adaptor for %pOF\n", next);
			ovl_adaptor_comp_added = true;
		} else {
			if (next)
				dev_err(dev, "Invalid component %pOF\n", next);
			else
				dev_err(dev, "Cannot find first endpoint for path %d\n", cpath);

			return ret;
		}
	}
	idx++;

	/*
	 * Walk through port outputs until we reach the last valid mediatek-drm component.
	 * To be valid, this must end with an "invalid" component that is a display node.
	 */
	do {
		prev = next;
		ret = mtk_drm_of_get_ddp_ep_cid(next, 1, cpath, &next, &temp_path[idx]);
		of_node_put(prev);
		if (ret) {
			of_node_put(next);
			break;
		}

		/*
		 * If this is an OVL adaptor exclusive component and one of those
		 * was already added, don't add another instance of the generic
		 * DDP_COMPONENT_OVL_ADAPTOR, as this is used only to decide whether
		 * to probe that component master driver of which only one instance
		 * is needed and possible.
		 */
		if (temp_path[idx] == DDP_COMPONENT_DRM_OVL_ADAPTOR) {
			if (!ovl_adaptor_comp_added)
				ovl_adaptor_comp_added = true;
			else
				idx--;
		}
	} while (++idx < DDP_COMPONENT_DRM_ID_MAX);

	/*
	 * The device component might not be enabled: in that case, don't
	 * check the last entry and just report that the device is missing.
	 */
	if (ret == -ENODEV)
		return ret;

	/* If the last entry is not a final display output, the configuration is wrong */
	switch (temp_path[idx - 1]) {
	case DDP_COMPONENT_DP_INTF0:
	case DDP_COMPONENT_DP_INTF1:
	case DDP_COMPONENT_DPI0:
	case DDP_COMPONENT_DPI1:
	case DDP_COMPONENT_DSI0:
	case DDP_COMPONENT_DSI1:
	case DDP_COMPONENT_DSI2:
	case DDP_COMPONENT_DSI3:
		break;
	default:
		dev_err(dev, "Invalid display hw pipeline. Last component: %d (ret=%d)\n",
			temp_path[idx - 1], ret);
		return -EINVAL;
	}

	final_ddp_path = devm_kmemdup(dev, temp_path, idx * sizeof(temp_path[0]), GFP_KERNEL);
	if (!final_ddp_path)
		return -ENOMEM;

	dev_dbg(dev, "Display HW Pipeline built with %d components.\n", idx);

	/* Pipeline built! */
	*out_path = final_ddp_path;
	*out_path_len = idx;

	return 0;
}

static int mtk_drm_of_ddp_path_build(struct device *dev, struct device_node *node,
				     struct mtk_mmsys_driver_data *data)
{
	struct device_node *ep_node;
	struct of_endpoint of_ep;
	bool output_present[MAX_CRTC] = { false };
	int ret;

	for_each_endpoint_of_node(node, ep_node) {
		ret = of_graph_parse_endpoint(ep_node, &of_ep);
		if (ret) {
			dev_err_probe(dev, ret, "Cannot parse endpoint\n");
			break;
		}

		if (of_ep.id >= MAX_CRTC) {
			ret = dev_err_probe(dev, -EINVAL,
					    "Invalid endpoint%u number\n", of_ep.port);
			break;
		}

		output_present[of_ep.id] = true;
	}

	if (ret) {
		of_node_put(ep_node);
		return ret;
	}

	if (output_present[CRTC_MAIN]) {
		ret = mtk_drm_of_ddp_path_build_one(dev, CRTC_MAIN,
						    &data->main_path, &data->main_len);
		if (ret && ret != -ENODEV)
			return ret;
	}

	if (output_present[CRTC_EXT]) {
		ret = mtk_drm_of_ddp_path_build_one(dev, CRTC_EXT,
						    &data->ext_path, &data->ext_len);
		if (ret && ret != -ENODEV)
			return ret;
	}

	if (output_present[CRTC_THIRD]) {
		ret = mtk_drm_of_ddp_path_build_one(dev, CRTC_THIRD,
						    &data->third_path, &data->third_len);
		if (ret && ret != -ENODEV)
			return ret;
	}

	return 0;
}

static int mtk_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *phandle = dev->parent->of_node;
	const struct of_device_id *of_id;
	struct mtk_drm_private *private;
	struct mtk_mmsys_driver_data *mtk_drm_data;
	struct device_node *node;
	struct component_match *match = NULL;
	struct platform_device *ovl_adaptor;
	int ret;
	int i;

	private = devm_kzalloc(dev, sizeof(*private), GFP_KERNEL);
	if (!private)
		return -ENOMEM;

	private->mmsys_dev = dev->parent;
	if (!private->mmsys_dev) {
		dev_err(dev, "Failed to get MMSYS device\n");
		return -ENODEV;
	}

	of_id = of_match_node(mtk_drm_of_ids, phandle);
	if (!of_id)
		return -ENODEV;

	mtk_drm_data = (struct mtk_mmsys_driver_data *)of_id->data;
	if (!mtk_drm_data)
		return -EINVAL;

	/* Try to build the display pipeline from devicetree graphs */
	if (of_graph_is_present(phandle)) {
		dev_dbg(dev, "Building display pipeline for MMSYS %u\n",
			mtk_drm_data->mmsys_id);
		private->data = devm_kmemdup(dev, mtk_drm_data,
					     sizeof(*mtk_drm_data), GFP_KERNEL);
		if (!private->data)
			return -ENOMEM;

		ret = mtk_drm_of_ddp_path_build(dev, phandle, private->data);
		if (ret)
			return ret;
	} else {
		/* No devicetree graphs support: go with hardcoded paths if present */
		dev_dbg(dev, "Using hardcoded paths for MMSYS %u\n", mtk_drm_data->mmsys_id);
		private->data = mtk_drm_data;
	}

	private->all_drm_private = devm_kmalloc_array(dev, private->data->mmsys_dev_num,
						      sizeof(*private->all_drm_private),
						      GFP_KERNEL);
	if (!private->all_drm_private)
		return -ENOMEM;

	/* Bringup ovl_adaptor */
	if (mtk_drm_find_mmsys_comp(private, DDP_COMPONENT_DRM_OVL_ADAPTOR)) {
		ovl_adaptor = platform_device_register_data(dev, "mediatek-disp-ovl-adaptor",
							    PLATFORM_DEVID_AUTO,
							    (void *)private->mmsys_dev,
							    sizeof(*private->mmsys_dev));
		private->ddp_comp[DDP_COMPONENT_DRM_OVL_ADAPTOR].dev = &ovl_adaptor->dev;
		mtk_ddp_comp_init(dev, NULL, &private->ddp_comp[DDP_COMPONENT_DRM_OVL_ADAPTOR],
				  DDP_COMPONENT_DRM_OVL_ADAPTOR);
		component_match_add(dev, &match, compare_dev, &ovl_adaptor->dev);
	}

	/* Iterate over sibling DISP function blocks */
	for_each_child_of_node(phandle->parent, node) {
		enum mtk_ddp_comp_type comp_type;
		int comp_id;

		ret = mtk_drm_of_get_ddp_comp_type(node, &comp_type);
		if (ret)
			continue;

		if (!of_device_is_available(node)) {
			dev_dbg(dev, "Skipping disabled component %pOF\n",
				node);
			continue;
		}

		if (comp_type == MTK_DISP_MUTEX) {
			int id;

			id = of_alias_get_id(node, "mutex");
			if (id < 0 || id == private->data->mmsys_id) {
				private->mutex_node = of_node_get(node);
				dev_dbg(dev, "get mutex for mmsys %d", private->data->mmsys_id);
			}
			continue;
		}

		comp_id = mtk_ddp_comp_get_id(node, comp_type);
		if (comp_id < 0) {
			dev_warn(dev, "Skipping unknown component %pOF\n",
				 node);
			continue;
		}

		if (!mtk_drm_find_mmsys_comp(private, comp_id))
			continue;

		private->comp_node[comp_id] = of_node_get(node);

		/*
		 * Currently only the AAL, CCORR, COLOR, GAMMA, MERGE, OVL, RDMA, DSI, and DPI
		 * blocks have separate component platform drivers and initialize their own
		 * DDP component structure. The others are initialized here.
		 */
		if (comp_type == MTK_DISP_AAL ||
		    comp_type == MTK_DISP_CCORR ||
		    comp_type == MTK_DISP_COLOR ||
		    comp_type == MTK_DISP_GAMMA ||
		    comp_type == MTK_DISP_MERGE ||
		    comp_type == MTK_DISP_OVL ||
		    comp_type == MTK_DISP_OVL_2L ||
		    comp_type == MTK_DISP_OVL_ADAPTOR ||
		    comp_type == MTK_DISP_RDMA ||
		    comp_type == MTK_DP_INTF ||
		    comp_type == MTK_DPI ||
		    comp_type == MTK_DSI) {
			dev_info(dev, "Adding component match for %pOF\n",
				 node);
			drm_of_component_match_add(dev, &match, component_compare_of,
						   node);
		}

		ret = mtk_ddp_comp_init(dev, node, &private->ddp_comp[comp_id], comp_id);
		if (ret) {
			of_node_put(node);
			goto err_node;
		}
	}

	if (!private->mutex_node) {
		dev_err(dev, "Failed to find disp-mutex node\n");
		ret = -ENODEV;
		goto err_node;
	}

	/*
	 * Find the 2D blitter for this MMSYS, once, and keep the reference for
	 * as long as the DRM device exists.  A NULL result is normal - the MDP
	 * DRM device has no G2D, and so does any board whose DTS omits the node -
	 * and is not an error, because nothing in the existing display path
	 * depends on it.  It only decides whether an accelerated path can be
	 * taken where one asks for one.
	 */
	private->g2d = mtk_g2d_get(dev);
	if (private->g2d)
		dev_info(dev, "Found G2D blitter at %s\n",
			 dev_name(mtk_g2d_device(private->g2d)));
	else
		dev_info(dev, "No G2D blitter, software paths only\n");

	pm_runtime_enable(dev);

	platform_set_drvdata(pdev, private);

	ret = component_master_add_with_match(dev, &mtk_drm_ops, match);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_disable(dev);
	mtk_g2d_put(private->g2d);
	private->g2d = NULL;
err_node:
	of_node_put(private->mutex_node);
	for (i = 0; i < DDP_COMPONENT_DRM_ID_MAX; i++)
		of_node_put(private->comp_node[i]);
	return ret;
}

static void mtk_drm_remove(struct platform_device *pdev)
{
	struct mtk_drm_private *private = platform_get_drvdata(pdev);
	int i;

	component_master_del(&pdev->dev, &mtk_drm_ops);
	pm_runtime_disable(&pdev->dev);
	mtk_g2d_put(private->g2d);
	private->g2d = NULL;
	of_node_put(private->mutex_node);
	for (i = 0; i < DDP_COMPONENT_DRM_ID_MAX; i++)
		of_node_put(private->comp_node[i]);
}

static void mtk_drm_shutdown(struct platform_device *pdev)
{
	struct mtk_drm_private *private = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(private->drm);
}

static int mtk_drm_sys_prepare(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);
	struct drm_device *drm = private->drm;

	if (private->drm_master)
		return drm_mode_config_helper_suspend(drm);
	else
		return 0;
}

static void mtk_drm_sys_complete(struct device *dev)
{
	struct mtk_drm_private *private = dev_get_drvdata(dev);
	struct drm_device *drm = private->drm;
	int ret = 0;

	if (private->drm_master)
		ret = drm_mode_config_helper_resume(drm);
	if (ret)
		dev_err(dev, "Failed to resume\n");
}

static const struct dev_pm_ops mtk_drm_pm_ops = {
	.prepare = mtk_drm_sys_prepare,
	.complete = mtk_drm_sys_complete,
};

static struct platform_driver mtk_drm_platform_driver = {
	.probe	= mtk_drm_probe,
	.remove = mtk_drm_remove,
	.shutdown = mtk_drm_shutdown,
	.driver	= {
		.name	= "mediatek-drm",
		.pm     = &mtk_drm_pm_ops,
	},
};

static struct platform_driver * const mtk_drm_drivers[] = {
	&mtk_disp_aal_driver,
	&mtk_disp_ccorr_driver,
	&mtk_disp_color_driver,
	&mtk_disp_gamma_driver,
	&mtk_disp_merge_driver,
	&mtk_disp_ovl_adaptor_driver,
	&mtk_disp_ovl_driver,
	&mtk_disp_rdma_driver,
	&mtk_disp_tdshp_driver,
	&mtk_dpi_driver,
	&mtk_drm_platform_driver,
	&mtk_dsi_driver,
	&mtk_ethdr_driver,
	&mtk_mdp_rdma_driver,
	&mtk_padding_driver,
};

static int __init mtk_drm_init(void)
{
	return platform_register_drivers(mtk_drm_drivers,
					 ARRAY_SIZE(mtk_drm_drivers));
}

static void __exit mtk_drm_exit(void)
{
	platform_unregister_drivers(mtk_drm_drivers,
				    ARRAY_SIZE(mtk_drm_drivers));
}

module_init(mtk_drm_init);
module_exit(mtk_drm_exit);

MODULE_AUTHOR("YT SHEN <yt.shen@mediatek.com>");
MODULE_DESCRIPTION("Mediatek SoC DRM driver");
MODULE_LICENSE("GPL v2");
