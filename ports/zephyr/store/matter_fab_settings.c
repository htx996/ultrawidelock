/* SPDX-License-Identifier: ISC */

/*
 * matter_fab_settings — see the header for why the port owns this.
 *
 * ONE KEY PER FIELD, not one blob for the lot. A single record would be about
 * 1.7 KB (two fabrics at ~500 B, a 254 B dataset, a 400 B ICAC slot) and would
 * need a buffer that size to build in. This node has ~8.7 KB of RAM left and
 * has already taken an MPU stack-guard fault on this board, so a 1.7 KB
 * temporary is not a neutral choice. Saving each field straight out of the
 * struct it already lives in costs no buffer at all.
 */
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "matter_fab_settings.h"

LOG_MODULE_REGISTER(matter_fab, CONFIG_LOG_DEFAULT_LEVEL);

#define FAB_TREE  "mfab"
#define KEY_VER   FAB_TREE "/ver"
/*
 * Per-fabric keys are BUILT from the index, never listed. The load path already
 * did this; the store and erase paths kept a hand-written KEY_FAB0/KEY_FAB1
 * pair, which is why growing the table needed a BUILD_ASSERT to stop someone
 * adding a slot the store path would silently never write -- a fabric that
 * lives until the next reboot and then is not there.
 */
#define KEY_FAB_TMPL FAB_TREE "/f0"
/* Index of the digit in the template above, so it is derived, not counted. */
#define KEY_FAB_DIGIT (sizeof(KEY_FAB_TMPL) - 2u)
#define KEY_TD    FAB_TREE "/td"
#define KEY_XP    FAB_TREE "/xp"
#define KEY_ICAC  FAB_TREE "/ic"
#define KEY_ICLEN FAB_TREE "/il"
#if MATTER_FEATURE_CLIENT
/*
 * The Binding table, in this record rather than one of its own.
 *
 * It rides along for the commit record: a binding names a node on a FABRIC, so
 * a binding that outlived the fabric table would point at an administrator this
 * node no longer has, and keeping the two under one KEY_OK is what makes them
 * impossible to restore separately. It also means every path that already
 * calls matter_fab_store() persists a binding for free.
 */
#define KEY_BIND FAB_TREE "/bn"
#endif
/*
 * The commit record, and the ONLY key whose presence means "this record is
 * whole". Written last, deleted first.
 *
 * One key per field is cheap in RAM (see the note above) but it gives up
 * atomicity: matter_fab_store() makes up to seven separate backend writes, and
 * a reset between any two of them used to leave a record that passed every
 * check here. KEY_VER is written FIRST, so the version guard saw a matching
 * version; the per-field size checks each saw a field that was either correct
 * or absent; and matter_fab_load() then set commissioning_complete on the
 * result, because "a stored record means commissioning finished" is only true
 * of one that finished being written.
 *
 * The node that came back from that advertised operationally with half an
 * identity and could never complete CASE -- exactly the state the load path
 * already calls worse than having nothing.
 */
#define KEY_OK FAB_TREE "/ok"
/* Any fixed value; only presence is meaningful. Distinctive to read in a dump. */
#define FAB_COMMITTED 0x0FABC0DEu

/*
 * Bumped whenever any persisted struct changes shape.
 *
 * struct matter_fabric is stored raw, so its layout IS the format. The stored
 * size is checked against sizeof() on load and a mismatch discards the record:
 * a node that re-pairs is a nuisance, a node that reads a stale layout as a
 * NOC and an operational private key is a device that fails CASE with no
 * explanation at all.
 */
#define FAB_VERSION 1u

BUILD_ASSERT(MATTER_SUPPORTED_FABRICS < 10u,
	     "the per-fabric settings key carries a single digit");

/**
 * Render every fabric index as "1/2/0", for one log line.
 *
 * Written out rather than printed as two arguments, because the two call sites
 * named fabrics[0] and fabrics[1] positionally and would have gone on reporting
 * exactly two however many the table holds -- a third fabric stored and
 * restored perfectly, with nothing in the log to say so.
 *
 * @param out at least MATTER_SUPPORTED_FABRICS * 2 bytes.
 */
