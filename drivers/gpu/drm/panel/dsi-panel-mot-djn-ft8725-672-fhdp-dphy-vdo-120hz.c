// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_modes.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#include "include/dsi-panel-mot-djn-ft8725-672-fhdp-dphy-vdo-120hz.h"
#endif

#define PANEL_LDO_VTP_EN

#ifdef PANEL_LDO_VTP_EN
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#endif

/* option function to read data from some panel address */
/* #define PANEL_SUPPORT_READBACK */

#define BIAS_SM5109
#ifdef BIAS_SM5109
extern int __attribute__ ((weak)) sm5109_BiasPower_disable(u32 pwrdown_delay);
extern int __attribute__ ((weak)) sm5109_BiasPower_enable(u32 avdd, u32 avee,u32 pwrup_delay);
#endif

static int tp_gesture_flag = 0;

struct dijing {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_n_gpio;
	struct gpio_desc *bias_p_gpio;

	bool prepared;
	bool enabled;

	int error;
	unsigned int hbm_mode;
	unsigned int cabc_mode;
};

#define dijing_dcs_write_seq(ctx, seq...)                                     \
	({                                                                     \
		const u8 d[] = {seq};                                          \
		BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64,                           \
				 "DCS sequence too big for stack");            \
		dijing_dcs_write(ctx, d, ARRAY_SIZE(d));                      \
	})

#define dijing_dcs_write_seq_static(ctx, seq...)                              \
	({                                                                     \
		static const u8 d[] = {seq};                                   \
		dijing_dcs_write(ctx, d, ARRAY_SIZE(d));                      \
	})

static inline struct dijing *panel_to_dijing(struct drm_panel *panel)
{
	return container_of(panel, struct dijing, panel);
}

#ifdef PANEL_LDO_VTP_EN
static struct regulator *reg_vtp_1p8;
static unsigned int lcm_get_reg_vtp_1p8(void)
{
	unsigned int volt = 0;

	if (regulator_is_enabled(reg_vtp_1p8))
		volt = regulator_get_voltage(reg_vtp_1p8);

	return volt;
}

static unsigned int lcm_enable_reg_vtp_1p8(int en)
{
	unsigned int ret = 0, volt = 0;
	static bool vddio_enable_flg = false;

	pr_info("[lcd_info]%s +\n", __func__);
	if (en) {
		if (!vddio_enable_flg) {
			if (!IS_ERR_OR_NULL(reg_vtp_1p8)) {
				ret = regulator_set_voltage(reg_vtp_1p8, 1800000, 1800000);
				if (ret < 0)
					pr_info("set voltage reg_vtp_1p8 fail, ret = %d\n", ret);

				ret = regulator_enable(reg_vtp_1p8);
				pr_info("[lh]Enable the Regulator vufs1p8 ret=%d.\n", ret);
				volt = lcm_get_reg_vtp_1p8();
				pr_info("[lh]get the Regulator vufs1p8 =%d.\n", volt);
				vddio_enable_flg = true;
			}
		}
	} else {
		if (vddio_enable_flg) {
			if (!IS_ERR_OR_NULL(reg_vtp_1p8)) {
				ret = regulator_disable(reg_vtp_1p8);
				pr_info("[lh]disable the Regulator vufs1p8 ret=%d.\n", ret);
				vddio_enable_flg = false;
			}
		}
	}

	pr_info("[lcd_info]%s -\n", __func__);

	return ret;
}
#endif

