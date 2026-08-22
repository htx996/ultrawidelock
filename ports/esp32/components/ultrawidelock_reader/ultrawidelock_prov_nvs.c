/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_prov (ESP32 backend) — the twin of the Zephyr port's
 * ultrawidelock_prov_settings.c and the FreeRTOS port's ultrawidelock_prov_kv.c. The
 * portable serialisation, dev fallback and trust logic all live in
 * ultrawidelock_prov.c; this file only moves that one blob in and out of the store
 * under ULTRAWIDELOCK_KV_KEY_CRED_PROV.
 *
 * It reaches the store through ultrawidelock_kv.h rather than through nvs_* directly.
 * The record used to be the namespace "uwl_prov" and the key "blob", and is now
 * whatever kv_nvs.c derives from the key, so a board provisioned by an older
 * image reads as never provisioned and comes up on the DEV identity. That is the
 * accepted cost of retiring the name table; re-provision the bench boards.
 *
 * The name caps that used to be static-asserted here went with the names. A
 * uint16_t key cannot be too long, which is the whole point of the seam: NVS
 * does not fail an over-long name symmetrically -- a read-only open simply never
 * matches it, so a load reads "never provisioned" and only a write says
 * ESP_ERR_NVS_KEY_TOO_LONG. An 18-character namespace survived a rename, a test
 * suite and a release exactly that way (docs/esp32-gotchas.md 8.4).
 *
 * The API is named without its parentheses on purpose. The purity gate finds
 * every ESP storage call site by grepping for the open call and demands a
 * PORTING.md row for the file it is in; that scan reads comments too, so
 * spelling it here would earn this file a row for a name it no longer writes.
 */
#include <string.h>

#include "esp_log.h"

#include "ultrawidelock_kv.h"
#include "ultrawidelock_prov.h"

static const char *TAG = "ultrawidelock_prov";

/*
 * The store has to hold the largest blob the serialiser can produce, and that
 * size moves with ULTRAWIDELOCK_TRUST_MAX. Caught here at build time rather than
 * at the first provisioning write on a customer's board. Both twins carry the
 * same assertion for the same reason.
 */
_Static_assert(ULTRAWIDELOCK_PROV_BLOB_MAX <= ULTRAWIDELOCK_KV_VALUE_MAX,
	       "the provisioning blob no longer fits one key-value record");

// Load the reader identity and trust store from the key-value store into id and ts.
// On any failure to init the store, read the record, or deserialize it, falls back to the default
// DEV identity (via ultrawidelock_prov_dev_default) and returns a nonzero status: 1 if no
// provisioning was ever stored, -1 on any other store or deserialization error. Returns 0 on a
// successful load of previously stored provisioning.
int ultrawidelock_prov_load(struct ultrawidelock_reader_identity *id,
		    struct ultrawidelock_trust_store *ts)
{
	int rc = ultrawidelock_kv_init();

	if (rc != ULTRAWIDELOCK_KV_OK) {
		ESP_LOGW(TAG, "kv init rc=%d; using DEV identity", rc);
		ultrawidelock_prov_dev_default(id, ts);
		return -1;
	}

	uint8_t buf[ULTRAWIDELOCK_PROV_BLOB_MAX];
	size_t sz = sizeof(buf);

	rc = ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_CRED_PROV, buf, &sz);
	if (rc == ULTRAWIDELOCK_KV_NOT_FOUND) {
		/* Never provisioned. Not an error. */
		ultrawidelock_prov_dev_default(id, ts);
		return 1;
	}
	if (rc != ULTRAWIDELOCK_KV_OK) {
		/*
		 * ULTRAWIDELOCK_KV_INVALID reaches here too, and it means the stored
		 * record is longer than any blob this firmware can produce. It is
		 * treated as malformed rather than retried with a bigger buffer:
		 * nothing that writes this key can exceed the static assert above,
		 * so a longer record is corruption or a future format, and neither
		 * is safe to parse.
		 */
		ESP_LOGW(TAG, "kv get rc=%d; using DEV identity", rc);
		ultrawidelock_prov_dev_default(id, ts);
		return -1;
	}
	if (ultrawidelock_prov_deserialize(buf, sz, id, ts) != 0) {
		ESP_LOGW(TAG, "stored prov blob malformed (%u B); using DEV identity",
			 (unsigned)sz);
		ultrawidelock_prov_dev_default(id, ts);
		return -1;
	}
	return 0;
}

// Serialize and persist the reader identity and trust store.
// Returns 0 on success. Returns -1 if serialization overflows the blob buffer, the store fails to
// init, or the record cannot be written.
int ultrawidelock_prov_store(const struct ultrawidelock_reader_identity *id,
			     const struct ultrawidelock_trust_store *ts)
{
	uint8_t buf[ULTRAWIDELOCK_PROV_BLOB_MAX];
	size_t n;

	if (ultrawidelock_prov_serialize(id, ts, buf, sizeof(buf), &n) != 0) {
		return -1;
	}

	int rc = ultrawidelock_kv_init();

	if (rc != ULTRAWIDELOCK_KV_OK) {
		ESP_LOGW(TAG, "kv init rc=%d", rc);
		return -1;
	}
	rc = ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_CRED_PROV, buf, n);
	if (rc != ULTRAWIDELOCK_KV_OK) {
		ESP_LOGW(TAG, "kv set rc=%d", rc);
		return -1;
	}
	ESP_LOGI(TAG, "provisioning persisted (%u B, %u trusted)", (unsigned)n,
		 ts != NULL ? ts->count : 0u);
	return 0;
}