static void fab_slots_str(const struct matter_device_info *info, char *out, size_t cap)
{
	size_t n = 0u;

	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS && n + 2u < cap; i++) {
		if (i != 0u) {
			out[n++] = '/';
		}
		/* Indices are 1..MATTER_SUPPORTED_FABRICS or 0 for empty, so a
		 * single digit covers every value the assert above permits. */
		out[n++] = (char)('0' + (info->fabrics[i].index % 10u));
	}
	out[n] = '\0';
}

/* Where a load puts what it finds. Set for the duration of matter_fab_load(). */
static struct matter_device_info *s_target;
static bool s_found_fabric;
/* Set only by the KEY_OK branch below; see the note on the key itself. */
static bool s_found_commit;

/**
 * Settings callback to deserialize and load stored Matter fabrics, Thread dataset, thread extended
 * PAN ID, ICAC metadata, and ICAC certificate. Validates version and layout match; rejects short
 * fabric reads and mismatched sizes to avoid loading truncated data. Returns -EINVAL on version or
 * layout mismatch, or the result of the read callback on success.
 */
static int fab_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next = NULL;
	struct matter_device_info *info = s_target;
	ssize_t got;

	if (info == NULL) {
		return 0;
	}

	if (settings_name_steq(name, "ver", &next)) {
		uint32_t ver = 0u;

		if (len != sizeof(ver) || read_cb(cb_arg, &ver, sizeof(ver)) < 0) {
			return -EINVAL;
		}
		if (ver != FAB_VERSION) {
			LOG_WRN("stored fabrics are version %u, this build wants %u -- discarding",
				(unsigned int)ver, FAB_VERSION);
			return -EINVAL;
		}
		return 0;
	}

	if (settings_name_steq(name, "ok", &next)) {
		uint32_t magic = 0u;

		if (len != sizeof(magic) || read_cb(cb_arg, &magic, sizeof(magic)) < 0) {
			return -EINVAL;
		}
		/* A wrong value is a record from something else, not a torn one:
		 * treat it as absent rather than rejecting the whole subtree. */
		s_found_commit = (magic == FAB_COMMITTED);
		return 0;
	}

	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		char key[3] = {'f', (char)('0' + i), '\0'};

		if (!settings_name_steq(name, key, &next)) {
			continue;
		}
		/*
		 * A short read here would leave HALF a fabric -- a valid index
		 * with a truncated NOC -- which answers Sigma1 and then fails
		 * every signature. Refuse the record instead.
		 */
		if (len != sizeof(info->fabrics[i])) {
			LOG_WRN("fabric %u is %u B, expected %u -- layout changed, discarding", i,
				(unsigned int)len, (unsigned int)sizeof(info->fabrics[i]));
			return -EINVAL;
		}
		got = read_cb(cb_arg, &info->fabrics[i], sizeof(info->fabrics[i]));
		if (got < 0) {
			return (int)got;
		}
		if (info->fabrics[i].index != 0u) {
			s_found_fabric = true;
		}
		return 0;
	}

	if (settings_name_steq(name, "td", &next)) {
		if (len > sizeof(info->thread_dataset)) {
			return -EINVAL;
		}
		got = read_cb(cb_arg, info->thread_dataset, len);
		if (got < 0) {
			return (int)got;
		}
		info->thread_dataset_len = (size_t)got;
		return 0;
	}

	if (settings_name_steq(name, "xp", &next)) {
		if (len != sizeof(info->thread_xpanid)) {
			return -EINVAL;
		}
		got = read_cb(cb_arg, info->thread_xpanid, sizeof(info->thread_xpanid));
		if (got < 0) {
			return (int)got;
		}
		info->have_thread_xpanid = true;
		return 0;
	}

#if MATTER_FEATURE_CLIENT
	if (settings_name_steq(name, "bn", &next)) {
		/*
		 * Refused rather than read short, for the reason the fabric
		 * above gives: a truncated table has a count that no longer
		 * describes the entries under it, and this node would then
		 * unlock a door named by whatever was left in the tail.
		 */
		if (len != sizeof(info->binding)) {
			LOG_WRN("stored bindings are %u B, expected %u -- discarding",
				(unsigned int)len, (unsigned int)sizeof(info->binding));
			return -EINVAL;
		}
		got = read_cb(cb_arg, &info->binding, sizeof(info->binding));
		if (got < 0) {
			return (int)got;
		}
		return 0;
	}