#ifdef PANEL_SUPPORT_READBACK
static int dijing_dcs_read(struct dijing *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void dijing_panel_get_data(struct dijing *ctx)
{
	u8 buffer[3] = {0};
	static int ret;

	if (ret == 0) {
		ret = dijing_dcs_read(ctx, 0x0A, buffer, 1);
		pr_info("disp: %s 0x%08x\n", __func__, buffer[0] | (buffer[1] << 8));
		dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			 ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif

static void dijing_dcs_write(struct dijing *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}


static void dijing_panel_init(struct dijing *ctx)
{
	pr_info("disp: %s+\n", __func__);

	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		//return;
	}
	else {
		gpiod_set_value(ctx->reset_gpio, 1);
		usleep_range(3 * 1000, 8 * 1000);
		gpiod_set_value(ctx->reset_gpio, 0);
		usleep_range(2 * 1000, 3 * 1000);
		gpiod_set_value(ctx->reset_gpio, 1);
		usleep_range(10 * 1000, 15 * 1000);
		devm_gpiod_put(ctx->dev, ctx->reset_gpio);
		pr_info("disp: %s reset_gpio\n", __func__);
	}
	dijing_dcs_write_seq_static(ctx, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0xFF, 0x87, 0x25, 0x01);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x80);
	dijing_dcs_write_seq_static(ctx, 0xFF, 0x87, 0x25);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x0E);
	dijing_dcs_write_seq_static(ctx, 0xF3, 0x80, 0xFF);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x90);
	dijing_dcs_write_seq_static(ctx, 0xA7, 0x00);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x81);
	dijing_dcs_write_seq_static(ctx, 0xA4, 0x73);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x87);
	dijing_dcs_write_seq_static(ctx, 0xC4, 0x08);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xA1);
	dijing_dcs_write_seq_static(ctx, 0xC0, 0x6E);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xA4);
	dijing_dcs_write_seq_static(ctx, 0xC1, 0x5E);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xB8);
	dijing_dcs_write_seq_static(ctx, 0xC1, 0x4E);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x85);
	dijing_dcs_write_seq_static(ctx, 0xCE, 0xBE);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x8D);
	dijing_dcs_write_seq_static(ctx, 0xCE, 0x8C);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x8F);
	dijing_dcs_write_seq_static(ctx, 0xCE, 0xCC);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x9C);
	dijing_dcs_write_seq_static(ctx, 0xCE, 0x10, 0x1B);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xB2);
	dijing_dcs_write_seq_static(ctx, 0xCF, 0xB4, 0xB8);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xB7);
	dijing_dcs_write_seq_static(ctx, 0xCF, 0x64, 0x68);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x62);
	dijing_dcs_write_seq_static(ctx, 0xCF, 0x85, 0x89);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x66);
	dijing_dcs_write_seq_static(ctx, 0xCF, 0x4D, 0x51);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x85);
	dijing_dcs_write_seq_static(ctx, 0xB4, 0x77);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0xE3, 0x00, 0x04, 0x08, 0x0F, 0x3F, 0x1B, 0x23, 0x29, 0x34, 0x08, 0x3C, 0x43, 0x49, 0x4E, 0x40, 0x52, 0x5B, 0x62, 0x69, 0xC5, 0x70, 0x77, 0x7E, 0x87, 0x5C, 0x90, 0x96, 0x9C, 0xA4, 0xDC, 0xAC, 0xB7, 0xC4, 0xCD, 0xE1, 0xD9, 0xE9, 0xF3, 0xFF, 0x18);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x30);
	dijing_dcs_write_seq_static(ctx, 0xE3, 0x00, 0x04, 0x08, 0x0F, 0x3F, 0x1B, 0x23, 0x29, 0x34, 0x08, 0x3C, 0x43, 0x49, 0x4E, 0x40, 0x52, 0x5B, 0x62, 0x69, 0xC5, 0x70, 0x77, 0x7E, 0x87, 0x5C, 0x90, 0x96, 0x9C, 0xA4, 0xDC, 0xAC, 0xB7, 0xC4, 0xCD, 0xE1, 0xD9, 0xE9, 0xF3, 0xFF, 0x18);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x60);
	dijing_dcs_write_seq_static(ctx, 0xE3, 0x00, 0x04, 0x08, 0x0F, 0x3F, 0x1B, 0x23, 0x29, 0x34, 0x08, 0x3C, 0x43, 0x49, 0x4E, 0x40, 0x52, 0x5B, 0x62, 0x69, 0xC5, 0x70, 0x77, 0x7E, 0x87, 0x5C, 0x90, 0x96, 0x9C, 0xA4, 0xDC, 0xAC, 0xB7, 0xC4, 0xCD, 0xE1, 0xD9, 0xE9, 0xF3, 0xFF, 0x18);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x90);
	dijing_dcs_write_seq_static(ctx, 0xE3, 0x00, 0x04, 0x08, 0x0F, 0x3F, 0x1B, 0x23, 0x29, 0x34, 0x08, 0x3C, 0x43, 0x49, 0x4E, 0x40, 0x52, 0x5B, 0x62, 0x69, 0xC5, 0x70, 0x77, 0x7E, 0x87, 0x5C, 0x90, 0x96, 0x9C, 0xA4, 0xDC, 0xAC, 0xB7, 0xC4, 0xCD, 0xE1, 0xD9, 0xE9, 0xF3, 0xFF, 0x18);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xC0);
	dijing_dcs_write_seq_static(ctx, 0xE3, 0x00, 0x04, 0x08, 0x0F, 0x3F, 0x1B, 0x23, 0x29, 0x34, 0x08, 0x3C, 0x43, 0x49, 0x4E, 0x40, 0x52, 0x5B, 0x62, 0x69, 0xC5, 0x70, 0x77, 0x7E, 0x87, 0x5C, 0x90, 0x96, 0x9C, 0xA4, 0xDC, 0xAC, 0xB7, 0xC4, 0xCD, 0xE1, 0xD9, 0xE9, 0xF3, 0xFF, 0x18);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xF0);
	dijing_dcs_write_seq_static(ctx, 0xE3, 0x00, 0x04, 0x08, 0x0F, 0x3F, 0x1B, 0x23, 0x29, 0x34, 0x08, 0x3C, 0x43, 0x49, 0x4E, 0x40, 0x52);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0xE4, 0x5B, 0x62, 0x69, 0xC5, 0x70, 0x77, 0x7E, 0x87, 0x5C, 0x90, 0x96, 0x9C, 0xA4, 0xDC, 0xAC, 0xB7, 0xC4, 0xCD, 0xE1, 0xD9, 0xE9, 0xF3, 0xFF, 0x18);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x80);
	dijing_dcs_write_seq_static(ctx, 0xCA, 0xEB, 0xD6, 0xC5, 0xB8, 0xAE, 0xA4, 0x9D, 0x96, 0x91, 0x8C, 0x87, 0x83);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x90);
	dijing_dcs_write_seq_static(ctx, 0xCA, 0xFD, 0xFF, 0xEA, 0xFC, 0xFF, 0xCC, 0xFA, 0xFF, 0x66);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xB5);
	dijing_dcs_write_seq_static(ctx, 0xCA, 0x04);
	dijing_dcs_write_seq_static(ctx, 0x00, 0xA0);
	dijing_dcs_write_seq_static(ctx, 0xCA, 0x04,0x04,0x04);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0xFF, 0x00, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x80);
	dijing_dcs_write_seq_static(ctx, 0xFF, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0x35, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0x51, 0xCC, 0x0C);
	dijing_dcs_write_seq_static(ctx, 0x53, 0x2C);
	dijing_dcs_write_seq_static(ctx, 0x55, 0x01);
	dijing_dcs_write_seq_static(ctx, 0x26, 0x02);

	dijing_dcs_write_seq_static(ctx, 0x11, 0x00);
	msleep(85);
	dijing_dcs_write_seq_static(ctx, 0x29, 0x00);
        msleep(10);
	pr_info("disp:init code %s-\n", __func__);
}

