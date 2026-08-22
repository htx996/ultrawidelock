/* SPDX-License-Identifier: ISC */

/*
 * Internal flash on NVMC, arbitrated against the radio with MPSL timeslots.
 *
 * Programming this flash stalls the CPU. A word write is tens of microseconds
 * and a page erase is 89.7 ms, which is longer than MPSL will ever hand out in
 * one piece, so an operation issued while the radio is scheduled would overrun
 * a radio event. With the radio down there is nothing to arbitrate and the work
 * is done directly; with it up the work is cut into pieces and each piece runs
 * inside a timeslot.
 *
 * A write fills as much of its slot as it measures room for. An erase uses the
 * controller's partial-erase mode, which lets a page be erased in fixed slices
 * with the CPU free between them: 89.7 ms of erasing in 3 ms pieces, thirty of
 * them, rather than one stall that would drop a BLE connection.
 *
 * Two things differ from Nordic's own Zephyr driver, deliberately. It gives a
 * kernel semaphore from inside the timeslot callback; this port does not,
 * because that callback runs at interrupt priority zero and FreeRTOS forbids
 * its API above configMAX_SYSCALL_INTERRUPT_PRIORITY. The callback here touches
 * only NVMC and a flag, and the calling task polls that flag. Flash operations
 * are rare and already tens of milliseconds, so a one-tick poll costs nothing
 * and removes the question rather than reasoning about it.
 *
 * Writes and erases are confined to a window that excludes the application
 * image. A store with an offset bug can then lose its own data but cannot erase
 * the firmware out from under a door lock.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hal/nrf_nvmc.h>
#include <hal/nrf_timer.h>
#include <mpsl_timeslot.h>
#include <nrfx.h>

#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>

#include <ultrawidelock_freertos_platform.h>
#include <ultrawidelock_freertos_radio.h>
#include <ultrawidelock_freertos_uwb.h>

#define FLASH_TAG "flash"

/* The part's whole program flash. Reads are allowed anywhere inside it. */
#define FLASH_DEVICE_BASE 0x00000000u
#define FLASH_DEVICE_SIZE (512u * 1024u)

/*
 * What may be written or erased. The default is the four pages the key-value
 * store owns; a board that adds a DFU slot widens it deliberately.
 *
 * AND ONE DID, WITHOUT WIDENING IT. The image links a DFU receiver, advertises
 * an update channel, opens a window on SW2 and accepts the connection -- and
 * then could not write one byte, because patch_staging begins at 0x74000 and
 * every write to it was refused here. The only symptom a user gets is the push
 * tool printing "a flash write or erase failed", which reads as a bad part
 * rather than as a window that was never opened for the partition. MEASURED on
 * hardware 2026-08-15: BEGIN accepted, staging erase refused, error 6.
 *
 * The target build now passes the staging base in, from the same linker script
 * that places the partition (board/nrf52833_lock.ld), so the two cannot drift.
 * The default below is what a build with no DFU slot wants, and it is also what
 * the host suite compiles against. ultrawidelock_flash_area_open() checks the
 * partition against this window at start-up so a future mismatch is a boot-time
 * line rather than a failed update months later.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_BASE
#define ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_BASE 0x0007c000u
#endif
#ifndef ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_LIMIT
#define ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_LIMIT 0x00080000u
#endif

/*
 * A page erase takes 89.7 ms on this part, from the SoC's own timing. Partial
 * erase splits that into slices; thirty 3 ms slices cover it with the CPU free
 * in between. Three milliseconds is Nordic's own default and is above the two
 * the controller requires.
 */
#define FLASH_PAGE_ERASE_TOTAL_US 89700u
#define FLASH_PARTIAL_ERASE_MS 3u
#define FLASH_PARTIAL_ERASE_US (FLASH_PARTIAL_ERASE_MS * 1000u)
#define FLASH_PARTIAL_ERASE_SLICES                                                                 \
	((FLASH_PAGE_ERASE_TOTAL_US + FLASH_PARTIAL_ERASE_US - 1u) / FLASH_PARTIAL_ERASE_US)

/* How much of a timeslot a write may use, matching Nordic's own driver. */
#define FLASH_SLOT_WRITE_US 7500u

/*
 * Slack on top of the work itself. The slot has to cover entry, the callback,
 * and the request for the next one, not just the programming.
 */
