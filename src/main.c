/*
 * Copyright (c) 2016-2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define SYS_LOG_DOMAIN "fota/main"
#define SYS_LOG_LEVEL SYS_LOG_LEVEL_DEBUG
#include <logging/sys_log.h>

#include <zephyr.h>
#include <board.h>
#include <gpio.h>
#include <pwm.h>
#include <net/lwm2m.h>
#include <tc_util.h>

/* Local helpers and functions */
#include "tstamp_log.h"
#include "app_work_queue.h"
#include "mcuboot.h"
#include "product_id.h"
#include "lwm2m.h"

struct device *flash_dev;

/* Color Unit used by the IPSO object */
#define COLOR_UNIT	"hex"
#define COLOR_WHITE	"#FFFFFF"

/* 100 is more than enough for it to be flicker free */
#define PWM_PERIOD (USEC_PER_SEC / 100)

/* Initial dimmer value (%) */
#define DIMMER_INITIAL	50

/* Support for up to 4 PWM devices */
#if defined(CONFIG_APP_PWM_WHITE)
static struct device *pwm_white;
static u8_t white_current;
#endif
#if defined(CONFIG_APP_PWM_RED)
static struct device *pwm_red;
#endif
#if defined(CONFIG_APP_PWM_GREEN)
static struct device *pwm_green;
#endif
#if defined(CONFIG_APP_PWM_BLUE)
static struct device *pwm_blue;
#endif

static u8_t color_rgb[3];

static u8_t char_to_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10U;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10U;
	}
	return 15U;
}

static u32_t scale_pulse(u8_t level, u8_t ceiling)
{
	if (level && ceiling) {
		/* Scale level based on ceiling and return period */
		return PWM_PERIOD / 255 * level * ceiling / 255;
	}

	return 0;
}

static int write_pwm_pin(struct device *pwm_dev, u32_t pwm_pin,
			 u8_t level, u8_t ceiling)
{
	u32_t pulse = scale_pulse(level, ceiling);

	SYS_LOG_DBG("Set PWM %d: level %d, ceiling %d, pulse %lu",
				pwm_pin, level, ceiling, pulse);

	return pwm_pin_set_usec(pwm_dev, pwm_pin, PWM_PERIOD, pulse);
}

static int update_pwm(u8_t *color_rgb, u8_t dimmer)
{
	u8_t rgb[3];
	int i, ret = 0;

	memcpy(&rgb, color_rgb, 3);

	if (dimmer < 0) {
		dimmer = 0;
	}
	if (dimmer > 100) {
		dimmer = 100;
	}

#if defined(CONFIG_APP_PWM_WHITE)
	u8_t white = 0;

	/* If a dedicated PWM is used for white, zero RGB */
	if (rgb[0] == 0xFF && rgb[1] == 0xFF && rgb[2] == 0xFF) {
		rgb[0] = rgb[1] = rgb[2] = 0;
		white = 255;
	}

	/*
	 * If switching from white->color we first need to disable white to
	 * avoid consuming 4 PWM pins (required for nRF5 devices).
	 */
	if (!white && white_current) {
		white_current = 0;
		ret = write_pwm_pin(pwm_white, CONFIG_APP_PWM_WHITE_PIN, 0, 0);
		if (ret) {
			SYS_LOG_ERR("Failed to update white PWM");
			return ret;
		}
	}
#endif

	/*
	 * Update individual color values based on dimmer.
	 *
	 * This is just a direct map between dimmer and PWM, but not the best
	 * way to control the light brightness, as the human eye perceives
	 * it differently, but good enough for now as it is a simple method.
	 */
	for (i = 0; i < 3; i++) {
		rgb[i] = rgb[i] * dimmer / 100;
	}

#if defined(CONFIG_APP_PWM_RED)
	ret = write_pwm_pin(pwm_red, CONFIG_APP_PWM_RED_PIN,
				rgb[0], CONFIG_APP_PWM_RED_PIN_CEILING);
	if (ret) {
		SYS_LOG_ERR("Failed to update red PWM");
		return ret;
	}
#endif

#if defined(CONFIG_APP_PWM_GREEN)
	ret = write_pwm_pin(pwm_green, CONFIG_APP_PWM_GREEN_PIN,
				rgb[1], CONFIG_APP_PWM_GREEN_PIN_CEILING);
	if (ret) {
		SYS_LOG_ERR("Failed to update green PWM");
		return ret;
	}
#endif

#if defined(CONFIG_APP_PWM_BLUE)
	ret = write_pwm_pin(pwm_blue, CONFIG_APP_PWM_BLUE_PIN,
				rgb[2], CONFIG_APP_PWM_BLUE_PIN_CEILING);
	if (ret) {
		SYS_LOG_ERR("Failed to update blue PWM");
		return ret;
	}
#endif

#if defined(CONFIG_APP_PWM_WHITE)
	white = white * dimmer / 100;
	if (white != white_current) {
		white_current = white;
		ret = write_pwm_pin(pwm_white, CONFIG_APP_PWM_WHITE_PIN, white,
					CONFIG_APP_PWM_WHITE_PIN_CEILING);
		if (ret) {
			SYS_LOG_ERR("Failed to update white PWM");
			return ret;
		}
	}
#endif

	return ret;
}

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int color_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
		     bool last_block, size_t total_size)
{
	int i, ret = 0;
	bool on_off;
	u8_t dimmer;
	char *color;

	color = (char *) data;

	/* Check if just HEX and #HEX */
	if (data_len < 6 || data_len > 7) {
		SYS_LOG_ERR("Invalid color length (%s)", color);
		return -EINVAL;
	}
	if (data_len == 7 && *color != '#') {
		SYS_LOG_ERR("Invalid color format (%s)", color);
		return -EINVAL;
	}

	/* Skip '#' if available */
	if (data_len == 7) {
		color += 1;
	}

	for (i = 0; i < 3; i++) {
		color_rgb[i] = (char_to_nibble(*color) << 4) |
					char_to_nibble(*(color + 1));
		color += 2;
	}

	SYS_LOG_DBG("RGB color updated to #%02x%02x%02x", color_rgb[0],
					color_rgb[1], color_rgb[2]);

	/* Update PWM output if light is 'on' */
	on_off = lwm2m_engine_get_bool("3311/0/5850");
	if (on_off) {
		dimmer = lwm2m_engine_get_u8("3311/0/5851");
		ret = update_pwm(color_rgb, dimmer);
		if (ret) {
			SYS_LOG_ERR("Failed to update color");
			return ret;
		}
	}

	return ret;
}

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int dimmer_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
		     bool last_block, size_t total_size)
{
	int ret = 0;
	bool on_off;
	u8_t dimmer;

	dimmer = *(u8_t *) data;
	if (dimmer < 0) {
		SYS_LOG_ERR("Invalid dimmer value, forcing it to 0");
		dimmer = 0;
	}
	if (dimmer > 100) {
		SYS_LOG_ERR("Invalid dimmer value, forcing it to 100");
		dimmer = 100;
	}

	/* Update PWM output if light is 'on' */
	on_off = lwm2m_engine_get_bool("3311/0/5850");
	if (on_off) {
		ret = update_pwm(color_rgb, dimmer);
		if (ret) {
			SYS_LOG_ERR("Failed to update dimmer");
			return ret;
		}
	}

	return ret;
}

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int on_off_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
			 bool last_block, size_t total_size)
{
	int ret = 0;
	u8_t on_off, dimmer = 0;

	on_off = *(u8_t *) data;
	if (on_off) {
		dimmer = lwm2m_engine_get_u8("3311/0/5851");
	}

	ret = update_pwm(color_rgb, dimmer);
	if (ret) {
		SYS_LOG_ERR("Failed to update light state");
		return ret;
	}

	/* TODO: Move to be set by the IPSO object itself */
	lwm2m_engine_set_s32("3311/0/5852", 0);

	return ret;
}