static int dijing_disable(struct drm_panel *panel)
{
	struct dijing *ctx = panel_to_dijing(panel);
	pr_info("%s\n", __func__);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static void dijing_panel_off(struct dijing *ctx)
{
	dijing_dcs_write_seq_static(ctx, MIPI_DCS_SET_DISPLAY_OFF);
	msleep(20);
	dijing_dcs_write_seq_static(ctx, MIPI_DCS_ENTER_SLEEP_MODE);
	msleep(100);

	dijing_dcs_write_seq_static(ctx, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0xff, 0x87, 0x25, 0x01);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x80);
	dijing_dcs_write_seq_static(ctx, 0xff, 0x87, 0x25);
	dijing_dcs_write_seq_static(ctx, 0x00, 0x00);
	dijing_dcs_write_seq_static(ctx, 0xf7, 0x5a, 0xa5, 0x95, 0x27);
	msleep(20);

}

static int panel_set_gesture_flag(int state)
{
	if(state == 1)
		tp_gesture_flag = 1;
	else
		tp_gesture_flag = 0;

	pr_info("%s:disp:set tp_gesture_flag:%d\n", __func__, tp_gesture_flag);
	return 0;
}

static int dijing_unprepare(struct drm_panel *panel)
{
	struct dijing *ctx = panel_to_dijing(panel);

	if (!ctx->prepared) {
		pr_info("%s, already unprepared, return\n", __func__);
		return 0;
	}
	pr_info("%s\n", __func__);

	dijing_panel_off(ctx);

	ctx->prepared = false;

	pr_info("%s:disp: tp_gesture_flag:%d\n",__func__, tp_gesture_flag);
	if(!tp_gesture_flag) {
#ifdef BIAS_SM5109
		sm5109_BiasPower_disable(5);
#endif

#ifdef PANEL_LDO_VTP_EN
		//lcm_enable_reg_vtp_1p8(0);
#endif
	}

	ctx->error = 0;
	return 0;
}

static int dijing_prepare(struct drm_panel *panel)
{
	struct dijing *ctx = panel_to_dijing(panel);
	int ret;

	pr_info("%s\n", __func__);
	if (ctx->prepared) {
		pr_info("%s, already prepared, return\n", __func__);
		return 0;
	}

#ifdef PANEL_LDO_VTP_EN
	ret = lcm_enable_reg_vtp_1p8(1);
	usleep_range(4 * 1000, 8 * 1000);
#endif
	if(!tp_gesture_flag) {
		gpiod_set_value(ctx->reset_gpio, 0);
		msleep(3);
#ifdef BIAS_SM5109
		sm5109_BiasPower_enable(15,15,5);
		msleep(3);
#endif
	}
	dijing_panel_init(ctx);
	ctx->hbm_mode = 0;
	ctx->cabc_mode = 0;

	ret = ctx->error;
	if (ret < 0) {
		pr_info("disp: %s error ret=%d\n", __func__, ret);
		dijing_unprepare(panel);
	}

	ctx->prepared = true;
/*#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
#endif

#ifdef PANEL_SUPPORT_READBACK
	dijing_panel_get_data(ctx);
#endif*/
	pr_info("disp: %s-\n", __func__);
	return ret;
}

static int dijing_enable(struct drm_panel *panel)
{
	struct dijing *ctx = panel_to_dijing(panel);

	pr_info("disp: %s+\n", __func__);
	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}

static const struct drm_display_mode default_mode = {
	.clock = 330456,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + MODE_120_HFP,
	.hsync_end = FRAME_WIDTH + MODE_120_HFP + HSA,
	.htotal = FRAME_WIDTH + MODE_120_HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + MODE_120_VFP,
	.vsync_end = FRAME_HEIGHT + MODE_120_VFP + VSA,
	.vtotal = FRAME_HEIGHT + MODE_120_VFP + VSA + VBP,
#if 0
	.hsync_start = 1080 + 12,//HFP
	.hsync_end = 1080 + 12 + 10,//HSA
	.htotal = 1080 + 12 + 10 + 12,//HBP
	.vdisplay = 2400,
	.vsync_start = 2400 + 20,//VFP
	.vsync_end = 2400 + 20 + 10,//VSA
	.vtotal = 2400 + 20 + 10 + 20,//VBP
#endif
};

#if 1
static const struct drm_display_mode performance_mode_30hz = {
	.clock = 330793,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + MODE_30_HFP,
	.hsync_end = FRAME_WIDTH + MODE_30_HFP + HSA,
	.htotal = FRAME_WIDTH + MODE_30_HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + MODE_30_VFP,
	.vsync_end = FRAME_HEIGHT + MODE_30_VFP + VSA,
	.vtotal = FRAME_HEIGHT + MODE_30_VFP + VSA + VBP,
};

