/* SPDX-License-Identifier: ISC */

/**
 * @file psa_its_freertos.c — PSA Internal Trusted Storage over the port's store.
 *
 * WHY THIS EXISTS, in one sentence: OpenThread's SRP client signs its
 * registrations with an ECDSA key that has to survive a reboot, and letting PSA
 * hold that key is what keeps Mbed TLS's PK, ECP and BIGNUM modules out of an
 * image that has no room for them.
 *
 * Mbed TLS ships an ITS backend, and it is file-based (MBEDTLS_PSA_ITS_FILE_C).
 * There is no filesystem here, so the four entry points are implemented
 * directly against ultrawidelock_freertos_kv.
 *
 * A DIRECTORY, NOT A HASH. PSA identifies an object by a 64-bit uid and this
 * store is keyed by uint16_t. Folding one onto the other would mean two keys
 * could collide, and a collision here is not a lost record -- it is a node
 * signing with the wrong key, or reading another key's bytes as its own. The
 * directory is a fixed array of slots, so it cannot alias, and it refuses when
 * full rather than evicting something it does not understand.
 *
 * WRITE ORDER IS THE CONSISTENCY STORY. There are two records per operation and
 * no transaction, so the order is chosen to make every interrupted write leave
 * the store readable:
 *
 *   set    slot first, then the directory. A crash between them leaves an
 *          orphan slot that nothing points at, which the next set reuses.
 *   remove directory first, then the slot. Same orphan, same outcome.
 *
 * The failure this ordering forbids is the other way round: a directory entry
 * naming a slot that was never written, which would hand PSA a key made of
 * whatever was in that slot before.
 */

#include <string.h>

/*
 * The PUBLIC PSA Storage header, supplied by this port in
 * crypto/psa_native_its/. psa_crypto_storage.c takes its "native ITS
 * implementation" branch whenever MBEDTLS_PSA_ITS_FILE_C is absent, and that
 * branch is the supported way to provide a backend Mbed TLS does not ship.
 *
 * MBEDTLS_PSA_ITS_FILE_C was tried first and is the wrong door: it selects the
 * internal interface but requires MBEDTLS_FS_IO, and there is no filesystem
 * here.
 */
#include <psa/error.h>
#include <psa/internal_trusted_storage.h>

#include <ultrawidelock_freertos_kv.h>
#include <ultrawidelock_freertos_platform.h>

#define ITS_TAG "psa_its"

/*
 * One entry per slot. Written as one record, so the directory is always
 * internally consistent even though it is not consistent with the slots across
 * a crash -- see the ordering note above.
 */
struct its_entry {
	uint64_t uid;
	uint32_t size;
	uint32_t flags;
	uint8_t used;
	uint8_t pad[3];
};

struct its_dir {
	/* Bumped if the layout ever changes. A directory written by a different
	 * layout is discarded rather than reinterpreted: the alternative is
	 * handing PSA a key whose length field came from another struct. */
	uint32_t version;
	struct its_entry slots[ULTRAWIDELOCK_KV_KEY_PSA_ITS_SLOTS];
};

#define ITS_DIR_VERSION 1u

static struct its_dir s_dir;
static bool s_dir_loaded;

static void dir_load(void)
{
	size_t length = sizeof(s_dir);

	if (s_dir_loaded) {
		return;
	}
	memset(&s_dir, 0, sizeof(s_dir));
	s_dir.version = ITS_DIR_VERSION;

	if (ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_PSA_ITS_DIR, &s_dir, &length) == 0) {
		if (length != sizeof(s_dir) || s_dir.version != ITS_DIR_VERSION) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, ITS_TAG,
					 "stored directory is %u bytes v%u, expected %u v%u; "
					 "starting empty",
					 (unsigned)length, (unsigned)s_dir.version,
					 (unsigned)sizeof(s_dir), ITS_DIR_VERSION);
			memset(&s_dir, 0, sizeof(s_dir));
			s_dir.version = ITS_DIR_VERSION;
		}
	}
	s_dir_loaded = true;
}

static int dir_store(void)
{
	return ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_PSA_ITS_DIR, &s_dir, sizeof(s_dir));
}

/** @return slot index, or -1. */
static int slot_of(psa_storage_uid_t uid)
{
	unsigned i;

	dir_load();
	for (i = 0; i < ULTRAWIDELOCK_KV_KEY_PSA_ITS_SLOTS; i++) {
		if (s_dir.slots[i].used != 0u && s_dir.slots[i].uid == uid) {
			return (int)i;
		}
	}
	return -1;
}

static int slot_free(void)
{
	unsigned i;

	dir_load();
	for (i = 0; i < ULTRAWIDELOCK_KV_KEY_PSA_ITS_SLOTS; i++) {
		if (s_dir.slots[i].used == 0u) {
			return (int)i;
		}
	}
	return -1;
}

