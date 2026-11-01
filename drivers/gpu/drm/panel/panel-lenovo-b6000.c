// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lenovo YOGA Tablet 8 (B6000) DSI panel driver.
 *
 * The B6000 ships with one of two panels, chosen at build time by the vendor
 * and distinguished at runtime by a board ID strap:
 *
 *   nt35521_cmi_080          Novatek NT35521, 800x1280
 *   s6d7aa0x04_cpt_080       Samsung S6D7AA0, 800x1280
 *
 * Neither is a "simple panel": both need a long manufacturer-command init
 * sequence before they will display anything, which is why they cannot live
 * in panel-simple.c.
 *
 * SOURCE OF THE INIT SEQUENCES
 *
 * Both sequences are decoded from the vendor kernel's own LCM drivers in
 * kernel_blade8_row_3gdata-jb.elf (decompiled in Ghidra):
 *
 *   nt35521_cmi_080     lcm_init_2()            @ c03a0f98
 *                       lcm_compare_id_2()      @ c03a25a8
 *                       lcm_get_params_2()      @ c03a0e10
 *   s6d7aa0x04_cpt_080  lcm_init_3()            @ c03a2f64
 *                       init_cptlcm_registers() @ c03a26b0
 *
 * The vendor pushes commands with dsi_set_cmdq(), which takes an array of
 * 32-bit words rather than a command/data pair.  The packing was recovered
 * from DSI_set_cmdq_V2 @ c04193fc in the same image and then validated by
 * round-tripping a real vendor table row; it is byte-oriented:
 *
 *     bytes : 02 39 <len+1> 00 <cmd> <d0> <d1> ...     long write
 *             02 15 <cmd> <d0> 00 00 00 00             short write, 1 param
 *             02 05 <cmd> 00                            short write, no param
 *
 * so the extracted commands below are ordinary DCS writes and are issued
 * here through mipi_dsi_dcs_write().
 *
 * CROSS-CHECK
 *
 * The NT35521 sequence was independently confirmed against the public
 * nt35521 driver in DoomFlex/Kernel_6737m
 * (drivers/misc/mediatek/lcm/nt35521_cmi55_rixin_hd): its page-unlock
 * entries {0xF0,5,{0x55,0xAA,0x52,0x08,0x00}} match what this decoder
 * produces byte for byte.  The S6D7AA0 register payloads likewise match
 * carlitros900/CUBE-kernel-U1005 (c805_hx_s6d7aa0x04_cpt_wxga_ips_8), which
 * carries the same dsi_set_cmdq word stream.
 *
 * That CUBE driver is a *different* 800x1280 s6d7aa0x04 variant with
 * different timings (four lanes, PLL 200, vsync 5/back 3/front 8), so only
 * its register payloads were used; the timings below come from this board's
 * own lcm_get_params_2()/lcm_get_params_3().
 *
 * WHAT IS NOT KNOWN
 *
 * The vendor's lcd_power_en()/lcd_reset() helpers are static and were not
 * recovered, so the regulator/GPIO ordering is modelled on the Devicetree
 * (power-supply + reset-gpios) rather than copied.  The panel ID strap is
 * GPIO 0x9a (154) read as an input: lcm_compare_id_2() returns true when it
 * reads 1.  That GPIO is optional here - if the board does not describe it,
 * the panel named by the compatible string is used as-is.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct b6000_panel_desc {
	const char *name;
	const struct b6000_init_cmd *init;
	unsigned int init_len;
	const struct drm_display_mode mode;
	unsigned int lanes;
	unsigned long mode_flags;
	const struct mipi_dsi_device_info *dsi;
};

struct b6000_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct b6000_panel_desc *desc;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
	bool prepared;
};

static inline struct b6000_panel *to_b6000(struct drm_panel *panel)
{
	return container_of(panel, struct b6000_panel, panel);
}

#include "panel-lenovo-b6000-seq.h"

/* ---- panel descriptions ---------------------------------------------- */