static const struct drm_display_mode performance_mode_90hz = {
	.clock = 330793,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + MODE_90_HFP,
	.hsync_end = FRAME_WIDTH + MODE_90_HFP + HSA,
	.htotal = FRAME_WIDTH + MODE_90_HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + MODE_90_VFP,
	.vsync_end = FRAME_HEIGHT + MODE_90_VFP + VSA,
	.vtotal = FRAME_HEIGHT + MODE_90_VFP + VSA + VBP,
};

static const struct drm_display_mode performance_mode_60hz = {
	.clock = 330861,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + MODE_60_HFP,
	.hsync_end = FRAME_WIDTH + MODE_60_HFP + HSA,
	.htotal = FRAME_WIDTH + MODE_60_HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + MODE_60_VFP,
	.vsync_end = FRAME_HEIGHT + MODE_60_VFP + VSA,
	.vtotal = FRAME_HEIGHT + MODE_60_VFP + VSA + VBP,
};
#endif

#if defined(CONFIG_MTK_PANEL_EXT)
static struct mtk_panel_params ext_params = {
	.change_fps_by_vfp_send_cmd = 0,
	//.vfp_low_power = 20,
	.phy_timcon.lpx = 8,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x1D,
		.count = 1,
		.para_list[0] = 0x00,
	},
	.panel_ver = 1,
	.panel_id = 0x01050a91,
	.panel_name = "djn_ft8725_vid_672_1080",
	.panel_supplier = "djn",
	.lcm_index = 1,
	.hbm_type = HBM_MODE_DCS_I2C,
	.max_bl_level = 2047,
	.ssc_enable = 0,
	.lane_swap_en = 0,
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable                =  DSC_ENABLE,
		.ver                   =  DSC_VER,
		.slice_mode            =  DSC_SLICE_MODE,
		.rgb_swap              =  DSC_RGB_SWAP,
		.dsc_cfg               =  DSC_DSC_CFG,
		.rct_on                =  DSC_RCT_ON,
		.bit_per_channel       =  DSC_BIT_PER_CHANNEL,
		.dsc_line_buf_depth    =  DSC_DSC_LINE_BUF_DEPTH,
		.bp_enable             =  DSC_BP_ENABLE,
		.bit_per_pixel         =  DSC_BIT_PER_PIXEL,
		.pic_height            =  FRAME_HEIGHT,
		.pic_width             =  FRAME_WIDTH,
		.slice_height          =  DSC_SLICE_HEIGHT,
		.slice_width           =  DSC_SLICE_WIDTH,
		.chunk_size            =  DSC_CHUNK_SIZE,
		.xmit_delay            =  DSC_XMIT_DELAY,
		.dec_delay             =  DSC_DEC_DELAY,
		.scale_value           =  DSC_SCALE_VALUE,
		.increment_interval    =  DSC_INCREMENT_INTERVAL,
		.decrement_interval    =  DSC_DECREMENT_INTERVAL,
		.line_bpg_offset       =  DSC_LINE_BPG_OFFSET,
		.nfl_bpg_offset        =  DSC_NFL_BPG_OFFSET,
		.slice_bpg_offset      =  DSC_SLICE_BPG_OFFSET,
		.initial_offset        =  DSC_INITIAL_OFFSET,
		.final_offset          =  DSC_FINAL_OFFSET,
		.flatness_minqp        =  DSC_FLATNESS_MINQP,
		.flatness_maxqp        =  DSC_FLATNESS_MAXQP,
		.rc_model_size         =  DSC_RC_MODEL_SIZE,
		.rc_edge_factor        =  DSC_RC_EDGE_FACTOR,
		.rc_quant_incr_limit0  =  DSC_RC_QUANT_INCR_LIMIT0,
		.rc_quant_incr_limit1  =  DSC_RC_QUANT_INCR_LIMIT1,
		.rc_tgt_offset_hi      =  DSC_RC_TGT_OFFSET_HI,
		.rc_tgt_offset_lo      =  DSC_RC_TGT_OFFSET_LO,
	},
	//.data_rate_khz = 808000,
	.data_rate = DATA_RATE,
	.lfr_enable = 1,
	.lfr_minimum_fps = MODE_60_FPS,
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 120,
		//.dfps_cmd_table[0] = {0, 4, {0xB9, 0x83, 0x10, 0x21} },
		//.dfps_cmd_table[1] = {0, 2, {0xE2, 0x00} },
		//.dfps_cmd_table[2] = {0, 2, {0xB9, 0x00} },
	},
	/* following MIPI hopping parameter might cause screen mess */
	.dyn = {
		.switch_en = 1,
		.hfp = 60,
		.data_rate = 908,
	},
};

