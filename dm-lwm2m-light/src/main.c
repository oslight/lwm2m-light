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

/* 100 is more than enough for it to be flicker free */
#define PWM_PERIOD (USEC_PER_SEC / 100)

/* Defines and configs for the IPSO elements */
#if defined(CONFIG_BOARD_96B_CARBON)
#define PWM_WHITE_DEV	CONFIG_PWM_STM32_3_DEV_NAME
#define PWM_WHITE_CH	1
#define PWM_RED_DEV	CONFIG_PWM_STM32_3_DEV_NAME
#define PWM_RED_CH	2
#define PWM_GREEN_DEV	CONFIG_PWM_STM32_3_DEV_NAME
#define PWM_GREEN_CH	3
#define PWM_BLUE_DEV	CONFIG_PWM_STM32_3_DEV_NAME
#define PWM_BLUE_CH	4
#else
#error "Choose supported board or add new board for the application"
#endif

/* Initial dimmer value (%) */
#define DIMMER_INITIAL	50

/* Support for up to 4 PWM devices */
static struct device *pwm_white;
static struct device *pwm_red;
static struct device *pwm_green;
static struct device *pwm_blue;

/* TODO: Extend to cover a scale factor for different voltage ranges */
static u32_t scale_pulse(u8_t dimmer)
{
	return PWM_PERIOD * dimmer / 100;
}

static int write_pwm_pin(struct device *pwm_dev, u32_t pwm_pin,
		         u32_t pulse_width)
{
	return pwm_pin_set_usec(pwm_dev, pwm_pin, PWM_PERIOD, pulse_width);
}

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int dimmer_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
		     bool last_block, size_t total_size)
{
	int ret = 0;
	bool on_off;
	u8_t dimmer;
	u32_t pulse;

	/* Only react if light is ON */
	on_off = lwm2m_engine_get_bool("3311/0/5850");
	if (!on_off) {
		SYS_LOG_DBG("Not updating PWM, light is OFF");
		return ret;
	}

	dimmer = *(u8_t *) data;
	if (dimmer < 0) {
		SYS_LOG_ERR("Invalid dimmer value, forcing it to 0");
		dimmer = 0;
	}
	if (dimmer > 100) {
		SYS_LOG_ERR("Invalid dimmer value, forcing it to 100");
		dimmer = 100;
	}

	pulse = scale_pulse(dimmer);

	/* TODO: Support other colors */
	ret = write_pwm_pin(pwm_white, PWM_WHITE_CH, pulse);
	if (ret) {
		SYS_LOG_ERR("Failed to write PWM pin");
		return ret;
	}

	return ret;
}

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int on_off_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
			 bool last_block, size_t total_size)
{
	int ret = 0;
	u8_t on_off, dimmer;
	u32_t pulse;

	on_off = *(u8_t *) data;

	/* Play safe until the engine can protect the range */
	dimmer = lwm2m_engine_get_u8("3311/0/5851");
	if (dimmer < 0) {
		dimmer = 0;
	}
	if (dimmer > 100) {
		dimmer = 100;
	}

	/* Pulse to write to the PWM pin */
	pulse = on_off * scale_pulse(dimmer);

	/* TODO: Support other colors */
	ret = write_pwm_pin(pwm_white, PWM_WHITE_CH, pulse);
	if (ret) {
		/*
		 * We need an extra hook in LWM2M to better handle
		 * failures before writing the data value and not in
		 * post_write_cb, as there is not much that can be
		 * done here.
		 */
		SYS_LOG_ERR("Failed to write PWM pin");
		return ret;
	}

	/* TODO: Move to be set by the IPSO object itself */
	lwm2m_engine_set_s32("3311/0/5852", 0);

	return ret;
}

static int init_pwm_devices(void)
{
#if defined(PWM_WHITE_DEV)
	pwm_white = device_get_binding(PWM_WHITE_DEV);
	if (!pwm_white) {
		SYS_LOG_ERR("Failed to get PWM device %s (white)",
				PWM_WHITE_DEV);
		return -ENODEV;
	}
#endif
#if defined(PWM_RED_DEV)
	pwm_red = device_get_binding(PWM_RED_DEV);
	if (!pwm_red) {
		SYS_LOG_ERR("Failed to get PWM device %s (red)",
				PWM_RED_DEV);
		return -ENODEV;
	}
#endif
#if defined(PWM_GREEN_DEV)
	pwm_green = device_get_binding(PWM_GREEN_DEV);
	if (!pwm_green) {
		SYS_LOG_ERR("Failed to get PWM device %s (green)",
				PWM_GREEN_DEV);
		return -ENODEV;
	}
#endif
#if defined(PWM_BLUE_DEV)
	pwm_blue = device_get_binding(PWM_BLUE_DEV);
	if (!pwm_blue) {
		SYS_LOG_ERR("Failed to get PWM device %s (blue)",
				PWM_BLUE_DEV);
		return -ENODEV;
	}
#endif

	return 0;
}

void main(void)
{
	tstamp_hook_install();
	app_wq_init();

	SYS_LOG_INF("LWM2M Smart Light Bulb");
	SYS_LOG_INF("Device: %s, Serial: %x",
		    product_id_get()->name, product_id_get()->number);

	TC_PRINT("Initializing LWM2M IPSO Light Control\n");
	if (init_pwm_devices()) {
		_TC_END_RESULT(TC_FAIL, "init_pwm_devices");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	lwm2m_engine_create_obj_inst("3311/0");
	lwm2m_engine_register_post_write_callback("3311/0/5850", on_off_cb);
	lwm2m_engine_set_u8("3311/0/5851", DIMMER_INITIAL);
	lwm2m_engine_register_post_write_callback("3311/0/5851", dimmer_cb);
	_TC_END_RESULT(TC_PASS, "init_pwm_devices");

	TC_PRINT("Initializing LWM2M Engine\n");
	if (lwm2m_init()) {
		_TC_END_RESULT(TC_FAIL, "lwm2m_init");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	_TC_END_RESULT(TC_PASS, "lwm2m_init");

	TC_END_REPORT(TC_PASS);

	/*
	 * From this point on, just handle work.
	 */
	app_wq_run();
}
