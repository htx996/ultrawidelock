/*
 * The persistent key-value store on two flash pages.
 *
 * The flash model enforces the part's rules rather than merely storing bytes,
 * so a write that sets a bit, lands at an odd address, or skips an erase fails
 * here the way it would on the board. The scenarios that matter are the ones
 * that cross a reset: the store's own state is static, so each of those runs in
 * its own process against a shared flash mapping, which is what makes a reboot
 * testable.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fake_flash.h"

#include <ultrawidelock_freertos_kv.h>
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

void ultrawidelock_freertos_log(enum ultrawidelock_freertos_log_level level, const char *tag,
				const char *fmt, ...)
{
	(void)level;
	(void)tag;
	(void)fmt;
}

#define PROV_KEY ULTRAWIDELOCK_KV_KEY_CRED_PROV
#define OT_KEY (ULTRAWIDELOCK_KV_KEY_OPENTHREAD_BASE + 3u)

/* Reads a key into a buffer, returning the store's result. */
static int get(uint16_t key, void *buffer, size_t capacity, size_t *length)
{
	*length = capacity;
	return ultrawidelock_freertos_kv_get(key, buffer, length);
}

static void fill(uint8_t *buffer, size_t length, uint8_t seed)
{
	size_t i;

	for (i = 0; i < length; i++) {
		buffer[i] = (uint8_t)(seed + i);
	}
}

/* ---- scenarios ---------------------------------------------------------- */

