/*
 * The board's flash hooks on NVMC.
 *
 * The controller model refuses a write outside write mode, an erase outside
 * erase mode, a write that would set a bit, and a misaligned address. Those are
 * the rules a driver gets wrong once and then corrupts flash with, so the test
 * asserts the violation counter stays at zero as well as asserting the results.
 *
 * The radio-arbitration half is not implemented, and what is checked here is
 * that its absence is a refusal rather than a write issued underneath a live
 * radio.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <hal/nrf_nvmc.h>
#include <hal/nrf_timer.h>
#include <mpsl_timeslot.h>

#include <FreeRTOS.h>
#include <task.h>

#include <ultrawidelock_freertos_platform.h>

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(label, condition)                                                                    \
	do {                                                                                       \
		g_checks++;                                                                        \
		if (!(condition)) {                                                                \
			g_failures++;                                                              \
			printf("  FAIL %s\n", (label));                                            \
		} else {                                                                           \
			printf("  ok   %s\n", (label));                                            \
		}                                                                                  \
	} while (0)

static unsigned g_error_logs;

void ultrawidelock_freertos_log(enum ultrawidelock_freertos_log_level level, const char *tag,
				const char *fmt, ...)
{
	(void)tag;
	(void)fmt;
	if (level == ULTRAWIDELOCK_FREERTOS_LOG_ERROR) {
		g_error_logs++;
	}
}

/* The radio's readiness is what decides whether arbitration is needed. */
static bool g_radio_ready;

bool ultrawidelock_freertos_radio_ready(void)
{
	return g_radio_ready;
}

/* The key-value store's four pages, which are the default writable window. */
#define KV_BASE 0x7c000u
#define KV_LIMIT 0x80000u
#define PAGE 4096u

static void reset_all(void)
{
	fake_nvmc_reset();
	fake_timer_reset();
	fake_timeslot_reset();
	fake_task_set_tick_count(0);
	g_radio_ready = false;
	g_error_logs = 0;
}

/* ---- reads -------------------------------------------------------------- */

static void test_read(void)
{
	uint8_t out[16];

	reset_all();
	memcpy(&fake_nvmc_flash[KV_BASE], "provisioned", 11);

	memset(out, 0, sizeof(out));
	CHECK("a read succeeds", ultrawidelock_freertos_flash_read(KV_BASE, out, 11) == 0);
	CHECK("a read returns what is stored", memcmp(out, "provisioned", 11) == 0);

	/* Reads are allowed anywhere in the device, not just the write window. */
	CHECK("a read outside the write window succeeds",
	      ultrawidelock_freertos_flash_read(0x1000u, out, 16) == 0);

	CHECK("a read past the end of the part is refused",
	      ultrawidelock_freertos_flash_read(FAKE_NVMC_FLASH_SIZE - 4u, out, 16) != 0);
	CHECK("a null read buffer is refused", ultrawidelock_freertos_flash_read(KV_BASE, NULL, 4) != 0);
	CHECK("a zero-length read succeeds", ultrawidelock_freertos_flash_read(KV_BASE, out, 0) == 0);

	CHECK("reading never touched the controller", fake_nvmc.mode_changes == 0u);
	CHECK("reading broke no controller rule", fake_nvmc.violations == 0u);
}

/* ---- writes ------------------------------------------------------------- */

static void test_write(void)
{
	static const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t out[8];

	reset_all();

	CHECK("a write succeeds", ultrawidelock_freertos_flash_write(KV_BASE, data, sizeof(data)) == 0);
	CHECK("a write lands in flash", memcmp(&fake_nvmc_flash[KV_BASE], data, sizeof(data)) == 0);
	CHECK("a write programs one word at a time", fake_nvmc.word_writes == 2u);
	CHECK("a write broke no controller rule", fake_nvmc.violations == 0u);

	/*
	 * The controller must be left in read mode. Leaving it in write mode
	 * turns any later stray store into a flash program.
	 */
	CHECK("the controller is left read-only", fake_nvmc.mode == NRF_NVMC_MODE_READONLY);

	CHECK("a write reads back", ultrawidelock_freertos_flash_read(KV_BASE, out, sizeof(out)) == 0 &&
					    memcmp(out, data, sizeof(data)) == 0);

	/* Clearing further bits is legal; setting one is not, and the model knows. */
	{
		static const uint8_t clear_more[4] = { 0, 0, 0, 0 };

		CHECK("clearing more bits succeeds",
		      ultrawidelock_freertos_flash_write(KV_BASE, clear_more, sizeof(clear_more)) == 0);
		CHECK("clearing more bits broke no rule", fake_nvmc.violations == 0u);
	}
}