#define FLASH_SLOT_SLACK_US 1000u

/*
 * How long a whole operation may take before the caller gives up. Generous on
 * purpose: under a busy radio a two-page erase is sixty slices, each waiting
 * its turn, and failing early would report an I/O error for an operation that
 * was merely slow.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_FLASH_TIMEOUT_MS
#define ULTRAWIDELOCK_FREERTOS_FLASH_TIMEOUT_MS 10000u
#endif

/*
 * How long an operation may be HELD BACK while a UWB ranging session is live,
 * before it runs anyway. The timeslots MPSL grants for flash are arbitrated
 * against BLE and 802.15.4 only; the UWB slot schedule is invisible to them,
 * and NVMC work stalls the CPU outright, so a grant can land a 3 ms erase
 * slice or a multi-millisecond write burst inside the ~1,836 us window between
 * a DW3110 frame and its armed response. Every caller here is a settings
 * persist with no deadline of its own, and a walk-up session ends within a few
 * seconds, so waiting for its idle is nearly free. The wait is still bounded,
 * because a phone parked in range keeps a session listening indefinitely and a
 * persist deferred forever is data lost to the next power cut -- for a lock,
 * the worse trade.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_FLASH_DEFER_MS
#define ULTRAWIDELOCK_FREERTOS_FLASH_DEFER_MS 5000u
#endif
#define FLASH_DEFER_POLL_MS 10u

/*
 * Weak so this file keeps linking where the UWB layer does not: the host tests
 * compile it against fakes, and an image without the radio stack never has a
 * session to defer for. uwb/ultrawidelock_freertos_uwb.c provides the real
 * answer.
 */
__attribute__((weak)) bool ultrawidelock_freertos_uwb_ranging_active(void)
{
	return false;
}

/* Hold the operation until the ranging session ends, or the bound expires. */
static void flash_defer_for_ranging(void)
{
	TickType_t deadline;

	if (__get_IPSR() != 0u || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		return; /* nothing to yield from; run_op() already handles both contexts */
	}
	if (!ultrawidelock_freertos_uwb_ranging_active()) {
		return;
	}
	deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ULTRAWIDELOCK_FREERTOS_FLASH_DEFER_MS);
	while (ultrawidelock_freertos_uwb_ranging_active()) {
		if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, FLASH_TAG,
						   "ranging still live after %u ms; flash work proceeds",
						   (unsigned)ULTRAWIDELOCK_FREERTOS_FLASH_DEFER_MS);
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(FLASH_DEFER_POLL_MS));
	}
}

/*
 * ONE WRITER AT A TIME, and this is not defensive programming.
 *
 * Everything below this line is module state: the operation in flight, the
 * timeslot session id, the request and the done/failed flags. There is also
 * exactly ONE MPSL timeslot session, because mpsl_timeslot_session_count_set()
 * below asks for one -- so a second task entering while the first holds the
 * session does not merely interleave the statics, it gets a hard failure from
 * mpsl_timeslot_session_open().
 *
 * WHICH IS WHAT HAPPENED. Two subsystems persist independently and neither
 * knows about the other: OpenThread writes its SRP key and PSA ITS records from
 * the Thread task, and the Matter handler writes the reader identity from the
 * commissioning path. During commissioning they overlap, and on 2026-08-14 the
 * collision landed exactly on SetAliroReaderConfig:
 *
 *   invoke: endpoint 1 cluster 0x0101 command 0x0028, 144 B fields
 *   E flash: timeslot session open failed
 *   W ultrawidelock_prov: kv set rc=-4
 *   E reader identity NOT stored (-4)
 *
 * and the controller, having provisioned a reader the device then failed to
 * keep, removed the fabric and reported "unable to add accessory". Writes
 * before and after it succeeded, which is what makes an unserialised race look
 * like flaky hardware.
 *
 * A mutex rather than a critical section: an erase can hold this for the whole
 * ULTRAWIDELOCK_FREERTOS_FLASH_TIMEOUT_MS, and blocking the scheduler for that long would
 * take the radio down with it. Priority inheritance comes with FreeRTOS mutexes
 * and is wanted here, since the Thread task outranks the work queue.
 */
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;

