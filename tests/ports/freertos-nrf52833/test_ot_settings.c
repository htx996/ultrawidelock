/*
 * The OpenThread settings adapter over the port's key-value store.
 *
 * This runs the real store over the flash model, because the property under
 * test is that a packed multi-value record survives rewriting and a reset, and
 * only the real store can be wrong about that. The store's state is static, so
 * every scenario forks; the reboot pair shares one flash mapping on purpose.
 *
 * The multi-value contract is exercised in full — Add, indexed Get, indexed
 * Delete, Delete of -1 — even though the shipped MTD configuration never
 * writes a multi-valued key. The adapter promises the whole contract, so the
 * whole contract is what gets checked.
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

#include <openthread/dataset.h>
#include <openthread/platform/settings.h>

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

static otInstance g_instance;

/* The record a native key owns, by the mapping the KV header documents. */
static uint16_t kv_key_of(uint16_t ot_key)
{
	return (uint16_t)(ULTRAWIDELOCK_KV_KEY_OPENTHREAD_BASE + ot_key);
}

/* Get one value and compare it whole: index, bytes, and reported length. */
static bool value_is(uint16_t key, int index, const void *expect, uint16_t expect_length)
{
	uint8_t buffer[ULTRAWIDELOCK_KV_VALUE_MAX];
	uint16_t length = sizeof(buffer);

	if (otPlatSettingsGet(&g_instance, key, index, buffer, &length) != OT_ERROR_NONE) {
		return false;
	}
	return length == expect_length && memcmp(buffer, expect, expect_length) == 0;
}

/* ---- scenarios ---------------------------------------------------------- */

/* A blank part: nothing found, nothing to delete, a wipe with nothing to do. */
static void scenario_empty(void)
{
	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("an empty store finds nothing",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 0, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("deleting all values of an absent key reports not found",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, -1) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("deleting one value of an absent key reports not found",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 0) ==
		      OT_ERROR_NOT_FOUND);

	otPlatSettingsWipe(&g_instance);
	CHECK("a wipe of an empty store breaks no flash rule", fake_flash_violations == 0);
}

/* The single-value life OpenThread's Set/Get keys lead. */
static void scenario_single_value(void)
{
	uint8_t dataset[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t shorter[3] = {9, 8, 7};
	uint8_t buffer[8];
	uint16_t length;

	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("a set succeeds",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, dataset,
				sizeof(dataset)) == OT_ERROR_NONE);
	CHECK("the value round-trips",
	      value_is(OT_SETTINGS_KEY_ACTIVE_DATASET, 0, dataset, sizeof(dataset)));
	CHECK("a presence check needs no buffers",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 0, NULL, NULL) ==
		      OT_ERROR_NONE);

	length = 0;
	CHECK("a length-only get reports the stored length",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 0, NULL, &length) ==
			      OT_ERROR_NONE &&
		      length == sizeof(dataset));

	/* A short buffer gets a truncated copy but the true length, so the
	 * caller can see the read was short. */
	memset(buffer, 0xaa, sizeof(buffer));
	length = 4;
	CHECK("a short buffer is filled to capacity and told the true length",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 0, buffer, &length) ==
			      OT_ERROR_NONE &&
		      length == sizeof(dataset) && memcmp(buffer, dataset, 4) == 0 &&
		      buffer[4] == 0xaa);

	CHECK("a set key has no second value",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 1, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("a negative get index names nothing",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, -1, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);

	CHECK("a re-set succeeds",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, shorter,
				sizeof(shorter)) == OT_ERROR_NONE);
	CHECK("the newer, shorter value wins whole",
	      value_is(OT_SETTINGS_KEY_ACTIVE_DATASET, 0, shorter, sizeof(shorter)));

	CHECK("a null value with a length is refused",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_NETWORK_INFO, NULL, 4) ==
			      OT_ERROR_INVALID_ARGS &&
		      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, NULL, 4) ==
			      OT_ERROR_INVALID_ARGS);
	CHECK("a zero-length value stores",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_NETWORK_INFO, NULL, 0) ==
		      OT_ERROR_NONE);
	length = 4;
	CHECK("a zero-length value reads back as present and empty",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_NETWORK_INFO, 0, NULL, &length) ==
			      OT_ERROR_NONE &&
		      length == 0);
}

