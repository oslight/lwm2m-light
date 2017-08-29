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
#include "neopixel.h"
#include "nrf_pwm.h"

/* Color Unit used by the IPSO object */
#define COLOR_UNIT	"hex"
#define COLOR_WHITE	"#FFFFFF"

/* Magic Timing Values */
#define MAGIC_T0H               6UL | (0x8000)
#define MAGIC_T1H              13UL | (0x8000)
#define CTOPVAL                20UL

#define NUMBYTES NEOPIXEL_COUNT * 4

struct device *flash_dev;
NRF_PWM_Type* pwm = NULL;
struct device *gpio = NULL;
uint32_t pattern_size  = NUMBYTES*8*sizeof(uint16_t)+2*sizeof(uint16_t);
uint32_t *pixels_pattern = NULL;

int neopixel_update_leds(uint8_t bit, uint32_t pattern_size, uint16_t loops)
{
	int ret = 0;
	uint16_t pos = 0;

        if ((pixels_pattern != NULL) && (pwm != NULL))
        {
                for( uint16_t n=0; n<loops; n++ )
                {
			for(uint8_t mask=0x80, i=0; mask>0; mask >>= 1, i++)
			{
				if (bit)
					pixels_pattern[pos] = MAGIC_T1H;
				else
					pixels_pattern[pos] = MAGIC_T0H;
			}
                        pos++;
                }
        } else {
                SYS_LOG_ERR("Pixel Pattern is NULL!");
                ret = 1;
                return ret;
        }

        pixels_pattern[++pos] = 0 | (0x8000); // Seq end
        pixels_pattern[++pos] = 0 | (0x8000); // Seq end

	pwm->MODE = (PWM_MODE_UPDOWN_Up << PWM_MODE_UPDOWN_Pos);

	pwm->PRESCALER = (PWM_PRESCALER_PRESCALER_DIV_1 << PWM_PRESCALER_PRESCALER_Pos);

	pwm->COUNTERTOP = (CTOPVAL << PWM_COUNTERTOP_COUNTERTOP_Pos);

	pwm->LOOP = (PWM_LOOP_CNT_Disabled << PWM_LOOP_CNT_Pos);

	pwm->DECODER = (PWM_DECODER_LOAD_Common << PWM_DECODER_LOAD_Pos) |
                       (PWM_DECODER_MODE_RefreshCount << PWM_DECODER_MODE_Pos);

        pwm->SEQ[0].PTR = (uint32_t)(pixels_pattern) << PWM_SEQ_PTR_PTR_Pos;

        pwm->SEQ[0].CNT = (pattern_size/sizeof(uint16_t)) << PWM_SEQ_CNT_CNT_Pos;

        pwm->SEQ[0].REFRESH  = 12;
        pwm->SEQ[0].ENDDELAY = 0;

        pwm->PSEL.OUT[0] = NEOPIXEL_PWM_PIN;

        pwm->ENABLE = 1;

        pwm->EVENTS_SEQEND[0]  = 0;
        pwm->TASKS_SEQSTART[0] = 1;

        while(!pwm->EVENTS_SEQEND[0]);

        pwm->EVENTS_SEQEND[0] = 0;

        pwm->ENABLE = 0;

        pwm->PSEL.OUT[0] = 0xFFFFFFFFUL;

	return ret;
}

void neopixel_clear(void)
{
        static uint8_t bit = 0;
	for( int n=0; n<1; n++ )
	{
		neopixel_update_leds(bit, pattern_size, NUMBYTES);
	}
}

/* TODO: Move to a pre write hook that can handle ret codes once available */
static int on_off_cb(u16_t obj_inst_id, u8_t *data, u16_t data_len,
                         bool last_block, size_t total_size)
{
        int ret = 0;
        u8_t on_off;

        on_off = *(u8_t *) data;
        if (on_off) {
                static uint8_t bit = 1;
		neopixel_update_leds(bit, pattern_size, NUMBYTES);
        } else {
		static uint8_t bit = 0;
		neopixel_update_leds(bit, pattern_size, NUMBYTES);
	}

        if (ret) {
                SYS_LOG_ERR("Failed to update light state");
                return ret;
        }

        /* TODO: Move to be set by the IPSO object itself */
        lwm2m_engine_set_s32("3311/0/5852", 0);

        return ret;
}


static int init_pwm_device(void)
{
	/* Set the pin low */
	gpio = device_get_binding(NEOPIXEL_PWM_PORT);
	gpio_pin_configure(gpio, NEOPIXEL_PWM_PIN, GPIO_DIR_OUT);
	gpio_pin_write(gpio, NEOPIXEL_PWM_PIN, LOW);
	/* Set up the PWM */
	NRF_PWM_Type* PWM[3] = {NRF_PWM0, NRF_PWM1, NRF_PWM2};
	for(int device = 0; device<3; device++)
	{
		if( (PWM[device]->ENABLE == 0)                            &&
		    (PWM[device]->PSEL.OUT[0] & PWM_PSEL_OUT_CONNECT_Msk) &&
		    (PWM[device]->PSEL.OUT[1] & PWM_PSEL_OUT_CONNECT_Msk) &&
		    (PWM[device]->PSEL.OUT[2] & PWM_PSEL_OUT_CONNECT_Msk) &&
		    (PWM[device]->PSEL.OUT[3] & PWM_PSEL_OUT_CONNECT_Msk)
		  ) {
			pwm = PWM[device];
			break;
		}
	}

	if (pwm == NULL)
	{
		return 1;
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

	pixels_pattern = (uint32_t *) k_malloc(sizeof(pattern_size));

	TC_PRINT("Initializing PWM device\n");
	if (init_pwm_device()) {
		_TC_END_RESULT(TC_FAIL, "init_pwm_device");
		TC_END_REPORT(TC_FAIL);
		return;
	}
	/* Force all PWM output pins to 0 */
	neopixel_clear();
	_TC_END_RESULT(TC_PASS, "init_pwm_device");

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
	_TC_END_RESULT(TC_PASS, "init_ipso_light");

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
