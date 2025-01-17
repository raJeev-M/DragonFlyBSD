/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Deepak M <m.deepak at intel.com>
 */

#include "intel_drv.h"
#include "intel_dsi.h"
#include "i915_drv.h"
#include <video/mipi_display.h>
#include <drm/drm_mipi_dsi.h>

#define CONTROL_DISPLAY_BCTRL		(1 << 5)
#define CONTROL_DISPLAY_DD		(1 << 3)
#define CONTROL_DISPLAY_BL		(1 << 2)

#define POWER_SAVE_OFF			(0 << 0)
#define POWER_SAVE_LOW			(1 << 0)
#define POWER_SAVE_MEDIUM		(2 << 0)
#define POWER_SAVE_HIGH			(3 << 0)
#define POWER_SAVE_OUTDOOR_MODE		(4 << 0)

#define PANEL_PWM_MAX_VALUE		0xFF

static u32 dcs_get_backlight(struct intel_connector *connector)
{
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi_device;
	u8 data;
	enum port port;

	/* FIXME: Need to take care of 16 bit brightness level */
	for_each_dsi_port(port, intel_dsi->dcs_backlight_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		mipi_dsi_dcs_read(dsi_device, MIPI_DCS_GET_DISPLAY_BRIGHTNESS,
				  &data, sizeof(data));
		break;
	}

	return data;
}

static void dcs_set_backlight(struct intel_connector *connector, u32 level)
{
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi_device;
	u8 data = level;
	enum port port;

	/* FIXME: Need to take care of 16 bit brightness level */
	for_each_dsi_port(port, intel_dsi->dcs_backlight_ports) {
		dsi_device = intel_dsi->dsi_hosts[port]->device;
		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				   &data, sizeof(data));
	}
}

static void dcs_disable_backlight(struct intel_connector *connector)
{
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct mipi_dsi_device *dsi_device;
	enum port port;

	dcs_set_backlight(connector, 0);

	for_each_dsi_port(port, intel_dsi->dcs_cabc_ports) {
		u8 cabc = POWER_SAVE_OFF;

		dsi_device = intel_dsi->dsi_hosts[port]->device;
		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_POWER_SAVE,
				   &cabc, sizeof(cabc));
	}

	for_each_dsi_port(port, intel_dsi->dcs_backlight_ports) {
		u8 ctrl = 0;

		dsi_device = intel_dsi->dsi_hosts[port]->device;

		mipi_dsi_dcs_read(dsi_device, MIPI_DCS_GET_CONTROL_DISPLAY,
				  &ctrl, sizeof(ctrl));

		ctrl &= ~CONTROL_DISPLAY_BL;
		ctrl &= ~CONTROL_DISPLAY_DD;
		ctrl &= ~CONTROL_DISPLAY_BCTRL;

		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				   &ctrl, sizeof(ctrl));
	}
}

static void dcs_enable_backlight(struct intel_connector *connector)
{
	struct intel_encoder *encoder = connector->encoder;
	struct intel_dsi *intel_dsi = enc_to_intel_dsi(&encoder->base);
	struct intel_panel *panel = &connector->panel;
	struct mipi_dsi_device *dsi_device;
	enum port port;

	for_each_dsi_port(port, intel_dsi->dcs_backlight_ports) {
		u8 ctrl = 0;

		dsi_device = intel_dsi->dsi_hosts[port]->device;

		mipi_dsi_dcs_read(dsi_device, MIPI_DCS_GET_CONTROL_DISPLAY,
				  &ctrl, sizeof(ctrl));

		ctrl |= CONTROL_DISPLAY_BL;
		ctrl |= CONTROL_DISPLAY_DD;
		ctrl |= CONTROL_DISPLAY_BCTRL;

		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				   &ctrl, sizeof(ctrl));
	}

	for_each_dsi_port(port, intel_dsi->dcs_cabc_ports) {
		u8 cabc = POWER_SAVE_MEDIUM;

		dsi_device = intel_dsi->dsi_hosts[port]->device;
		mipi_dsi_dcs_write(dsi_device, MIPI_DCS_WRITE_POWER_SAVE,
				   &cabc, sizeof(cabc));
	}

	dcs_set_backlight(connector, panel->backlight.level);
}

static int dcs_setup_backlight(struct intel_connector *connector,
			       enum i915_pipe unused)
{
	struct intel_panel *panel = &connector->panel;

	panel->backlight.max = PANEL_PWM_MAX_VALUE;
	panel->backlight.level = PANEL_PWM_MAX_VALUE;

	return 0;
}

int intel_dsi_dcs_init_backlight_funcs(struct intel_connector *intel_connector)
{
	struct drm_device *dev = intel_connector->base.dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_encoder *encoder = intel_connector->encoder;
	struct intel_panel *panel = &intel_connector->panel;

	if (dev_priv->vbt.backlight.type != INTEL_BACKLIGHT_DSI_DCS)
		return -ENODEV;

	if (WARN_ON(encoder->type != INTEL_OUTPUT_DSI))
		return -EINVAL;

	panel->backlight.setup = dcs_setup_backlight;
	panel->backlight.enable = dcs_enable_backlight;
	panel->backlight.disable = dcs_disable_backlight;
	panel->backlight.set = dcs_set_backlight;
	panel->backlight.get = dcs_get_backlight;

	return 0;
}
