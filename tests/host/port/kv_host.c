/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_kv.h on host RAM. The host backend and the test fake are the
 * same file, for the reason flash_host.c gives: a fake more permissive than the
 * contract hides the bugs the contract exists to catch.
 *
 * So this one enforces what the header promises -- a value over
 * ULTRAWIDELOCK_KV_VALUE_MAX is refused, ULTRAWIDELOCK_KV_KEY_NONE is not a key,
 * and an undersized read buffer is refused with the stored length handed back
 * rather than a truncated value. Truncation is the failure mode that would
 * otherwise reach a caller as a short key: fine on the bench, wrong on a device
 * holding a longer record.
 *
 * It does NOT enforce window membership. The windows in the header are how
 * consumers avoid colliding, not something a real store can check, and a fake
 * that rejects keys the devices accept would fail tests that should pass.
 *
 * A fixed table, not a heap: the host suite should not need an allocator to
 * describe what a flash-page store does.
 */

#include "ultrawidelock_kv.h"

#include <stdbool.h>
#include <string.h>

#define SLOTS 64u

struct slot {
	uint16_t key;
	uint16_t len;
	uint8_t value[ULTRAWIDELOCK_KV_VALUE_MAX];
};

static struct slot s_slots[SLOTS];
static bool s_mounted;

static void format(void)
{
	for (size_t i = 0; i < SLOTS; i++) {
		s_slots[i].key = ULTRAWIDELOCK_KV_KEY_NONE;
		s_slots[i].len = 0u;
	}
}

static struct slot *find(uint16_t key)
{
	for (size_t i = 0; i < SLOTS; i++) {
		if (s_slots[i].key == key) {
			return &s_slots[i];
		}
	}
	return NULL;
}

int ultrawidelock_kv_init(void)
{
	/* Later calls are no-ops, per the contract: a second init must not be a
	 * way to lose the store. */
	if (!s_mounted) {
		format();
		s_mounted = true;
	}
	return ULTRAWIDELOCK_KV_OK;
}

int ultrawidelock_kv_get(uint16_t key, void *value, size_t *length)
{
	const struct slot *s;

	if (length == NULL || key == ULTRAWIDELOCK_KV_KEY_NONE) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (!s_mounted) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	s = find(key);
	if (s == NULL) {
		return ULTRAWIDELOCK_KV_NOT_FOUND;
	}
	/* The stored length goes back on BOTH the success and the too-small
	 * path, which is what lets a caller size a second attempt. */
	if (value == NULL || *length < s->len) {
		size_t stored = s->len;
		int rc = (value == NULL && *length == 0u) ? ULTRAWIDELOCK_KV_OK
							  : ULTRAWIDELOCK_KV_INVALID;

		*length = stored;
		return rc;
	}
	memcpy(value, s->value, s->len);
	*length = s->len;
	return ULTRAWIDELOCK_KV_OK;
}

int ultrawidelock_kv_set(uint16_t key, const void *value, size_t length)
{
	struct slot *s;

	if (key == ULTRAWIDELOCK_KV_KEY_NONE || length > ULTRAWIDELOCK_KV_VALUE_MAX) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (length > 0u && value == NULL) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (!s_mounted) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	s = find(key);
	if (s == NULL) {
		s = find(ULTRAWIDELOCK_KV_KEY_NONE);
		if (s == NULL) {
			return ULTRAWIDELOCK_KV_FULL;
		}
	}
	if (length > 0u) {
		memcpy(s->value, value, length);
	}
	s->len = (uint16_t)length;
	s->key = key;
	return ULTRAWIDELOCK_KV_OK;
}

int ultrawidelock_kv_delete(uint16_t key)
{
	struct slot *s;

	if (key == ULTRAWIDELOCK_KV_KEY_NONE) {
		return ULTRAWIDELOCK_KV_INVALID;
	}
	if (!s_mounted) {
		return ULTRAWIDELOCK_KV_CORRUPT;
	}
	s = find(key);
	if (s == NULL) {
		return ULTRAWIDELOCK_KV_NOT_FOUND;
	}
	/* Wiped, not just unlinked: a key deleted here held a credential in
	 * every consumer that has one so far. */
	memset(s->value, 0, sizeof(s->value));
	s->key = ULTRAWIDELOCK_KV_KEY_NONE;
	s->len = 0u;
	return ULTRAWIDELOCK_KV_OK;
}

int ultrawidelock_kv_erase_all(void)
{
	memset(s_slots, 0, sizeof(s_slots));
	format();
	s_mounted = true;
	return ULTRAWIDELOCK_KV_OK;
}
