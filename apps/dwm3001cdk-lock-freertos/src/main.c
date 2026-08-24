/* SPDX-License-Identifier: ISC */

/*
 * Entry point for the DWM3001CDK lock.
 *
 * Startup order here is not arbitrary and is the thing to preserve as the rest
 * of the product is added below it.
 *
 * The radio starts first, before the scheduler. MPSL owns CLOCK and starts the
 * low-frequency crystal inside mpsl_init(); RTC1 carries the FreeRTOS tick and
 * counts from that same crystal. Starting the scheduler first would mean
 * starting it on a clock that is not running yet, which board/tick_freertos.c
 * turns into a named fatal rather than a hang, but which is still a board that
 * never boots.
 *
 * Creating tasks before the scheduler runs is legal, and the notifications
 * MPSL's low-priority handler posts in the meantime are latched and delivered
 * once it starts, so nothing is lost in the window.
 *
 * A failed radio start is fatal here rather than degraded. A lock with no radio
 * cannot be opened by any of the three ways this product supports, so coming up
 * far enough to log the reason and reset is more useful than coming up far
 * enough to look healthy.
 */
#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_gpio.h>

#include <FreeRTOS.h>
#include <task.h>

#include <ultrawidelock_prov.h>
#include <ultrawidelock/reader.h>

#include <openthread/instance.h>

#include <ultrawidelock_freertos_board.h>
#include <ultrawidelock_freertos_crypto.h>
#include <ultrawidelock_freertos_dfu.h>
#if ULTRAWIDELOCK_HAVE_PROV_CONSOLE
#include <ultrawidelock_freertos_usb.h>
#endif
#if ULTRAWIDELOCK_HAVE_MATTER
#include <matter_ble_freertos.h>
#include <matter_commission.h>
#endif
#include <ultrawidelock_freertos_nimble_host.h>
#include <ultrawidelock_freertos_openthread.h>
#include <ultrawidelock_freertos_platform.h>
#include <ultrawidelock_freertos_kv.h>
#include <ultrawidelock_freertos_radio.h>
#include <ultrawidelock_freertos_uwb.h>
#include <ultrawidelock/uwb.h>
#include <ultrawidelock_osal.h>

#include "grant.h"
#include "status_led.h"

#define MAIN_TAG "main"

/* The UWB bring-up trace's runtime gate (modules/ultrawidelock_uwb,
 * ultrawidelock_diag.h). */
extern volatile int ultrawidelock_uwb_diag_on;

/*
 * The housekeeping cadence, and how many of those passes make one log line.
 *
 * This is the floor, not the rate: the loop normally wakes on a range latch and
 * only falls back to this when none comes. 250 ms is inside
 * GRANT_RANGE_HOLD_MS, so a walk-up cannot start and finish between two passes
 * even if every latch were missed. The log stays at one a second.
 */
#define RUN_POLL_MS 250
#define RUN_LOG_EVERY 4

/*
 * Wake the grant loop on an accepted range latch.
 *
 * Runs on the UWB RX path, so it does nothing but give the semaphore. The
 * approach controller's float maths stays on the loop, where it competes with
 * no deadline; doing it here would put it inside the ~1836 us window the
 * DW3110 gives to arm a reply.
 */
static ultrawidelock_sem_t s_range_sig;

static void on_range_latched(void)
{
	ultrawidelock_sem_give(&s_range_sig);
}

/*
 * The reader identity and its trust store, static because they outlive the
 * boot task and are large enough that a stack copy would dominate it.
 */
static struct ultrawidelock_reader_identity s_identity;
static struct ultrawidelock_trust_store s_trust;

/* Latched in main() from SW2 before the scheduler starts; see the note there. */
static bool s_prov_mode;

/*
 * Written to GPREGRET2 to ask the NEXT boot for the provisioning console. The
 * value is arbitrary but must not be 0 or 0xFF: those are what the register
 * reads as when it has never been written and after some resets, and a magic
 * that collides with either would put the board into provisioning mode by
 * accident. 0x5A is neither, and is distinctive in a memory dump.
 */
