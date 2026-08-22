/* SPDX-License-Identifier: ISC */

/* Durable Matter identity storage for the custom DWM3001CDK stack. */
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "matter_fab_settings.h"

LOG_MODULE_REGISTER(matter_fab, CONFIG_LOG_DEFAULT_LEVEL);

#define FAB_TREE "mf2"
#define KEY_META FAB_TREE "/meta"
#define KEY_NET  FAB_TREE "/net"
#define KEY_ICAC FAB_TREE "/ic"
#define KEY_FAB_TMPL FAB_TREE "/f0"
#define KEY_ACL_TMPL FAB_TREE "/a0"
#if MATTER_FEATURE_CLIENT
#define KEY_BIND FAB_TREE "/bn"
#endif
#define KEY_SLOT_DIGIT (sizeof(KEY_FAB_TMPL) - 2u)

#define FAB_MAGIC   0x32424146u /* "FAB2", little endian. */
#define FAB_VERSION 1u
#define REC_LIVE    1u
#define REC_DELETED 2u

enum record_kind {
	REC_META = 1,
	REC_NETWORK = 2,
	REC_FABRIC = 3,
	REC_ACL = 4,
	REC_ICAC = 5,
#if MATTER_FEATURE_CLIENT
	REC_BINDING = 6,
#endif
};

struct record_header {
	uint32_t magic;
	uint32_t epoch;
	uint32_t crc;
	uint8_t version;
	uint8_t kind;
	uint8_t state;
	uint8_t slot;
};

struct meta_record {
	struct record_header h;
};

struct network_record {
	struct record_header h;
	uint16_t dataset_len;
	uint8_t xpanid[MATTER_THREAD_XPANID_LEN];
	uint8_t dataset[MATTER_THREAD_DATASET_MAX];
};

struct fabric_record {
	struct record_header h;
	struct matter_fabric fabric;
};

struct acl_record {
	struct record_header h;
	uint64_t fabric_id;
	uint64_t node_id;
	uint16_t len;
	uint8_t fabric_index;
	uint8_t reserved;
	uint8_t data[MATTER_ACL_MAX];
};

struct icac_record {
	struct record_header h;
	uint16_t len;
	uint8_t owner_index;
	uint8_t reserved;
	uint8_t data[MATTER_CERT_MAX];
};

#if MATTER_FEATURE_CLIENT
struct binding_record {
	struct record_header h;
	struct matter_binding_table table;
};
#endif

union record_io {
	struct meta_record meta;
	struct network_record network;
	struct fabric_record fabric;
	struct acl_record acl;
	struct icac_record icac;
#if MATTER_FEATURE_CLIENT
	struct binding_record binding;
#endif
};

/* One bounded static codec buffer. Settings writes previously overflowed the
 * OpenThread stack; no record is ever assembled there now. */
static union record_io s_io;
static struct matter_device_info *s_target;
static uint32_t s_epoch = 1u;
static bool s_have_meta;
static uint32_t s_fabric_epoch[MATTER_SUPPORTED_FABRICS];
static uint32_t s_acl_epoch[MATTER_SUPPORTED_FABRICS];
static uint8_t s_fabric_state[MATTER_SUPPORTED_FABRICS];
static uint8_t s_acl_state[MATTER_SUPPORTED_FABRICS];
static uint64_t s_acl_fabric_id[MATTER_SUPPORTED_FABRICS];
static uint64_t s_acl_node_id[MATTER_SUPPORTED_FABRICS];
static uint8_t s_acl_fabric_index[MATTER_SUPPORTED_FABRICS];
static uint32_t s_network_epoch;
static uint8_t s_network_state;
static uint32_t s_icac_epoch;
static uint8_t s_icac_state;
#if MATTER_FEATURE_CLIENT
static uint32_t s_binding_epoch;
static uint8_t s_binding_state;
#endif

BUILD_ASSERT(MATTER_SUPPORTED_FABRICS < 10u,
	     "the per-fabric settings key carries a single digit");
BUILD_ASSERT(sizeof(struct fabric_record) <= 768u,
	     "fabric record exceeds the FreeRTOS settings value ceiling");
BUILD_ASSERT(sizeof(struct acl_record) <= 768u,
	     "ACL record exceeds the FreeRTOS settings value ceiling");
#if MATTER_FEATURE_CLIENT
BUILD_ASSERT(sizeof(struct binding_record) <= 768u,
	     "binding record exceeds the FreeRTOS settings value ceiling");
