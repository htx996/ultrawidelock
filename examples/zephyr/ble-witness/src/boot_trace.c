/* SPDX-License-Identifier: ISC */

/*
 * Boot trace for a board with no probe and no console.
 *
 * WHY THIS EXISTS. The nRF52840 dongle has no debug connector. When its
 * firmware dies before USB enumerates there is no console, no RTT and no way
 * to attach: the board is a brick that tells you nothing, and every diagnosis
 * costs a DFU cycle and a guess. MEASURED 2026-08-20: a stock Zephyr blinky
 * boots on this dongle over the same DFU path that leaves the witness image
 * dark, so the image is entered and dies somewhere in init.
 *
 * The four LEDs are the only output the board has left. Each is lit at a
 * different point in the boot and never cleared, so the pattern still showing
 * when everything stops says how far it got:
 *
 *   LD1 green   PRE_KERNEL_1   the image is entered and running C
 *   LD2 red     POST_KERNEL    drivers and the kernel came up
 *   LD2 green   APPLICATION    every SYS_INIT ran, including USB and settings
 *   LD2 blue    main()         the application itself started
 *
 * All four, then the app's own pattern, means init is not the problem.
 *
 * Registers, not the GPIO driver: PRE_KERNEL_1 runs before the driver is
 * initialised, and an instrument that depends on the thing it is measuring
 * measures nothing. Pins are ACTIVE_LOW on this board, so clearing drives the
 * LED on.
 *
 * BENCH ONLY. CONFIG_WITNESS_BOOT_TRACE defaults n and this file compiles to
 * nothing without it; the LEDs belong to the status indication in steady
 * state, and two owners for one lamp is worse than no instrument.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <hal/nrf_gpio.h>

/* Board pins, from nrf52840dongle_nrf52840_common.dtsi. Absolute numbering:
 * P1.x is 32 + x, which is what nrf_gpio_* takes. */
#define TRACE_P0_06 6U       /* LD1 green   */
#define TRACE_P0_08 8U       /* LD2 red     */
#define TRACE_P1_09 (32U + 9U) /* LD2 green */
#define TRACE_P0_12 12U      /* LD2 blue    */

static void trace_on(uint32_t pin)
{
	nrf_gpio_cfg_output(pin);
	nrf_gpio_pin_clear(pin); /* ACTIVE_LOW: clear lights it */
}

static int trace_pre_kernel(void)
{
	trace_on(TRACE_P0_06);
	return 0;
}

static int trace_post_kernel(void)
{
	trace_on(TRACE_P0_08);
	return 0;
}

static int trace_application(void)
{
	trace_on(TRACE_P1_09);
	return 0;
}

void witness_boot_trace_main(void)
{
	trace_on(TRACE_P0_12);
}

/*
 * Priority 0 at each level, so the mark lands BEFORE anything else at that
 * level rather than after whatever is about to fault.
 */
SYS_INIT(trace_pre_kernel, PRE_KERNEL_1, 0);
SYS_INIT(trace_post_kernel, POST_KERNEL, 0);
SYS_INIT(trace_application, APPLICATION, 0);