#define ULTRAWIDELOCK_PROV_REQUEST_MAGIC 0x5Au

static StaticTask_t s_boot_tcb;

/*
 * 1,536 words, and the number is measured rather than chosen.
 *
 * This task carries three different peaks. The ordinary boot path high-waters
 * at 1,540 bytes, read off the fill pattern on a running board. The run loop
 * after it adds the approach controller's float maths. And in provisioning mode
 * the same stack runs ultrawidelock_freertos_prov_console_run(), whose `import` does a
 * software P-256 derive -- kilobytes, not the 508 bytes that were left.
 *
 * At 512 words it did not survive that. The import reset the board mid-transfer:
 * the USB device dropped off the bus, the console vanished, and the board came
 * back up on the DEV identity with the store untouched, which from outside
 * looks exactly like an import that was refused. There is no fault code to find
 * afterwards either, because the overflow hook resets and a reset clears CFSR.
 *
 * The console's own header already said this stack was "sized for the P-256
 * derive inside import". It never was; that comment described an intent nobody
 * implemented. It is now, with headroom to measure against rather than trust.
 *
 * The Matter build never runs the console, so its only peaks are the boot
 * path and the run loop: 2,604 B measured on hardware 2026-08-14 across a
 * commissioning restore and a full walk-up unlock. 1,024 words keeps 1.5 KB
 * over that; the console build keeps the import-proof depth.
 */
#if ULTRAWIDELOCK_HAVE_PROV_CONSOLE
static StackType_t s_boot_stack[1536];
#else
static StackType_t s_boot_stack[1024];
#endif

/*
 * What the board does once it is a lock: sample, decide, report, forever.
 *
 * The I/O lives here and the decision lives in grant.c, which is what makes a
 * walk-up something a test can write down. This function reads the radio,
 * notifies the reader, drives the lamps and logs; it makes no judgement of its
 * own about when a door should open.
 *
 * ONE SIGNAL IS STILL NOT ASSERTED, and it is a gap rather than a decision.
 * STATUS_LED_UNCOMMISSIONED needs a Matter fabric to ask about, and no Matter
 * image fits an nRF52833 -- which is the whole reason this board adopts a
 * credential over the USB console instead. So D12 stays dark unless something
 * failed.
 */
static void run_loop(void)
{
	struct grant_ctx grant;
	unsigned poll = 0;

	/*
	 * Both lines before the listener can fire: the semaphore has to exist
	 * before anything may give it, and the controller has to be initialised
	 * before a signal can reach it.
	 */
	ultrawidelock_sem_init(&s_range_sig, 0, 1);
	grant_init(&grant, ultrawidelock_uwb_range_generation());
	ultrawidelock_uwb_set_range_listener(on_range_latched);

	for (;;) {
		int64_t now_us = ultrawidelock_freertos_uptime_us();
		struct grant_input in = {0};
		struct grant_output out = {0};
		int32_t cm = 0;

		in.now_ms = now_us / 1000;
		in.gen = ultrawidelock_uwb_range_generation();
		in.trusted_valid = ultrawidelock_uwb_trusted_range_cm(&cm);
		in.trusted_cm = cm;
		cm = 0;
		in.raw_valid = ultrawidelock_uwb_last_range_cm(&cm);
		in.raw_cm = cm;
		in.session_active = ultrawidelock_reader_session_active();

		ultrawidelock_reader_status_tick(in.now_ms);

		grant_step(&grant, &in, &out);

		if (out.departure_fallback) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
					 "departure fallback: last fed %d cm "
					 "(gate: >= %d cm held for %d ms)",
					 (int)grant.approach.last_cm,
					 (int)grant.approach.cfg.relock_cm,
					 (int)grant.approach.cfg.far_silence_ms);
		}
		if (out.lock_changed) {
			ultrawidelock_reader_notify_unlock(out.unlocked);
			status_led_signal(STATUS_LED_UNLOCKED, out.unlocked);
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MAIN_TAG, "%s at %d cm",
					 out.unlocked ? "unlock" : "relock",
					 (int)grant.approach.last_cm);
		}

		status_led_signal(STATUS_LED_RANGING, out.ranging);
		status_led_signal(STATUS_LED_SESSION, in.session_active);

		if (++poll >= RUN_LOG_EVERY) {
			/*
			 * Seconds and milliseconds as two 32-bit values, NOT one
			 * %lld. This image links newlib-nano, whose printf does
			 * not implement the ll modifier: it consumes four bytes
			 * where eight were passed, so the %s after it takes the
			 * uptime's low word as a char *, memchr dereferences it,
			 * and the bus fault lands in default_handler and spins
			 * there -- stopping the tick, so the board goes silent
			 * rather than crashing visibly. The division is done
			 * here, where libgcc handles the 64-bit maths, instead
			 * of in a formatter that cannot.
			 */
			uint32_t up_s = (uint32_t)(now_us / 1000000);
			uint32_t up_ms = (uint32_t)((now_us / 1000) % 1000);

			poll = 0;
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MAIN_TAG,
					 "uptime %u.%03u s, radio %s, uwb %s", (unsigned)up_s,
					 (unsigned)up_ms,
					 ultrawidelock_freertos_radio_ready() ? "ready" : "down",
					 ultrawidelock_freertos_uwb_ready() ? "ready" : "down");
		}

		/*
		 * Wake on the next latch, or on the housekeeping tick if none
		 * comes. A latch that lands while this pass is still running
		 * leaves the semaphore given, so the take returns at once and no
		 * range waits for the next tick. A walk-up is therefore served
		 * at the ranging rate rather than at 4 Hz, while an idle board
		 * still costs four wakes a second.
		 */
		(void)ultrawidelock_sem_take(&s_range_sig, RUN_POLL_MS);
	}
}

