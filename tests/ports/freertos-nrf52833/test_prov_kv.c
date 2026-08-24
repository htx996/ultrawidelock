/*
 * The credential provisioning backend over the port's key-value store.
 *
 * This runs the real store over the flash model rather than a stub, because the
 * property that matters is that a provisioned identity survives a reset, and
 * only the real store can be wrong about that. The store's own state is static,
 * so the reboot scenarios each run in their own process against a shared flash
 * mapping.
 *
 * The portable serialiser is exercised elsewhere; what is checked here is the
 * three return contracts ultrawidelock_prov.h states, and that a failure of any kind
 * still leaves a usable identity behind.
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

#include "ultrawidelock_prov.h"

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

/* A provisioned identity that is nothing like the dev default. */
static void make_identity(struct ultrawidelock_reader_identity *id,
			  struct ultrawidelock_trust_store *ts)
{
	size_t i;

	memset(id, 0, sizeof(*id));
	memset(ts, 0, sizeof(*ts));

	for (i = 0; i < ULTRAWIDELOCK_READER_ID_LEN; i++) {
		id->reader_id[i] = (uint8_t)(0xa0 + i);
	}
	for (i = 0; i < ULTRAWIDELOCK_READER_PRIV_LEN; i++) {
		id->sign_priv[i] = (uint8_t)(0x50 + i);
	}
	for (i = 0; i < ULTRAWIDELOCK_GRK_LEN; i++) {
		id->grk[i] = (uint8_t)(0x10 + i);
	}
	id->is_dev = false;

	ts->count = 2;
	for (i = 0; i < ULTRAWIDELOCK_CRED_PUB_LEN; i++) {
		ts->cred_pub[0][i] = (uint8_t)(0x04 + i);
		ts->cred_pub[1][i] = (uint8_t)(0x80 + i);
	}
	ts->cred_type[0] = 6;
	ts->cred_index[0] = 3;
	ts->user_index[0] = 9;
}

static bool identity_matches(const struct ultrawidelock_reader_identity *a,
			     const struct ultrawidelock_reader_identity *b)
{
	return memcmp(a->reader_id, b->reader_id, ULTRAWIDELOCK_READER_ID_LEN) == 0 &&
	       memcmp(a->sign_priv, b->sign_priv, ULTRAWIDELOCK_READER_PRIV_LEN) == 0 &&
	       memcmp(a->grk, b->grk, ULTRAWIDELOCK_GRK_LEN) == 0 && a->is_dev == b->is_dev;
}

/* True if the identity is the built-in bench one. */
static bool is_dev_default(const struct ultrawidelock_reader_identity *id)
{
	struct ultrawidelock_reader_identity dev;
	struct ultrawidelock_trust_store unused;

	ultrawidelock_prov_dev_default(&dev, &unused);
	return identity_matches(id, &dev);
}

/* ---- scenarios ---------------------------------------------------------- */

/* A part that has never been provisioned yields the dev identity, not an error. */
static void scenario_unprovisioned(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;

	memset(&id, 0x5a, sizeof(id));
	memset(&ts, 0x5a, sizeof(ts));

	CHECK("unprovisioned load reports nothing stored", ultrawidelock_prov_load(&id, &ts) == 1);
	CHECK("unprovisioned load yields the dev identity", is_dev_default(&id));
	CHECK("unprovisioned load yields an empty trust store", ts.count == 0);
	CHECK("unprovisioned load marks the identity as dev", id.is_dev);

	/* A factory reset with nothing to undo has still succeeded. */
	CHECK("erasing an unprovisioned store succeeds", ultrawidelock_prov_erase() == 0);
}

