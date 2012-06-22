/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* PWM control module for Chrome EC */

#include "board.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "lpc.h"
#include "ec_commands.h"
#include "pwm.h"
#include "registers.h"
#include "task.h"
#include "thermal.h"
#include "timer.h"
#include "util.h"

/* Maximum RPM for fan controller */
#define MAX_RPM 0x1fff
/* Max PWM for fan controller */
#define MAX_PWM 0x1ff
/* Scaling factor for requested/actual RPM for CPU fan.  We need this because
 * the fan controller on Blizzard filters tach pulses that are less than 64
 * 15625Hz ticks apart, which works out to ~7000rpm on an unscaled fan.  By
 * telling the controller we actually have twice as many edges per revolution,
 * the controller can handle fans that actually go twice as fast.  See
 * crosbug.com/p/7718. */
#define CPU_FAN_SCALE 2


/* Configures the GPIOs for the fan module. */
static void configure_gpios(void)
{
	/* PK6 alternate function 1 = channel 1 PWM */
	gpio_set_alternate_function(LM4_GPIO_K, 0x40, 1);
	/* PM6:7 alternate function 1 = channel 0 PWM/tach */
	gpio_set_alternate_function(LM4_GPIO_M, 0xc0, 1);
}


int pwm_enable_fan(int enable)
{
	if (enable)
		LM4_FAN_FANCTL |= (1 << FAN_CH_CPU);
	else
		LM4_FAN_FANCTL &= ~(1 << FAN_CH_CPU);

	return EC_SUCCESS;
}


int pwm_get_fan_rpm(void)
{
	return (LM4_FAN_FANCST(FAN_CH_CPU) & MAX_RPM) * CPU_FAN_SCALE;
}


int pwm_get_fan_target_rpm(void)
{
	return (LM4_FAN_FANCMD(FAN_CH_CPU) & MAX_RPM) * CPU_FAN_SCALE;
}


int pwm_set_fan_target_rpm(int rpm)
{
	/* Apply fan scaling */
	if (rpm > 0)
		rpm /= CPU_FAN_SCALE;

	/* Treat out-of-range requests as requests for maximum fan speed */
	if (rpm < 0 || rpm > MAX_RPM)
		rpm = MAX_RPM;

	LM4_FAN_FANCMD(FAN_CH_CPU) = rpm;
	return EC_SUCCESS;
}


int pwm_enable_keyboard_backlight(int enable)
{
	if (enable)
		LM4_FAN_FANCTL |= (1 << FAN_CH_KBLIGHT);
	else
		LM4_FAN_FANCTL &= ~(1 << FAN_CH_KBLIGHT);

	return EC_SUCCESS;
}


int pwm_get_keyboard_backlight_enabled(void)
{
	return (LM4_FAN_FANCTL & (1 << FAN_CH_KBLIGHT)) ? 1 : 0;
}


int pwm_get_keyboard_backlight(void)
{
	return ((LM4_FAN_FANCMD(FAN_CH_KBLIGHT) >> 16) * 100 +
		MAX_PWM / 2) / MAX_PWM;
}


int pwm_set_keyboard_backlight(int percent)
{
	LM4_FAN_FANCMD(FAN_CH_KBLIGHT) = ((percent * MAX_PWM + 50) / 100) << 16;
	return EC_SUCCESS;
}


static void update_lpc_mapped_memory(void)
{
	int i, r;
	uint16_t *mapped = (uint16_t *)(lpc_get_memmap_range() +
					EC_MEMMAP_FAN);

	for (i = 0; i < 4; ++i)
		mapped[i] = 0xffff;

	r = pwm_get_fan_rpm();

	/* Write fan speed. Or 0xFFFE for fan stalled. */
	if (r)
		mapped[0] = r;
	else
		mapped[0] = 0xfffe;
}


static void check_fan_failure(void)
{
	if (pwm_get_fan_target_rpm() != 0 &&
	    (LM4_FAN_FANCTL & (1 << FAN_CH_CPU)) &&
	    ((LM4_FAN_FANSTS >> (2 * FAN_CH_CPU)) & 0x03) == 0) {
		/* Fan enabled but stalled. Issues warning.
		 * As we have thermal shutdown protection, issuing warning
		 * here should be enough.
		 */
		lpc_set_host_events(
			EC_HOST_EVENT_MASK(EC_HOST_EVENT_THERMAL));
		cputs(CC_PWM, "[Fan stalled!]\n");
	}
}


void pwm_task(void)
{
	while (1) {
		check_fan_failure();
		update_lpc_mapped_memory();
		usleep(1000000);
	}
}


/*****************************************************************************/
/* Console commands */