static void test_write_rejections(void)
{
	static const uint8_t data[8] = { 0 };

	reset_all();

	CHECK("an unaligned write offset is refused",
	      ultrawidelock_freertos_flash_write(KV_BASE + 1u, data, 4) != 0);
	CHECK("a write length that is not whole words is refused",
	      ultrawidelock_freertos_flash_write(KV_BASE, data, 3) != 0);
	CHECK("a null write buffer is refused", ultrawidelock_freertos_flash_write(KV_BASE, NULL, 4) != 0);
	CHECK("a zero-length write succeeds", ultrawidelock_freertos_flash_write(KV_BASE, data, 0) == 0);

	/*
	 * The window is what stops a store with an offset bug erasing the
	 * firmware out from under a door lock.
	 */
	CHECK("a write below the writable window is refused",
	      ultrawidelock_freertos_flash_write(KV_BASE - 4u, data, 4) != 0);
	CHECK("a write past the writable window is refused",
	      ultrawidelock_freertos_flash_write(KV_LIMIT - 4u, data, 8) != 0);
	CHECK("a write into the application image is refused",
	      ultrawidelock_freertos_flash_write(0x8000u, data, 4) != 0);

	CHECK("no rejected write reached the controller", fake_nvmc.word_writes == 0u);
	CHECK("no rejected write changed the mode", fake_nvmc.mode_changes == 0u);
	CHECK("no rejected write broke a rule", fake_nvmc.violations == 0u);

	/*
	 * The predicate the partition owners ask before they promise anything.
	 *
	 * It exists because the window is a build-time constant and the
	 * partitions are a linker one, and when they disagreed the DFU receiver
	 * advertised an update channel it could not write a byte to: the failure
	 * surfaced as "a flash write or erase failed" during a real update,
	 * months from the decision that caused it. Asked at start-up instead.
	 *
	 * This suite compiles the DEFAULT window -- the store's four pages -- so
	 * the staging partition at 0x74000 is correctly outside it here. The
	 * target build widens the base to cover staging; what is pinned here is
	 * that the predicate agrees with the window it was compiled against,
	 * whatever that window is.
	 */
	CHECK("the writable predicate accepts the store's own pages",
	      ultrawidelock_freertos_flash_writable(KV_BASE, KV_LIMIT - KV_BASE));
	CHECK("and agrees with the write path below the window",
	      !ultrawidelock_freertos_flash_writable(KV_BASE - 4u, 4u));
	CHECK("and past it", !ultrawidelock_freertos_flash_writable(KV_LIMIT - 4u, 8u));
	CHECK("and over the application image",
	      !ultrawidelock_freertos_flash_writable(0x8000u, 4u));
	/* A partition the default window does not cover: the DFU staging area,
	 * which is exactly the case that shipped broken. */
	CHECK("a staging partition below the window is reported unwritable",
	      !ultrawidelock_freertos_flash_writable(0x74000u, 0x8000u));
}

/* ---- erases ------------------------------------------------------------- */

static void test_erase(void)
{
	uint8_t out[4];

	reset_all();
	memset(&fake_nvmc_flash[KV_BASE], 0x00, PAGE);

	CHECK("an erase succeeds", ultrawidelock_freertos_flash_erase(KV_BASE, PAGE) == 0);
	CHECK("an erase leaves ones", ultrawidelock_freertos_flash_read(KV_BASE, out, sizeof(out)) == 0 &&
					     out[0] == 0xffu && out[3] == 0xffu);
	CHECK("an erase covers one page per call", fake_nvmc.page_erases == 1u);
	CHECK("the controller is left read-only after an erase",
	      fake_nvmc.mode == NRF_NVMC_MODE_READONLY);
	CHECK("an erase broke no controller rule", fake_nvmc.violations == 0u);

	/* Both pages at once, which is what a compaction asks for. */
	reset_all();
	CHECK("a two-page erase succeeds", ultrawidelock_freertos_flash_erase(KV_BASE, 2u * PAGE) == 0);
	CHECK("a two-page erase erases both", fake_nvmc.page_erases == 2u);
}