/* Store, then read back in the same process. */
static void scenario_round_trip(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_reader_identity got;
	struct ultrawidelock_trust_store ts;
	struct ultrawidelock_trust_store got_ts;

	make_identity(&id, &ts);
	CHECK("store succeeds", ultrawidelock_prov_store(&id, &ts) == 0);

	memset(&got, 0, sizeof(got));
	memset(&got_ts, 0, sizeof(got_ts));
	CHECK("load reports a stored blob", ultrawidelock_prov_load(&got, &got_ts) == 0);
	CHECK("the identity round-trips", identity_matches(&got, &id));
	CHECK("the anchor count round-trips", got_ts.count == 2);
	CHECK("the first anchor round-trips",
	      memcmp(got_ts.cred_pub[0], ts.cred_pub[0], ULTRAWIDELOCK_CRED_PUB_LEN) == 0);
	CHECK("the second anchor round-trips",
	      memcmp(got_ts.cred_pub[1], ts.cred_pub[1], ULTRAWIDELOCK_CRED_PUB_LEN) == 0);
	CHECK("the Matter binding round-trips",
	      got_ts.cred_type[0] == 6 && got_ts.cred_index[0] == 3 && got_ts.user_index[0] == 9);
	CHECK("a stored identity is not marked dev", !got.is_dev);

	/* Overwriting is the ordinary case: re-provisioning happens on any
	 * credential change, so the newest blob has to win. */
	id.grk[0] = 0xee;
	ts.count = 1;
	CHECK("re-store succeeds", ultrawidelock_prov_store(&id, &ts) == 0);
	CHECK("re-load reports a stored blob", ultrawidelock_prov_load(&got, &got_ts) == 0);
	CHECK("the newer identity wins", got.grk[0] == 0xee);
	CHECK("the newer anchor count wins", got_ts.count == 1);
}

/* Erasing removes the identity without disturbing OpenThread's keys. */
static void scenario_erase(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_reader_identity got;
	struct ultrawidelock_trust_store ts;
	struct ultrawidelock_trust_store got_ts;
	uint8_t ot_value[16];
	uint8_t ot_read[16];
	size_t length;

	memset(ot_value, 0x77, sizeof(ot_value));
	CHECK("kv init succeeds", ultrawidelock_kv_init() == ULTRAWIDELOCK_KV_OK);
	CHECK("a neighbouring OpenThread key stores",
	      ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_OPENTHREAD_BASE + 1u, ot_value,
					    sizeof(ot_value)) == ULTRAWIDELOCK_KV_OK);

	make_identity(&id, &ts);
	CHECK("store before erase succeeds", ultrawidelock_prov_store(&id, &ts) == 0);
	CHECK("erase succeeds", ultrawidelock_prov_erase() == 0);

	CHECK("load after erase reports nothing stored", ultrawidelock_prov_load(&got, &got_ts) == 1);
	CHECK("load after erase yields the dev identity", is_dev_default(&got));
	CHECK("load after erase yields an empty trust store", got_ts.count == 0);

	/*
	 * The reason erase deletes one key rather than the store: OpenThread's
	 * SRP client key lives in the same two pages, and losing it costs up to
	 * a fortnight of being attached to Thread but unreachable on it.
	 */
	length = sizeof(ot_read);
	CHECK("erase leaves the OpenThread key alone",
	      ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_OPENTHREAD_BASE + 1u, ot_read,
					    &length) == ULTRAWIDELOCK_KV_OK &&
		      length == sizeof(ot_value) && memcmp(ot_read, ot_value, length) == 0);

	CHECK("a second erase succeeds", ultrawidelock_prov_erase() == 0);
}

/* A stored blob the parser rejects must not take the reader down with it. */
static void scenario_malformed(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;
	uint8_t garbage[12];

	memset(garbage, 0x33, sizeof(garbage));
	CHECK("kv init succeeds", ultrawidelock_kv_init() == ULTRAWIDELOCK_KV_OK);
	CHECK("garbage stores under the prov key",
	      ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_CRED_PROV, garbage,
					    sizeof(garbage)) == ULTRAWIDELOCK_KV_OK);

	CHECK("a malformed blob reports an error", ultrawidelock_prov_load(&id, &ts) == -1);
	CHECK("a malformed blob still yields the dev identity", is_dev_default(&id));
	CHECK("a malformed blob still yields an empty trust store", ts.count == 0);
}