#if 1
static struct mtk_panel_params ext_params_mode_30 = {
//	.vfp_low_power = 7476,//30hz
	.data_rate = DATA_RATE,
	.phy_timcon.lpx = 8,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x1D,
		.count = 1,
		.para_list[0] = 0x00,
	},
	.panel_ver = 1,
	.panel_id = 0x10050753,
	.panel_name = "djn_ft8725_vid_672_1080",
	.panel_supplier = "djn",
	.lcm_index = 1,
	.hbm_type = HBM_MODE_DCS_I2C,
	.max_bl_level = 2047,
	.ssc_enable = 1,
	.lane_swap_en = 0,
	.lp_perline_en = 0,
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable                =  DSC_ENABLE,
		.ver                   =  DSC_VER,
		.slice_mode            =  DSC_SLICE_MODE,
		.rgb_swap              =  DSC_RGB_SWAP,
		.dsc_cfg               =  DSC_DSC_CFG,
		.rct_on                =  DSC_RCT_ON,
		.bit_per_channel       =  DSC_BIT_PER_CHANNEL,
		.dsc_line_buf_depth    =  DSC_DSC_LINE_BUF_DEPTH,
		.bp_enable             =  DSC_BP_ENABLE,
		.bit_per_pixel         =  DSC_BIT_PER_PIXEL,
		.pic_height            =  FRAME_HEIGHT,
		.pic_width             =  FRAME_WIDTH,
		.slice_height          =  DSC_SLICE_HEIGHT,
		.slice_width           =  DSC_SLICE_WIDTH,
		.chunk_size            =  DSC_CHUNK_SIZE,
		.xmit_delay            =  DSC_XMIT_DELAY,
		.dec_delay             =  DSC_DEC_DELAY,
		.scale_value           =  DSC_SCALE_VALUE,
		.increment_interval    =  DSC_INCREMENT_INTERVAL,
		.decrement_interval    =  DSC_DECREMENT_INTERVAL,
		.line_bpg_offset       =  DSC_LINE_BPG_OFFSET,
		.nfl_bpg_offset        =  DSC_NFL_BPG_OFFSET,
		.slice_bpg_offset      =  DSC_SLICE_BPG_OFFSET,
		.initial_offset        =  DSC_INITIAL_OFFSET,
		.final_offset          =  DSC_FINAL_OFFSET,
		.flatness_minqp        =  DSC_FLATNESS_MINQP,
		.flatness_maxqp        =  DSC_FLATNESS_MAXQP,
		.rc_model_size         =  DSC_RC_MODEL_SIZE,
		.rc_edge_factor        =  DSC_RC_EDGE_FACTOR,
		.rc_quant_incr_limit0  =  DSC_RC_QUANT_INCR_LIMIT0,
		.rc_quant_incr_limit1  =  DSC_RC_QUANT_INCR_LIMIT1,
		.rc_tgt_offset_hi      =  DSC_RC_TGT_OFFSET_HI,
		.rc_tgt_offset_lo      =  DSC_RC_TGT_OFFSET_LO,
	},
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 30,
		//.dfps_cmd_table[0] = {0, 4, {0xB9, 0x83, 0x10, 0x21} },
		//.dfps_cmd_table[1] = {0, 2, {0xE2, 0x00} },
		//.dfps_cmd_table[2] = {0, 2, {0xB9, 0x00} },
	},
	/* following MIPI hopping parameter might cause screen mess */
	.dyn = {
		.switch_en = 1,
		.hfp = 60,
		.data_rate = 908,
	},
};

static struct mtk_panel_params ext_params_mode_90 = {
//	.vfp_low_power = 7476,//30hz
	.data_rate = DATA_RATE,
	.phy_timcon.lpx = 8,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x1D,
		.count = 1,
		.para_list[0] = 0x00,
	},
	.panel_ver = 1,
	.panel_id = 0x10050753,
	.panel_name = "djn_ft8725_vid_672_1080",
	.panel_supplier = "djn",
	.lcm_index = 1,
	.hbm_type = HBM_MODE_DCS_I2C,
	.max_bl_level = 2047,
	.ssc_enable = 1,
	.lane_swap_en = 0,
	.lp_perline_en = 0,
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable                =  DSC_ENABLE,
		.ver                   =  DSC_VER,
		.slice_mode            =  DSC_SLICE_MODE,
		.rgb_swap              =  DSC_RGB_SWAP,
		.dsc_cfg               =  DSC_DSC_CFG,
		.rct_on                =  DSC_RCT_ON,
		.bit_per_channel       =  DSC_BIT_PER_CHANNEL,
		.dsc_line_buf_depth    =  DSC_DSC_LINE_BUF_DEPTH,
		.bp_enable             =  DSC_BP_ENABLE,
		.bit_per_pixel         =  DSC_BIT_PER_PIXEL,
		.pic_height            =  FRAME_HEIGHT,
		.pic_width             =  FRAME_WIDTH,
		.slice_height          =  DSC_SLICE_HEIGHT,
		.slice_width           =  DSC_SLICE_WIDTH,
		.chunk_size            =  DSC_CHUNK_SIZE,
		.xmit_delay            =  DSC_XMIT_DELAY,
		.dec_delay             =  DSC_DEC_DELAY,
		.scale_value           =  DSC_SCALE_VALUE,
		.increment_interval    =  DSC_INCREMENT_INTERVAL,
		.decrement_interval    =  DSC_DECREMENT_INTERVAL,
		.line_bpg_offset       =  DSC_LINE_BPG_OFFSET,
		.nfl_bpg_offset        =  DSC_NFL_BPG_OFFSET,
		.slice_bpg_offset      =  DSC_SLICE_BPG_OFFSET,
		.initial_offset        =  DSC_INITIAL_OFFSET,
		.final_offset          =  DSC_FINAL_OFFSET,
		.flatness_minqp        =  DSC_FLATNESS_MINQP,
		.flatness_maxqp        =  DSC_FLATNESS_MAXQP,
		.rc_model_size         =  DSC_RC_MODEL_SIZE,
		.rc_edge_factor        =  DSC_RC_EDGE_FACTOR,
		.rc_quant_incr_limit0  =  DSC_RC_QUANT_INCR_LIMIT0,
		.rc_quant_incr_limit1  =  DSC_RC_QUANT_INCR_LIMIT1,
		.rc_tgt_offset_hi      =  DSC_RC_TGT_OFFSET_HI,
		.rc_tgt_offset_lo      =  DSC_RC_TGT_OFFSET_LO,
	},
	.lfr_enable = LFR_EN,
	.lfr_minimum_fps = MODE_90_FPS,
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 90,
		//.dfps_cmd_table[0] = {0, 4, {0xB9, 0x83, 0x10, 0x21} },
		//.dfps_cmd_table[1] = {0, 2, {0xE2, 0x00} },
		//.dfps_cmd_table[2] = {0, 2, {0xB9, 0x00} },
	},
	/* following MIPI hopping parameter might cause screen mess */
	.dyn = {
		.switch_en = 1,
		.hfp = 60,
		.data_rate = 908,
	},
};