/* The multi-value contract CHILD_INFO would use on an FTD build. */
static void scenario_multi_value(void)
{
	uint8_t child_a[6] = {0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
	uint8_t child_b[4] = {0xb0, 0xb1, 0xb2, 0xb3};
	uint8_t child_c[5] = {0xc0, 0xc1, 0xc2, 0xc3, 0xc4};

	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("a first add succeeds",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child_a,
				sizeof(child_a)) == OT_ERROR_NONE);
	CHECK("a second add succeeds",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child_b,
				sizeof(child_b)) == OT_ERROR_NONE);
	CHECK("a third add succeeds",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child_c,
				sizeof(child_c)) == OT_ERROR_NONE);

	CHECK("index 0 reads the first value",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 0, child_a, sizeof(child_a)));
	CHECK("index 1 reads the second value",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 1, child_b, sizeof(child_b)));
	CHECK("index 2 reads the third value",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 2, child_c, sizeof(child_c)));
	CHECK("index 3 is past the end",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 3, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);

	CHECK("deleting the middle value succeeds",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 1) == OT_ERROR_NONE);
	CHECK("the first value is untouched",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 0, child_a, sizeof(child_a)));
	CHECK("the third value closed the gap",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 1, child_c, sizeof(child_c)));
	CHECK("only two values remain",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 2, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);

	CHECK("deleting past the end reports not found",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 5) ==
		      OT_ERROR_NOT_FOUND);

	CHECK("deleting the last-but-one value succeeds",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 1) == OT_ERROR_NONE);
	CHECK("deleting the final value succeeds",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 0) == OT_ERROR_NONE);
	CHECK("a fully deleted key reads as absent",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 0, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("a fully deleted key holds no record",
	      ultrawidelock_kv_get(kv_key_of(OT_SETTINGS_KEY_CHILD_INFO), NULL, &(size_t){0}) ==
		      ULTRAWIDELOCK_KV_NOT_FOUND);

	CHECK("an add after full deletion starts over",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child_b,
				sizeof(child_b)) == OT_ERROR_NONE);
	CHECK("the restarted key reads at index 0",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 0, child_b, sizeof(child_b)));
}

/* Index -1 removes every value of one key and touches nothing else. */
static void scenario_delete_all(void)
{
	uint8_t child[4] = {1, 2, 3, 4};
	uint8_t dataset[5] = {5, 6, 7, 8, 9};

	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("two values store under the key",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child, sizeof(child)) ==
			      OT_ERROR_NONE &&
		      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child,
					sizeof(child)) == OT_ERROR_NONE);
	CHECK("a neighbouring key stores",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, dataset,
				sizeof(dataset)) == OT_ERROR_NONE);

	CHECK("delete of index -1 succeeds",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, -1) == OT_ERROR_NONE);
	CHECK("every value of the key is gone",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 0, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("the neighbouring key is untouched",
	      value_is(OT_SETTINGS_KEY_ACTIVE_DATASET, 0, dataset, sizeof(dataset)));
	CHECK("a second delete of index -1 reports not found",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, -1) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("an undefined negative index names nothing",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, -2) ==
		      OT_ERROR_NOT_FOUND);
}