#endif

	if (settings_name_steq(name, "il", &next)) {
		uint32_t both = 0u;

		if (len != sizeof(both) || read_cb(cb_arg, &both, sizeof(both)) < 0) {
			return -EINVAL;
		}
		info->icac.len = (size_t)(both & 0xFFFFu);
		info->icac.owner_index = (uint8_t)(both >> 16);
		return 0;
	}

	if (settings_name_steq(name, "ic", &next)) {
		if (len > sizeof(info->icac.buf)) {
			return -EINVAL;
		}
		got = read_cb(cb_arg, info->icac.buf, len);
		if (got < 0) {
			return (int)got;
		}
		return 0;
	}

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(matter_fab, FAB_TREE, NULL, fab_set, NULL, NULL);

int matter_fab_store(const struct matter_device_info *info)
{
	uint32_t ver = FAB_VERSION;
	uint32_t ok = FAB_COMMITTED;
	int rc;

	if (info == NULL) {
		return -EINVAL;
	}

	/*
	 * Invalidate before touching anything, so the window in which this
	 * record is half-written is also the window in which it is unreadable.
	 * A failure here has to abort: leaving a previous commit record in
	 * place while overwriting the fields under it is precisely the torn
	 * state this exists to prevent.
	 */
	rc = settings_delete(KEY_OK);
	if (rc != 0) {
		LOG_ERR("cannot invalidate the stored identity before rewriting it (%d)", rc);
		return rc;
	}

	rc = settings_save_one(KEY_VER, &ver, sizeof(ver));
	if (rc != 0) {
		LOG_ERR("cannot store the fabric version (%d)", rc);
		return rc;
	}

	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		char key[] = KEY_FAB_TMPL;

		key[KEY_FAB_DIGIT] = (char)('0' + i);
		rc = settings_save_one(key, &info->fabrics[i], sizeof(info->fabrics[i]));
		if (rc != 0) {
			/* Which one. "cannot store a fabric (-28)" named the
			 * error and not the slot, and on a full partition the
			 * slot is the whole diagnosis. */
			LOG_ERR("cannot store fabric %u (%d)", (unsigned int)i, rc);
			return rc;
		}
	}

	if (info->thread_dataset_len != 0u) {
		rc = settings_save_one(KEY_TD, info->thread_dataset, info->thread_dataset_len);
		if (rc != 0) {
			LOG_ERR("cannot store the Thread dataset (%d)", rc);
			return rc;
		}
	}
	if (info->have_thread_xpanid) {
		rc = settings_save_one(KEY_XP, info->thread_xpanid, sizeof(info->thread_xpanid));
		if (rc != 0) {
			/* Was discarded behind a (void). Low blast radius on its
			 * own -- the xpanid only feeds an attribute report, and
			 * the dataset carries it redundantly for rejoining -- but
			 * a write that failed still means this record is not the
			 * one that was asked for, and the commit below must not
			 * claim otherwise. */
			LOG_ERR("cannot store the Thread extended PAN id (%d)", rc);
			return rc;
		}
	}

#if MATTER_FEATURE_CLIENT
	/*
	 * Written whole, including an empty one: "no bindings" is a
	 * configuration an administrator can choose, and a store that skipped
	 * an empty table would restore the last non-empty one after a reboot --
	 * a lock that starts unlocking a door its owner unbound.
	 */
	rc = settings_save_one(KEY_BIND, &info->binding, sizeof(info->binding));
	if (rc != 0) {
		LOG_ERR("cannot store the binding table (%d)", rc);
		return rc;
	}