static struct mtk_panel_params ext_params_mode_60 = {
//	.vfp_low_power = 7476,//30hz
	.data_rate = DATA_RATE,
	.phy_timcon.lpx = 8,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x1D,
		.count = 1,
		.para_list[0] = 0x00,
	},
	.panel_ver = 1,
	.panel_id = 0x10050753,
	.panel_name = "djn_ft8725_vid_672_1080",
	.panel_supplier = "djn",
	.lcm_index = 1,
	.hbm_type = HBM_MODE_DCS_I2C,
	.max_bl_level = 2047,
	.ssc_enable = 1,
	.lane_swap_en = 0,
	.lp_perline_en = 0,
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.output_mode = MTK_PANEL_DSC_SINGLE_PORT,
	.dsc_params = {
		.enable                =  DSC_ENABLE,
		.ver                   =  DSC_VER,
		.slice_mode            =  DSC_SLICE_MODE,
		.rgb_swap              =  DSC_RGB_SWAP,
		.dsc_cfg               =  DSC_DSC_CFG,
		.rct_on                =  DSC_RCT_ON,
		.bit_per_channel       =  DSC_BIT_PER_CHANNEL,
		.dsc_line_buf_depth    =  DSC_DSC_LINE_BUF_DEPTH,
		.bp_enable             =  DSC_BP_ENABLE,
		.bit_per_pixel         =  DSC_BIT_PER_PIXEL,
		.pic_height            =  FRAME_HEIGHT,
		.pic_width             =  FRAME_WIDTH,
		.slice_height          =  DSC_SLICE_HEIGHT,
		.slice_width           =  DSC_SLICE_WIDTH,
		.chunk_size            =  DSC_CHUNK_SIZE,
		.xmit_delay            =  DSC_XMIT_DELAY,
		.dec_delay             =  DSC_DEC_DELAY,
		.scale_value           =  DSC_SCALE_VALUE,
		.increment_interval    =  DSC_INCREMENT_INTERVAL,
		.decrement_interval    =  DSC_DECREMENT_INTERVAL,
		.line_bpg_offset       =  DSC_LINE_BPG_OFFSET,
		.nfl_bpg_offset        =  DSC_NFL_BPG_OFFSET,
		.slice_bpg_offset      =  DSC_SLICE_BPG_OFFSET,
		.initial_offset        =  DSC_INITIAL_OFFSET,
		.final_offset          =  DSC_FINAL_OFFSET,
		.flatness_minqp        =  DSC_FLATNESS_MINQP,
		.flatness_maxqp        =  DSC_FLATNESS_MAXQP,
		.rc_model_size         =  DSC_RC_MODEL_SIZE,
		.rc_edge_factor        =  DSC_RC_EDGE_FACTOR,
		.rc_quant_incr_limit0  =  DSC_RC_QUANT_INCR_LIMIT0,
		.rc_quant_incr_limit1  =  DSC_RC_QUANT_INCR_LIMIT1,
		.rc_tgt_offset_hi      =  DSC_RC_TGT_OFFSET_HI,
		.rc_tgt_offset_lo      =  DSC_RC_TGT_OFFSET_LO,
	},
	.lfr_enable = LFR_EN,
	.lfr_minimum_fps = MODE_60_FPS,
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 60,
		//.dfps_cmd_table[0] = {0, 4, {0xB9, 0x83, 0x10, 0x21} },
		//.dfps_cmd_table[1] = {0, 2, {0xE2, 0x00} },
		//.dfps_cmd_table[2] = {0, 2, {0xB9, 0x00} },
	},
	/* following MIPI hopping parameter might cause screen mess */
	.dyn = {
		.switch_en = 1,
		.hfp = 60,
		.data_rate = 908,
	},
};
#endif

static int dijing_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
	void *handle, unsigned int level)
{
	pr_info("%s: skip for using bl ic, level=%d\n", __func__, level);

#if 0

	if (!cb) {
		pr_info("%s cb NULL!\n", __func__);
		return -1;
	}

	bl_tb0[1] = (u8)(level&0xFF);
	bl_tb0[2] = (u8)((level>>8)&0x7);

	pr_info("%s set level:%d, bl_tb:0x%02x%02x\n", __func__, level, bl_tb0[1], bl_tb0[2]);
	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
#endif

	return 0;
}