/* A wipe empties OpenThread's window and must not touch the credential blob. */
static void scenario_wipe(void)
{
	uint8_t dataset[10];
	uint8_t child[6];
	uint8_t srp_key[32];
	uint8_t prov_blob[40];
	uint8_t prov_read[40];
	size_t length;

	memset(dataset, 0x11, sizeof(dataset));
	memset(child, 0x22, sizeof(child));
	memset(srp_key, 0x33, sizeof(srp_key));
	memset(prov_blob, 0x44, sizeof(prov_blob));

	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("the credential provisioning blob stores beside the settings",
	      ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_CRED_PROV, prov_blob,
					    sizeof(prov_blob)) == ULTRAWIDELOCK_KV_OK);
	CHECK("the settings to be wiped store",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, dataset,
				sizeof(dataset)) == OT_ERROR_NONE &&
		      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child,
					sizeof(child)) == OT_ERROR_NONE &&
		      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_SRP_ECDSA_KEY, srp_key,
					sizeof(srp_key)) == OT_ERROR_NONE);
	/* Both edges of the native range, so an off-by-one wipe cannot hide. */
	CHECK("the first and last native keys store",
	      otPlatSettingsSet(&g_instance, 0x0000, child, sizeof(child)) == OT_ERROR_NONE &&
		      otPlatSettingsSet(&g_instance, 0x00ff, child, sizeof(child)) ==
			      OT_ERROR_NONE);

	otPlatSettingsWipe(&g_instance);

	CHECK("the first native key is wiped",
	      otPlatSettingsGet(&g_instance, 0x0000, 0, NULL, NULL) == OT_ERROR_NOT_FOUND);
	CHECK("the last native key is wiped",
	      otPlatSettingsGet(&g_instance, 0x00ff, 0, NULL, NULL) == OT_ERROR_NOT_FOUND);

	CHECK("the dataset is wiped",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 0, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("the child info is wiped",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 0, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
	CHECK("the SRP key is wiped — a wipe is OpenThread's own factory reset",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_SRP_ECDSA_KEY, 0, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);

	/*
	 * The reason the wipe walks keys instead of erasing the store: the
	 * credential provisioning blob shares the two pages, and a reader that
	 * forgets its identity and trust anchors on a Thread factory reset no
	 * longer opens the door.
	 */
	length = sizeof(prov_read);
	CHECK("the wipe leaves the credential provisioning blob byte-for-byte intact",
	      ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_CRED_PROV, prov_read, &length) ==
			      ULTRAWIDELOCK_KV_OK &&
		      length == sizeof(prov_blob) && memcmp(prov_read, prov_blob, length) == 0);
	CHECK("the wipe broke no flash rule", fake_flash_violations == 0);
}

/* Keys past the native range, the vendor range included, are refused whole. */
static void scenario_key_range(void)
{
	uint8_t value[4] = {1, 2, 3, 4};
	size_t length = 0;

	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("a vendor-range set is refused",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_VENDOR_RESERVED_MIN, value,
				sizeof(value)) == OT_ERROR_NOT_IMPLEMENTED);
	CHECK("a vendor-range add is refused",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_VENDOR_RESERVED_MIN, value,
				sizeof(value)) == OT_ERROR_NOT_IMPLEMENTED);
	CHECK("a vendor-range get is refused",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_VENDOR_RESERVED_MIN, 0, NULL, NULL) ==
		      OT_ERROR_NOT_IMPLEMENTED);
	CHECK("a vendor-range delete is refused",
	      otPlatSettingsDelete(&g_instance, OT_SETTINGS_KEY_VENDOR_RESERVED_MIN, -1) ==
		      OT_ERROR_NOT_IMPLEMENTED);
	CHECK("the first key past the native range is refused",
	      otPlatSettingsSet(&g_instance, 0x0100, value, sizeof(value)) ==
		      OT_ERROR_NOT_IMPLEMENTED);

	/*
	 * 0x8000 masked to a byte would be 0x00: prove the refused write did
	 * not alias into the bottom of the window.
	 */
	CHECK("a refused vendor key stored nothing in the window",
	      ultrawidelock_kv_get(kv_key_of(0x0000), NULL, &length) ==
		      ULTRAWIDELOCK_KV_NOT_FOUND);
}