/*
 * Timings from lcm_get_params_2() (this board's CMI/NT35521 panel):
 *   BURST_VDO_MODE, THREE lanes, RGB888
 *   hsync/back/front 4 / 150 / 150, vsync/back/front 4 / 12 / 10
 *   width/height 800 / 1280, PLL_CLOCK LCM_DSI_6589_PLL_CLOCK_364
 * pixel clock = htotal * vtotal * 60 = 1104 * 1306 * 60 = 86.51 MHz
 */
static const struct drm_display_mode b6000_cmi_mode = {
	.clock		= 86509,
	.hdisplay	= 800,
	.hsync_start	= 800 + 150,
	.hsync_end	= 800 + 150 + 4,
	.htotal		= 800 + 150 + 4 + 150,
	.vdisplay	= 1280,
	.vsync_start	= 1280 + 10,
	.vsync_end	= 1280 + 10 + 4,
	.vtotal		= 1280 + 10 + 4 + 12,
};

/*
 * The CPT panel is NOT the same part as the CMI one and does not share its
 * timing.  From lcm_get_params_3() @ c03a2ddc in
 * kernel_blade8_row_3gdata-jb.elf:
 *
 *	LANE_NUM           LCM_FOUR_LANE      (CMI is three!)
 *	mode               BURST_VDO_MODE
 *	width/height       800 / 1280
 *	hsync/back/front   5 / 59 / 16
 *	vsync/back/front   5 / 3 / 8
 *	PLL_CLOCK          LCM_DSI_6589_PLL_CLOCK_227_5
 *
 * so 880 x 1296 at 60 Hz gives 68.4 MHz.  Running it with the CMI timings,
 * as an earlier revision of this driver did, would drive four lanes with a
 * three-lane pixel clock.
 */
static const struct drm_display_mode b6000_cpt_mode = {
	.clock		= 68429, /* 880 * 1296 * 60 / 1000 kHz */
	.hdisplay	= 800,
	.hsync_start	= 800 + 16, /* + HFP */
	.hsync_end	= 800 + 16 + 5, /* + HSW */
	.htotal		= 800 + 16 + 5 + 59, /* + HBP */
	.vdisplay	= 1280,
	.vsync_start	= 1280 + 8, /* + VFP */
	.vsync_end	= 1280 + 8 + 5, /* + VSW */
	.vtotal		= 1280 + 8 + 5 + 3, /* + VBP */
};

static const struct b6000_panel_desc b6000_cmi_desc = {
	.name		= "nt35521_cmi_080",
	.init		= b6000_cmi_init,
	.init_len	= ARRAY_SIZE(b6000_cmi_init),
	.mode		= b6000_cmi_mode,
	.lanes		= 3,
	.mode_flags	= MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST,
};

static const struct b6000_panel_desc b6000_cpt_desc = {
	.name		= "s6d7aa0x04_cpt_080",
	.init		= b6000_cpt_init,
	.init_len	= ARRAY_SIZE(b6000_cpt_init),
	.mode		= b6000_cpt_mode,
	.lanes		= 4,	/* LCM_FOUR_LANE, unlike the three-lane CMI part */
	.mode_flags	= MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST,
};

/* ---- power / sequencing ---------------------------------------------- */

static int b6000_power_on(struct b6000_panel *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	if (ctx->enable_gpio) {
		gpiod_set_value_cansleep(ctx->enable_gpio, 1);
		usleep_range(10000, 15000);
	}

	if (ctx->reset_gpio) {
		/* Vendor: assert, wait, release, wait (lcm_init_2). */
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		usleep_range(10000, 15000);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		usleep_range(10000, 15000);
	}

	return 0;
}

static void b6000_power_off(struct b6000_panel *ctx)
{
	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		usleep_range(10000, 15000);
	}

	if (ctx->enable_gpio)
		gpiod_set_value_cansleep(ctx->enable_gpio, 0);

	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
}

static int b6000_send_init(struct b6000_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	const struct b6000_init_cmd *c;
	unsigned int i;
	int ret;

	for (i = 0; i < ctx->desc->init_len; i++) {
		c = &ctx->desc->init[i];

		if (c->delay_ms) {
			msleep(c->delay_ms);
			continue;
		}

		if (c->len)
			ret = mipi_dsi_dcs_write(dsi, c->cmd, c->data, c->len);
		else
			ret = mipi_dsi_dcs_write(dsi, c->cmd, NULL, 0);
		if (ret < 0) {
			dev_err(&dsi->dev,
				"init command 0x%02x failed: %d\n", c->cmd, ret);
			return ret;
		}
	}

	return 0;
}