struct drm_display_mode *get_mode_by_id(struct drm_connector *connector,
	unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	list_for_each_entry(m, &connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}
	return NULL;
}

static int mtk_panel_ext_param_set(struct drm_panel *panel,
			 struct drm_connector *connector, unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id(connector, mode);

	if (!m)
		return ret;
	printk("[lcd_info][%s] switch fps to %d\n",__func__,drm_mode_vrefresh(m));
	pr_info("%s:disp: mode fps=%d", __func__, drm_mode_vrefresh(m));
	if (drm_mode_vrefresh(m) == MODE_120_FPS)
		ext->params = &ext_params;
#if 1
	else if (drm_mode_vrefresh(m) == MODE_30_FPS)
		ext->params = &ext_params_mode_30;
	else if (drm_mode_vrefresh(m) == MODE_90_FPS)
		ext->params = &ext_params_mode_90;
	else if (drm_mode_vrefresh(m) == MODE_60_FPS)
		ext->params = &ext_params_mode_60;
#endif
	else
		ret = 1;

	return ret;
}

static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct dijing *ctx = panel_to_dijing(panel);

	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

static enum mtk_lcm_version panel_get_lcm_version(void)
{
	return MTK_LEGACY_LCM_DRV_WITH_BACKLIGHTCLASS;
}

static int panel_cabc_set_cmdq(struct dijing *ctx, void *dsi, dcs_grp_write_gce cb, void *handle, uint32_t cabc_state)
{
	unsigned int para_count = 0;
	struct mtk_panel_para_table cabc_ui_table[] = {{2, {0x55, 0x01}}, {2, {0x26, 0x02}}};
	struct mtk_panel_para_table cabc_mv_table[] = {{2, {0x55, 0x03}}, {2, {0x26, 0x02}}};
	struct mtk_panel_para_table cabc_off_table[] = {{2, {0x55, 0x00}}, {2, {0x26, 0x01}}};

	if (cabc_state > 3) {
		pr_info("%s: invalid CABC mode:%d, return\n", __func__, cabc_state);
		return -1;
	}

	switch (cabc_state)
	{
		case 0:
			para_count = sizeof(cabc_ui_table) / sizeof(struct mtk_panel_para_table);
			cb(dsi, handle, cabc_ui_table, para_count);
			pr_info("%s: set CABC mode 0", __func__);
			break;
		case 1:
			para_count = sizeof(cabc_mv_table) / sizeof(struct mtk_panel_para_table);
			cb(dsi, handle, cabc_mv_table, para_count);
			pr_info("%s: set CABC mode 1", __func__);
			break;
		case 2:
			para_count = sizeof(cabc_off_table) / sizeof(struct mtk_panel_para_table);
			cb(dsi, handle, cabc_off_table, para_count);
			pr_info("%s: set CABC mode 2", __func__);
			break;
		default:
			break;
	}

	return 0;
}

static int panel_hbm_set_cmdq(struct dijing *ctx, void *dsi, dcs_grp_write_gce cb, void *handle, uint32_t hbm_state)
{
	struct mtk_panel_para_table hbm_on_table = {3, {0x51, 0xFF, 0x0F}};
	struct mtk_panel_para_table hbm_off_table = {3, {0x51, 0xCC, 0x0C}};

	if (hbm_state > 2) {
		pr_info("%s: invalid HBM mode:%d, return\n", __func__, hbm_state);
		return -1;
	}

	switch (hbm_state) {
		case 0:
			cb(dsi, handle, &hbm_off_table, 1);
			pr_info("%s: set HBM off", __func__);
			break;
		case 1:
			cb(dsi, handle, &hbm_on_table, 1);
			pr_info("%s: set HBM on", __func__);
			break;
		default:
			break;
	}

	return 0;
}
static int panel_feature_set(struct drm_panel *panel, void *dsi,
			      dcs_grp_write_gce cb, void *handle, struct panel_param_info param_info)
{

	struct dijing *ctx = panel_to_dijing(panel);
	int ret = -1;

	if (!cb) {
		pr_info("%s: cb NULL\n", __func__);
		return -1;
	}

	if (!ctx->enabled) {
		pr_info("%s: skip set feature %d to %d, panel not enabled\n", __func__, param_info.param_idx, param_info.value);
		return -1;
	}

	pr_info("%s: start set feature %d to %d\n", __func__, param_info.param_idx, param_info.value);
	switch (param_info.param_idx) {
		case PARAM_CABC:
			if (ctx->cabc_mode != param_info.value) {
				ctx->cabc_mode = param_info.value;
				panel_cabc_set_cmdq(ctx, dsi, cb, handle, param_info.value);
				pr_debug("%s: set CABC to %d end\n", __func__, param_info.value);
				ret = 0;
			}
			else
				pr_info("%s: skip same CABC mode:%d\n", __func__, ctx->cabc_mode);
			break;
		case PARAM_HBM:
			if (ctx->hbm_mode != param_info.value) {
				ctx->hbm_mode = param_info.value;
				panel_hbm_set_cmdq(ctx, dsi, cb, handle, param_info.value);
				pr_debug("%s: set HBM to %d end\n", __func__, param_info.value);
				ret = 0;
			}
			else
				pr_info("%s: skip same HBM mode:%d\n", __func__, ctx->hbm_mode);
			break;
		default:
			pr_info("%s: skip unsupport feature %d to %d\n", __func__, param_info.param_idx, param_info.value);
			break;
	}

	pr_debug("%s: set feature %d to %d, ret %d\n", __func__, param_info.param_idx, param_info.value, ret);
	return ret;
}

