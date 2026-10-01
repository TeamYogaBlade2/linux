// SPDX-License-Identifier: GPL-2.0-only
/*
 * PowerVR SGX — standalone DRM/KMS display path
 *
 * Provides a normal DRM device (GEM DMA buffers, simple display pipe:
 * plane + CRTC + encoder + connector) owned by the SGX platform device.
 *
 * This intentionally does NOT depend on:
 *  - CONFIG_DRM_MEDIATEK / mtk_drm
 *  - simple-framebuffer / pre-initialized firmware framebuffers
 *
 * Scanout: on plane update, program MT6589 OVL layer-0 address with the
 * GEM buffer DMA address when the OVL MMIO region is available. Full
 * display-pipeline modeset (clocks, DSI, mutex) remains outside this
 * driver; if the pipeline is already running, page-flips become visible.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_module.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "pvr_drm_display.h"

#define DRIVER_NAME		"pvrsgx"
#define DRIVER_DESC		"PowerVR SGX DRM display"
#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0

/* Default panel mode for Lenovo B8000 / Blade family */
#define PVR_DRM_DEF_WIDTH	1280
#define PVR_DRM_DEF_HEIGHT	800
#define PVR_DRM_DEF_REFRESH	60

/*
 * MT6589 OVL (phys 0x14003000). Layer-0 source address on early MTK OVL
 * matches the MT2701 layout (offset 0x40). Adjustable via DT.
 */
#define PVR_OVL_DEFAULT_PHYS	0x14003000
#define PVR_OVL_DEFAULT_SIZE	0x1000
#define PVR_OVL_L0_ADDR_OFF	0x0040

struct pvr_drm_priv {
	struct drm_device		drm;
	struct drm_simple_display_pipe	pipe;
	struct drm_connector		connector;
	void __iomem			*ovl_regs;
	u32				ovl_addr_offset;
	unsigned int			width;
	unsigned int			height;
	unsigned int			refresh;
	bool				registered;
};

/* Avoid platform_set_drvdata — SGX probe owns the platform device */
static struct pvr_drm_priv *pvr_drm_priv;

static const uint32_t pvr_drm_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGB565,
};

static inline struct pvr_drm_priv *drm_to_pvr(struct drm_device *drm)
{
	return container_of(drm, struct pvr_drm_priv, drm);
}

static inline struct pvr_drm_priv *pipe_to_pvr(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct pvr_drm_priv, pipe);
}

static void pvr_drm_scanout(struct pvr_drm_priv *priv, dma_addr_t addr)
{
	if (!priv->ovl_regs)
		return;

	writel(lower_32_bits(addr), priv->ovl_regs + priv->ovl_addr_offset);
}

static int pvr_drm_connector_get_modes(struct drm_connector *connector)
{
	struct pvr_drm_priv *priv = container_of(connector, struct pvr_drm_priv,
						 connector);
	struct drm_display_mode *mode;
	int count;

	count = drm_add_modes_noedid(connector, priv->width, priv->height);

	mode = drm_cvt_mode(connector->dev, priv->width, priv->height,
			    priv->refresh, false, false, false);
	if (mode) {
		mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	} else {
		drm_set_preferred_mode(connector, priv->width, priv->height);
	}

	return count;
}

static const struct drm_connector_helper_funcs pvr_drm_connector_helper_funcs = {
	.get_modes = pvr_drm_connector_get_modes,
};

static const struct drm_connector_funcs pvr_drm_connector_funcs = {
	.reset			= drm_atomic_helper_connector_reset,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static enum drm_mode_status pvr_drm_mode_valid(struct drm_simple_display_pipe *pipe,
					       const struct drm_display_mode *mode)
{
	struct pvr_drm_priv *priv = pipe_to_pvr(pipe);

	if (mode->hdisplay > priv->width || mode->vdisplay > priv->height)
		return MODE_BAD;

	return MODE_OK;
}

static void pvr_drm_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	struct pvr_drm_priv *priv = pipe_to_pvr(pipe);
	struct drm_gem_dma_object *gem;

	if (!plane_state || !plane_state->fb)
		return;

	gem = drm_fb_dma_get_gem_obj(plane_state->fb, 0);
	if (gem)
		pvr_drm_scanout(priv, gem->dma_addr);

}

static void pvr_drm_disable(struct drm_simple_display_pipe *pipe)
{
}

static void pvr_drm_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct pvr_drm_priv *priv = pipe_to_pvr(pipe);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_gem_dma_object *gem;

	if (!state || !state->fb)
		return;

	gem = drm_fb_dma_get_gem_obj(state->fb, 0);
	if (!gem)
		return;

	pvr_drm_scanout(priv, gem->dma_addr);

}

static int pvr_drm_prepare_fb(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *plane_state)
{
	return drm_gem_plane_helper_prepare_fb(&pipe->plane, plane_state);
}

static const struct drm_simple_display_pipe_funcs pvr_drm_pipe_funcs = {
	.mode_valid	= pvr_drm_mode_valid,
	.enable		= pvr_drm_enable,
	.disable	= pvr_drm_disable,
	.update		= pvr_drm_update,
	.prepare_fb	= pvr_drm_prepare_fb,
};