static uint16_t slot_key(int slot)
{
	return (uint16_t)(ULTRAWIDELOCK_KV_KEY_PSA_ITS_SLOT0 + (unsigned)slot);
}

psa_status_t psa_its_set(psa_storage_uid_t uid, uint32_t data_length, const void *p_data,
			 psa_storage_create_flags_t create_flags)
{
	int slot;
	bool replacing;

	if (p_data == NULL && data_length != 0u) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (data_length > ULTRAWIDELOCK_KV_VALUE_MAX) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, ITS_TAG,
				 "object of %u bytes exceeds the %u-byte record ceiling",
				 (unsigned)data_length, (unsigned)ULTRAWIDELOCK_KV_VALUE_MAX);
		return PSA_ERROR_INSUFFICIENT_STORAGE;
	}

	slot = slot_of(uid);
	replacing = (slot >= 0);
	if (replacing) {
		/*
		 * WRITE_ONCE means what it says. PSA uses it for objects that
		 * must never change after creation, and silently overwriting one
		 * would break the guarantee its owner is relying on.
		 */
		if ((s_dir.slots[slot].flags & PSA_STORAGE_FLAG_WRITE_ONCE) != 0u) {
			return PSA_ERROR_NOT_PERMITTED;
		}
	} else {
		slot = slot_free();
		if (slot < 0) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, ITS_TAG,
					 "all %u slots are taken; refusing a new object",
					 (unsigned)ULTRAWIDELOCK_KV_KEY_PSA_ITS_SLOTS);
			return PSA_ERROR_INSUFFICIENT_STORAGE;
		}
	}

	/* Slot first. A crash before the directory is written leaves bytes
	 * nothing points at, which the next allocation reuses. */
	if (ultrawidelock_kv_set(slot_key(slot), p_data, data_length) != 0) {
		return PSA_ERROR_STORAGE_FAILURE;
	}

	s_dir.slots[slot].uid = uid;
	s_dir.slots[slot].size = data_length;
	s_dir.slots[slot].flags = create_flags;
	s_dir.slots[slot].used = 1u;

	if (dir_store() != 0) {
		/* The slot holds the new bytes and the directory still describes
		 * the old ones. Undo the in-memory half so a later read does not
		 * trust what was not committed. */
		s_dir_loaded = false;
		return PSA_ERROR_STORAGE_FAILURE;
	}
	return PSA_SUCCESS;
}

psa_status_t psa_its_get(psa_storage_uid_t uid, uint32_t data_offset, uint32_t data_length,
			 void *p_data, size_t *p_data_length)
{
	static uint8_t s_record[ULTRAWIDELOCK_KV_VALUE_MAX];
	size_t length = sizeof(s_record);
	int slot = slot_of(uid);

	if (slot < 0) {
		return PSA_ERROR_DOES_NOT_EXIST;
	}
	if (p_data == NULL || p_data_length == NULL) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	if (ultrawidelock_kv_get(slot_key(slot), s_record, &length) != 0) {
		/*
		 * The directory names a slot the store does not have. That is
		 * the inconsistency the write ordering is meant to prevent, so
		 * say so rather than returning DOES_NOT_EXIST and letting it
		 * look like a first boot.
		 */
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, ITS_TAG,
				 "slot %d is in the directory and not in the store", slot);
		return PSA_ERROR_STORAGE_FAILURE;
	}
	if (data_offset > length || data_length > length - data_offset) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	memcpy(p_data, &s_record[data_offset], data_length);
	*p_data_length = data_length;
	return PSA_SUCCESS;
}

psa_status_t psa_its_get_info(psa_storage_uid_t uid, struct psa_storage_info_t *p_info)
{
	int slot = slot_of(uid);

	if (slot < 0) {
		return PSA_ERROR_DOES_NOT_EXIST;
	}
	if (p_info == NULL) {
		return PSA_ERROR_INVALID_ARGUMENT;
	}
	p_info->size = s_dir.slots[slot].size;
	p_info->flags = s_dir.slots[slot].flags;
	return PSA_SUCCESS;
}

psa_status_t psa_its_remove(psa_storage_uid_t uid)
{
	int slot = slot_of(uid);

	if (slot < 0) {
		return PSA_ERROR_DOES_NOT_EXIST;
	}
	if ((s_dir.slots[slot].flags & PSA_STORAGE_FLAG_WRITE_ONCE) != 0u) {
		return PSA_ERROR_NOT_PERMITTED;
	}

	/* Directory first, so an interrupted remove leaves an orphan slot rather
	 * than an entry pointing at bytes that are gone. */
	memset(&s_dir.slots[slot], 0, sizeof(s_dir.slots[slot]));
	if (dir_store() != 0) {
		s_dir_loaded = false;
		return PSA_ERROR_STORAGE_FAILURE;
	}
	(void)ultrawidelock_kv_delete(slot_key(slot));
	return PSA_SUCCESS;
}