/*
 * Created on first use inside a critical section rather than from an init hook,
 * because the first flash access happens while settings are loaded during
 * bring-up and adding an ordering requirement between that and a new init call
 * is a worse trade than one uncontended critical section.
 */
static SemaphoreHandle_t flash_lock(void)
{
	if (s_lock == NULL) {
		taskENTER_CRITICAL();
		if (s_lock == NULL) {
			s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
		}
		taskEXIT_CRITICAL();
	}
	return s_lock;
}

/*
 * Locking is skipped before the scheduler runs and inside an interrupt. Neither
 * can contend: nothing else is running yet in the first case, and the second is
 * already refused by run_op() whenever the radio is up, which is the only time
 * the session matters.
 *
 * Returns 0 to proceed and -1 to give up. *held says whether a release is owed,
 * and the two are deliberately separate: "no lock was needed" and "the lock
 * could not be had" both mean not-held, and treating them alike would let a
 * timed-out caller walk into the shared state it just failed to reserve.
 */
static int flash_lock_take(bool *held)
{
	SemaphoreHandle_t lock = flash_lock();

	*held = false;
	if (lock == NULL || __get_IPSR() != 0u ||
	    xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		return 0;
	}
	/*
	 * Bounded rather than portMAX_DELAY. A holder always finishes -- it
	 * polls against its own deadline -- so waiting longer than two of those
	 * means something is wrong that waiting will not fix, and a caller that
	 * is told no can report a failed write instead of never returning.
	 */
	if (xSemaphoreTake(lock, pdMS_TO_TICKS(2u * ULTRAWIDELOCK_FREERTOS_FLASH_TIMEOUT_MS)) != pdTRUE) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, FLASH_TAG,
				 "another task has held the flash for %u ms",
				 (unsigned)(2u * ULTRAWIDELOCK_FREERTOS_FLASH_TIMEOUT_MS));
		return -1;
	}
	*held = true;
	return 0;
}

static void flash_lock_give(bool held)
{
	if (held) {
		(void)xSemaphoreGive(s_lock);
	}
}

/* One session's worth of MPSL context, aligned as the API requires. */
static uint32_t s_timeslot_context[(MPSL_TIMESLOT_CONTEXT_SIZE + 3u) / 4u];
static bool s_sessions_configured;

static mpsl_timeslot_session_id_t s_session;
static mpsl_timeslot_request_t s_request;
static mpsl_timeslot_signal_return_param_t s_return;

/*
 * The operation in flight. Written by the calling task before the first
 * request and then only by the timeslot callback, which is why it is volatile:
 * the task polls s_done and reads s_failed without any lock, and the two
 * contexts never run at the same time because the task does nothing but wait.
 */
struct flash_op {
	bool erase;
	uint32_t offset;
	const uint8_t *data;
	size_t remaining;
	unsigned slices;
};

static volatile struct flash_op s_op;
static volatile bool s_done;
static volatile bool s_failed;

/*
 * Bring-up instrumentation, off by default.
 *
 * The timeslot callback runs at interrupt priority zero, where logging is not
 * safe, so it only counts. The task-context wait prints the counts, which is
 * enough to tell "MPSL never granted a slot" from "the slices are running and
 * the operation is merely slow" -- the two failures that look identical from
 * outside, because both leave the caller sitting in the same poll loop.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_FLASH_DIAG
#define ULTRAWIDELOCK_FREERTOS_FLASH_DIAG 0
#endif

#if ULTRAWIDELOCK_FREERTOS_FLASH_DIAG
static volatile uint32_t s_sig_start;
static volatile uint32_t s_sig_idle;
static volatile uint32_t s_sig_retry;
static volatile uint32_t s_sig_other;
#define FLASH_DIAG_COUNT(counter) ((counter)++)
#else
#define FLASH_DIAG_COUNT(counter) ((void)0)
#endif

/*
 * The address a flash offset maps to. On the part they are the same, because
 * program flash is memory-mapped from zero and reads never touch the
 * controller. The host test points this at its model, which is the only way to
 * exercise the read path off target.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_FLASH_MAPPED
#define ULTRAWIDELOCK_FREERTOS_FLASH_MAPPED(offset) ((const void *)(uintptr_t)(offset))
#endif

static bool in_device(uint32_t offset, size_t length)
{
	if (length == 0u) {
		return true;
	}
	/*
	 * Only the upper bound is tested. The device base is zero on this part,
	 * so a lower-bound test on an unsigned offset can never fail, and
	 * writing one anyway is a warning at target-compile time rather than
	 * defence. The static assertion keeps the omission honest: if the base
	 * ever moves, this stops compiling instead of silently accepting
	 * anything below it.
	 */
	_Static_assert(FLASH_DEVICE_BASE == 0u,
		       "a non-zero flash base needs a lower-bound check here");
	if (offset > FLASH_DEVICE_BASE + FLASH_DEVICE_SIZE) {
		return false;
	}
	return length <= (FLASH_DEVICE_BASE + FLASH_DEVICE_SIZE) - offset;
}

