/* SPDX-License-Identifier: ISC */

/**
 * @file matter_settings_freertos.c — Zephyr's settings API over the port's store.
 *
 * The shared Matter Thread transport keeps exactly one thing here: the SRP
 * host-name suffix, a 32-bit value minted once per lifetime. Everything about
 * this file is sized by that.
 *
 * WHY THE PATH TABLE IS FIXED, and why an unknown path is an error rather than
 * a miss: Zephyr's settings are a string-keyed tree, and this port's store is a
 * flat uint16_t keyspace. A general translation would have to invent a hash,
 * and a hash collision here would silently alias two records. A table cannot
 * collide, and it makes the set of things Matter persists something you can
 * read in one screen.
 *
 * The failure that matters is a path this table does not know. A tree-shaped
 * backend would answer "nothing stored", which is exactly what a first boot
 * looks like -- so the caller would mint a fresh value, fail to save it, and do
 * the same again next boot, with the only symptom a name that changes every
 * time. This backend refuses instead, and says which path.
 */

/*
 * The port's own header, not <zephyr/settings/settings.h>.
 *
 * This file is port code and nothing forces the foreign spelling on it. The
 * Zephyr-named header in matter_compat/ forwards HERE; only the shared sources,
 * which live in the Zephyr tree and are compiled unmodified, reach it. Written
 * the other way round this file was the single Zephyr include under
 * ports/freertos-nrf52833/, and the purity guard was right to say so.
 */
#include "matter_settings_freertos.h"

#include <string.h>

#include <ultrawidelock_freertos_kv.h>
#include <ultrawidelock_freertos_platform.h>

#define SETTINGS_TAG "matter_settings"

/** One entry per record Matter persists. */
struct ultrawidelock_matter_setting {
	const char *path;
	uint16_t key;
	size_t len;
};

/*
 * SRP_HOST_ID_KEY in the shared transport. Kept in step by
 * scripts/freertos-matter-source-check.sh, which fails the build if the string
 * on the other side ever stops matching this one -- a rename there would
 * otherwise land here as "unknown path" at run time, on a board.
 */
/*
 * LEN 0 means "variable, bounded by the store". The SRP host id is the only
 * fixed-width record; every fabric record is written at whatever length the
 * store decided, and checking those lengths is the fabric store's own job --
 * it version-stamps them and discards a record whose size does not match the
 * layout it expects, which is a stronger check than anything possible here.
 */
#define ULTRAWIDELOCK_SETTING_VARIABLE 0u