#endif

	/*
	 * The ICAC costs 400 B and Apple has never sent one (icac_len is 0 on
	 * every fabric this node has held), so it is written only when it
	 * exists rather than reserved against a case that has not occurred.
	 */
	if (info->icac.len != 0u) {
		uint32_t both = (uint32_t)info->icac.len | ((uint32_t)info->icac.owner_index << 16);

		rc = settings_save_one(KEY_ICLEN, &both, sizeof(both));
		if (rc == 0) {
			rc = settings_save_one(KEY_ICAC, info->icac.buf, info->icac.len);
		}
		if (rc != 0) {
			LOG_ERR("cannot store the intermediate certificate (%d)", rc);
			return rc;
		}
	}

	/*
	 * LAST. Everything above is now on the medium, so this is the write
	 * that makes the record mean something. Until it lands, a load sees an
	 * uncommitted record and comes up commissionable -- which costs a
	 * re-pair, against a half-identity costing a device the controller can
	 * neither reach nor repair.
	 */
	rc = settings_save_one(KEY_OK, &ok, sizeof(ok));
	if (rc != 0) {
		LOG_ERR("cannot commit the stored identity (%d)", rc);
		return rc;
	}

	{
		char slots[MATTER_SUPPORTED_FABRICS * 2u];

		fab_slots_str(info, slots, sizeof(slots));
		LOG_INF("operational identity stored (fabric %s, dataset %u B)", slots,
			(unsigned int)info->thread_dataset_len);
	}
	return 0;
}

/* Undo a partial read. Shared, because two paths reject a record and both have
 * to leave the same nothing behind. */
static void discard_partial(struct matter_device_info *info)
{
	memset(info->fabrics, 0, sizeof(info->fabrics));
	info->thread_dataset_len = 0u;
	info->have_thread_xpanid = false;
	info->icac.len = 0u;
	info->icac.owner_index = 0u;
#if MATTER_FEATURE_CLIENT
	memset(&info->binding, 0, sizeof(info->binding));
#endif
}

int matter_fab_load(struct matter_device_info *info)
{
	int rc;

	if (info == NULL) {
		return -EINVAL;
	}

	s_target = info;
	s_found_fabric = false;
	s_found_commit = false;
	rc = settings_load_subtree(FAB_TREE);
	s_target = NULL;

	if (rc != 0) {
		/*
		 * A rejected record leaves the table PARTLY filled, and half an
		 * identity is worse than none: it advertises operational and
		 * cannot complete CASE. Clear what was read.
		 */
		LOG_WRN("stored fabrics unusable (%d); clearing and coming up commissionable", rc);
		discard_partial(info);
		return rc;
	}

	/*
	 * Read something, but nobody ever said it was finished. See KEY_OK: the
	 * per-field checks above cannot detect this, because each field they
	 * saw was individually well formed -- it is the SET that is incomplete.
	 * Treated as an empty store rather than as an error, because that is
	 * what it is: no commissioning ever completed against this record.
	 */
	if (!s_found_commit) {
		if (s_found_fabric) {
			LOG_WRN("stored identity was never committed -- discarding a torn record "
				"and coming up commissionable");
		}
		discard_partial(info);
		return 1;
	}

	if (!s_found_fabric) {
		return 1;
	}

	/*
	 * A stored record MEANS commissioning finished -- nothing writes one
	 * before CommissioningComplete. Restoring it as false leaves a fully
	 * commissioned node looking like it is mid-pairing, and the next
	 * commissioner to open a PASE session rolls back the very fabrics that
	 * were just restored. That silently destroys a working pairing on the
	 * first failed connection attempt after a reboot.
	 */
	info->commissioning_complete = true;

	{
		char slots[MATTER_SUPPORTED_FABRICS * 2u];

		fab_slots_str(info, slots, sizeof(slots));
		LOG_INF("operational identity restored (fabric %s, dataset %u B)", slots,
			(unsigned int)info->thread_dataset_len);
	}
	return 0;
}

int matter_fab_erase(void)
{
	/* KEY_OK first: if the erase is interrupted part way, what is left
	 * behind is already uncommitted rather than a plausible half-record. */
	static const char *const keys[] = { KEY_OK, KEY_VER,   KEY_TD,
					    KEY_XP, KEY_ICLEN, KEY_ICAC,
#if MATTER_FEATURE_CLIENT
					    KEY_BIND,
#endif
	};
	int first_err = 0;

	/*
	 * Every return code checked. These were discarded behind a (void) and
	 * the function logged "erased" unconditionally, so a wipe that deleted
	 * NOTHING was indistinguishable from one that worked -- the board came
	 * back with the same fabrics, which reads as the erase never having been
	 * asked for rather than having failed.
	 */
	for (size_t i = 0u; i < ARRAY_SIZE(keys); i++) {
		int rc = settings_delete(keys[i]);

		if (rc != 0 && first_err == 0) {
			first_err = rc;
		}
		LOG_WRN("erase %s -> rc=%d", keys[i], rc);
	}
	/*
	 * After KEY_OK, so an interrupted erase never leaves fabrics behind a
	 * commit record that still says they are whole.
	 */
	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		char key[] = KEY_FAB_TMPL;
		int rc;

		key[KEY_FAB_DIGIT] = (char)('0' + i);
		rc = settings_delete(key);
		if (rc != 0 && first_err == 0) {
			first_err = rc;
		}
		LOG_WRN("erase %s -> rc=%d", key, rc);
	}
	if (first_err != 0) {
		LOG_ERR("operational identity NOT fully erased (first rc=%d)", first_err);
		return first_err;
	}
	LOG_WRN("operational identity erased");
	return 0;
}

