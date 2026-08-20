/* SPDX-License-Identifier: ISC */

/*
 * Boot trace for a board with no probe and no console.
 *
 * WHY THIS EXISTS. The nRF52840 dongle has no debug connector. Firmware that
 * dies before USB enumerates leaves nothing to attach to: no console, no RTT,
 * no fault dump. MEASURED 2026-08-20: a stock Zephyr blinky boots on this
 * dongle over the same DFU path that leaves the witness image dark, so the
 * image is entered and dies somewhere in init. The LEDs are the only output
 * the board has left.
 *
 * ONE LAMP, ONE COLOUR. The RGB LED (LD2) shows the last milestone passed and
 * nothing else; LD1 is left dark on purpose. An earlier version lit LEDs
 * cumulatively and the reading came back "red and green", which was two
 * different answers depending on which lamp the reader meant. A single colour
 * cannot be miscounted.
 *
 *   dark      the image was never entered
 *   red       PRE_KERNEL_1
 *   green     POST_KERNEL, before anything else at that level
 *   blue      past P_40   SoftDevice Controller, MPSL, CC310
 *   yellow    past P_50   workqueue, flash, USB device controller, mbedTLS
 *   magenta   past P_90   PSA crypto, USB core, net core
 *   cyan      all of POST_KERNEL   OpenThread, 802.15.4, BT host
 *   white     main() was entered
 *
 * The priorities come from the init order in this image's own map file, so the
 * bands correspond to real groups of drivers rather than round numbers.
 *
 * Registers, not the GPIO driver: the first mark lands before that driver is
 * initialised, and an instrument that depends on the thing it measures
 * measures nothing.
 *
 * BENCH ONLY. CONFIG_WITNESS_BOOT_TRACE defaults n and this file compiles to
 * nothing without it; in steady state these LEDs belong to the status
 * indication, and two owners for one lamp is worse than no instrument.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <hal/nrf_gpio.h>

/* LD2's three channels, from nrf52840dongle_nrf52840_common.dtsi. Absolute pin
 * numbering: P1.x is 32 + x. All ACTIVE_LOW, so clearing lights one. */
#define LD2_RED   8U
#define LD2_GREEN (32U + 9U)
#define LD2_BLUE  12U

#define C_RED     0x1U
#define C_GREEN   0x2U
#define C_BLUE    0x4U
#define C_YELLOW  (C_RED | C_GREEN)
#define C_MAGENTA (C_RED | C_BLUE)
#define C_CYAN    (C_GREEN | C_BLUE)
#define C_WHITE   (C_RED | C_GREEN | C_BLUE)

static void colour(uint32_t mask)
{
	static const uint32_t pin[3] = { LD2_RED, LD2_GREEN, LD2_BLUE };

	for (unsigned int i = 0; i < 3u; i++) {
		nrf_gpio_cfg_output(pin[i]);
		if ((mask & (1u << i)) != 0u) {
			nrf_gpio_pin_clear(pin[i]); /* ACTIVE_LOW: on */
		} else {
			nrf_gpio_pin_set(pin[i]);
		}
	}
}

static int mark_pre_kernel(void)
{
	colour(C_RED);
	return 0;
}

static int mark_post_kernel(void)
{
	colour(C_GREEN);
	return 0;
}

static int mark_past_40(void)
{
	colour(C_BLUE);
	return 0;
}

static int mark_past_50(void)
{
	colour(C_YELLOW);
	return 0;
}

static int mark_past_90(void)
{
	colour(C_MAGENTA);
	return 0;
}

static int mark_application(void)
{
	colour(C_CYAN);
	return 0;
}

void witness_boot_trace_main(void)
{
	colour(C_WHITE);
}

SYS_INIT(mark_pre_kernel, PRE_KERNEL_1, 0);
SYS_INIT(mark_post_kernel, POST_KERNEL, 0);
SYS_INIT(mark_past_40, POST_KERNEL, 45);
SYS_INIT(mark_past_50, POST_KERNEL, 55);
SYS_INIT(mark_past_90, POST_KERNEL, 95);
SYS_INIT(mark_application, APPLICATION, 0);
