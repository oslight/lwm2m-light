# Copyright (c) 2017 Linaro Limited
#
# SPDX-License-Identifier: Apache-2.0
#

config APP_PWM_WHITE
	bool
	prompt "Enable dedicated PWM for white color"
	default y

if APP_PWM_WHITE

config APP_PWM_WHITE_DEV
	string
	prompt "PWM device name used for white"

config APP_PWM_WHITE_PIN
	int
	prompt "PWM pin number used for white"
	default 0

config APP_PWM_WHITE_PIN_CEILING
	int
	prompt "PWM pin level ceiling"
	default 255
	range 1 255

endif # APP_PWM_WHITE

config APP_PWM_RED
	bool
	prompt "Enable dedicated PWM for red"
	default n

if APP_PWM_RED

config APP_PWM_RED_DEV
	string
	prompt "PWM device name used for red"

config APP_PWM_RED_PIN
	int
	prompt "PWM pin number used for red"
	default 0

config APP_PWM_RED_PIN_CEILING
	int
	prompt "PWM pin level ceiling"
	default 255
	range 1 255

endif # APP_PWM_RED

config APP_PWM_GREEN
	bool
	prompt "Enable dedicated PWM for green"
	default n

if APP_PWM_GREEN

config APP_PWM_GREEN_DEV
	string
	prompt "PWM device name used for green"

config APP_PWM_GREEN_PIN
	int
	prompt "PWM pin number used for green"
	default 0

config APP_PWM_GREEN_PIN_CEILING
	int
	prompt "PWM pin level ceiling"
	default 255
	range 1 255

endif # APP_PWM_GREEN

config APP_PWM_BLUE
	bool
	prompt "Enable dedicated PWM for blue"
	default n

if APP_PWM_BLUE

config APP_PWM_BLUE_DEV
	string
	prompt "PWM device name used for blue"

config APP_PWM_BLUE_PIN
	int
	prompt "PWM pin number used for blue"
	default 0

config APP_PWM_BLUE_PIN_CEILING
	int
	prompt "PWM pin level ceiling"
	default 255
	range 1 255

endif # APP_PWM_BLUE