#endif

static uint32_t crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0u; i < len; i++) {
		crc ^= data[i];
		for (uint8_t bit = 0u; bit < 8u; bit++) {
			crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
		}
	}
	return ~crc;
}

static void record_init(struct record_header *h, uint8_t kind, uint8_t state, uint8_t slot)
{
	h->magic = FAB_MAGIC;
	h->epoch = s_epoch;
	h->crc = 0u;
	h->version = FAB_VERSION;
	h->kind = kind;
	h->state = state;
	h->slot = slot;
}

static void record_seal(void *record, size_t len)
{
	struct record_header *h = record;

	h->crc = 0u;
	h->crc = crc32(record, len);
}

static bool record_valid(void *record, size_t len, uint8_t kind)
{
	struct record_header *h = record;
	uint32_t stored;
	uint32_t actual;

	if (len < sizeof(*h) || h->magic != FAB_MAGIC || h->version != FAB_VERSION ||
	    h->kind != kind || (h->state != REC_LIVE && h->state != REC_DELETED)) {
		return false;
	}
	stored = h->crc;
	h->crc = 0u;
	actual = crc32(record, len);
	h->crc = stored;
	return stored == actual;
}

static int record_read(size_t len, size_t expected, settings_read_cb read_cb, void *cb_arg)
{
	ssize_t got;

	if (len != expected || expected > sizeof(s_io)) {
		return -EINVAL;
	}
	memset(&s_io, 0, sizeof(s_io));
	got = read_cb(cb_arg, &s_io, expected);
	return got == (ssize_t)expected ? 0 : -EINVAL;
}

static int slot_from_name(const char *name, char prefix)
{
	if (name == NULL || name[0] != prefix || name[1] < '0' || name[1] > '9' ||
	    name[2] != '\0') {
		return -1;
	}
	return name[1] - '0';
}

static bool load_meta_record(size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (record_read(len, sizeof(s_io.meta), read_cb, cb_arg) != 0 ||
	    !record_valid(&s_io.meta, sizeof(s_io.meta), REC_META) ||
	    s_io.meta.h.state != REC_LIVE || s_io.meta.h.epoch == 0u) {
		return false;
	}
	s_epoch = s_io.meta.h.epoch;
	s_have_meta = true;
	return true;
}

static int fab_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next = NULL;
	int slot;

	if (s_target == NULL) {
		return 0;
	}
	if (settings_name_steq(name, "meta", &next)) {
		(void)load_meta_record(len, read_cb, cb_arg);
		return 0;
	}
	if (settings_name_steq(name, "net", &next)) {
		if (record_read(len, sizeof(s_io.network), read_cb, cb_arg) == 0 &&
		    record_valid(&s_io.network, sizeof(s_io.network), REC_NETWORK) &&
		    s_io.network.dataset_len <= MATTER_THREAD_DATASET_MAX) {
			s_network_epoch = s_io.network.h.epoch;
			s_network_state = s_io.network.h.state;
			if (s_network_state == REC_LIVE) {
				memcpy(s_target->thread_dataset, s_io.network.dataset,
				       s_io.network.dataset_len);
				s_target->thread_dataset_len = s_io.network.dataset_len;
				memcpy(s_target->thread_xpanid, s_io.network.xpanid,
				       MATTER_THREAD_XPANID_LEN);
				s_target->have_thread_xpanid = true;
			}
		}
		return 0;
	}
	if (settings_name_steq(name, "ic", &next)) {
		if (record_read(len, sizeof(s_io.icac), read_cb, cb_arg) == 0 &&
		    record_valid(&s_io.icac, sizeof(s_io.icac), REC_ICAC) &&
		    s_io.icac.len <= MATTER_CERT_MAX) {
			s_icac_epoch = s_io.icac.h.epoch;
			s_icac_state = s_io.icac.h.state;
			if (s_icac_state == REC_LIVE) {
				s_target->icac.len = s_io.icac.len;
				s_target->icac.owner_index = s_io.icac.owner_index;
				memcpy(s_target->icac.buf, s_io.icac.data, s_io.icac.len);
			}
		}
		return 0;
	}
#if MATTER_FEATURE_CLIENT
	if (settings_name_steq(name, "bn", &next)) {
		if (record_read(len, sizeof(s_io.binding), read_cb, cb_arg) == 0 &&
		    record_valid(&s_io.binding, sizeof(s_io.binding), REC_BINDING)) {
			s_binding_epoch = s_io.binding.h.epoch;
			s_binding_state = s_io.binding.h.state;
			if (s_binding_state == REC_LIVE) {
				s_target->binding = s_io.binding.table;
			}
		}
		return 0;
	}
