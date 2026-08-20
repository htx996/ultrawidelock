/* SPDX-License-Identifier: ISC */

/*
 * uart-bridge — the second nRF52840 dongle as a USB-to-serial adapter.
 *
 * One dongle on the bench cannot speak (its boot dies before USB exists) but
 * its uart0 TX pad works from PRE_KERNEL_1. This image turns the OTHER dongle
 * into the adapter that listens: everything arriving on its uart0 RX pad goes
 * out its USB CDC console, and anything typed at the CDC console goes back out
 * its uart0 TX, so the target's pad console is fully usable both ways.
 *
 * Wiring, pad silkscreen to pad silkscreen, both dongles on USB power:
 *
 *   target 0.20 (its TX) -> bridge 0.24 (this RX)   the crash dump
 *   target 0.24 (its RX) <- bridge 0.20 (this TX)   optional: typing back
 *   target GND          <-> bridge GND              required
 *
 * Both ends are 115200 8N1, the dongle board's uart0 default.
 *
 * The LED is SOLID GREEN from the first moment: a bridge cannot be mistaken
 * for a witness image, whose trace never rests on green this early.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

static const struct device *const pads = DEVICE_DT_GET(DT_NODELABEL(uart0));
static const struct device *const host = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

RING_BUF_DECLARE(pads_to_host, 2048);
RING_BUF_DECLARE(host_to_pads, 256);

static void rx_isr(const struct device *dev, void *user_data)
{
	struct ring_buf *rb = user_data;
	uint8_t c;

	if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
		return;
	}
	while (uart_fifo_read(dev, &c, 1) == 1) {
		/* A full buffer drops the new byte rather than blocking the
		 * ISR; at 115200 the pump empties it 180 times faster than the
		 * wire can fill it. */
		(void)ring_buf_put(rb, &c, 1);
	}
}

static void pump(struct ring_buf *rb, const struct device *out)
{
	uint8_t c;

	while (ring_buf_get(rb, &c, 1) == 1) {
		uart_poll_out(out, c);
	}
}

int main(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(led1_green), okay)
	static const struct gpio_dt_spec led =
		GPIO_DT_SPEC_GET(DT_ALIAS(led1_green), gpios);

	if (gpio_is_ready_dt(&led)) {
		(void)gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	}
#endif

	if (!device_is_ready(pads) || !device_is_ready(host)) {
		return 0;
	}
	uart_irq_callback_user_data_set(pads, rx_isr, &pads_to_host);
	uart_irq_rx_enable(pads);
	uart_irq_callback_user_data_set(host, rx_isr, &host_to_pads);
	uart_irq_rx_enable(host);

	while (1) {
		pump(&pads_to_host, host);
		pump(&host_to_pads, pads);
		k_sleep(K_MSEC(1));
	}
	return 0;
}
