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
#include <net/lwm2m.h>
#include <tc_util.h>

/* Local helpers and functions */
#include "tstamp_log.h"
#include "app_work_queue.h"
#include "mcuboot.h"
#include "product_id.h"
#include "lwm2m.h"

/* Defines and configs for the IPSO elements */
#define LED_GPIO_PIN		LED0_GPIO_PIN
#define LED_GPIO_PORT		LED0_GPIO_PORT

static struct device *led_dev;
static u32_t led_current;

struct device *flash_dev;

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int led_on_off_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
			 bool last_block, size_t total_size)
{
	int ret = 0;
	u32_t led_val;

	led_val = *(u8_t *) data;
	if (led_val != led_current) {
		ret = gpio_pin_write(led_dev, LED_GPIO_PIN, led_val);
		if (ret) {
			/*
			 * We need an extra hook in LWM2M to better handle
			 * failures before writing the data value and not in
			 * post_write_cb, as there is not much that can be
			 * done here.
			 */
			SYS_LOG_ERR("Fail to write to GPIO %d", LED_GPIO_PIN);
			return ret;
		}
		led_current = led_val;
		/* TODO: Move to be set by the IPSO object itself */
		lwm2m_engine_set_s32("3311/0/5852", 0);
	}

	return ret;
}

static int init_led_device(void)
{
	int ret;

	led_dev = device_get_binding(LED_GPIO_PORT);
	SYS_LOG_INF("%s LED GPIO port %s",
			led_dev ? "Found" : "Did not find",
			LED_GPIO_PORT);

	if (!led_dev) {
		SYS_LOG_ERR("No LED device found.");
		return -ENODEV;
	}

	ret = gpio_pin_configure(led_dev, LED_GPIO_PIN, GPIO_DIR_OUT);
	if (ret) {
		SYS_LOG_ERR("Error configuring LED GPIO.");
		return ret;
	}

	ret = gpio_pin_write(led_dev, LED_GPIO_PIN, 0);
	if (ret) {
		SYS_LOG_ERR("Error setting LED GPIO.");
		return ret;
	}

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
	if (init_led_device()) {
		_TC_END_RESULT(TC_FAIL, "init_led_device");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	lwm2m_engine_create_obj_inst("3311/0");
	lwm2m_engine_register_post_write_callback("3311/0/5850",
			led_on_off_cb);
	_TC_END_RESULT(TC_PASS, "init_led_device");

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