static void test_erase_rejections(void)
{
	reset_all();

	CHECK("an unaligned erase offset is refused",
	      ultrawidelock_freertos_flash_erase(KV_BASE + 4u, PAGE) != 0);
	CHECK("an erase length that is not whole pages is refused",
	      ultrawidelock_freertos_flash_erase(KV_BASE, PAGE + 4u) != 0);
	CHECK("a zero-length erase succeeds", ultrawidelock_freertos_flash_erase(KV_BASE, 0) == 0);
	CHECK("an erase below the writable window is refused",
	      ultrawidelock_freertos_flash_erase(KV_BASE - PAGE, PAGE) != 0);
	CHECK("an erase past the writable window is refused",
	      ultrawidelock_freertos_flash_erase(KV_BASE, 5u * PAGE) != 0);
	CHECK("an erase of the application image is refused",
	      ultrawidelock_freertos_flash_erase(0x8000u, PAGE) != 0);

	CHECK("no rejected erase reached the controller", fake_nvmc.page_erases == 0u);
	CHECK("no rejected erase changed the mode", fake_nvmc.mode_changes == 0u);
}

/* ---- arbitration -------------------------------------------------------- */

/*
 * Programming this flash stalls the CPU for far longer than a radio event can
 * wait, so with the radio up the work is cut into pieces and each piece runs
 * inside an MPSL timeslot.
 */
static void test_write_under_timeslot(void)
{
	/*
	 * Big enough to outlast one slot on purpose: 512 words at the model's
	 * 50 us a word is 25.6 ms, and a write slot is 7.5 ms.
	 */
	static uint8_t data[2048];
	static uint8_t out[2048];
	unsigned i;

	reset_all();
	for (i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)i;
	}
	g_radio_ready = true;

	CHECK("a write under a live radio succeeds",
	      ultrawidelock_freertos_flash_write(KV_BASE, data, sizeof(data)) == 0);
	CHECK("the write landed", memcmp(&fake_nvmc_flash[KV_BASE], data, sizeof(data)) == 0);
	CHECK("the write read back", ultrawidelock_freertos_flash_read(KV_BASE, out, sizeof(out)) == 0 &&
					    memcmp(out, data, sizeof(data)) == 0);
	CHECK("the write went through a timeslot", fake_timeslot_grants > 0u);
	CHECK("the write broke no controller rule", fake_nvmc.violations == 0u);
	CHECK("the session was opened once", fake_timeslot_opens == 1u);
	CHECK("the session was closed again", fake_timeslot_closes == 1u);
	CHECK("the controller is left read-only", fake_nvmc.mode == NRF_NVMC_MODE_READONLY);

	/*
	 * The request has to be one MPSL will accept: earliest, and inside the
	 * hundred milliseconds it will ever grant in one piece.
	 */
	CHECK("the request asked for the earliest slot",
	      fake_timeslot_last_request_type == MPSL_TIMESLOT_REQ_TYPE_EARLIEST);
	CHECK("the request fits what MPSL will grant",
	      fake_timeslot_last_length_us <= MPSL_TIMESLOT_LENGTH_MAX_US);
	CHECK("the request did not demand the crystal",
	      fake_timeslot_last_hfclk == MPSL_TIMESLOT_HFCLK_CFG_NO_GUARANTEE);
	CHECK("no request was malformed", fake_timeslot_violations == 0u);

	/*
	 * The point of measuring the slot clock: 128 words cannot fit in one
	 * 7500 us slot at the model's 50 us a word, so the work has to have
	 * asked for more than one.
	 */
	CHECK("a write too long for one slot took several", fake_timeslot_grants >= 4u);
}

/* A page erase is 89.7 ms, which MPSL will never grant in one piece. */
static void test_erase_under_timeslot(void)
{
	uint8_t out[4];

	reset_all();
	memset(&fake_nvmc_flash[KV_BASE], 0x00, PAGE);
	g_radio_ready = true;

	CHECK("an erase under a live radio succeeds",
	      ultrawidelock_freertos_flash_erase(KV_BASE, PAGE) == 0);
	CHECK("the erase completed", ultrawidelock_freertos_flash_read(KV_BASE, out, sizeof(out)) == 0 &&
					    out[0] == 0xffu && out[3] == 0xffu);
	CHECK("the erase was sliced", fake_nvmc.partial_slices > 1u);
	CHECK("the erase used partial erase, not a blocking one",
	      fake_nvmc.page_erases == 1u && fake_nvmc.partial_slices >= 30u);
	CHECK("each slice had its own timeslot", fake_timeslot_grants == fake_nvmc.partial_slices);
	CHECK("the slice length was programmed",
	      nrf_nvmc_partial_erase_duration_get(NRF_NVMC) >= 2u);
	CHECK("no slice exceeded what MPSL will grant",
	      fake_timeslot_last_length_us <= MPSL_TIMESLOT_LENGTH_MAX_US);
	CHECK("the erase broke no controller rule", fake_nvmc.violations == 0u);
}