/*
 * A record longer than any blob this firmware can write is corruption or a
 * future format. Either way it is refused rather than parsed.
 */
static void scenario_oversized(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;
	static uint8_t big[ULTRAWIDELOCK_PROV_BLOB_MAX + 4u];

	memset(big, 0x22, sizeof(big));
	CHECK("kv init succeeds", ultrawidelock_kv_init() == ULTRAWIDELOCK_KV_OK);
	CHECK("an oversized record stores",
	      ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_CRED_PROV, big, sizeof(big)) ==
		      ULTRAWIDELOCK_KV_OK);

	CHECK("an oversized record reports an error", ultrawidelock_prov_load(&id, &ts) == -1);
	CHECK("an oversized record still yields the dev identity", is_dev_default(&id));
}

/* A store that the flash refuses must report the failure, not swallow it. */
static void scenario_store_fails(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;

	make_identity(&id, &ts);
	CHECK("kv init succeeds", ultrawidelock_kv_init() == ULTRAWIDELOCK_KV_OK);

	/* Fail the next write, which is the one carrying the record header. */
	fake_flash_fail_write_after = 1;
	CHECK("a refused write reports a store error", ultrawidelock_prov_store(&id, &ts) < 0);
	fake_flash_fail_write_after = 0;
}

/* Provision, then leave the flash for the next process to mount. */
static void scenario_provision_then_reboot(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;

	make_identity(&id, &ts);
	CHECK("store before reboot succeeds", ultrawidelock_prov_store(&id, &ts) == 0);
}

/* Fresh statics, the previous process's flash: a real reset. */
static void scenario_after_reboot(void)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_reader_identity expect;
	struct ultrawidelock_trust_store ts;
	struct ultrawidelock_trust_store expect_ts;

	make_identity(&expect, &expect_ts);
	CHECK("load after reboot reports a stored blob", ultrawidelock_prov_load(&id, &ts) == 0);
	CHECK("the identity survives a reboot", identity_matches(&id, &expect));
	CHECK("the anchors survive a reboot",
	      ts.count == 2 && memcmp(ts.cred_pub[1], expect_ts.cred_pub[1],
				      ULTRAWIDELOCK_CRED_PUB_LEN) == 0);
	CHECK("no flash rule was broken across the reboot", fake_flash_violations == 0);
}

/* ---- driver ------------------------------------------------------------- */

enum scenario {
	SCENARIO_UNPROVISIONED,
	SCENARIO_ROUND_TRIP,
	SCENARIO_ERASE,
	SCENARIO_MALFORMED,
	SCENARIO_OVERSIZED,
	SCENARIO_STORE_FAILS,
	SCENARIO_PROVISION_THEN_REBOOT,
	SCENARIO_AFTER_REBOOT,
};

static int run_scenario(int scenario)
{
	switch (scenario) {
	case SCENARIO_UNPROVISIONED:
		scenario_unprovisioned();
		break;
	case SCENARIO_ROUND_TRIP:
		scenario_round_trip();
		break;
	case SCENARIO_ERASE:
		scenario_erase();
		break;
	case SCENARIO_MALFORMED:
		scenario_malformed();
		break;
	case SCENARIO_OVERSIZED:
		scenario_oversized();
		break;
	case SCENARIO_STORE_FAILS:
		scenario_store_fails();
		break;
	case SCENARIO_PROVISION_THEN_REBOOT:
		scenario_provision_then_reboot();
		break;
	default:
		scenario_after_reboot();
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
	int scenario;

	/* Each of these starts from a blank part. */
	for (scenario = SCENARIO_UNPROVISIONED; scenario <= SCENARIO_STORE_FAILS; scenario++) {
		fake_flash_reset();
		failures += run_child(scenario) ? 0u : 1u;
	}

	/* The reboot pair shares one flash on purpose. */
	fake_flash_reset();
	failures += run_child(SCENARIO_PROVISION_THEN_REBOOT) ? 0u : 1u;
	failures += run_child(SCENARIO_AFTER_REBOOT) ? 0u : 1u;

	printf("RESULT: %s (8 scenarios)\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