/* The sizing this adapter promises at build time, exercised at the limits. */
static void scenario_sizing(void)
{
	static uint8_t dataset[OT_OPERATIONAL_DATASET_MAX_LENGTH];
	static uint8_t oversized[ULTRAWIDELOCK_KV_VALUE_MAX];
	uint8_t child[20];
	unsigned added = 0;
	unsigned i;
	otError rc = OT_ERROR_NONE;

	memset(dataset, 0x5a, sizeof(dataset));
	memset(oversized, 0x66, sizeof(oversized));
	memset(child, 0x77, sizeof(child));

	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("the largest single OpenThread value stores",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, dataset,
				sizeof(dataset)) == OT_ERROR_NONE);
	CHECK("the largest single value round-trips",
	      value_is(OT_SETTINGS_KEY_ACTIVE_DATASET, 0, dataset, sizeof(dataset)));
	CHECK("a value the record can never hold is refused with no buffers",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_PENDING_DATASET, oversized,
				sizeof(oversized)) == OT_ERROR_NO_BUFS);

	/* Fill one key until its record is full; the store must survive it. */
	for (i = 0; i < 100; i++) {
		rc = otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child,
				       sizeof(child));
		if (rc != OT_ERROR_NONE) {
			break;
		}
		added++;
	}
	CHECK("a full record refuses another value with no buffers", rc == OT_ERROR_NO_BUFS);
	CHECK("the refusal comes at the packed capacity",
	      added == ULTRAWIDELOCK_KV_VALUE_MAX / (2u + sizeof(child)));
	CHECK("the values already added survive the refusal",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, (int)added - 1, child, sizeof(child)));
	CHECK("no value appeared beyond the last added",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, (int)added, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);

	/* An entry that lands exactly on the record limit fits; the next byte
	 * does not. The boundary itself, not just its neighbourhood. */
	CHECK("an add that exactly fills the record succeeds",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, dataset,
				(uint16_t)(ULTRAWIDELOCK_KV_VALUE_MAX - added * (2u + sizeof(child)) -
					   2u)) == OT_ERROR_NONE);
	CHECK("even an empty value is refused once the record is exactly full",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, NULL, 0) ==
		      OT_ERROR_NO_BUFS);
	CHECK("filling the record broke no flash rule", fake_flash_violations == 0);

	/* Fill the store itself: distinct keys until the live set outgrows a
	 * page. The store's refusal must surface as no-buffers, the one error
	 * the stack understands as out of room. */
	rc = OT_ERROR_NONE;
	for (i = 0x20; i < 0x40; i++) {
		rc = otPlatSettingsSet(&g_instance, (uint16_t)i, dataset, sizeof(dataset));
		if (rc != OT_ERROR_NONE) {
			break;
		}
	}
	CHECK("a store out of room refuses with no buffers", rc == OT_ERROR_NO_BUFS);
	CHECK("the settings already stored survive the store filling",
	      value_is(0x0020, 0, dataset, sizeof(dataset)));
}

/* A write the flash refuses fails the call, loudly, and does not wedge. */
static void scenario_io_failure(void)
{
	uint8_t value[8];
	otError rc;

	memset(value, 0x99, sizeof(value));

	/* Refuse the format write, so the store cannot mount at all. */
	fake_flash_fail_write_after = 1;
	CHECK("a store that cannot mount finds nothing",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, 0, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
	fake_flash_fail_write_after = 1;
	CHECK("an add on a store that cannot mount fails",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, value, sizeof(value)) ==
		      OT_ERROR_FAILED);
	fake_flash_fail_write_after = 0;

	otPlatSettingsInit(&g_instance, NULL, 0);

	fake_flash_fail_write_after = 1;
	rc = otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, value, sizeof(value));
	fake_flash_fail_write_after = 0;
	CHECK("a refused flash write fails the set", rc != OT_ERROR_NONE);

	CHECK("the store recovers for the next set",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, value,
				sizeof(value)) == OT_ERROR_NONE);
	CHECK("the recovered value round-trips",
	      value_is(OT_SETTINGS_KEY_ACTIVE_DATASET, 0, value, sizeof(value)));
}

/* A record with a corrupt tail serves its good entries and drops the rest. */
static void scenario_corrupt_tail(void)
{
	/* One whole entry, then a header claiming 200 bytes with 3 behind it. */
	uint8_t record[] = {5, 0, 'h', 'e', 'l', 'l', 'o', 200, 0, 1, 2, 3};
	uint8_t fresh[4] = {9, 9, 9, 9};

	otPlatSettingsInit(&g_instance, NULL, 0);

	CHECK("a corrupt-tailed record plants",
	      ultrawidelock_kv_set(kv_key_of(OT_SETTINGS_KEY_CHILD_INFO), record,
				  sizeof(record)) == ULTRAWIDELOCK_KV_OK);

	CHECK("the entry in front of the corruption still reads",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 0, "hello", 5));
	CHECK("the corrupt tail is not served as a value",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 1, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);

	CHECK("an add lands where the corruption was dropped",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, fresh, sizeof(fresh)) ==
		      OT_ERROR_NONE);
	CHECK("the good entry survived the rewrite", value_is(OT_SETTINGS_KEY_CHILD_INFO, 0, "hello", 5));
	CHECK("the added value reads at index 1",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 1, fresh, sizeof(fresh)));
	CHECK("the rewritten record ends after the added value",
	      otPlatSettingsGet(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, 2, NULL, NULL) ==
		      OT_ERROR_NOT_FOUND);
}