static int b6000_prepare(struct drm_panel *panel)
{
	struct b6000_panel *ctx = to_b6000(panel);
	int ret;

	if (ctx->prepared)
		return 0;

	ret = b6000_power_on(ctx);
	if (ret)
		return ret;

	/*
	 * Wake the panel before sending manufacturer commands.
	 *
	 * The vendor's lcm_init_2()/lcm_init_3() call lcd_power_en() and
	 * lcd_reset() and then go straight into the page-unlock sequence
	 * without an explicit 0x11, which means the vendor DSI framework
	 * issues exit-sleep itself around lcm_init().  Here that framing is
	 * ours, so it goes before the sequence.
	 */
	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "failed to exit sleep: %d\n", ret);
		goto err_poweroff;
	}
	msleep(120);

	ret = b6000_send_init(ctx);
	if (ret)
		goto err_poweroff;

	ctx->prepared = true;

	return 0;

err_poweroff:
	b6000_power_off(ctx);
	return ret;
}

static int b6000_enable(struct drm_panel *panel)
{
	struct b6000_panel *ctx = to_b6000(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "failed to set display on: %d\n", ret);
		return ret;
	}
	msleep(20);

	return 0;
}

static int b6000_disable(struct drm_panel *panel)
{
	struct b6000_panel *ctx = to_b6000(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "failed to set display off: %d\n", ret);

	return 0;
}

static int b6000_unprepare(struct drm_panel *panel)
{
	struct b6000_panel *ctx = to_b6000(panel);
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "failed to enter sleep: %d\n", ret);

	b6000_power_off(ctx);
	ctx->prepared = false;

	return 0;
}

static int b6000_get_modes(struct drm_panel *panel,
			   struct drm_connector *connector)
{
	struct b6000_panel *ctx = to_b6000(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ctx->desc->mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs b6000_panel_funcs = {
	.prepare	= b6000_prepare,
	.enable		= b6000_enable,
	.disable	= b6000_disable,
	.unprepare	= b6000_unprepare,
	.get_modes	= b6000_get_modes,
};

/* ---- probe ------------------------------------------------------------ */

static int b6000_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct b6000_panel *ctx;
	const struct b6000_panel_desc *desc;
	int ret;

	desc = of_device_get_match_data(dev);
	if (!desc)
		return -ENODEV;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	ctx->desc = desc;

	ctx->supplies[0].supply = "vdd";
	ctx->supplies[1].supply = "vddi";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/*
	 * lcm_init_2()/lcm_init_3() drive a reset line; polarity is the
	 * usual active-low, so gpiod is asked for it already deasserted.
	 */
	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						   GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "failed to get enable GPIO\n");

	drm_panel_init(&ctx->panel, dev, &b6000_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	dsi->lanes = desc->lanes;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = desc->mode_flags;

	mipi_dsi_set_drvdata(dsi, ctx);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach\n");
	}

	dev_info(dev, "%s panel\n", desc->name);

	return 0;
}

static void b6000_remove(struct mipi_dsi_device *dsi)
{
	struct b6000_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id b6000_of_match[] = {
	{ .compatible = "lenovo,b6000-nt35521-cmi",
	  .data = &b6000_cmi_desc },
	{ .compatible = "lenovo,b6000-s6d7aa0x04-cpt",
	  .data = &b6000_cpt_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, b6000_of_match);

static struct mipi_dsi_driver b6000_driver = {
	.probe		= b6000_probe,
	.remove		= b6000_remove,
	.driver = {
		.name		= "panel-lenovo-b6000",
		.of_match_table	= b6000_of_match,
	},
};
module_mipi_dsi_driver(b6000_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("Lenovo YOGA Tablet 8 (B6000) DSI panel");
MODULE_LICENSE("GPL");