static const struct ultrawidelock_matter_setting s_settings[] = {
	{ "srp/hid", ULTRAWIDELOCK_KV_KEY_MATTER_SRP_HOST_ID, sizeof(uint32_t) },
	/* The operational identity. "mfab" is FAB_TREE in the shared store. */
	{ "mfab/ver", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_VER, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/ok", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_OK, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/td", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_TD, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/xp", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_XP, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/il", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_ICLEN, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/ic", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_ICAC, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/f0", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT0 + 0u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/f1", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT0 + 1u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/f2", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT0 + 2u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/f3", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT0 + 3u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mfab/f4", ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT0 + 4u, ULTRAWIDELOCK_SETTING_VARIABLE },
	/* Atomic per-slot schema used by the shared DWM Matter stack. */
	{ "mf2/meta", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_META, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/net", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_NET, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/ic", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ICAC, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/f0", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB0 + 0u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/f1", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB0 + 1u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/f2", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB0 + 2u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/f3", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB0 + 3u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/f4", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB0 + 4u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/a0", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL0 + 0u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/a1", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL0 + 1u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/a2", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL0 + 2u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/a3", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL0 + 3u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "mf2/a4", ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL0 + 4u, ULTRAWIDELOCK_SETTING_VARIABLE },
	/*
	 * Subscriptions held over a reboot, one per CASE session slot ("msub" in
	 * matter_commission.c). Variable length: the record layout belongs to
	 * the app, and its loader discards a record whose size it does not
	 * recognise.
	 */
	{ "msub/0", ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 + 0u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "msub/1", ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 + 1u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "msub/2", ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 + 2u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "msub/3", ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 + 3u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "msub/4", ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 + 4u, ULTRAWIDELOCK_SETTING_VARIABLE },
	{ "msub/5", ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 + 5u, ULTRAWIDELOCK_SETTING_VARIABLE },
	/* The two Door Lock attributes a controller writes ("mdl" in
	 * matter_commission.c). Fixed-width: the value is the attribute itself,
	 * so a record of any other size is not a record. */
	{ "mdl/art", ULTRAWIDELOCK_KV_KEY_MATTER_DL_AUTO_RELOCK, sizeof(uint32_t) },
	{ "mdl/apd", ULTRAWIDELOCK_KV_KEY_MATTER_DL_APPROACH, sizeof(uint8_t) },
};

/*
 * The fabric slots are spelled out above rather than generated, so this table
 * is the whole truth about what this node persists. If MATTER_SUPPORTED_FABRICS
 * grows past the slots listed here the store will ask for a new `mf2/fN`, and an
 * unknown path is refused and NAMED rather than silently lost -- which is the
 * property this backend is built around.
 */
_Static_assert(ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT0 + 5u <= ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT_LIMIT,
	       "fabric slot keys have outgrown their window");
_Static_assert(ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB0 + 5u <=
		       ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB_LIMIT,
	       "mf2 fabric slot keys have outgrown their window");
_Static_assert(ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL0 + 5u <=
		       ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL_LIMIT,
	       "mf2 ACL slot keys have outgrown their window");
_Static_assert(ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 + 6u <= ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT_LIMIT,
	       "subscription slot keys have outgrown their window");

static const struct ultrawidelock_matter_setting *lookup(const char *path)
{
	size_t i;

	for (i = 0; i < sizeof(s_settings) / sizeof(s_settings[0]); i++) {
		if (strcmp(s_settings[i].path, path) == 0) {
			return &s_settings[i];
		}
	}
	return NULL;
}

/*
 * The store is brought up by the board before any of this runs, and doing it
 * twice is not free -- it walks the flash. Zephyr's call is idempotent and the
 * transport relies on that, so this is the honest translation of "already
 * done".
 */
int settings_subsys_init(void)
{
	return 0;
}

/** Copies out of the record this backend already holds; no second read. */
static ssize_t direct_read(void *cb_arg, void *data, size_t len)
{
	const uint8_t *src = cb_arg;

	memcpy(data, src, len);
	return (ssize_t)len;
}

/*
 * One record at a time, and it has to be static: the largest is a fabric entry
 * at over 400 bytes, which is far too much for the stack of whatever task
 * happens to be committing an identity. Nothing here reenters -- every caller
 * is the fabric store or the Thread transport, both on their own tasks and
 * neither holding this across a yield.
 */
static uint8_t s_record[ULTRAWIDELOCK_KV_VALUE_MAX];

int settings_load_subtree_direct(const char *subtree, settings_load_direct_cb cb, void *param)
{
	const struct ultrawidelock_matter_setting *s = lookup(subtree);
	size_t length = sizeof(s_record);

	if (s == NULL) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, SETTINGS_TAG,
				 "no key for settings path '%s'", subtree);
		return -22; /* -EINVAL */
	}
	if (ultrawidelock_kv_get(s->key, s_record, &length) != 0) {
		/* Nothing stored yet. Zephyr reports this by not calling the
		 * callback, and the caller reads that as "mint one". */
		return 0;
	}
	/*
	 * A fixed-width record of the wrong size is not a record. Handing it to
	 * the callback would have it read past a shorter buffer; dropping it
	 * makes the next boot mint a fresh value, which is the recoverable
	 * outcome. Variable records are checked by their own owner.
	 */
	if (s->len != ULTRAWIDELOCK_SETTING_VARIABLE && length != s->len) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, SETTINGS_TAG,
				 "'%s' stored as %u bytes, expected %u; ignoring", subtree,
				 (unsigned)length, (unsigned)s->len);
		return 0;
	}
	return cb("", length, direct_read, s_record, param);
}