#endif
	slot = slot_from_name(name, 'f');
	if (slot >= 0 && slot < (int)MATTER_SUPPORTED_FABRICS) {
		if (record_read(len, sizeof(s_io.fabric), read_cb, cb_arg) == 0 &&
		    record_valid(&s_io.fabric, sizeof(s_io.fabric), REC_FABRIC) &&
		    s_io.fabric.h.slot == (uint8_t)slot) {
			s_fabric_epoch[slot] = s_io.fabric.h.epoch;
			s_fabric_state[slot] = s_io.fabric.h.state;
			if (s_fabric_state[slot] == REC_LIVE) {
				s_target->fabrics[slot] = s_io.fabric.fabric;
			}
		}
		return 0;
	}
	slot = slot_from_name(name, 'a');
	if (slot >= 0 && slot < (int)MATTER_SUPPORTED_FABRICS) {
		if (record_read(len, sizeof(s_io.acl), read_cb, cb_arg) == 0 &&
		    record_valid(&s_io.acl, sizeof(s_io.acl), REC_ACL) &&
		    s_io.acl.h.slot == (uint8_t)slot && s_io.acl.len <= MATTER_ACL_MAX) {
			s_acl_epoch[slot] = s_io.acl.h.epoch;
			s_acl_state[slot] = s_io.acl.h.state;
			s_acl_fabric_id[slot] = s_io.acl.fabric_id;
			s_acl_node_id[slot] = s_io.acl.node_id;
			s_acl_fabric_index[slot] = s_io.acl.fabric_index;
			if (s_acl_state[slot] == REC_LIVE) {
				s_target->fabric_acls[slot].len = s_io.acl.len;
				memcpy(s_target->fabric_acls[slot].data, s_io.acl.data,
				       s_io.acl.len);
			}
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(matter_fab, FAB_TREE, NULL, fab_set, NULL, NULL);

static int ensure_meta(void)
{
	int rc;

	if (s_have_meta) {
		return 0;
	}
	memset(&s_io, 0, sizeof(s_io));
	record_init(&s_io.meta.h, REC_META, REC_LIVE, 0xffu);
	record_seal(&s_io.meta, sizeof(s_io.meta));
	rc = settings_save_one(KEY_META, &s_io.meta, sizeof(s_io.meta));
	if (rc == 0) {
		s_have_meta = true;
	}
	return rc;
}

static int store_network(const struct matter_device_info *info)
{
	const uint8_t *dataset = info->thread_dataset;
	const uint8_t *xpanid = info->thread_xpanid;
	size_t len = info->thread_dataset_len;

	if (info->attempt.have_thread_candidate) {
		dataset = info->attempt.thread_dataset;
		xpanid = info->attempt.thread_xpanid;
		len = info->attempt.thread_dataset_len;
	}
	if (len == 0u || len > MATTER_THREAD_DATASET_MAX) {
		return -EINVAL;
	}
	memset(&s_io, 0, sizeof(s_io));
	record_init(&s_io.network.h, REC_NETWORK, REC_LIVE, 0xffu);
	s_io.network.dataset_len = (uint16_t)len;
	memcpy(s_io.network.xpanid, xpanid, MATTER_THREAD_XPANID_LEN);
	memcpy(s_io.network.dataset, dataset, len);
	record_seal(&s_io.network, sizeof(s_io.network));
	return settings_save_one(KEY_NET, &s_io.network, sizeof(s_io.network));
}

static int store_acl(const struct matter_device_info *info, uint8_t slot,
		     const uint8_t *value, size_t value_len)
{
	char key[] = KEY_ACL_TMPL;
	const struct matter_fabric *f = &info->fabrics[slot];

	if (value_len > MATTER_ACL_MAX || (value_len != 0u && value == NULL)) {
		return -EINVAL;
	}
	memset(&s_io, 0, sizeof(s_io));
	record_init(&s_io.acl.h, REC_ACL, REC_LIVE, slot);
	s_io.acl.fabric_id = f->fabric_id;
	s_io.acl.node_id = f->node_id;
	s_io.acl.fabric_index = f->index;
	s_io.acl.len = (uint16_t)value_len;
	if (value_len != 0u) {
		memcpy(s_io.acl.data, value, value_len);
	}
	record_seal(&s_io.acl, sizeof(s_io.acl));
	key[KEY_SLOT_DIGIT] = (char)('0' + slot);
	return settings_save_one(key, &s_io.acl, sizeof(s_io.acl));
}

static int store_icac(const struct matter_device_info *info)
{
	uint8_t state = REC_LIVE;

	if (info->icac.len > MATTER_CERT_MAX) {
		return -EINVAL;
	}
	if (info->icac.len == 0u || info->icac.owner_index == 0u) {
		state = REC_DELETED;
	}
	memset(&s_io, 0, sizeof(s_io));
	record_init(&s_io.icac.h, REC_ICAC, state, 0xffu);
	s_io.icac.len = (uint16_t)info->icac.len;
	s_io.icac.owner_index = info->icac.owner_index;
	if (state == REC_LIVE) {
		memcpy(s_io.icac.data, info->icac.buf, info->icac.len);
	}
	record_seal(&s_io.icac, sizeof(s_io.icac));
	return settings_save_one(KEY_ICAC, &s_io.icac, sizeof(s_io.icac));
}

#if MATTER_FEATURE_CLIENT
/*
 * Written whole, including an empty table: "no bindings" is a configuration an
 * administrator can choose, and a store that skipped an empty one would restore
 * the last non-empty table after a reboot -- a lock that starts unlocking a
 * door its owner unbound. So this record is always LIVE and never DELETED.
 */
static int store_binding(const struct matter_device_info *info)
{
	memset(&s_io, 0, sizeof(s_io));
	record_init(&s_io.binding.h, REC_BINDING, REC_LIVE, 0xffu);
	s_io.binding.table = info->binding;
	record_seal(&s_io.binding, sizeof(s_io.binding));
	return settings_save_one(KEY_BIND, &s_io.binding, sizeof(s_io.binding));
}
#endif

int matter_fab_commit(const struct matter_device_info *info,
		      enum matter_fabric_store_operation operation, uint8_t slot,
		      const uint8_t *value, size_t value_len)
{
	char key[] = KEY_FAB_TMPL;
	int rc;

	if (info == NULL) {
		return -EINVAL;
	}
#if MATTER_FEATURE_CLIENT
	if (operation == MATTER_FABRIC_STORE_BINDING) {
		rc = ensure_meta();
		return rc != 0 ? rc : store_binding(info);
	}
#endif
	if (slot >= MATTER_SUPPORTED_FABRICS) {
		return -EINVAL;
	}
	rc = ensure_meta();
	if (rc != 0) {
		return rc;
	}
	if (operation == MATTER_FABRIC_STORE_ACL) {
		return store_acl(info, slot, value, value_len);
	}
	if (operation == MATTER_FABRIC_STORE_COMMIT_ATTEMPT) {
		if (info->committed_slots == 0u) {
			rc = store_network(info);
			if (rc != 0) {
				return rc;
			}
		}
		rc = store_icac(info);
		if (rc != 0) {
			return rc;
		}
		rc = store_acl(info, slot, info->fabric_acls[slot].data,
			       info->fabric_acls[slot].len);
		if (rc != 0) {
			return rc;
		}
	}
	memset(&s_io, 0, sizeof(s_io));
	record_init(&s_io.fabric.h, REC_FABRIC,
		    operation == MATTER_FABRIC_STORE_REMOVE ? REC_DELETED : REC_LIVE, slot);
	if (operation == MATTER_FABRIC_STORE_COMMIT_ATTEMPT ||
	    operation == MATTER_FABRIC_STORE_UPDATE) {
		s_io.fabric.fabric = info->fabrics[slot];
	} else if (operation != MATTER_FABRIC_STORE_REMOVE) {
		return -EINVAL;
	}
	record_seal(&s_io.fabric, sizeof(s_io.fabric));
	key[KEY_SLOT_DIGIT] = (char)('0' + slot);
	return settings_save_one(key, &s_io.fabric, sizeof(s_io.fabric));
}

static void clear_identity(struct matter_device_info *info)
{
	memset(info->fabrics, 0, sizeof(info->fabrics));
	memset(info->fabric_acls, 0, sizeof(info->fabric_acls));
	memset(&info->icac, 0, sizeof(info->icac));
#if MATTER_FEATURE_CLIENT
	memset(&info->binding, 0, sizeof(info->binding));
#endif
	info->committed_slots = 0u;
	info->thread_dataset_len = 0u;
	info->have_thread_xpanid = false;
}

static void legacy_cleanup(void)
{
	static const char *const fixed[] = {
		"mfab/ok", "mfab/ver", "mfab/td", "mfab/xp", "mfab/il", "mfab/ic",
	};

	for (size_t i = 0u; i < ARRAY_SIZE(fixed); i++) {
		(void)settings_delete(fixed[i]);
	}
	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		char key[] = "mfab/f0";

		key[sizeof(key) - 2u] = (char)('0' + i);
		(void)settings_delete(key);
	}
}

int matter_fab_load(struct matter_device_info *info)
{
	bool used_index[MATTER_SUPPORTED_FABRICS + 1u] = { false };
	bool icac_valid;
	int rc;

	if (info == NULL) {
		return -EINVAL;
	}
	clear_identity(info);
	memset(s_fabric_epoch, 0, sizeof(s_fabric_epoch));
	memset(s_acl_epoch, 0, sizeof(s_acl_epoch));
	memset(s_fabric_state, 0, sizeof(s_fabric_state));
	memset(s_acl_state, 0, sizeof(s_acl_state));
	memset(s_acl_fabric_id, 0, sizeof(s_acl_fabric_id));
	memset(s_acl_node_id, 0, sizeof(s_acl_node_id));
	memset(s_acl_fabric_index, 0, sizeof(s_acl_fabric_index));
	s_network_epoch = 0u;
	s_network_state = 0u;
	s_icac_epoch = 0u;
	s_icac_state = 0u;
#if MATTER_FEATURE_CLIENT
	s_binding_epoch = 0u;
	s_binding_state = 0u;
#endif
	s_have_meta = false;
	s_target = info;
	rc = settings_load_subtree(FAB_TREE);
	s_target = NULL;
	legacy_cleanup();
	if (rc != 0 || !s_have_meta) {
		clear_identity(info);
		return rc != 0 ? rc : 1;
	}
	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		struct matter_fabric *f = &info->fabrics[i];

		if (s_fabric_epoch[i] != s_epoch || s_fabric_state[i] != REC_LIVE ||
		    f->index == 0u || f->index > MATTER_SUPPORTED_FABRICS ||
		    used_index[f->index]) {
			memset(f, 0, sizeof(*f));
			memset(&info->fabric_acls[i], 0, sizeof(info->fabric_acls[i]));
			continue;
		}
		used_index[f->index] = true;
		info->committed_slots |= MATTER_FABRIC_SLOT_BIT(i);
		if (s_acl_epoch[i] != s_epoch || s_acl_state[i] != REC_LIVE ||
		    s_acl_fabric_index[i] != f->index || s_acl_fabric_id[i] != f->fabric_id ||
		    s_acl_node_id[i] != f->node_id) {
			memset(&info->fabric_acls[i], 0, sizeof(info->fabric_acls[i]));
		}
	}
	if (info->committed_slots == 0u || s_network_epoch != s_epoch ||
	    s_network_state != REC_LIVE || info->thread_dataset_len == 0u ||
	    !info->have_thread_xpanid) {
		clear_identity(info);
		return 1;
	}
#if MATTER_FEATURE_CLIENT
	/*
	 * A table from a superseded epoch is not this node's, and an entry
	 * naming a fabric that did not survive the checks above would unlock a
	 * door for an administrator this node no longer has.
	 */
	if (s_binding_epoch != s_epoch || s_binding_state != REC_LIVE) {
		memset(&info->binding, 0, sizeof(info->binding));
	} else {
		for (uint8_t i = 1u; i <= MATTER_SUPPORTED_FABRICS; i++) {
			if (!used_index[i]) {
				matter_binding_forget_fabric(&info->binding, i);
			}
		}
	}
#endif
	icac_valid = s_icac_epoch == s_epoch && s_icac_state == REC_LIVE &&
		     info->icac.len > 0u && info->icac.owner_index > 0u &&
		     info->icac.owner_index <= MATTER_SUPPORTED_FABRICS &&
		     used_index[info->icac.owner_index];
	if (!icac_valid) {
		memset(&info->icac, 0, sizeof(info->icac));
	}
	/*
	 * A fabric record is authoritative only when every certificate it names
	 * is intact.  The ICAC is shared to save RAM, so a corrupt ICAC record
	 * must discard its one owner without taking healthy neighbouring fabrics
	 * with it.  An empty slot remains recoverable through normal commissioning.
	 */
	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		struct matter_fabric *f = &info->fabrics[i];

		if ((info->committed_slots & MATTER_FABRIC_SLOT_BIT(i)) == 0u ||
		    f->icac_len == 0u) {
			continue;
		}
		if (!icac_valid || info->icac.owner_index != f->index ||
		    info->icac.len != f->icac_len) {
			memset(f, 0, sizeof(*f));
			memset(&info->fabric_acls[i], 0, sizeof(info->fabric_acls[i]));
			info->committed_slots &= (uint8_t)~MATTER_FABRIC_SLOT_BIT(i);
		}
	}
	if (icac_valid) {
		bool owner_present = false;

		for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
			const struct matter_fabric *f = &info->fabrics[i];

			if ((info->committed_slots & MATTER_FABRIC_SLOT_BIT(i)) != 0u &&
			    f->index == info->icac.owner_index && f->icac_len == info->icac.len) {
				owner_present = true;
				break;
			}
		}
		if (!owner_present) {
			memset(&info->icac, 0, sizeof(info->icac));
		}
	}
	if (info->committed_slots == 0u) {
		clear_identity(info);
		return 1;
	}
	return 0;
}