static void boot_task(void *arg)
{
	(void)arg;

	ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MAIN_TAG, "controller pool used: %u bytes",
			 (unsigned)ultrawidelock_freertos_radio_memory_used());

	/*
	 * The lamps first, before anything that can fail.
	 *
	 * They are the only output a fielded board has -- RTT needs a probe and
	 * the exact ELF that was flashed, uart0 belongs to the J-Link OB, and
	 * the USB console exists only in provisioning mode. So the display has
	 * to be up before the steps that might want to report a fault on it,
	 * which is every step below this line.
	 *
	 * Here rather than in main() because the tick runs on the OSAL work
	 * queue, and that does not exist until the scheduler does.
	 */
	status_led_start();

	/*
	 * The persistent store, and the reader identity that lives in it.
	 *
	 * This runs on the scheduler because the flash driver arbitrates NVMC
	 * against the radio with MPSL timeslots and waits on them, and it runs
	 * before the BLE host because the identity is what the host will
	 * eventually advertise.
	 *
	 * Neither failure is fatal. ultrawidelock_kv_init() reformats a store it
	 * cannot read, and ultrawidelock_prov_load() yields a usable development
	 * identity on every failure path, because a reader that will not boot is
	 * worse than one that boots unprovisioned.
	 */
	if (ultrawidelock_kv_init() != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG, "key-value store unavailable");
	}
	if (ultrawidelock_prov_load(&s_identity, &s_trust) != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
				 "no stored identity; running on the development one");
	}

	/*
	 * Provisioning mode: SW2 held through reset.
	 *
	 * Sampled in main() before the scheduler, latched into s_prov_mode, and
	 * acted on here -- because this is where the store is up and where a
	 * task exists to block on a serial port.
	 *
	 * NOTHING GOES ON AIR on this path, and it does not return. An operator
	 * typing an identity into a lock that is also advertising it is the one
	 * situation where the half-written state is reachable from outside the
	 * board; refusing to be a lock at all while being provisioned removes the
	 * question. The way out is a reset without the button, which is also how
	 * the operator confirms the import took.
	 *
	 * ONE HONEST DIFFERENCE from the Zephyr image, whose provisioning mode
	 * starts no radio at all: MPSL is already running by the time this line
	 * is reached, because main() starts it before the scheduler and because
	 * USB needs the crystal that MPSL arbitrates. MPSL by itself transmits
	 * nothing -- the controller is never told to advertise, the 802.15.4
	 * driver and the DW3110 are never started, and the credential reader never
	 * runs. What the two images share is the property that matters: in
	 * provisioning mode the board is not reachable over any radio.
	 */