static struct mtk_panel_funcs ext_funcs = {
	.set_backlight_cmdq = dijing_setbacklight_cmdq,
	.reset = panel_ext_reset,
	.ext_param_set = mtk_panel_ext_param_set,
	.get_lcm_version = panel_get_lcm_version,
//	.ata_check = panel_ata_check,
	.set_gesture_flag = panel_set_gesture_flag,
	.panel_feature_set = panel_feature_set,
};
#endif

static int dijing_get_modes(struct drm_panel *panel,
						struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct drm_display_mode *mode_1;
	struct drm_display_mode *mode_2;
	struct drm_display_mode *mode_3;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	printk("[%d  %s]disp: mode:\n",__LINE__, __FUNCTION__,mode);
	if (!mode) {
		dev_err(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

#if 1
	mode_1 = drm_mode_duplicate(connector->dev, &performance_mode_30hz);
	printk("[%d  %s]disp mode:%d\n",__LINE__, __FUNCTION__,mode_1);
	if (!mode_1) {
		dev_err(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			performance_mode_30hz.hdisplay,
			performance_mode_30hz.vdisplay,
			drm_mode_vrefresh(&performance_mode_30hz));
		return -ENOMEM;
	}
	drm_mode_set_name(mode_1);
	mode_1->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode_1);


	mode_2 = drm_mode_duplicate(connector->dev, &performance_mode_90hz);
	printk("[%d  %s]disp mode:%d\n",__LINE__, __FUNCTION__,mode_2);
	if (!mode_2) {
		dev_err(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			performance_mode_90hz.hdisplay,
			performance_mode_90hz.vdisplay,
			drm_mode_vrefresh(&performance_mode_90hz));
		return -ENOMEM;
	}
	drm_mode_set_name(mode_2);
	mode_2->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode_2);

	mode_3 = drm_mode_duplicate(connector->dev, &performance_mode_60hz);
	if (!mode_3) {
		dev_err(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			performance_mode_60hz.hdisplay,
			performance_mode_60hz.vdisplay,
			drm_mode_vrefresh(&performance_mode_60hz));
		return -ENOMEM;
	}

	drm_mode_set_name(mode_3);
	mode_3->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode_3);
	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 150;
	printk("[%d  %s]end\n",__LINE__, __FUNCTION__);
#endif

	return 1;
}

static const struct drm_panel_funcs dijing_drm_funcs = {
	.disable = dijing_disable,
	.unprepare = dijing_unprepare,
	.prepare = dijing_prepare,
	.enable = dijing_enable,
	.get_modes = dijing_get_modes,
};

static int dijing_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;
	struct dijing *ctx;
	struct device_node *backlight;
	int ret;

	pr_info("%s+ disp:djn,ft8725,672,vdo,v0\n", __func__);

	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_info("No panel connected,skip probe lcm\n");
				return -ENODEV;
			}
			pr_info("disp:device node name:%s\n", remote_node->name);
		}
	}
	if (remote_node != dev->of_node) {
		pr_info("%s+ skip probe due to not current lcm\n", __func__);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct dijing), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST
			 | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

#ifdef PANEL_LDO_VTP_EN
	reg_vtp_1p8 = regulator_get(dev, "1p8");
	if (IS_ERR(reg_vtp_1p8)) {
		dev_info(dev, "%s[lh]: cannot get reg_vufs18 %ld\n",
			__func__, PTR_ERR(reg_vtp_1p8));
	}
	lcm_enable_reg_vtp_1p8(1);
#endif

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);

	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel, dev, &dijing_drm_funcs, DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

	pr_info("[%d  %s]- djn,ft8725,672,vdo,120hz ret:%d  \n", __LINE__, __func__,ret);

	return ret;
}

static int dijing_remove(struct mipi_dsi_device *dsi)
{
	struct dijing *ctx = mipi_dsi_get_drvdata(dsi);
#if defined(CONFIG_MTK_PANEL_EXT)
	struct mtk_panel_ctx *ext_ctx = find_panel_ctx(&ctx->panel);
#endif

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_detach(ext_ctx);
	mtk_panel_remove(ext_ctx);
#endif

	return 0;
}

static const struct of_device_id dijing_of_match[] = {
	{
		.compatible = "djn,ft8725,672,vdo,120hz",
	},
	{}
};

MODULE_DEVICE_TABLE(of, dijing_of_match);

static struct mipi_dsi_driver dijing_driver = {
	.probe = dijing_probe,
	.remove = dijing_remove,
	.driver = {
		.name = "djn_ft8725_vid_672_1080",
		.owner = THIS_MODULE,
		.of_match_table = dijing_of_match,
	},
};

module_mipi_dsi_driver(dijing_driver);

MODULE_AUTHOR("mediatek");
MODULE_DESCRIPTION("djn ft8725 incell 120hz Panel Driver");
MODULE_LICENSE("GPL v2");