#define EPOCH_SCAN_MAX (3u + 2u * MATTER_SUPPORTED_FABRICS)

struct epoch_scan {
	uint32_t value[EPOCH_SCAN_MAX];
	size_t count;
};

static int scan_epoch(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		      void *param)
{
	struct epoch_scan *out = param;
	struct record_header h;

	(void)key;
	if (len < sizeof(h) || out->count >= ARRAY_SIZE(out->value) ||
	    read_cb(cb_arg, &h, sizeof(h)) != (ssize_t)sizeof(h) || h.magic != FAB_MAGIC ||
	    h.version != FAB_VERSION || h.epoch == 0u) {
		return 0;
	}
	out->value[out->count++] = h.epoch;
	return 0;
}

int matter_fab_erase(void)
{
	struct epoch_scan scan = {0};
	static const char *const fixed[] = { KEY_META, KEY_NET, KEY_ICAC,
#if MATTER_FEATURE_CLIENT
					     KEY_BIND,
#endif
	};
	uint32_t next = 1u;
	int rc;

	/* A clean-break erase may run before load and even with a corrupt meta
	 * record. Pick an epoch no current record uses, so publishing the new
	 * meta can never resurrect a stale fabric by collision. */
	for (size_t i = 0u; i < ARRAY_SIZE(fixed); i++) {
		(void)settings_load_subtree_direct(fixed[i], scan_epoch, &scan);
	}
	for (uint8_t i = 0u; i < MATTER_SUPPORTED_FABRICS; i++) {
		char fab[] = KEY_FAB_TMPL;
		char acl[] = KEY_ACL_TMPL;

		fab[KEY_SLOT_DIGIT] = (char)('0' + i);
		acl[KEY_SLOT_DIGIT] = (char)('0' + i);
		(void)settings_load_subtree_direct(fab, scan_epoch, &scan);
		(void)settings_load_subtree_direct(acl, scan_epoch, &scan);
	}
	for (;;) {
		bool collision = false;

		for (size_t i = 0u; i < scan.count; i++) {
			if (scan.value[i] == next) {
				collision = true;
				break;
			}
		}
		if (!collision) {
			break;
		}
		next++;
		if (next == 0u) {
			next = 1u;
		}
	}
	s_epoch = next;
	s_have_meta = false;
	/* The one atomic replacement is the erase authority. Before it lands the
	 * old epoch is live; after it lands every older record is unreachable. */
	rc = ensure_meta();
	if (rc == 0) {
		legacy_cleanup();
		LOG_WRN("Matter operational identity erased at epoch %u",
			(unsigned int)s_epoch);
	}
	return rc;
}

/* ---- the writable Door Lock attributes ------------------------------------ */

/*
 * A tree of their own rather than more keys under the Matter identity.
 * These settings survive selective fabric removal and Matter-only erase.
 */
#define DL_TREE          "mdl"
#define KEY_AUTO_RELOCK  DL_TREE "/art"
#define KEY_APPROACH     DL_TREE "/apd"

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
	if (len != t->len || read_cb(cb_arg, t->value, t->len) < 0) {
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
			first_err = rc;
		}
	}
	if (info->approach_direction != prev_approach_direction) {
		rc = settings_save_one(KEY_APPROACH, &info->approach_direction,
				       sizeof(info->approach_direction));
		if (rc != 0 && first_err == 0) {
			first_err = rc;
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