static bool in_writable_window(uint32_t offset, size_t length)
{
	if (offset < ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_BASE) {
		return false;
	}
	return length <= ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_LIMIT - offset;
}

bool ultrawidelock_freertos_flash_writable(uint32_t offset, size_t length)
{
	return in_writable_window(offset, length);
}

/* Spin until the controller has finished. NVMC stalls the CPU regardless. */
static void wait_ready(void)
{
	while (!nrf_nvmc_ready_check(NRF_NVMC)) {
		/* Spin. */
	}
}

/*
 * Microseconds since this timeslot began. MPSL resets TIMER0 at the start
 * signal and documents it as readable inside the slot, which is how the work
 * knows when to stop without assuming a per-word programming time.
 */
static uint32_t slot_elapsed_us(void)
{
	nrf_timer_task_trigger(NRF_TIMER0, NRF_TIMER_TASK_CAPTURE0);
	return nrf_timer_cc_get(NRF_TIMER0, NRF_TIMER_CC_CHANNEL0);
}

/* Program one word of the pending write. */
static void write_one_word(void)
{
	uint32_t word;

	/*
	 * Assembled rather than dereferenced. The caller is not required to
	 * align its buffer, and casting a byte pointer to uint32_t would break
	 * strict aliasing and rely on the core tolerating an unaligned load;
	 * this core tolerates one for a plain LDR but not for the
	 * multiple-register forms a compiler is free to emit here.
	 */
	memcpy(&word, (const void *)&s_op.data[0], sizeof(word));
	nrf_nvmc_word_write(s_op.offset, word);
	wait_ready();

	s_op.offset += ULTRAWIDELOCK_FREERTOS_FLASH_WRITE_ALIGN;
	s_op.data += ULTRAWIDELOCK_FREERTOS_FLASH_WRITE_ALIGN;
	s_op.remaining -= ULTRAWIDELOCK_FREERTOS_FLASH_WRITE_ALIGN;
}

/* Issue one partial erase slice on the page the operation is standing on. */
static void erase_one_slice(void)
{
	nrf_nvmc_page_partial_erase_start(NRF_NVMC, s_op.offset);
	wait_ready();

	s_op.slices++;
	if (s_op.slices >= FLASH_PARTIAL_ERASE_SLICES) {
		s_op.slices = 0;
		s_op.offset += ULTRAWIDELOCK_FREERTOS_FLASH_PAGE_SIZE;
		s_op.remaining -= ULTRAWIDELOCK_FREERTOS_FLASH_PAGE_SIZE;
	}
}

/*
 * Do as much of the operation as fits, and report whether it is finished.
 *
 * bounded says whether there is a deadline: inside a timeslot there is, and a
 * write stops once another word would not fit. With the radio down there is
 * none and the whole operation runs in one pass.
 */
static bool op_step(bool bounded)
{
	if (s_op.erase) {
		/*
		 * nRF52833 has partial erase but no partial-erase WEN mode: the
		 * MDK defines NVMC_ERASEPAGEPARTIALCFG_DURATION but not
		 * NVMC_CONFIG_WEN_PEen, so the pinned HAL compiles
		 * NRF_NVMC_MODE_PARTIAL_ERASE out of the enum entirely on this
		 * part. The duration register and ERASEPAGEPARTIAL still work;
		 * they are driven from the ordinary erase mode.
		 *
		 * The mode exists on the nRF52840 and the nRF91 series, so the
		 * choice is made by the HAL's own capability symbol rather than
		 * hardcoded, and this file stays correct if it is ever built for
		 * one of those.
		 */
#if NRF_NVMC_HAS_PARTIAL_ERASE_MODE
		nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_PARTIAL_ERASE);
#else
		nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_ERASE);
