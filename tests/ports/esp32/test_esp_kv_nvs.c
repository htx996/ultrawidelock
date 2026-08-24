/*
 * Host test for the ESP32 kv backend (ports/esp32/components/ultrawidelock_port/
 * kv_nvs.c) against the in-RAM NVS fake (sdkfake/fake_nvs.c).
 *
 * The assertion that matters is the derived name. NVS caps a namespace and a
 * key at 15 characters and does NOT fail an over-long one symmetrically -- a
 * read-only open simply never matches it, so the read side reports "never
 * stored" and stays quiet. This backend spells every key as four hex digits
 * inside a three-character namespace, so the cap is unreachable; the fake
 * enforces the same asymmetry, so a regression that reintroduced a long name
 * would show up here rather than on a bench.
 *
 * "Theatre" suite, stated plainly: no flash. Passing proves the branch logic and
 * the mapping onto nvs_*, not durability or wear.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"

#include "ultrawidelock_kv.h"

/* What kv_nvs.c owns. Spelled here so the test asserts on the real name rather
 * than on whatever the backend happened to build. */
#define KV_NS "uwl"

static int fails;

static void okc(const char *name, int cond)
{
	if (!cond) {
		fails++;
		printf("  FAIL %s\n", name);
	}
}

int main(void)
{
	uint8_t out[64];
	uint8_t big[ULTRAWIDELOCK_KV_VALUE_MAX];
	size_t len;

	memset(big, 0x5a, sizeof(big));

	printf("== host: ultrawidelock_kv NVS backend vs in-RAM NVS fake ==\n");

	printf("-- names are derived, and short --\n");
	fake_nvs_reset();
	okc("init ok", ultrawidelock_kv_init() == ULTRAWIDELOCK_KV_OK);
	okc("set ok", ultrawidelock_kv_set(0x4001u, "abcd", 4u) == ULTRAWIDELOCK_KV_OK);
	okc("one record in the namespace", fake_nvs_key_count(KV_NS) == 1);
	/* The names the backend actually used, read back through the fake by the
	 * exact namespace and key it must have chosen. */
	len = fake_nvs_stored(KV_NS, "4001", out, sizeof(out));
	okc("stored under uwl/4001", len == 4 && memcmp(out, "abcd", 4) == 0);
	okc("low key zero-padded", ultrawidelock_kv_set(0x0001u, "z", 1u) == ULTRAWIDELOCK_KV_OK);
	okc("stored under uwl/0001", fake_nvs_stored(KV_NS, "0001", out, sizeof(out)) == 1);
	okc("high key not truncated",
	    ultrawidelock_kv_set(0xfffeu, "z", 1u) == ULTRAWIDELOCK_KV_OK);
	okc("stored under uwl/fffe", fake_nvs_stored(KV_NS, "fffe", out, sizeof(out)) == 1);

	printf("-- round trip --\n");
	len = sizeof(out);
	okc("get ok", ultrawidelock_kv_get(0x4001u, out, &len) == ULTRAWIDELOCK_KV_OK);
	okc("stored length", len == 4);
	okc("value round-trips", memcmp(out, "abcd", 4) == 0);
	len = sizeof(out);
	okc("absent key -> NOT_FOUND",
	    ultrawidelock_kv_get(0x4002u, out, &len) == ULTRAWIDELOCK_KV_NOT_FOUND);
	okc("set replaces", ultrawidelock_kv_set(0x4001u, "ef", 2u) == ULTRAWIDELOCK_KV_OK);
	okc("still one record for that key", fake_nvs_key_count(KV_NS) == 3);

	printf("-- undersized read is refused, not truncated --\n");
	okc("store 40 bytes", ultrawidelock_kv_set(0x4003u, big, 40u) == ULTRAWIDELOCK_KV_OK);
	len = 8u;
	memset(out, 0xee, sizeof(out));
	okc("short buffer -> INVALID",
	    ultrawidelock_kv_get(0x4003u, out, &len) == ULTRAWIDELOCK_KV_INVALID);
	okc("stored length handed back", len == 40);
	okc("buffer untouched", out[0] == 0xee);

	printf("-- guards --\n");
	okc("KEY_NONE refused",
	    ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_NONE, "x", 1u) == ULTRAWIDELOCK_KV_INVALID);
	okc("oversize refused",
	    ultrawidelock_kv_set(0x4004u, big, ULTRAWIDELOCK_KV_VALUE_MAX + 1u) ==
		    ULTRAWIDELOCK_KV_INVALID);

	printf("-- a namespace that was never written --\n");
	fake_nvs_reset();
	len = sizeof(out);
	/* Read-only open misses, and every key in it is NOT_FOUND rather than an
	 * error the caller has to special-case. */
	okc("get before any set -> NOT_FOUND",
	    ultrawidelock_kv_get(0x4000u, out, &len) == ULTRAWIDELOCK_KV_NOT_FOUND);

	printf("-- delete --\n");
	fake_nvs_reset();
	okc("set", ultrawidelock_kv_set(0x4000u, "v", 1u) == ULTRAWIDELOCK_KV_OK);
	okc("delete ok", ultrawidelock_kv_delete(0x4000u) == ULTRAWIDELOCK_KV_OK);
	okc("gone", fake_nvs_key_count(KV_NS) == 0);
	okc("second delete -> NOT_FOUND",
	    ultrawidelock_kv_delete(0x4000u) == ULTRAWIDELOCK_KV_NOT_FOUND);

	printf("-- erase_all takes this namespace and no other --\n");
	fake_nvs_reset();
	okc("a kv record", ultrawidelock_kv_set(0x4000u, "v", 1u) == ULTRAWIDELOCK_KV_OK);
	okc("another kv record", ultrawidelock_kv_set(0x4001u, "v", 1u) == ULTRAWIDELOCK_KV_OK);
	/* The reader's provisioning blob lives in its own namespace on the same
	 * part; a factory reset of this store must not take it. */
	fake_nvs_preload("uwl_prov", "blob", "keepme", 6u);
	okc("erase_all ok", ultrawidelock_kv_erase_all() == ULTRAWIDELOCK_KV_OK);
	okc("this namespace is empty", fake_nvs_key_count(KV_NS) == 0);
	okc("the neighbour survived", fake_nvs_key_count("uwl_prov") == 1);
	okc("and still holds its value",
	    fake_nvs_stored("uwl_prov", "blob", out, sizeof(out)) == 6);

	printf("-- injected failures --\n");
	fake_nvs_reset();
	(void)ultrawidelock_kv_set(0x4000u, "v", 1u);
	fake_nvs_set_rc = ESP_FAIL;
	okc("set failure -> IO", ultrawidelock_kv_set(0x4000u, "v", 1u) == ULTRAWIDELOCK_KV_IO);
	fake_nvs_set_rc = ESP_OK;
	fake_nvs_erase_rc = ESP_FAIL;
	okc("delete failure -> IO", ultrawidelock_kv_delete(0x4000u) == ULTRAWIDELOCK_KV_IO);
	fake_nvs_erase_rc = ESP_OK;

	if (fails > 0) {
		printf("  esp-kv-nvs: FAIL (%d)\n", fails);
		return 1;
	}
	printf("  esp-kv-nvs: PASS (in-RAM NVS, no flash durability)\n");
	return 0;
}