/* Populate, then leave the flash for the next process to mount. */
static void scenario_populate_then_reboot(void)
{
	uint8_t dataset[32];
	uint8_t child_a[8];
	uint8_t child_b[8];

	memset(dataset, 0xd5, sizeof(dataset));
	memset(child_a, 0xca, sizeof(child_a));
	memset(child_b, 0xcb, sizeof(child_b));

	otPlatSettingsInit(&g_instance, NULL, 0);
	CHECK("the dataset stores before the reboot",
	      otPlatSettingsSet(&g_instance, OT_SETTINGS_KEY_ACTIVE_DATASET, dataset,
				sizeof(dataset)) == OT_ERROR_NONE);
	CHECK("two child entries store before the reboot",
	      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child_a,
				sizeof(child_a)) == OT_ERROR_NONE &&
		      otPlatSettingsAdd(&g_instance, OT_SETTINGS_KEY_CHILD_INFO, child_b,
					sizeof(child_b)) == OT_ERROR_NONE);
}

/* Fresh statics, the previous process's flash: a real reset. */
static void scenario_after_reboot(void)
{
	uint8_t dataset[32];
	uint8_t child_a[8];
	uint8_t child_b[8];

	memset(dataset, 0xd5, sizeof(dataset));
	memset(child_a, 0xca, sizeof(child_a));
	memset(child_b, 0xcb, sizeof(child_b));

	otPlatSettingsInit(&g_instance, NULL, 0);
	CHECK("the dataset survives a reboot",
	      value_is(OT_SETTINGS_KEY_ACTIVE_DATASET, 0, dataset, sizeof(dataset)));
	CHECK("the first child entry survives a reboot",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 0, child_a, sizeof(child_a)));
	CHECK("the second child entry survives a reboot",
	      value_is(OT_SETTINGS_KEY_CHILD_INFO, 1, child_b, sizeof(child_b)));
	CHECK("no flash rule was broken across the reboot", fake_flash_violations == 0);
}

/* ---- driver ------------------------------------------------------------- */

enum scenario {
	SCENARIO_EMPTY,
	SCENARIO_SINGLE_VALUE,
	SCENARIO_MULTI_VALUE,
	SCENARIO_DELETE_ALL,
	SCENARIO_WIPE,
	SCENARIO_KEY_RANGE,
	SCENARIO_SIZING,
	SCENARIO_IO_FAILURE,
	SCENARIO_CORRUPT_TAIL,
	SCENARIO_POPULATE_THEN_REBOOT,
	SCENARIO_AFTER_REBOOT,
};

static int run_scenario(int scenario)
{
	switch (scenario) {
	case SCENARIO_EMPTY:
		scenario_empty();
		break;
	case SCENARIO_SINGLE_VALUE:
		scenario_single_value();
		break;
	case SCENARIO_MULTI_VALUE:
		scenario_multi_value();
		break;
	case SCENARIO_DELETE_ALL:
		scenario_delete_all();
		break;
	case SCENARIO_WIPE:
		scenario_wipe();
		break;
	case SCENARIO_KEY_RANGE:
		scenario_key_range();
		break;
	case SCENARIO_SIZING:
		scenario_sizing();
		break;
	case SCENARIO_IO_FAILURE:
		scenario_io_failure();
		break;
	case SCENARIO_CORRUPT_TAIL:
		scenario_corrupt_tail();
		break;
	case SCENARIO_POPULATE_THEN_REBOOT:
		scenario_populate_then_reboot();
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
	for (scenario = SCENARIO_EMPTY; scenario <= SCENARIO_CORRUPT_TAIL; scenario++) {
		fake_flash_reset();
		failures += run_child(scenario) ? 0u : 1u;
	}

	/* The reboot pair shares one flash on purpose. */
	fake_flash_reset();
	failures += run_child(SCENARIO_POPULATE_THEN_REBOOT) ? 0u : 1u;
	failures += run_child(SCENARIO_AFTER_REBOOT) ? 0u : 1u;

	printf("RESULT: %s (11 scenarios)\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