#if ULTRAWIDELOCK_HAVE_PROV_CONSOLE
	if (s_prov_mode) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MAIN_TAG,
				 "provisioning mode: radios stay down, console on USB");
		/* D10 solid: the one state an operator has to be able to see
		 * without a terminal, because it is the state in which the
		 * board deliberately answers nothing. */
		status_led_signal(STATUS_LED_PROV_MODE, true);
		if (ultrawidelock_freertos_usb_start() != 0) {
			ultrawidelock_freertos_fatal("provisioning console unavailable");
		}
		ultrawidelock_freertos_prov_console_run();
	}
#else
	/*
	 * Built without the console, which is what a Matter node is: the fabric
	 * arrives from a commissioner, so there is nothing to type in.
	 *
	 * SW2 STILL MEANS SOMETHING and it is not this. The button opens the DFU
	 * window later in this file, and it is read again there. What is gone is
	 * only the branch that would have answered a serial port, along with the
	 * 18,151 bytes of flash and 10,249 of RAM behind it.
	 *
	 * Saying so out loud costs one log line and removes the one way this can
	 * be misread on a bench: a board held on SW2 that comes up as a normal
	 * lock has not ignored the button, it was built without the mode.
	 */
	if (s_prov_mode) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
				 "SW2 held, but this image has no provisioning "
				 "console; continuing as a lock");
	}
#endif

	/*
	 * Thread.
	 *
	 * The radio driver comes up first and separately, because the two fail
	 * for different reasons and at different costs. Bringing up the nRF
	 * 802.15.4 driver is a peripheral claim that either succeeds or says
	 * which peripheral it lost; otInstanceInitSingle() after it is an
	 * allocation out of OpenThread's own pools. Reporting them as one step
	 * would turn "the radio is not there" and "the stack did not fit" into
	 * the same log line.
	 *
	 * Not fatal, for the same reason UWB is not: Thread is one of the paths
	 * into this lock, and BLE carries the others. A board whose Thread is
	 * down is still a lock.
	 */
	if (ultrawidelock_freertos_openthread_radio_start() != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
				 "802.15.4 radio unavailable; Thread will not run");
	} else {
		otInstance *ot = otInstanceInitSingle();

		if (ot == NULL) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MAIN_TAG,
					 "OpenThread instance allocation failed");
		} else if (ultrawidelock_freertos_openthread_start(ot) != 0) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MAIN_TAG,
					 "OpenThread task did not start");
		}
	}

	/*
	 * The DW3110, before the BLE host rather than after it.
	 *
	 * Ordering is for the bench, not for correctness: the two radios share
	 * no peripheral, so either order works. Bringing UWB up first means a
	 * board with a broken SPI line or an unconnected reset says so in the
	 * first few lines of the boot log, instead of after the BLE host has
	 * finished announcing itself.
	 *
	 * Not fatal, unlike the controller start in main(). A lock whose UWB is
	 * down still opens by the other two paths this product supports, and a
	 * board that refuses to boot cannot tell anyone which step failed.
	 */
	if (ultrawidelock_freertos_uwb_start() != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
				 "UWB unavailable; ranging will not be offered");
	}

	/*
	 * The update channel, registered BEFORE the reader and not after.
	 *
	 * That ordering is the whole reason this line is here rather than
	 * further down: ultrawidelock_reader_start() starts the NimBLE host, and every
	 * GATT service in this image has to be registered inside that startup
	 * sequence -- after the memory pools exist, before the host task begins
	 * consuming events. A layer that registered afterwards would be adding
	 * services to a running host.
	 *
	 * Not fatal. An update channel that failed to register leaves a board
	 * that still opens doors and can still be recovered over SWD, and
	 * refusing to boot over it would take away the working half too.
	 */
	if (dfu_ble_start() != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
				 "no update channel; this board can only be updated by cable");
	}