static void scenario_basics(void)
{
	uint8_t out[ULTRAWIDELOCK_KV_VALUE_MAX];
	uint8_t value[37];
	size_t length;

	fill(value, sizeof(value), 0x10);

	CHECK("a blank part mounts as an empty store",
	      ultrawidelock_freertos_kv_init() == ULTRAWIDELOCK_KV_OK);
	CHECK("nothing is stored yet",
	      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_NOT_FOUND);

	CHECK("a value that is not a whole number of words round-trips",
	      ultrawidelock_freertos_kv_set(PROV_KEY, value, sizeof(value)) == ULTRAWIDELOCK_KV_OK &&
		      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK &&
		      length == sizeof(value) && memcmp(out, value, sizeof(value)) == 0);

	fill(value, sizeof(value), 0x80);
	CHECK("rewriting a key returns the new value, not the old",
	      ultrawidelock_freertos_kv_set(PROV_KEY, value, sizeof(value)) == ULTRAWIDELOCK_KV_OK &&
		      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK &&
		      memcmp(out, value, sizeof(value)) == 0);

	CHECK("a second key does not disturb the first",
	      ultrawidelock_freertos_kv_set(OT_KEY, "thread", 6) == ULTRAWIDELOCK_KV_OK &&
		      get(OT_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK && length == 6 &&
		      memcmp(out, "thread", 6) == 0 &&
		      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK &&
		      memcmp(out, value, sizeof(value)) == 0);

	/*
	 * Every copy of a key has to be struck out, not only the newest: an
	 * older one left valid would resurrect a value the caller deleted.
	 */
	CHECK("deleting a key that was written twice forgets both copies",
	      ultrawidelock_freertos_kv_delete(PROV_KEY) == ULTRAWIDELOCK_KV_OK &&
		      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_NOT_FOUND);
	CHECK("deleting it again reports that it was not there",
	      ultrawidelock_freertos_kv_delete(PROV_KEY) == ULTRAWIDELOCK_KV_NOT_FOUND);
	CHECK("and the other key survived the delete",
	      get(OT_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK && length == 6);

	CHECK("a zero-length value is a value, not an absence",
	      ultrawidelock_freertos_kv_set(OT_KEY, NULL, 0) == ULTRAWIDELOCK_KV_OK &&
		      get(OT_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK && length == 0);

	length = 0;
	CHECK("a buffer too small reports the stored length rather than truncating",
	      ultrawidelock_freertos_kv_set(PROV_KEY, value, sizeof(value)) == ULTRAWIDELOCK_KV_OK &&
		      get(PROV_KEY, out, 4, &length) == ULTRAWIDELOCK_KV_INVALID &&
		      length == sizeof(value));

	CHECK("an oversized value is refused",
	      ultrawidelock_freertos_kv_set(PROV_KEY, out, ULTRAWIDELOCK_KV_VALUE_MAX + 1u) ==
		      ULTRAWIDELOCK_KV_INVALID);
	CHECK("the erased-flash key is not a key",
	      ultrawidelock_freertos_kv_set(ULTRAWIDELOCK_KV_KEY_NONE, value, 4) ==
			      ULTRAWIDELOCK_KV_INVALID &&
		      get(ULTRAWIDELOCK_KV_KEY_NONE, out, sizeof(out), &length) ==
			      ULTRAWIDELOCK_KV_INVALID);

	CHECK("no flash rule was broken along the way", fake_flash_violations == 0);
}

/* Writes enough to force at least one compaction, then checks what survived. */
static void scenario_compaction(void)
{
	uint8_t value[700];
	uint8_t out[ULTRAWIDELOCK_KV_VALUE_MAX];
	size_t length;
	unsigned erases_before;
	unsigned i;

	(void)ultrawidelock_freertos_kv_init();
	CHECK("a fresh store has most of a page free",
	      ultrawidelock_freertos_kv_free_bytes() > 4000u);

	/*
	 * Rewritten a few times, so it has superseded copies of its own when the
	 * churn below forces a compaction that is not skipping it.
	 */
	fill(value, 200, 0x40);
	CHECK("a long-lived key is stored, and rewritten",
	      ultrawidelock_freertos_kv_set(OT_KEY, value, 200) == ULTRAWIDELOCK_KV_OK &&
		      ultrawidelock_freertos_kv_set(OT_KEY, value, 200) == ULTRAWIDELOCK_KV_OK &&
		      ultrawidelock_freertos_kv_set(OT_KEY, value, 200) == ULTRAWIDELOCK_KV_OK);

	erases_before = fake_flash_erase_calls;
	for (i = 0; i < 12u; i++) {
		fill(value, sizeof(value), (uint8_t)i);
		if (ultrawidelock_freertos_kv_set(PROV_KEY, value, sizeof(value)) != ULTRAWIDELOCK_KV_OK) {
			break;
		}
	}
	CHECK("writing past the end of a page compacts rather than failing", i == 12u);
	CHECK("and that took at least one erase", fake_flash_erase_calls > erases_before);

	CHECK("the last value written is the one that survives",
	      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK && length == sizeof(value) &&
		      memcmp(out, value, sizeof(value)) == 0);
	fill(out, 200, 0x40);
	CHECK("a key untouched by the churn survives it, at its newest value",
	      get(OT_KEY, value, sizeof(value), &length) == ULTRAWIDELOCK_KV_OK && length == 200 &&
		      memcmp(value, out, 200) == 0);
	fill(value, sizeof(value), 11);
	CHECK("compaction broke no flash rule", fake_flash_violations == 0);
	/*
	 * Reclaiming the churn is the whole point: eleven superseded copies of a
	 * 700-byte value carried across would leave almost nothing.
	 */
	/*
	 * Reclaiming the churn is the whole point. Carrying the superseded
	 * copies over instead leaves under a kilobyte here, which is less than
	 * two more writes of the value that caused the compaction.
	 */
	CHECK("and it reclaimed the superseded copies rather than carrying them over",
	      ultrawidelock_freertos_kv_free_bytes() > 1400u);

	CHECK("erasing everything leaves a store, not a hole",
	      ultrawidelock_freertos_kv_erase_all() == ULTRAWIDELOCK_KV_OK &&
		      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_NOT_FOUND &&
		      get(OT_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_NOT_FOUND &&
		      ultrawidelock_freertos_kv_set(PROV_KEY, "again", 5) == ULTRAWIDELOCK_KV_OK);
}

/* Leaves a store with two keys behind for the reboot scenario to find. */
static void scenario_write_then_reboot(void)
{
	uint8_t value[64];

	fill(value, sizeof(value), 0x30);
	CHECK("values are written before the reset",
	      ultrawidelock_freertos_kv_set(PROV_KEY, value, sizeof(value)) == ULTRAWIDELOCK_KV_OK &&
		      ultrawidelock_freertos_kv_set(OT_KEY, "srp", 3) == ULTRAWIDELOCK_KV_OK);
}

static void scenario_after_reboot(void)
{
	uint8_t out[ULTRAWIDELOCK_KV_VALUE_MAX];
	uint8_t expect[64];
	size_t length;

	fill(expect, sizeof(expect), 0x30);
	CHECK("a store written before the reset mounts again",
	      ultrawidelock_freertos_kv_init() == ULTRAWIDELOCK_KV_OK);
	CHECK("and both values are still there",
	      get(PROV_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK &&
		      length == sizeof(expect) && memcmp(out, expect, sizeof(expect)) == 0 &&
		      get(OT_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK && length == 3);
	CHECK("the log continues where it left off, without a second copy",
	      ultrawidelock_freertos_kv_free_bytes() < 8184u);
}

/*
 * A record whose state word never lands is not a record. The value written
 * before it must not appear after the reset, and the store must still mount.
 */
static void scenario_torn_write_then_reboot(void)
{
	CHECK("the good value is stored first",
	      ultrawidelock_freertos_kv_set(OT_KEY, "good", 4) == ULTRAWIDELOCK_KV_OK);
	/*
	 * Three writes make a record: header, payload, state. Failing the third
	 * is the power loss that leaves a payload nobody claimed.
	 */
	fake_flash_write_calls = 0;
	fake_flash_fail_write_after = 3;
	CHECK("a write whose state word never lands reports the failure",
	      ultrawidelock_freertos_kv_set(OT_KEY, "torn", 4) == ULTRAWIDELOCK_KV_IO);
}

static void scenario_after_torn_write(void)
{
	uint8_t out[ULTRAWIDELOCK_KV_VALUE_MAX];
	size_t length;

	CHECK("the store still mounts after a torn record",
	      ultrawidelock_freertos_kv_init() == ULTRAWIDELOCK_KV_OK);
	CHECK("and the unclaimed value is not visible",
	      get(OT_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK && length == 4 &&
		      memcmp(out, "good", 4) == 0);
	CHECK("writing again after the torn record still works",
	      ultrawidelock_freertos_kv_set(OT_KEY, "after", 5) == ULTRAWIDELOCK_KV_OK &&
		      get(OT_KEY, out, sizeof(out), &length) == ULTRAWIDELOCK_KV_OK && length == 5);
}

/* ---- scenario harness --------------------------------------------------- */

enum {
	SCENARIO_BASICS,
	SCENARIO_COMPACTION,
	SCENARIO_WRITE_THEN_REBOOT,
	SCENARIO_AFTER_REBOOT,
	SCENARIO_TORN_WRITE,
	SCENARIO_AFTER_TORN_WRITE,
};

static int run_scenario(int scenario)
{
	switch (scenario) {
	case SCENARIO_BASICS:
		scenario_basics();
		break;
	case SCENARIO_COMPACTION:
		scenario_compaction();
		break;
	case SCENARIO_WRITE_THEN_REBOOT:
		scenario_write_then_reboot();
		break;
	case SCENARIO_AFTER_REBOOT:
		scenario_after_reboot();
		break;
	case SCENARIO_TORN_WRITE:
		scenario_torn_write_then_reboot();
		break;
	default:
		scenario_after_torn_write();
		break;
	}
	printf("RESULT-PART: %u checks\n", g_checks);
	return g_failures == 0 ? 0 : 1;
}

static bool run_child(int scenario)
{
	pid_t pid;
	int status = 0;

	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		int rc = run_scenario(scenario);

		fflush(stdout);
		_exit(rc);
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid) {
		printf("  FAIL could not fork scenario %d\n", scenario);
		return false;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void)
{
	unsigned failures = 0;

	/* Each group starts from a blank part; the reboot halves do not. */
	fake_flash_reset();
	failures += run_child(SCENARIO_BASICS) ? 0u : 1u;

	fake_flash_reset();
	failures += run_child(SCENARIO_COMPACTION) ? 0u : 1u;

	fake_flash_reset();
	failures += run_child(SCENARIO_WRITE_THEN_REBOOT) ? 0u : 1u;
	failures += run_child(SCENARIO_AFTER_REBOOT) ? 0u : 1u;

	fake_flash_reset();
	failures += run_child(SCENARIO_TORN_WRITE) ? 0u : 1u;
	failures += run_child(SCENARIO_AFTER_TORN_WRITE) ? 0u : 1u;

	printf("RESULT: %s (6 scenarios)\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
