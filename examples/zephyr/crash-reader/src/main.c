/* SPDX-License-Identifier: ISC */

/*
 * crash-reader — prints the crash record the witness image burned to flash.
 *
 * Flash this AFTER the witness image has crashed (the LED fault loop is the
 * signal), open the USB CDC console, and the record repeats every two
 * seconds until it has been seen. The seq field says whether the record is
 * new: it climbs by one per crash, so a number you have read before means
 * the page was not rewritten since.
 *
 * LED: slow green blink, so this image cannot be confused with the witness
 * (whose trace never settles into a slow blink this early) or the bridge
 * (solid green).
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "crash_record.h"

static const char *reason_name(uint32_t r)
{
	switch (r) {
	case 0: return "CPU exception";
	case 2: return "stack check fail";
	case 3: return "kernel oops";
	case 4: return "kernel panic";
	case 25: return "bus fault, precise";
	case 26: return "bus fault, imprecise";
	default: return "other";
	}
}

int main(void)
{
	const struct crash_record *rec =
		(const struct crash_record *)CRASH_RECORD_ADDR;
	bool on = false;

#if DT_NODE_HAS_STATUS(DT_ALIAS(led1_green), okay)
	static const struct gpio_dt_spec led =
		GPIO_DT_SPEC_GET(DT_ALIAS(led1_green), gpios);
	bool have_led = gpio_is_ready_dt(&led) &&
			gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) == 0;
#else
	bool have_led = false;
#endif

	while (1) {
		if (rec->magic != CRASH_RECORD_MAGIC) {
			printk("crash-reader: no record at 0x%08x (page reads 0x%08x)\n",
			       CRASH_RECORD_ADDR, rec->magic);
		} else {
			printk("crash-reader: record seq=%u\n", rec->seq);
			printk("  reason %u (%s)\n", rec->reason,
			       reason_name(rec->reason));
			printk("  cfsr   0x%08x\n", rec->cfsr);
			printk("  hfsr   0x%08x\n", rec->hfsr);
			printk("  bfar   0x%08x\n", rec->bfar);
			printk("  mmfar  0x%08x\n", rec->mmfar);
			printk("  pc     0x%08x\n", rec->pc);
			printk("  lr     0x%08x\n", rec->lr);
			printk("  xpsr   0x%08x\n", rec->xpsr);
			printk("  thread %.15s\n", rec->thread);
		}
		if (have_led) {
			on = !on;
#if DT_NODE_HAS_STATUS(DT_ALIAS(led1_green), okay)
			(void)gpio_pin_set_dt(&led, on ? 1 : 0);
#endif
		}
		k_sleep(K_SECONDS(2));
	}
	return 0;
}