/* ---- the writable Door Lock attributes ------------------------------------ */

/*
 * A tree of their own rather than more keys under "mfab".
 *
 * These are not part of the operational identity and must not share its
 * lifetime: matter_fab_erase() un-commissions the node, and a controller's
 * auto-lock timing is not something to forget while doing that. Separated, a
 * factory reset that clears the whole store still clears both, which is the
 * only case where they should go.
 */
#define DL_TREE          "mdl"
#define KEY_AUTO_RELOCK  DL_TREE "/art"
#define KEY_APPROACH     DL_TREE "/apd"

/**
 * Load one fixed-width attribute. A record of any other length is left for the
 * caller's boot default -- it was written by a layout this build does not know,
 * and half of it is not a value.
 */
struct dl_attr_target {
	void *value;
	size_t len;
	bool found;
};

static int dl_attr_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg,
		       void *param)
{
	struct dl_attr_target *t = param;

	(void)name;

	if (len != t->len) {
		LOG_WRN("stored Door Lock attribute is %u B, this build wants %u -- discarding",
			(unsigned int)len, (unsigned int)t->len);
		return 0;
	}
	if (read_cb(cb_arg, t->value, t->len) < 0) {
		return 0;
	}
	t->found = true;
	return 0;
}

int matter_dl_attr_store(const struct matter_device_info *info, uint32_t prev_auto_relock_s,
			 uint8_t prev_approach_direction)
{
	int first_err = 0;
	int rc;

	if (info == NULL) {
		return -EINVAL;
	}
	if (info->auto_relock_time_s != prev_auto_relock_s) {
		rc = settings_save_one(KEY_AUTO_RELOCK, &info->auto_relock_time_s,
				       sizeof(info->auto_relock_time_s));
		if (rc != 0) {
			LOG_ERR("AutoRelockTime not persisted (rc=%d); a reboot forgets it", rc);
			first_err = rc;
		}
	}
	if (info->approach_direction != prev_approach_direction) {
		rc = settings_save_one(KEY_APPROACH, &info->approach_direction,
				       sizeof(info->approach_direction));
		if (rc != 0) {
			LOG_ERR("approach direction not persisted (rc=%d); a reboot forgets it",
				rc);
			if (first_err == 0) {
				first_err = rc;
			}
		}
	}
	return first_err;
}

int matter_dl_attr_load(struct matter_device_info *info)
{
	struct dl_attr_target relock;
	struct dl_attr_target approach;

	if (info == NULL) {
		return -EINVAL;
	}

	relock.value = &info->auto_relock_time_s;
	relock.len = sizeof(info->auto_relock_time_s);
	relock.found = false;
	(void)settings_load_subtree_direct(KEY_AUTO_RELOCK, dl_attr_set, &relock);

	approach.value = &info->approach_direction;
	approach.len = sizeof(info->approach_direction);
	approach.found = false;
	(void)settings_load_subtree_direct(KEY_APPROACH, dl_attr_set, &approach);

	if (relock.found || approach.found) {
		LOG_INF("Door Lock settings restored (auto-relock %u s, approach 0x%02x)",
			(unsigned int)info->auto_relock_time_s,
			(unsigned int)info->approach_direction);
	}
	return 0;
}

int matter_dl_attr_erase(void)
{
	static const char *const keys[] = { KEY_AUTO_RELOCK, KEY_APPROACH };
	int first_err = 0;

	for (size_t i = 0u; i < ARRAY_SIZE(keys); i++) {
		int rc = settings_delete(keys[i]);

		if (rc != 0 && first_err == 0) {
			first_err = rc;
		}
	}
	return first_err;
}