#if ULTRAWIDELOCK_HAVE_MATTER
	/*
	 * The 0xFFF6 commissioning transport, registered for the same reason and
	 * in the same window as the update channel above: before the reader
	 * starts the host, because a GATT service cannot be added to a running
	 * one.
	 *
	 * Not fatal, and for the same shape of reason. A board that fails to
	 * register this one is still a credential reader that opens doors; refusing
	 * to boot would take the working half away too.
	 *
	 * This call is also what puts the Matter code in the image at all.
	 * Nothing else references it, so without this line --gc-sections removes
	 * the protocol, the transport and the Thread glue, and every build check
	 * still passes -- which is exactly what the first Matter build did.
	 */
	if (matter_ble_start() != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
				 "no commissioning transport; this board cannot be added to a home");
	}
#endif

	/*
	 * The credential reader, which brings the BLE host up underneath itself.
	 *
	 * This is the whole product on one line: ultrawidelock_reader_start() does the
	 * crypto, loads the provisioned identity, arms the UWB ranging adapter,
	 * and hands its transport config to the port's ultrawidelock_ble_start(), which
	 * registers the GATT service and starts the controller and host.
	 *
	 * It runs on the scheduler, unlike the radio: the host has a task of its
	 * own, it waits for the controller to answer a reset, and NimBLE's
	 * porting layer allocates from the heap. None of that works before
	 * vTaskStartScheduler(), which is why this is here and not in main().
	 *
	 * Fatal, unlike UWB above. The reader is the product: a lock that cannot
	 * advertise cannot be unlocked by any path, so there is nothing to
	 * degrade to and a silent boot would be the worst outcome on a bench.
	 */
	if (ultrawidelock_reader_start() != 0) {
		/* D12 solid before the reset, so a board that cannot be a lock
		 * says so on the one output it still has. */
		status_led_signal(STATUS_LED_FAULT, true);
		ultrawidelock_freertos_fatal("credential reader start failed");
	}

#if ULTRAWIDELOCK_HAVE_MATTER
	/*
	 * The commissioning handlers, after the reader because they use the
	 * identity and the crypto it brought up.
	 *
	 * Nothing here touches a radio. Whether the board is DISCOVERABLE as a
	 * commissionable node is decided by the advertiser, which asks
	 * matter_commission_has_fabric() -- one advertising set carries the
	 * Matter payload or the credential tag, never both.
	 *
	 * Not fatal, and the asymmetry with the reader above is deliberate: a
	 * reader that cannot commission is still a lock that opens for a phone
	 * already enrolled. A bad verifier is refused per attempt rather than at
	 * startup for the same reason.
	 */
	if (matter_commission_init() != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MAIN_TAG,
				 "Matter commissioning unavailable; this board cannot join a fabric");
	}
#endif

	run_loop();
}

/*
 * Let the SW2 pull-up charge the pin, counted in instructions rather than time.
 *
 * NOT ultrawidelock_freertos_busy_wait_us(). That spins on RTC1, and RTC1 is not started
 * until the FreeRTOS port sets the tick up inside vTaskStartScheduler() -- so
 * calling it from here reaches time_freertos.c's ensure_clock(), which
 * correctly finds a counter that never advances and takes the fatal path. The
 * board then resets, MCUboot boots it again, and it resets again: a loop that
 * from outside looks like a bootloader that will not hand over, and says
 * nothing about why.
 *
 * That is not hypothetical. It is what this image did on the first boot ever
 * attempted on hardware, and the host tests could not have caught it because
 * their RTC fake always counts.
 *
 * ~6,400 cycles at 64 MHz is ~100 us, and the loop is deliberately crude: the
 * only requirement is a floor, the pin has microseconds of RC at most, and
 * nothing here can afford a dependency on a clock that is not running yet.
 */
static void settle_pullup(void)
{
	for (volatile uint32_t i = 0; i < 2000u; i++) {
		__NOP();
	}
}

