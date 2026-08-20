/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_prov (Zephyr settings backend) — the DWM3001CDK twin of the ESP32
 * port's ultrawidelock_prov_nvs.c. The portable serialisation, dev fallback and trust
 * logic all live in ultrawidelock_prov.c; this file only moves that one blob in and out
 * of the settings store on `storage_partition`.
 */
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "ultrawidelock_prov.h"

LOG_MODULE_REGISTER(ultrawidelock_prov, CONFIG_LOG_DEFAULT_LEVEL);

#define ULTRAWIDELOCK_PROV_KEY "ultrawidelock/prov"

static uint8_t s_blob[ULTRAWIDELOCK_PROV_BLOB_MAX];
static size_t s_blob_len;
static bool s_blob_seen;
K_MUTEX_DEFINE(s_backend_lock);

/**
 * Settings callback to deserialize and store a provisioning blob read from persistent storage.
 * Validates that the blob does not exceed s_blob size and returns -EINVAL on overflow or read
 * error; on success stores the blob length and returns 0.
 */
static int prov_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	/* settings_load_subtree() visits every key below the namespace. Only the
	 * exact record is provisioning; accepting a sibling as this blob turns an
	 * unrelated setting into a misleading "malformed provisioning" state. */
	if (strcmp(name, "prov") != 0) {
		return 0;
	}
	s_blob_seen = true;

	if (len > sizeof(s_blob)) {
		return -EINVAL;
	}

	ssize_t got = read_cb(cb_arg, s_blob, len);

	if (got < 0) {
		return (int)got;
	}
	if ((size_t)got != len) {
		return -EIO;
	}
	s_blob_len = (size_t)got;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ultrawidelock_prov, "ultrawidelock", NULL, prov_set, NULL, NULL);

/**
 * Load credential reader identity and trust anchors from persistent settings. Returns 0 on success
 * with stored data loaded, ULTRAWIDELOCK_PROV_LOAD_EMPTY if never provisioned, and a specific
 * negative errno on settings/read/malformed data. Outputs use the marked DEV identity for
 * diagnostics and recovery, but the reader keeps transport offline for every negative result.
 */
static int prov_load_locked(struct ultrawidelock_reader_identity *id,
			    struct ultrawidelock_trust_store *ts)
{
	int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_WRN("settings init rc=%d; using DEV identity", rc);
		ultrawidelock_prov_dev_default(id, ts);
		return rc < 0 ? rc : -EIO;
	}

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_PROV_CLEAR_ON_BOOT)
	/* Before the load, not after: the point is that nothing ever sees the old
	 * blob, so the identity this boot reports is the DEV one. */
	rc = settings_delete(ULTRAWIDELOCK_PROV_KEY);
	LOG_WRN("clear-on-boot: erased " ULTRAWIDELOCK_PROV_KEY " (rc=%d)", rc);
#endif

	s_blob_len = 0;
	s_blob_seen = false;
	rc = settings_load_subtree("ultrawidelock");
	if (rc != 0) {
		LOG_WRN("settings load rc=%d; using DEV identity", rc);
		ultrawidelock_prov_dev_default(id, ts);
		return rc < 0 ? rc : -EIO;
	}

	if (!s_blob_seen) {
		/* Never provisioned. */
		ultrawidelock_prov_dev_default(id, ts);
		return ULTRAWIDELOCK_PROV_LOAD_EMPTY;
	}

	if (ultrawidelock_prov_deserialize(s_blob, s_blob_len, id, ts) != 0) {
		LOG_WRN("stored blob malformed; using DEV identity");
		ultrawidelock_prov_dev_default(id, ts);
		return -EBADMSG;
	}
	return 0;
}

int ultrawidelock_prov_load(struct ultrawidelock_reader_identity *id,
			    struct ultrawidelock_trust_store *ts)
{
	(void)k_mutex_lock(&s_backend_lock, K_FOREVER);
	int rc = prov_load_locked(id, ts);
	(void)k_mutex_unlock(&s_backend_lock);
	return rc;
}

/**
 * Erase the stored credential provisioning blob from persistent settings. Returns 0 on success,
 * negative on settings error; the error is logged as a warning and returned rather than suppressed,
 * because a silent factory reset that left the old anchors in place would pair but then reject the
 * phone.
 */
static int prov_erase_locked(void)
{
	int rc = settings_subsys_init();

	if (rc != 0) {
		LOG_ERR("settings init rc=%d; nothing erased", rc);
		return rc;
	}
	rc = settings_delete(ULTRAWIDELOCK_PROV_KEY);
	/* The rc is reported, not swallowed. A factory reset that quietly did
	 * nothing is worse than one that fails loudly: the board comes back
	 * looking reset, pairs, and then rejects the phone with the old
	 * anchors still in the store. */
	LOG_WRN("factory reset: erased " ULTRAWIDELOCK_PROV_KEY " (rc=%d)", rc);
	return rc;
}

int ultrawidelock_prov_erase(void)
{
	(void)k_mutex_lock(&s_backend_lock, K_FOREVER);
	int rc = prov_erase_locked();
	(void)k_mutex_unlock(&s_backend_lock);
	return rc;
}

/**
 * Serialize and store credential reader identity and trust anchors to persistent settings. Uses a
 * static blob buffer to avoid stack overflow. A backend-local mutex serializes load/store/erase,
 * including direct shell/reset callers outside the portable reader. Returns settings_save_one.
 */
static int prov_store_locked(const struct ultrawidelock_reader_identity *id,
			     const struct ultrawidelock_trust_store *ts)
{
	/*
	 * STATIC, not on the stack. ULTRAWIDELOCK_PROV_BLOB_MAX scales with
	 * ULTRAWIDELOCK_TRUST_MAX, and raising that 4 -> 8 put this at 864 B on a frame
	 * that also holds a 778 B struct ultrawidelock_trust_store. The result was an
	 * MPU fault through the bottom of the 4 KB main stack about four
	 * seconds into boot -- the board advertised, froze, and the phone
	 * reported "transaction timed out" with nothing in the log after the
	 * advert line.
	 *
	 * Safe as static because s_backend_lock covers this complete serialization
	 * and settings write, as well as every load and erase using the same record.
	 */
	static uint8_t blob[ULTRAWIDELOCK_PROV_BLOB_MAX];
	size_t len = 0;

	if (ultrawidelock_prov_serialize(id, ts, blob, sizeof(blob), &len) != 0) {
		return -EINVAL;
	}
	return settings_save_one(ULTRAWIDELOCK_PROV_KEY, blob, len);
}

int ultrawidelock_prov_store(const struct ultrawidelock_reader_identity *id,
			     const struct ultrawidelock_trust_store *ts)
{
	(void)k_mutex_lock(&s_backend_lock, K_FOREVER);
	int rc = prov_store_locked(id, ts);
	(void)k_mutex_unlock(&s_backend_lock);
	return rc;
}