#endif
		do {
			erase_one_slice();
		} while (s_op.remaining > 0u && !bounded);
	} else {
		nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_WRITE);
		while (s_op.remaining > 0u) {
			write_one_word();
			if (bounded) {
				/*
				 * Stop when another word of the same cost would
				 * not fit. Measuring beats assuming a
				 * programming time this file cannot verify.
				 */
				uint32_t now = slot_elapsed_us();
				uint32_t per_word = now / (s_op.slices + 1u);

				s_op.slices++;
				if (now + per_word >= FLASH_SLOT_WRITE_US) {
					break;
				}
			}
		}
	}

	nrf_nvmc_mode_set(NRF_NVMC, NRF_NVMC_MODE_READONLY);
	wait_ready();
	return s_op.remaining == 0u;
}

/*
 * Runs at interrupt priority zero. It must not call FreeRTOS, must not block
 * beyond the slot it was given, and must return a pointer that outlives it,
 * which is why the return parameter is static.
 */
static mpsl_timeslot_signal_return_param_t *timeslot_callback(mpsl_timeslot_session_id_t session_id,
							      uint32_t signal)
{
	(void)session_id;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		FLASH_DIAG_COUNT(s_sig_start);
		if (op_step(true)) {
			s_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		} else {
			s_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST;
			s_return.params.request.p_next = &s_request;
		}
		return &s_return;

	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		/* Every request is done, so the operation is done. */
		FLASH_DIAG_COUNT(s_sig_idle);
		s_done = true;
		return NULL;

	case MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED:
		return NULL;

	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		/*
		 * The radio took the slot. Ask again rather than failing: the
		 * work is half done and abandoning it here would leave a
		 * partially erased page, which reads as neither old nor new.
		 */
		FLASH_DIAG_COUNT(s_sig_retry);
		if (mpsl_timeslot_request(s_session, &s_request) != 0) {
			s_failed = true;
			s_done = true;
		}
		return NULL;

	default:
		FLASH_DIAG_COUNT(s_sig_other);
		s_failed = true;
		s_done = true;
		return NULL;
	}
}

static int run_under_timeslot(uint32_t slot_us)
{
	TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ULTRAWIDELOCK_FREERTOS_FLASH_TIMEOUT_MS);
	int rc = 0;

	if (!s_sessions_configured) {
		if (mpsl_timeslot_session_count_set(s_timeslot_context, 1) != 0) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, FLASH_TAG,
					 "no MPSL timeslot session available");
			return -1;
		}
		s_sessions_configured = true;
	}

	if (mpsl_timeslot_session_open(timeslot_callback, &s_session) != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, FLASH_TAG,
					   "timeslot session open failed");
		return -1;
	}

	s_request.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST;
	s_request.params.earliest.hfclk = MPSL_TIMESLOT_HFCLK_CFG_NO_GUARANTEE;
	s_request.params.earliest.priority = MPSL_TIMESLOT_PRIORITY_NORMAL;
	s_request.params.earliest.length_us = slot_us + FLASH_SLOT_SLACK_US;
	s_request.params.earliest.timeout_us = MPSL_TIMESLOT_EARLIEST_TIMEOUT_MAX_US;

	s_done = false;
	s_failed = false;

#if ULTRAWIDELOCK_FREERTOS_FLASH_DIAG
	s_sig_start = 0;
	s_sig_idle = 0;
	s_sig_retry = 0;
	s_sig_other = 0;
	ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, FLASH_TAG, "%s %u bytes at 0x%x, slot %u us",
			 s_op.erase ? "erase" : "write", (unsigned)s_op.remaining,
			 (unsigned)s_op.offset, (unsigned)slot_us);
#endif

	if (mpsl_timeslot_request(s_session, &s_request) != 0) {
		mpsl_timeslot_session_close(s_session);
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, FLASH_TAG,
					   "timeslot request failed");
		return -1;
	}

	/*
	 * Poll rather than block on a semaphore. The callback that finishes the
	 * operation runs at interrupt priority zero, where FreeRTOS API calls
	 * are not permitted, so it cannot wake anything.
	 */
	while (!s_done) {
		if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, FLASH_TAG,
					 "timed out waiting for a timeslot");
			rc = -1;
			break;
		}
		vTaskDelay(1);
	}

