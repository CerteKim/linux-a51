// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree.
 * Copyright (c) 2024 Luca Weiss <luca.weiss@fairphone.com>
 * Copyright (c) 2024 bigsaltyfishes <bigsaltyfishes@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

/* Manufacturer specific DSI commands */
#define HX83121A_SETDISP 0xb2 
#define HX83121A_SETEXTC 0xb9
#define HX83121A_SETBANK 0xbd
#define HX83121A_UNKNOWN1 0xcd

struct hx83121a_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[3];
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *enable_gpio;
};

static inline struct hx83121a_panel *to_hx83121a_panel(struct drm_panel *panel)
{
	return container_of(panel, struct hx83121a_panel, panel);
}

static void hx83121a_reset(struct hx83121a_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(50);
}

static void hx83121a_init_dsc(struct hx83121a_panel *ctx)
{
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	ctx->dsc.slice_height = 40;
	ctx->dsc.slice_width = 1600;
	WARN_ON(1600 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1600 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;
}

static int hx83121a_on(struct hx83121a_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* CSOT PPC357DB1-4 DSC-on sequence from upstream HX83121A driver. */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETEXTC,
				     0x83, 0x12, 0x1a, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb1,
				     0x1c, 0x6b, 0x6b, 0x27, 0xe7, 0x00, 0x1b, 0x25,
				     0x21, 0x21, 0x2d, 0x2d, 0x17, 0x33, 0x31, 0x40,
				     0xcd, 0xff, 0x1a, 0x05, 0x15, 0x98, 0x00, 0x88,
				     0x7f, 0xff, 0xff, 0xcf, 0x1a, 0xcc, 0x02, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd1, 0x37, 0x03, 0x0c, 0xfd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETDISP,
				     0x00, 0x6a, 0x40, 0x00, 0x00, 0x14, 0x98, 0x60,
				     0x3c, 0x02, 0x80, 0x21, 0x21, 0x00, 0x00, 0xf0,
				     0x27);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc0, 0x23, 0x23, 0xcc, 0x22, 0x99, 0xd8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4,
				     0x46, 0x06, 0x0c, 0xbe, 0x0c, 0xbe, 0x09, 0x46,
				     0x0f, 0x57, 0x0f, 0x57, 0x03, 0x4a, 0x00, 0x00,
				     0x04, 0x0c, 0x00, 0x18, 0x01, 0x06, 0x08, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0xff, 0x00, 0xff, 0x10, 0x00, 0x02,
				     0x14, 0x14, 0x14, 0x14);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1, 0x01, 0x3f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe9, 0xe2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7, 0x49);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd3,
				     0x00, 0xc0, 0x08, 0x08, 0x08, 0x04, 0x04, 0x04,
				     0x16, 0x02, 0x07, 0x07, 0x07, 0x31, 0x13, 0x19,
				     0x12, 0x12, 0x03, 0x03, 0x03, 0x32, 0x10, 0x18,
				     0x00, 0x11, 0x32, 0x10, 0x03, 0x00, 0x03, 0x32,
				     0x10, 0x03, 0x00, 0x03, 0x00, 0x00, 0xff, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe1,
				     0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0a, 0x00,
				     0x03, 0x20, 0x00, 0x14, 0x03, 0x20, 0x03, 0x20,
				     0x02, 0x00, 0x02, 0x91, 0x00, 0x20, 0x02, 0x47,
				     0x00, 0x0b, 0x00, 0x0c, 0x05, 0x0e, 0x03, 0x68,
				     0x18, 0x00, 0x10, 0xe0, 0x03, 0x0c, 0x20, 0x00,
				     0x06, 0x0b, 0x0b, 0x33, 0x0e, 0x1c, 0x2a, 0x38,
				     0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b,
				     0x7d, 0x7e, 0x01, 0x02, 0x01, 0x00, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7,
				     0x17, 0x08, 0x08, 0x2c, 0x46, 0x1e, 0x02, 0x23,
				     0x5d, 0x02, 0xc9, 0x00, 0x00, 0x00, 0x00, 0x12,
				     0x05, 0x02, 0x02, 0x07, 0x10, 0x10, 0x00, 0x1d,
				     0xb9, 0x23, 0xb9, 0x00, 0x33, 0x02, 0x88);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7,
				     0x02, 0x00, 0xb2, 0x01, 0x56, 0x07, 0x56, 0x08,
				     0x48, 0x14, 0xfd, 0x26);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7,
				     0x08, 0x08, 0x01, 0x03, 0x01, 0x03, 0x07, 0x02,
				     0x02, 0x47, 0x00, 0x47, 0x81, 0x02, 0x40, 0x00,
				     0x18, 0x4a, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
				     0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00,
				     0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbf,
				     0xfd, 0x00, 0x80, 0x9c, 0x36, 0x00, 0x81, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_UNKNOWN1,
				     0x81, 0x00, 0x80, 0x77, 0x00, 0x01, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1, 0xe1,
				     0xc7, 0xb2, 0xa0, 0x90, 0x81, 0x75, 0x69, 0x5f,
				     0x55, 0x4c, 0x44, 0x3d, 0x36, 0x2f, 0x2a, 0x24,
				     0x1e, 0x19, 0x14, 0x10, 0x09, 0x08, 0x07, 0x54,
				     0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0xaa, 0xd4, 0xff, 0x2a, 0x55, 0x7f, 0xaa, 0xd4,
				     0xff, 0xea, 0xff, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETBANK, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x01, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd9, 0x5f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, HX83121A_SETEXTC, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 140);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_msleep(&dsi_ctx, 20);

	if (dsi_ctx.accum_err)
		goto err;

	return 0;

