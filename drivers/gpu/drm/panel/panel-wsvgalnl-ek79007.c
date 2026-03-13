// SPDX-License-Identifier: GPL-2.0+

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/display_timing.h>
#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct ek79007 {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *standby_gpio;
	struct regulator *vcc;
	enum drm_panel_orientation orientation;
};

static inline struct ek79007 *panel_to_ek79007(struct drm_panel *panel)
{
	return container_of(panel, struct ek79007, panel);
}

static void ek79007_init_sequence(struct mipi_dsi_multi_context *dsi_ctx)
{
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0x10);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x80, 0xbb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x81, 0xbb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x82, 0xbb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x83, 0xbb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x84, 0xbb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x85, 0xbb);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x86, 0xbb);
}

static int ek79007_unprepare(struct drm_panel *panel)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	regulator_disable(ctx->vcc);

	return 0;
}

static int ek79007_prepare(struct drm_panel *panel)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);

	dsi_ctx.accum_err = regulator_enable(ctx->vcc);
	if (dsi_ctx.accum_err) {
		dev_err(ctx->dev, "Failed to enable regulator: %d\n",
			dsi_ctx.accum_err);
		return dsi_ctx.accum_err;
	}
	msleep(100);

	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	gpiod_set_value_cansleep(ctx->standby_gpio, 1);
	msleep(30);

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	gpiod_set_value_cansleep(ctx->standby_gpio, 1);
	msleep(30);

	ek79007_init_sequence(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);

	if (dsi_ctx.accum_err)
		goto disable_vcc;

	return 0;

disable_vcc:
	regulator_disable(ctx->vcc);
	return dsi_ctx.accum_err;
}

static const struct drm_display_mode ek79007_mode = {
	.clock = 47000, //43207,
	.hdisplay = 1024,
	.hsync_start = 1024 + 60,
	.hsync_end = 1024 + 60 + 10,
	.htotal = 1024 + 60 + 10 + 60,
	.vdisplay = 600,
	.vsync_start = 600 + 10,
	.vsync_end = 600 + 10 + 50,
	.vtotal = 600 + 10 + 10 + 50,
	.width_mm = 154,
	.height_mm = 90,
};

static int ek79007_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ek79007_mode);
	if (!mode) {
		dev_err(ctx->dev, "Failed to add mode %ux%u@%u\n",
			ek79007_mode.hdisplay, ek79007_mode.vdisplay,
			drm_mode_vrefresh(&ek79007_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static enum drm_panel_orientation
ek79007_get_orientation(struct drm_panel *panel)
{
	struct ek79007 *ctx = panel_to_ek79007(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs ek79007_funcs = {
	.unprepare = ek79007_unprepare,
	.prepare = ek79007_prepare,
	.get_modes = ek79007_get_modes,
	.get_orientation = ek79007_get_orientation,
};

static int ek79007_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ek79007 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct ek79007, panel, &ek79007_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->enable_gpio =
		devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio)) {
		dev_err(dev, "cannot get enable gpio\n");
		return PTR_ERR(ctx->enable_gpio);
	}

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset gpio\n");
		return PTR_ERR(ctx->reset_gpio);
	}

	ctx->standby_gpio =
		devm_gpiod_get_optional(dev, "standby", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->standby_gpio)) {
		dev_err(dev, "cannot get standby gpio\n");
		return PTR_ERR(ctx->standby_gpio);
	}

	ctx->vcc = devm_regulator_get(dev, "vcc");
	if (IS_ERR(ctx->vcc)) {
		ret = PTR_ERR(ctx->vcc);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to request vcc regulator: %d\n",
				ret);
		return ret;
	}

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0) {
		dev_err(dev, "%pOF: failed to get orientation %d\n",
			dev->of_node, ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
			  MIPI_DSI_MODE_LPM;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void ek79007_remove(struct mipi_dsi_device *dsi)
{
	struct ek79007 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ek79007_of_match[] = {
	{ .compatible = "wsvgalnl,ek79007" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ek79007_of_match);

static struct mipi_dsi_driver ek79007_driver = {
	.driver = {
		.name = "panel-wsvgalnl-ek79007",
		.of_match_table = ek79007_of_match,
	},
	.probe	= ek79007_probe,
	.remove = ek79007_remove,
};
module_mipi_dsi_driver(ek79007_driver);

MODULE_DESCRIPTION("DRM driver for wsvgalnl ek79007 MIPI DSI panel");
MODULE_LICENSE("GPL");