static int init_pwm_devices(void)
{
#if defined(CONFIG_APP_PWM_WHITE)
	pwm_white = device_get_binding(CONFIG_APP_PWM_WHITE_DEV);
	if (!pwm_white) {
		SYS_LOG_ERR("Failed to get PWM device used for white");
		return -ENODEV;
	}
#endif
#if defined(CONFIG_APP_PWM_RED)
	pwm_red = device_get_binding(CONFIG_APP_PWM_RED_DEV);
	if (!pwm_red) {
		SYS_LOG_ERR("Failed to get PWM device used for red");
		return -ENODEV;
	}
#endif
#if defined(CONFIG_APP_PWM_GREEN)
	pwm_green = device_get_binding(CONFIG_APP_PWM_GREEN_DEV);
	if (!pwm_green) {
		SYS_LOG_ERR("Failed to get PWM device used for green");
		return -ENODEV;
	}
#endif
#if defined(CONFIG_APP_PWM_BLUE)
	pwm_blue = device_get_binding(CONFIG_APP_PWM_BLUE_DEV);
	if (!pwm_blue) {
		SYS_LOG_ERR("Failed to get PWM device used for blue");
		return -ENODEV;
	}
#endif

	return 0;
}

void main(void)
{
	tstamp_hook_install();
	app_wq_init();

	/* Initial color: white */
	color_rgb[0] = color_rgb[1] = color_rgb[2] = 0xFF;

	SYS_LOG_INF("LWM2M Smart Light Bulb");
	SYS_LOG_INF("Device: %s, Serial: %x",
		    product_id_get()->name, product_id_get()->number);

	TC_PRINT("Initializing PWM devices\n");
	if (init_pwm_devices()) {
		_TC_END_RESULT(TC_FAIL, "init_pwm_devices");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "init_pwm_devices");

	TC_PRINT("Initializing LWM2M IPSO Light Control\n");
	if (lwm2m_engine_create_obj_inst("3311/0")) {
		_TC_END_RESULT(TC_FAIL, "init_ipso_light");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	if (lwm2m_engine_register_post_write_callback("3311/0/5850",
				on_off_cb)) {
		_TC_END_RESULT(TC_FAIL, "init_ipso_light");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	if (lwm2m_engine_set_u8("3311/0/5851", DIMMER_INITIAL)) {
		_TC_END_RESULT(TC_FAIL, "init_ipso_light");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	if (lwm2m_engine_register_post_write_callback("3311/0/5851",
				dimmer_cb)) {
		_TC_END_RESULT(TC_FAIL, "init_ipso_light");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	if (lwm2m_engine_set_string("3311/0/5701", COLOR_UNIT)) {
		_TC_END_RESULT(TC_FAIL, "init_ipso_light");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	if (lwm2m_engine_set_string("3311/0/5706", COLOR_WHITE)) {
		_TC_END_RESULT(TC_FAIL, "init_ipso_light");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	if (lwm2m_engine_register_post_write_callback("3311/0/5706",
				color_cb)) {
		_TC_END_RESULT(TC_FAIL, "init_ipso_light");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "init_ipso_light");
	TC_END_REPORT(TC_PASS);

	/* Set initial light state based on DIMMER_INITIAL */
	lwm2m_engine_set_bool("3311/0/5850", true);

	if (lwm2m_init()) {
		return;
	}

	/*
	 * From this point on, just handle work.
	 */
	app_wq_run();
}