static int command_fan_info(int argc, char **argv)
{
	ccprintf("Actual: %4d rpm\n", pwm_get_fan_rpm());
	ccprintf("Target: %4d rpm\n",
		 (LM4_FAN_FANCMD(FAN_CH_CPU) & MAX_RPM) * CPU_FAN_SCALE);
	ccprintf("Duty:   %d%%\n",
		 ((LM4_FAN_FANCMD(FAN_CH_CPU) >> 16)) * 100 / MAX_PWM);
	ccprintf("Status: %d\n",
		 (LM4_FAN_FANSTS >> (2 * FAN_CH_CPU)) & 0x03);
	ccprintf("Enable: %s\n",
		 LM4_FAN_FANCTL & (1 << FAN_CH_CPU) ? "yes" : "no");
	ccprintf("Power:  %s\n",
		 gpio_get_level(GPIO_PGOOD_5VALW) ? "yes" : "no");

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(faninfo, command_fan_info,
			NULL,
			"Print fan info",
			NULL);


static int command_fan_set(int argc, char **argv)
{
	int rpm = 0;
	char *e;

	if (argc < 2)
		return EC_ERROR_PARAM_COUNT;

	rpm = strtoi(argv[1], &e, 0);
	if (*e)
		return EC_ERROR_PARAM1;

        /* Move the fan to automatic control */
        if (LM4_FAN_FANCH(FAN_CH_CPU) & 0x0001) {
		pwm_enable_fan(0);
		LM4_FAN_FANCH(FAN_CH_CPU) &= ~0x0001;
        }
	/* Always enable the fan */
	pwm_enable_fan(1);

#ifdef CONFIG_TASK_THERMAL
	/* Disable thermal engine automatic fan control. */
	thermal_toggle_auto_fan_ctrl(0);
#endif

	return pwm_set_fan_target_rpm(rpm);
}
DECLARE_CONSOLE_COMMAND(fanset, command_fan_set,
			"rpm",
			"Set fan speed",
			NULL);

int pwm_set_fan_duty(int percent)
{
	int pwm;

	pwm = (MAX_PWM * percent) / 100;

        /* Move the fan to manual control */
        if (!(LM4_FAN_FANCH(FAN_CH_CPU) & 0x0001)) {
		pwm_enable_fan(0);
		LM4_FAN_FANCH(FAN_CH_CPU) |= 0x0001;
        }
	/* Always enable the fan */
	pwm_enable_fan(1);

#ifdef CONFIG_TASK_THERMAL
	/* Disable thermal engine automatic fan control. */
	thermal_toggle_auto_fan_ctrl(0);
#endif

        /* Set the duty cycle */
	LM4_FAN_FANCMD(FAN_CH_CPU) = pwm << 16;

	return EC_SUCCESS;
}

static int ec_command_fan_duty(int argc, char **argv)
{
	int percent = 0;
	char *e;

	if (argc < 2)
		return EC_ERROR_PARAM_COUNT;

	percent = strtoi(argv[1], &e, 0);
	if (*e)
		return EC_ERROR_PARAM1;

	ccprintf("Setting fan duty cycle to %d%%\n", percent);
	pwm_set_fan_duty(percent);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(fanduty, ec_command_fan_duty,
			"percent",
			"Set fan duty cycle",
			NULL);

static int command_kblight(int argc, char **argv)
{
	int rv = EC_SUCCESS;

	if (argc >= 2) {
		char *e;
		int i = strtoi(argv[1], &e, 0);
		if (*e)
			return EC_ERROR_PARAM1;
		rv = pwm_set_keyboard_backlight(i);
	}

	ccprintf("Keyboard backlight: %d%%\n", pwm_get_keyboard_backlight());
	return rv;
}
DECLARE_CONSOLE_COMMAND(kblight, command_kblight,
			"percent",
			"Set keyboard backlight",
			NULL);

/*****************************************************************************/
/* Initialization */

static int pwm_init(void)
{
	volatile uint32_t scratch  __attribute__((unused));

	/* Enable the fan module and delay a few clocks */
	LM4_SYSTEM_RCGCFAN = 1;
	scratch = LM4_SYSTEM_RCGCFAN;

	/* Configure GPIOs */
	configure_gpios();

	/* Disable all fans */
	LM4_FAN_FANCTL = 0;

	/* Configure CPU fan:
	 * 0x8000 = bit 15     = auto-restart
	 * 0x0000 = bit 14     = slow acceleration
	 * 0x0000 = bits 13:11 = no hysteresis
	 * 0x0000 = bits 10:8  = start period (2<<0) edges
	 * 0x0000 = bits 7:6   = no fast start
	 * 0x0020 = bits 5:4   = average 4 edges when calculating RPM
	 * 0x000c = bits 3:2   = 8 pulses per revolution
	 *                       (see note at top of file)
	 * 0x0000 = bit 0      = automatic control */
	LM4_FAN_FANCH(FAN_CH_CPU) = 0x802c;

	/* Configure keyboard backlight:
	 * 0x0000 = bit 15     = auto-restart
	 * 0x0000 = bit 14     = slow acceleration
	 * 0x0000 = bits 13:11 = no hysteresis
	 * 0x0000 = bits 10:8  = start period (2<<0) edges
	 * 0x0000 = bits 7:6   = no fast start
	 * 0x0000 = bits 5:4   = average 4 edges when calculating RPM
	 * 0x0000 = bits 3:2   = 4 pulses per revolution
	 * 0x0001 = bit 0      = manual control */
	LM4_FAN_FANCH(FAN_CH_KBLIGHT) = 0x0001;

	/* Set initial fan speed to maximum, backlight off */
	pwm_set_fan_target_rpm(-1);
	pwm_set_keyboard_backlight(0);

	/* Enable keyboard backlight.  Fan will be enabled later by whatever
	 * controls the fan power supply. */
	LM4_FAN_FANCTL |= (1 << FAN_CH_KBLIGHT);

	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_INIT, pwm_init, HOOK_PRIO_DEFAULT);


static int pwm_resume(void)
{
	pwm_enable_fan(1);
	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, pwm_resume, HOOK_PRIO_DEFAULT);


static int pwm_suspend(void)
{
	pwm_enable_fan(0);
	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, pwm_suspend, HOOK_PRIO_DEFAULT);