int main(void)
{
	int err;

	/*
	 * Off before the radio comes up, exactly as the Zephyr application's
	 * main() does: the per-round DIAGK budget re-arms at every session
	 * start, so a shipping image would otherwise print synchronously from
	 * the ranging callbacks for each walk-up's first 16 rounds -- after the
	 * arm, but ahead of the next dispatch. The arm-leg histograms this
	 * build carries are plain statics, so the latency picture stays fully
	 * readable over J-Link with the trace off; a debugger can also write
	 * this variable back to 1 to re-enable the trace without reflashing.
	 */
	ultrawidelock_uwb_diag_on = 0;

	/*
	 * Crypto first, because it is the one bring-up step that can be retried:
	 * ultrawidelock_freertos_crypto_init() logs and returns non-zero rather than
	 * halting, so a transient PSA failure does not cost the whole boot. It
	 * needs no scheduler and no radio.
	 */
	(void)ultrawidelock_freertos_crypto_init();

	/*
	 * SW2, sampled here because "held through reset" can only be asked once
	 * and this is the first place that can ask it.
	 *
	 * Read before anything else claims a peripheral, and latched rather than
	 * re-read later: by the time the boot task runs, an operator who let go
	 * of the button would look like one who never pressed it.
	 *
	 * The pin has the module's pull-up and the switch shorts it to ground, so
	 * pressed reads low. The delay is for the pull-up to charge the pin
	 * capacitance after the input buffer is enabled -- without it the first
	 * read can return the floating level, which is a factory reset nobody
	 * asked for on the Zephyr image and a provisioning console nobody asked
	 * for here.
	 */
	nrf_gpio_cfg_input(ULTRAWIDELOCK_FREERTOS_PIN_SW2, NRF_GPIO_PIN_PULLUP);
	settle_pullup();
	s_prov_mode = (nrf_gpio_pin_read(ULTRAWIDELOCK_FREERTOS_PIN_SW2) == 0u);

	/*
	 * The same request, without a finger on the board.
	 *
	 * GPREGRET2 survives a soft reset and a watchdog reset but not a power
	 * cycle, which is exactly the lifetime this needs: a debugger or a future
	 * console command writes the magic, resets, and the next boot answers a
	 * serial port. Nothing else in this image or in MCUboot's configuration
	 * touches this register -- GPREGRET (the first one) is left alone because
	 * Nordic's own bootloaders claim it, and a collision here would look like
	 * a board that randomly refuses to run the application.
	 *
	 * CLEARED BEFORE IT IS ACTED ON, and that ordering is the whole safety
	 * argument. A board that entered provisioning mode and then lost power
	 * mid-session must come back as a lock, not as a console that answers
	 * nothing over the radio: leaving the magic set would make one operator
	 * mistake permanent until someone reflashed.
	 *
	 * It only ever ADDS the mode. Holding SW2 still works and still wins, so
	 * the documented recovery path is unchanged for anyone who does not have
	 * a debugger attached.
	 */
	if (NRF_POWER->GPREGRET2 == ULTRAWIDELOCK_PROV_REQUEST_MAGIC) {
		NRF_POWER->GPREGRET2 = 0u;
		s_prov_mode = true;
	}

	err = ultrawidelock_freertos_radio_start(ultrawidelock_freertos_radio_sdc_dispatcher());
	if (err != 0) {
		/*
		 * The value is the negated ultrawidelock_freertos_radio_stage that failed,
		 * which is the one piece of information worth carrying into the
		 * log: the stages fail for quite different reasons.
		 */
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MAIN_TAG, "radio start failed at stage %d",
				 -err);
		ultrawidelock_freertos_fatal("radio start failed");
	}

	(void)xTaskCreateStatic(boot_task, "boot",
				(uint32_t)(sizeof(s_boot_stack) / sizeof(s_boot_stack[0])), NULL, 1,
				s_boot_stack, &s_boot_tcb);

	vTaskStartScheduler();

	ultrawidelock_freertos_fatal("scheduler returned");
}