/*
 * The radio can take a slot away. Abandoning the work there would leave a
 * partially erased page, which reads as neither the old contents nor the new,
 * so a blocked request has to be retried rather than failed.
 */
static void test_blocked_requests_are_retried(void)
{
	static const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	reset_all();
	g_radio_ready = true;
	fake_timeslot_blocks_before_grant = 3;

	CHECK("a write survives the radio taking the slot",
	      ultrawidelock_freertos_flash_write(KV_BASE, data, sizeof(data)) == 0);
	CHECK("the write still landed",
	      memcmp(&fake_nvmc_flash[KV_BASE], data, sizeof(data)) == 0);
	CHECK("the blocked requests were retried", fake_timeslot_requests > 1u);
	CHECK("nothing was programmed outside a slot", fake_nvmc.violations == 0u);
}

/* A radio that never yields must end as a timeout, not a hang. */
static void test_timeslot_never_granted(void)
{
	static const uint8_t data[4] = { 1, 2, 3, 4 };

	reset_all();
	g_radio_ready = true;
	fake_timeslot_never_grants = true;

	CHECK("a write that never gets a slot fails",
	      ultrawidelock_freertos_flash_write(KV_BASE, data, sizeof(data)) != 0);
	CHECK("a timeout is logged as an error", g_error_logs > 0u);
	/*
	 * Within its budget, not merely eventually. The tick here costs no real
	 * time, so a timeout that fired after the counter wrapped would look
	 * identical to one that respected its deadline; on the board that is the
	 * difference between ten seconds and twenty-five days.
	 */
	CHECK("the timeout fired inside its budget", xTaskGetTickCount() <= 11000u);
	CHECK("nothing was programmed", fake_nvmc.word_writes == 0u);
	CHECK("the session was still closed", fake_timeslot_closes == 1u);
	CHECK("the flash was left untouched", fake_nvmc_flash[KV_BASE] == 0xffu);
}

/* The MPSL calls that can fail have to be reported, not ignored. */
static void test_session_failures(void)
{
	static const uint8_t data[4] = { 1, 2, 3, 4 };

	reset_all();
	g_radio_ready = true;
	fake_timeslot_open_fails = true;
	CHECK("a session that will not open fails the write",
	      ultrawidelock_freertos_flash_write(KV_BASE, data, sizeof(data)) != 0);
	CHECK("nothing was programmed without a session", fake_nvmc.word_writes == 0u);

	reset_all();
	g_radio_ready = true;
	fake_timeslot_request_fails = true;
	CHECK("a request that will not be accepted fails the write",
	      ultrawidelock_freertos_flash_write(KV_BASE, data, sizeof(data)) != 0);
	CHECK("a refused request still closes the session", fake_timeslot_closes == 1u);
	CHECK("nothing was programmed without a slot", fake_nvmc.word_writes == 0u);
}

/* With the radio down there is nothing to arbitrate. */
static void test_no_radio_no_timeslot(void)
{
	static const uint8_t data[4] = { 1, 2, 3, 4 };

	reset_all();
	CHECK("a write with the radio down succeeds",
	      ultrawidelock_freertos_flash_write(KV_BASE, data, sizeof(data)) == 0);
	CHECK("no timeslot was taken", fake_timeslot_grants == 0u && fake_timeslot_opens == 0u);

	reset_all();
	CHECK("an erase with the radio down succeeds",
	      ultrawidelock_freertos_flash_erase(KV_BASE, PAGE) == 0);
	CHECK("the erase still ran every slice", fake_nvmc.partial_slices >= 30u);
	CHECK("no timeslot was taken for the erase", fake_timeslot_opens == 0u);

	/* Reads never touch the controller, radio or no radio. */
	{
		uint8_t out[4];

		g_radio_ready = true;
		CHECK("a read while the radio is up still succeeds",
		      ultrawidelock_freertos_flash_read(KV_BASE, out, sizeof(out)) == 0);
		CHECK("a read took no timeslot", fake_timeslot_opens == 0u);
	}
}

int main(void)
{
	test_read();
	test_write();
	test_write_rejections();
	test_erase();
	test_erase_rejections();
	test_write_under_timeslot();
	test_erase_under_timeslot();
	test_blocked_requests_are_retried();
	test_timeslot_never_granted();
	test_session_failures();
	test_no_radio_no_timeslot();

	printf("RESULT: %s (%u checks)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks);
	return g_failures == 0 ? 0 : 1;
}
