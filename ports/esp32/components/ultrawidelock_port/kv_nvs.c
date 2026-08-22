/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_kv.h on ESP-IDF NVS.
 *
 * The twin of ports/zephyr/store/kv_zephyr.c, and the same trick: the caller
 * hands over a uint16_t and key_of() spells it "%04x" -- four characters, always
 * -- inside a namespace of three. NVS caps both at 15, so neither can be reached
 * by any key a caller can pass.
 *
 * That cap is why this seam exists rather than being a tidy-up. NVS does not
 * fail an over-long name symmetrically: nvs_open(NVS_READONLY) simply never
 * matches it, so a read reports "never stored" and only a write says
 * ESP_ERR_NVS_KEY_TOO_LONG out loud. An 18-character namespace survived a
 * rename, a test suite and a release that way (docs/esp32-gotchas.md 8.4). A
 * derived four-digit name cannot reproduce that bug.
 *
 * One namespace for the whole store, so erase_all is nvs_erase_all() on that
 * handle: every other namespace on the part -- the reader's provisioning blob,
 * PIV's keys, Matter's own storage -- survives a factory reset of this one.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "ultrawidelock_kv.h"

/* Three characters against a cap of 15. */
#define KV_NS "uwl"
/* Four hex digits plus NUL. */
#define KV_KEY_LEN 5u

static bool s_ready;

static void key_of(uint16_t key, char out[KV_KEY_LEN])
{
	(void)snprintf(out, KV_KEY_LEN, "%04x", (unsigned)key);
}

static int from_esp(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return ULTRAWIDELOCK_KV_OK;
	case ESP_ERR_NVS_NOT_FOUND:
		return ULTRAWIDELOCK_KV_NOT_FOUND;
	case ESP_ERR_NVS_INVALID_LENGTH:
		return ULTRAWIDELOCK_KV_INVALID;
	case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
		return ULTRAWIDELOCK_KV_FULL;
	default:
		return ULTRAWIDELOCK_KV_IO;
	}
}

int ultrawidelock_kv_init(void)
{
	esp_err_t err;

	if (s_ready) {
		return ULTRAWIDELOCK_KV_OK;
	}
	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		/* The one recoverable mount failure, and the same recovery the
		 * reader's provisioning backend does: an unusable partition is
		 * reformatted rather than left to fail every call after it. */
		if (nvs_flash_erase() != ESP_OK) {
			return ULTRAWIDELOCK_KV_CORRUPT;
		}
		err = nvs_flash_init();
	}
	if (err != ESP_OK) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	s_ready = true;
	return ULTRAWIDELOCK_KV_OK;
}

int ultrawidelock_kv_get(uint16_t key, void *value, size_t *length)
{
	char name[KV_KEY_LEN];
	nvs_handle_t h;
	esp_err_t err;
	size_t len;
	int rc;

	if (length == NULL || key == ULTRAWIDELOCK_KV_KEY_NONE) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (!s_ready) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	/* A namespace that was never written does not exist, and read-only open
	 * says NOT_FOUND rather than creating it. That is the right answer for
	 * every key in it. */
	err = nvs_open(KV_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		return err == ESP_ERR_NVS_NOT_FOUND ? ULTRAWIDELOCK_KV_NOT_FOUND : from_esp(err);
	}
	key_of(key, name);
	len = *length;
	err = nvs_get_blob(h, name, value, &len);
	nvs_close(h);
	rc = from_esp(err);
	/* NVS reports the stored length on both the success and the too-short
	 * path, which is exactly the contract: refused, never truncated, and
	 * the caller can size a second attempt. */
	if (rc == ULTRAWIDELOCK_KV_OK || rc == ULTRAWIDELOCK_KV_INVALID) {
		*length = len;
	}
	return rc;
}

int ultrawidelock_kv_set(uint16_t key, const void *value, size_t length)
{
	char name[KV_KEY_LEN];
	nvs_handle_t h;
	esp_err_t err;

	if (key == ULTRAWIDELOCK_KV_KEY_NONE || length > ULTRAWIDELOCK_KV_VALUE_MAX) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (length > 0u && value == NULL) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (!s_ready) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	err = nvs_open(KV_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return from_esp(err);
	}
	key_of(key, name);
	err = nvs_set_blob(h, name, value, length);
	if (err == ESP_OK) {
		/* Uncommitted writes are lost on reset, and everything stored
		 * here is something a reboot is supposed to survive. */
		err = nvs_commit(h);
	}
	nvs_close(h);
	return from_esp(err);
}

int ultrawidelock_kv_delete(uint16_t key)
{
	char name[KV_KEY_LEN];
	nvs_handle_t h;
	esp_err_t err;

	if (key == ULTRAWIDELOCK_KV_KEY_NONE) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (!s_ready) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	err = nvs_open(KV_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return from_esp(err);
	}
	key_of(key, name);
	/* nvs_erase_key already reports NOT_FOUND for a key that was not there,
	 * so unlike the settings backend this needs no probe first. */
	err = nvs_erase_key(h, name);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	nvs_close(h);
	return from_esp(err);
}

int ultrawidelock_kv_erase_all(void)
{
	nvs_handle_t h;
	esp_err_t err;

	if (!s_ready) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	err = nvs_open(KV_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return from_esp(err);
	}
	/* One namespace, not the partition: nvs_flash_erase() would take the
	 * reader's provisioning blob, PIV's keys and Matter's storage with it. */
	err = nvs_erase_all(h);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	nvs_close(h);
	return from_esp(err);
}