#if ULTRAWIDELOCK_FREERTOS_FLASH_DIAG
	ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, FLASH_TAG,
			 "rc=%d start=%u idle=%u retry=%u other=%u left=%u", rc,
			 (unsigned)s_sig_start, (unsigned)s_sig_idle, (unsigned)s_sig_retry,
			 (unsigned)s_sig_other, (unsigned)s_op.remaining);
#endif

	/* Closing cancels anything still pending. */
	mpsl_timeslot_session_close(s_session);

	if (rc == 0 && s_failed) {
		rc = -1;
	}
	return rc;
}

/* Run the pending operation, arbitrated if the radio is up. */
static int run_op(uint32_t slot_us)
{
	if (!ultrawidelock_freertos_radio_ready()) {
		(void)op_step(false);
		return 0;
	}
	if (__get_IPSR() != 0u) {
		/*
		 * The wait needs a task to yield from, and a flash operation at
		 * interrupt priority would stall the radio anyway.
		 */
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, FLASH_TAG,
				 "refused: flash from an interrupt while the radio is up");
		return -1;
	}
	return run_under_timeslot(slot_us);
}

int ultrawidelock_freertos_flash_read(uint32_t offset, void *buffer, size_t length)
{
	if (buffer == NULL && length != 0u) {
		return -1;
	}
	if (!in_device(offset, length)) {
		return -1;
	}
	/* Flash is memory-mapped for reading; the controller is not involved. */
	memcpy(buffer, ULTRAWIDELOCK_FREERTOS_FLASH_MAPPED(offset), length);
	return 0;
}

int ultrawidelock_freertos_flash_write(uint32_t offset, const void *data, size_t length)
{
	if (data == NULL && length != 0u) {
		return -1;
	}
	if (!in_device(offset, length) || !in_writable_window(offset, length)) {
		return -1;
	}
	if ((offset % ULTRAWIDELOCK_FREERTOS_FLASH_WRITE_ALIGN) != 0u ||
	    (length % ULTRAWIDELOCK_FREERTOS_FLASH_WRITE_ALIGN) != 0u) {
		return -1;
	}
	if (length == 0u) {
		return 0;
	}

	/* Taken BEFORE s_op is touched: the operation record is the shared
	 * state, not just the timeslot session. The ranging defer runs first,
	 * outside the lock, so a held-back writer does not also stall a later
	 * one for the whole defer bound on top of its own. */
	bool held;
	int rc;

	flash_defer_for_ranging();
	if (flash_lock_take(&held) != 0) {
		return -1;
	}
	s_op.erase = false;
	s_op.offset = offset;
	s_op.data = data;
	s_op.remaining = length;
	s_op.slices = 0;
	rc = run_op(FLASH_SLOT_WRITE_US);
	flash_lock_give(held);
	return rc;
}

int ultrawidelock_freertos_flash_erase(uint32_t offset, size_t length)
{
	if (!in_device(offset, length) || !in_writable_window(offset, length)) {
		return -1;
	}
	if ((offset % ULTRAWIDELOCK_FREERTOS_FLASH_PAGE_SIZE) != 0u ||
	    (length % ULTRAWIDELOCK_FREERTOS_FLASH_PAGE_SIZE) != 0u) {
		return -1;
	}
	if (length == 0u) {
		return 0;
	}

	bool held;
	int rc;

	flash_defer_for_ranging();
	if (flash_lock_take(&held) != 0) {
		return -1;
	}
	s_op.erase = true;
	s_op.offset = offset;
	s_op.data = NULL;
	s_op.remaining = length;
	s_op.slices = 0;

	/*
	 * Partial erase needs its slice length programmed once. The controller
	 * keeps it, but setting it per operation costs nothing and means a
	 * board that never calls anything else still gets the right value.
	 */
	nrf_nvmc_partial_erase_duration_set(NRF_NVMC, FLASH_PARTIAL_ERASE_MS);
	rc = run_op(FLASH_PARTIAL_ERASE_US);
	flash_lock_give(held);
	return rc;
}