static const struct drm_mode_config_funcs pvr_drm_mode_config_funcs = {
	.fb_create	= drm_gem_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(pvr_drm_fops);

static const struct drm_driver pvr_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &pvr_drm_fops,
	.name			= DRIVER_NAME,
	.desc			= DRIVER_DESC,
	.major			= DRIVER_MAJOR,
	.minor			= DRIVER_MINOR,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
};

static void pvr_drm_read_mode(struct platform_device *pdev, struct pvr_drm_priv *priv)
{
	struct device_node *np = pdev->dev.of_node;
	u32 val;

	priv->width = PVR_DRM_DEF_WIDTH;
	priv->height = PVR_DRM_DEF_HEIGHT;
	priv->refresh = PVR_DRM_DEF_REFRESH;

	if (!np)
		return;

	if (!of_property_read_u32(np, "pvr,output-width", &val) && val)
		priv->width = val;
	if (!of_property_read_u32(np, "pvr,output-height", &val) && val)
		priv->height = val;
	if (!of_property_read_u32(np, "pvr,output-refresh", &val) && val)
		priv->refresh = val;
}

static int pvr_drm_map_ovl(struct platform_device *pdev, struct pvr_drm_priv *priv)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *ovl_np;
	struct resource res;
	resource_size_t size;
	u32 offset = PVR_OVL_L0_ADDR_OFF;
	int ret;

	priv->ovl_addr_offset = offset;

	if (np && !of_property_read_u32(np, "pvr,ovl-addr-offset", &offset))
		priv->ovl_addr_offset = offset;

	/* Prefer a phandle to the real OVL node when present in DT */
	ovl_np = of_parse_phandle(np, "pvr,ovl", 0);
	if (ovl_np) {
		ret = of_address_to_resource(ovl_np, 0, &res);
		of_node_put(ovl_np);
		if (ret) {
			dev_warn(&pdev->dev, "pvr,ovl: bad address (%d)\n", ret);
			return 0;
		}
	} else {
		/* Fallback: fixed MT6589 OVL physical address */
		res.start = PVR_OVL_DEFAULT_PHYS;
		res.end = PVR_OVL_DEFAULT_PHYS + PVR_OVL_DEFAULT_SIZE - 1;
		res.flags = IORESOURCE_MEM;
	}

	size = resource_size(&res);
	/* Non-exclusive map: OVL may be shared with a future display driver */
	priv->ovl_regs = ioremap(res.start, size);
	if (!priv->ovl_regs) {
		dev_warn(&pdev->dev,
			 "OVL ioremap failed — KMS will run without HW scanout\n");
		return 0;
	}

	dev_info(&pdev->dev, "scanout via OVL %pR addr_off=0x%x\n",
		 &res, priv->ovl_addr_offset);
	return 0;
}

int pvr_drm_display_init(struct platform_device *pdev)
{
	struct pvr_drm_priv *priv;
	struct drm_device *drm;
	int ret;

	if (!IS_ENABLED(CONFIG_DRM))
		return 0;

	/* Contiguous scanout buffers for OVL */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(&pdev->dev, "dma_set_mask failed: %d\n", ret);

	priv = devm_drm_dev_alloc(&pdev->dev, &pvr_drm_driver,
				  struct pvr_drm_priv, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	drm = &priv->drm;
	pvr_drm_read_mode(pdev, priv);
	pvr_drm_map_ovl(pdev, priv);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 64;
	drm->mode_config.min_height = 64;
	drm->mode_config.max_width = priv->width;
	drm->mode_config.max_height = priv->height;
	drm->mode_config.preferred_depth = 32;
	drm->mode_config.funcs = &pvr_drm_mode_config_funcs;

	ret = drm_connector_init(drm, &priv->connector,
				 &pvr_drm_connector_funcs,
				 DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;

	drm_connector_helper_add(&priv->connector, &pvr_drm_connector_helper_funcs);

	ret = drm_simple_display_pipe_init(drm, &priv->pipe, &pvr_drm_pipe_funcs,
					   pvr_drm_formats,
					   ARRAY_SIZE(pvr_drm_formats),
					   NULL, &priv->connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		pvr_drm_priv = NULL;
		return ret;
	}
	priv->registered = true;
	pvr_drm_priv = priv;

	drm_client_setup_with_fourcc(drm, DRM_FORMAT_XRGB8888);

	dev_info(&pdev->dev,
		 "DRM display registered (%ux%u@%u) card%u%s\n",
		 priv->width, priv->height, priv->refresh,
		 drm->primary->index,
		 priv->ovl_regs ? "" : " [no OVL map]");

	return 0;
}

void pvr_drm_display_fini(struct platform_device *pdev)
{
	struct pvr_drm_priv *priv = pvr_drm_priv;

	if (!priv || !IS_ENABLED(CONFIG_DRM))
		return;

	if (priv->registered) {
		drm_dev_unregister(&priv->drm);
		drm_atomic_helper_shutdown(&priv->drm);
		priv->registered = false;
	}

	if (priv->ovl_regs) {
		iounmap(priv->ovl_regs);
		priv->ovl_regs = NULL;
	}

	pvr_drm_priv = NULL;
}