int settings_save_one(const char *name, const void *value, size_t val_len)
{
	const struct ultrawidelock_matter_setting *s = lookup(name);

	if (s == NULL) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, SETTINGS_TAG,
				 "no key for settings path '%s'", name);
		return -22;
	}
	if (s->len != ULTRAWIDELOCK_SETTING_VARIABLE && val_len != s->len) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, SETTINGS_TAG,
				 "'%s' saved as %u bytes, table says %u", name,
				 (unsigned)val_len, (unsigned)s->len);
		return -22;
	}
	return ultrawidelock_kv_set(s->key, value, val_len);
}

int settings_delete(const char *name)
{
	const struct ultrawidelock_matter_setting *s = lookup(name);

	if (s == NULL) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, SETTINGS_TAG,
				 "no key for settings path '%s'", name);
		return -22;
	}
	/*
	 * Deleting what was never written is success, not failure. The fabric
	 * store erases the commit marker before rewriting an identity and calls
	 * this on every key when forgetting one, so "already absent" is the
	 * normal case on a node that was never commissioned.
	 */
	(void)ultrawidelock_kv_delete(s->key);
	return 0;
}

/* ---- the registered-handler half ------------------------------------------ */

/*
 * Zephyr collects these from a linker section. This port has exactly one
 * registrant, the Matter fabric store, so a single slot is the whole registry.
 * A second one is refused loudly rather than overwriting the first, because a
 * silently dropped handler means an identity that saves and never loads.
 */
static const struct settings_handler_static *s_handler;

void ultrawidelock_settings_register(const struct settings_handler_static *h)
{
	if (s_handler != NULL && s_handler != h) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, SETTINGS_TAG,
				 "a second settings handler ('%s') was registered; "
				 "this backend holds one",
				 h != NULL && h->name != NULL ? h->name : "?");
		return;
	}
	s_handler = h;
}

bool settings_name_steq(const char *name, const char *key, const char **next)
{
	size_t n;

	if (next != NULL) {
		*next = NULL;
	}
	if (name == NULL || key == NULL) {
		return false;
	}
	n = strlen(key);
	if (strncmp(name, key, n) != 0) {
		return false;
	}
	/* A match ends at the end of the name or at a separator; "ic" must not
	 * match "iclen". */
	if (name[n] == '\0') {
		return true;
	}
	if (name[n] == '/') {
		if (next != NULL) {
			*next = &name[n + 1];
		}
		return true;
	}
	return false;
}

int settings_load_subtree(const char *subtree)
{
	size_t i;
	size_t tree_len;

	if (s_handler == NULL || s_handler->h_set == NULL || subtree == NULL) {
		return 0;
	}
	tree_len = strlen(subtree);

	/*
	 * Zephyr walks what is STORED. This walks what is KNOWN and asks the
	 * store for each, which reaches the same set because the table is the
	 * only way a record can be written in the first place -- there is no
	 * path into this backend that the table does not name.
	 *
	 * Order is table order, and the fabric store does not depend on it: it
	 * validates the version and the commit marker after the walk, not
	 * during it.
	 */
	for (i = 0; i < sizeof(s_settings) / sizeof(s_settings[0]); i++) {
		const struct ultrawidelock_matter_setting *s = &s_settings[i];
		size_t length = sizeof(s_record);
		int rc;

		if (strncmp(s->path, subtree, tree_len) != 0 || s->path[tree_len] != '/') {
			continue;
		}
		if (ultrawidelock_kv_get(s->key, s_record, &length) != 0) {
			continue;
		}
		/* The handler is given the leaf, as Zephyr gives it. */
		rc = s_handler->h_set(&s->path[tree_len + 1], length, direct_read, s_record);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}
