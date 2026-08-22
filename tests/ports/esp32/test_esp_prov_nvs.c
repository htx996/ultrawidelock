/*
 * Host test for the ESP32 NVS provisioning backend (ultrawidelock_prov_nvs.c) against
 * the in-RAM NVS fake (sdkfake/fake_nvs.c). "Theatre" suite: no flash is
 * involved, so passing proves the load/store branch logic and the blob's
 * round-trip through the real ultrawidelock_prov serializer — not NVS durability.
 * The serializer/deserializer itself is the real shared-core code; only the
 * nvs_* calls are doubles.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"

#include "ultrawidelock_prov.h"

/* Where ultrawidelock_prov_nvs.c puts the blob. Spelled here too because the
 * fake holds many records now and a test has to say which one it means. */
#define PROV_NS  "uwl_prov"
#define PROV_KEY "blob"

static int fails;

static void okc(const char *name, int cond)
{
	if (!cond) {
		printf("  FAIL %s\n", name);
		fails++;
	} else {
		printf("  ok   %s\n", name);
	}
}

static struct ultrawidelock_reader_identity mkid(uint8_t seed)
{
	struct ultrawidelock_reader_identity id;

	memset(&id, seed, sizeof(id));
	return id;
}

int main(void)
{
	struct ultrawidelock_reader_identity id, dev_id, out_id;
	struct ultrawidelock_trust_store ts, dev_ts, out_ts;

	ultrawidelock_prov_dev_default(&dev_id, &dev_ts);

	printf("-- load branches --\n");

	/* Fresh NVS: namespace never written -> DEV identity, rc 1. */
	fake_nvs_reset();
	okc("fresh load -> 1", ultrawidelock_prov_load(&out_id, &out_ts) == 1);
	okc("fresh load = DEV identity",
	    memcmp(&out_id, &dev_id, sizeof(dev_id)) == 0 && out_ts.count == dev_ts.count);

	/* NVS init failure -> DEV identity, rc -1. */
	fake_nvs_reset();
	fake_nvs_init_rc = ESP_FAIL;
	okc("init failure -> -1", ultrawidelock_prov_load(&out_id, &out_ts) == -1);
	okc("init failure = DEV identity", memcmp(&out_id, &dev_id, sizeof(dev_id)) == 0);

	/* Recoverable init (no free pages): erase + retry succeeds. */
	fake_nvs_reset();
	fake_nvs_init_rc_once = ESP_ERR_NVS_NO_FREE_PAGES;
	okc("no-free-pages load -> 1 (after erase)", ultrawidelock_prov_load(&out_id, &out_ts) == 1);
	okc("erase ran once", fake_nvs_erase_calls == 1);

	/* nvs_open hard failure (not NOT_FOUND) -> -1. */
	fake_nvs_reset();
	fake_nvs_open_rc = ESP_FAIL;
	okc("open failure -> -1", ultrawidelock_prov_load(&out_id, &out_ts) == -1);

	/* Namespace exists but the key does not -> rc 1. */
	fake_nvs_reset();
	fake_nvs_preload(PROV_NS, PROV_KEY, "x", 1); /* creates the namespace... */
	fake_nvs_get_rc = ESP_ERR_NVS_NOT_FOUND; /* ...but the key read misses */
	okc("key missing -> 1", ultrawidelock_prov_load(&out_id, &out_ts) == 1);

	/* nvs_get_blob hard failure -> -1. */
	fake_nvs_reset();
	fake_nvs_preload(PROV_NS, PROV_KEY, "x", 1);
	fake_nvs_get_rc = ESP_FAIL;
	okc("get_blob failure -> -1", ultrawidelock_prov_load(&out_id, &out_ts) == -1);

	/* Stored blob is garbage -> deserialize rejects -> -1 + DEV identity. */
	fake_nvs_reset();
	fake_nvs_preload(PROV_NS, PROV_KEY, (const uint8_t[]){0xBA, 0xD0, 0xBA, 0xD0}, 4);
	okc("malformed blob -> -1", ultrawidelock_prov_load(&out_id, &out_ts) == -1);
	okc("malformed blob = DEV identity", memcmp(&out_id, &dev_id, sizeof(dev_id)) == 0);

	printf("-- store branches + round trip --\n");

	/* Store a provisioned identity + one trusted credential, then load it. */
	id = mkid(0x5A);
	memset(&ts, 0, sizeof(ts));

	uint8_t cred[ULTRAWIDELOCK_CRED_PUB_LEN];

	memset(cred, 0x77, sizeof(cred));
	cred[0] = 0x04;
	okc("trust_add rc", ultrawidelock_prov_trust_add(&ts, cred) == 0);

	fake_nvs_reset();
	okc("store rc", ultrawidelock_prov_store(&id, &ts) == 0);

	uint8_t blob[ULTRAWIDELOCK_PROV_BLOB_MAX];
	size_t blob_len = fake_nvs_stored(PROV_NS, PROV_KEY, blob, sizeof(blob));

	okc("blob landed in NVS", blob_len > 0);
	okc("stored blob deserializes",
	    ultrawidelock_prov_deserialize(blob, blob_len, &out_id, &out_ts) == 0);

	memset(&out_id, 0, sizeof(out_id));
	memset(&out_ts, 0, sizeof(out_ts));
	okc("round-trip load -> 0", ultrawidelock_prov_load(&out_id, &out_ts) == 0);
	okc("round-trip identity",
	    memcmp(out_id.reader_id, id.reader_id, sizeof(id.reader_id)) == 0 &&
	    memcmp(out_id.sign_priv, id.sign_priv, sizeof(id.sign_priv)) == 0 &&
	    memcmp(out_id.grk, id.grk, sizeof(id.grk)) == 0 && !out_id.is_dev);
	okc("round-trip trust store",
	    out_ts.count == 1 && ultrawidelock_prov_trust_check(&out_ts, cred) == 0);

	/* Store failure injection: init, open, set, commit. */
	fake_nvs_init_rc = ESP_FAIL;
	okc("store init failure", ultrawidelock_prov_store(&id, &ts) == -1);
	fake_nvs_init_rc = ESP_OK;
	fake_nvs_open_rc = ESP_FAIL;
	okc("store open failure", ultrawidelock_prov_store(&id, &ts) == -1);
	fake_nvs_open_rc = ESP_OK;
	fake_nvs_set_rc = ESP_FAIL;
	okc("store set_blob failure", ultrawidelock_prov_store(&id, &ts) == -1);
	fake_nvs_set_rc = ESP_OK;
	fake_nvs_commit_rc = ESP_FAIL;
	okc("store commit failure", ultrawidelock_prov_store(&id, &ts) == -1);
	fake_nvs_commit_rc = ESP_OK;

	/* NULL trust store serializes as count 0 (ts != NULL guards throughout). */
	okc("store NULL ts", ultrawidelock_prov_store(&id, NULL) == 0);

	/* Serializer rejection (impossible trust count) surfaces as -1. */
	ts.count = 255;
	okc("store serialize failure", ultrawidelock_prov_store(&id, &ts) == -1);

	printf("-- NVS name caps (the double's contract) --\n");

	/* NVS names top out at NVS_NS_NAME_MAX_SIZE - 1 (15) characters, and the
	 * target does not fail this symmetrically: a read-only open of an over-long
	 * namespace can never match, so it misses as NOT_FOUND ("never provisioned"),
	 * and only the read-write open that would create it says KEY_TOO_LONG. The
	 * backend's namespace once outgrew the cap in a rename, this suite passed
	 * because the fake ignored the name, and every store on hardware failed.
	 * The backend now static-asserts its own names; this pins the fake so the
	 * suite would also have caught it. */
	static const char too_long[] = "sixteen_chars_ns"; /* one over the cap */
	nvs_handle_t h;

	fake_nvs_reset();
	okc("cap is 15", NVS_NS_NAME_MAX_SIZE - 1 == 15 && sizeof(too_long) - 1 == 16);
	okc("over-long ns, ro open -> NOT_FOUND",
	    nvs_open(too_long, NVS_READONLY, &h) == ESP_ERR_NVS_NOT_FOUND);
	okc("over-long ns, rw open -> KEY_TOO_LONG",
	    nvs_open(too_long, NVS_READWRITE, &h) == ESP_ERR_NVS_KEY_TOO_LONG);
	okc("at-cap ns, rw open -> OK", nvs_open("fifteen_chars_n", NVS_READWRITE, &h) == ESP_OK);
	okc("over-long key, set -> KEY_TOO_LONG",
	    nvs_set_blob(h, too_long, "x", 1) == ESP_ERR_NVS_KEY_TOO_LONG);
	okc("at-cap key, set -> OK", nvs_set_blob(h, "fifteen_chars_k", "x", 1) == ESP_OK);

	printf("\nRESULT: %s\n", fails == 0 ? "PASS" : "FAIL");
	return fails == 0 ? 0 : 1;
}