err:
	dev_err(dev, "DSI command sequence failed: %d\n", dsi_ctx.accum_err);
	return dsi_ctx.accum_err;
}

static int hx83121a_disable(struct drm_panel *panel)
{
	struct hx83121a_panel *ctx = to_hx83121a_panel(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	msleep(20);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(128);

	return 0;
}

static int hx83121a_prepare(struct drm_panel *panel)
{
	struct hx83121a_panel *ctx = to_hx83121a_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	if (ctx->enable_gpio)
		gpiod_set_value_cansleep(ctx->enable_gpio, 1);

	hx83121a_reset(ctx);

	ret = hx83121a_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		if (ctx->enable_gpio)
			gpiod_set_value_cansleep(ctx->enable_gpio, 0);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS on link1: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		return ret;
	}

	msleep(50); /* TODO: Is this panel-dependent? */

	return 0;
}

static int hx83121a_unprepare(struct drm_panel *panel)
{
	struct hx83121a_panel *ctx = to_hx83121a_panel(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	if (ctx->enable_gpio)
		gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode hx83121a_mode = {
    .clock = (1600 + 60 + 20 + 40) * (2560 + 112 + 4 + 18) * 60 / 1000, // Adjusted based on Horizontal and Vertical timings
    .hdisplay = 1600,
    .hsync_start = 1600 + 60, // HorizontalActive + HorizontalFrontPorch
    .hsync_end = 1600 + 60 + 20, // HorizontalActive + HorizontalFrontPorch + HorizontalSyncPulse
    .htotal = 1600 + 60 + 20 + 40, // HorizontalActive + HorizontalFrontPorch + HorizontalSyncPulse + HorizontalBackPorch
    .vdisplay = 2560,
    .vsync_start = 2560 + 112, // VerticalActive + VerticalFrontPorch
    .vsync_end = 2560 + 112 + 4, // VerticalActive + VerticalFrontPorch + VerticalSyncPulse
    .vtotal = 2560 + 112 + 4 + 18, // VerticalActive + VerticalFrontPorch + VerticalSyncPulse + VerticalBackPorch
    .width_mm = 265, // Converted from HorizontalScreenSizeMM (0x109) which is 265 in decimal
    .height_mm = 166, // Converted from VerticalScreenSizeMM (0xA6) which is 166 in decimal
    .type = DRM_MODE_TYPE_DRIVER,
};

static int hx83121a_get_modes(struct drm_panel *panel,
				  struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &hx83121a_mode);
}

static const struct drm_panel_funcs hx83121a_panel_funcs = {
	.prepare = hx83121a_prepare,
	.unprepare = hx83121a_unprepare,
	.disable = hx83121a_disable,
	.get_modes = hx83121a_get_modes,
};

static int hx83121a_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct hx83121a_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;

	hx83121a_init_dsc(ctx);
	dsi->dsc = &ctx->dsc;

	ctx->supplies[0].supply = "vdd1";
	ctx->supplies[1].supply = "vddi";
	ctx->supplies[2].supply = "vdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio),
				     "Failed to get enable-gpios\n");

	drm_panel_init(&ctx->panel, dev, &hx83121a_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
		goto err_remove_panel;
	}

	return 0;

err_remove_panel:
	drm_panel_remove(&ctx->panel);
	return ret;
}

static void hx83121a_remove(struct mipi_dsi_device *dsi)
{
	struct hx83121a_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id hx83121a_of_match[] = {
	{ .compatible = "csot,pnc357db1-4" },
	{ .compatible = "csot,pnc357db14" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hx83121a_of_match);

static struct mipi_dsi_driver hx83121a_driver = {
	.probe = hx83121a_probe,
	.remove = hx83121a_remove,
	.driver = {
		.name = "panel-himax-hx83121a",
		.of_match_table = hx83121a_of_match,
	},
};
module_mipi_dsi_driver(hx83121a_driver);

MODULE_DESCRIPTION("DRM driver for hx83121a-equipped DSI panels");
MODULE_LICENSE("GPL");
