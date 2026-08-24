/*
 * fake_nvs — in-RAM double of the ESP-IDF NVS blob API: real namespaces, real
 * keys, with per-call failure injection. Backs the ultrawidelock_prov_nvs.c,
 * ultrawidelock_ble.c and kv_nvs.c host suites.
 *
 * It was one namespace and one blob, which was enough while the only consumer
 * stored a single provisioning record. A key-value backend stores many, needs
 * delete, and needs a namespace-wide erase, and a fake that cannot hold two
 * keys cannot show that any of those work.
 *
 * The name caps are still enforced the way ESP-IDF enforces them, because that
 * asymmetry is the entire reason the kv seam derives its names.
 */
#include <stdio.h>
#include <string.h>

#include "nvs_flash.h"

esp_err_t fake_nvs_init_rc = ESP_OK;
esp_err_t fake_nvs_init_rc_once = ESP_OK;
esp_err_t fake_nvs_open_rc = ESP_OK;
esp_err_t fake_nvs_get_rc = ESP_OK;
esp_err_t fake_nvs_set_rc = ESP_OK;
esp_err_t fake_nvs_commit_rc = ESP_OK;
esp_err_t fake_nvs_erase_rc = ESP_OK;
int fake_nvs_erase_calls;

#define FAKE_NS   4u
#define FAKE_KEYS 48u
#define FAKE_VAL  1024u
#define FAKE_NAME 16u

struct entry {
	char ns[FAKE_NAME];
	char key[FAKE_NAME];
	uint8_t val[FAKE_VAL];
	size_t len;
	int used;
};

static struct entry s_entries[FAKE_KEYS];
static char s_namespaces[FAKE_NS][FAKE_NAME];
static int s_ns_used[FAKE_NS];
/* Handles index the namespace table, so a get and a set through two different
 * handles land in two different namespaces -- which is the whole point. */
static char s_handle_ns[FAKE_NS + 1u][FAKE_NAME];

void fake_nvs_reset(void)
{
	fake_nvs_init_rc = ESP_OK;
	fake_nvs_init_rc_once = ESP_OK;
	fake_nvs_open_rc = ESP_OK;
	fake_nvs_get_rc = ESP_OK;
	fake_nvs_set_rc = ESP_OK;
	fake_nvs_commit_rc = ESP_OK;
	fake_nvs_erase_rc = ESP_OK;
	fake_nvs_erase_calls = 0;
	memset(s_entries, 0, sizeof(s_entries));
	memset(s_namespaces, 0, sizeof(s_namespaces));
	memset(s_ns_used, 0, sizeof(s_ns_used));
	memset(s_handle_ns, 0, sizeof(s_handle_ns));
}

static struct entry *find(const char *ns, const char *key)
{
	for (size_t i = 0; i < FAKE_KEYS; i++) {
		if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0 &&
		    strcmp(s_entries[i].key, key) == 0) {
			return &s_entries[i];
		}
	}
	return NULL;
}

static struct entry *claim(const char *ns, const char *key)
{
	struct entry *e = find(ns, key);

	if (e != NULL) {
		return e;
	}
	for (size_t i = 0; i < FAKE_KEYS; i++) {
		if (!s_entries[i].used) {
			e = &s_entries[i];
			snprintf(e->ns, sizeof(e->ns), "%s", ns);
			snprintf(e->key, sizeof(e->key), "%s", key);
			e->used = 1;
			e->len = 0;
			return e;
		}
	}
	return NULL;
}

static int ns_exists(const char *ns)
{
	for (size_t i = 0; i < FAKE_NS; i++) {
		if (s_ns_used[i] && strcmp(s_namespaces[i], ns) == 0) {
			return 1;
		}
	}
	return 0;
}

static void ns_create(const char *ns)
{
	if (ns_exists(ns)) {
		return;
	}
	for (size_t i = 0; i < FAKE_NS; i++) {
		if (!s_ns_used[i]) {
			snprintf(s_namespaces[i], sizeof(s_namespaces[i]), "%s", ns);
			s_ns_used[i] = 1;
			return;
		}
	}
}

void fake_nvs_preload(const char *ns, const char *key, const void *blob, size_t len)
{
	struct entry *e;

	ns_create(ns);
	e = claim(ns, key);
	if (e == NULL || len > FAKE_VAL) {
		return;
	}
	memcpy(e->val, blob, len);
	e->len = len;
}

size_t fake_nvs_stored(const char *ns, const char *key, void *out, size_t cap)
{
	const struct entry *e = find(ns, key);
	size_t n;

	if (e == NULL) {
		return 0;
	}
	n = e->len < cap ? e->len : cap;
	memcpy(out, e->val, n);
	return n;
}

int fake_nvs_key_count(const char *ns)
{
	int n = 0;

	for (size_t i = 0; i < FAKE_KEYS; i++) {
		if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0) {
			n++;
		}
	}
	return n;
}

esp_err_t nvs_flash_init(void)
{
	if (fake_nvs_init_rc_once != ESP_OK) {
		esp_err_t rc = fake_nvs_init_rc_once;

		fake_nvs_init_rc_once = ESP_OK;
		return rc;
	}
	return fake_nvs_init_rc;
}

esp_err_t nvs_flash_erase(void)
{
	fake_nvs_erase_calls++;
	memset(s_entries, 0, sizeof(s_entries));
	memset(s_namespaces, 0, sizeof(s_namespaces));
	memset(s_ns_used, 0, sizeof(s_ns_used));
	return ESP_OK;
}

/* The name caps, enforced the way ESP-IDF enforces them (nvs_storage.cpp /
 * nvs_page.cpp): a lookup never matches an over-long name, so reads and read-only
 * opens miss with NOT_FOUND; only a write -- creating the namespace, or setting the
 * key -- reports ESP_ERR_NVS_KEY_TOO_LONG. Both halves matter: the read-side silence
 * is what makes an over-long namespace look like "never provisioned" on the bench. */
static int name_too_long(const char *name)
{
	return strlen(name) > NVS_KEY_NAME_MAX_SIZE - 1;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
	if (fake_nvs_open_rc != ESP_OK) {
		return fake_nvs_open_rc;
	}
	if (name_too_long(ns)) {
		return mode == NVS_READWRITE ? ESP_ERR_NVS_KEY_TOO_LONG : ESP_ERR_NVS_NOT_FOUND;
	}
	if (mode == NVS_READONLY && !ns_exists(ns)) {
		return ESP_ERR_NVS_NOT_FOUND;
	}
	if (mode == NVS_READWRITE) {
		ns_create(ns);
	}
	/* Handle 0 is never issued, so a zeroed handle is not a valid one. */
	for (size_t i = 1; i <= FAKE_NS; i++) {
		if (s_handle_ns[i][0] == '\0') {
			snprintf(s_handle_ns[i], sizeof(s_handle_ns[i]), "%s", ns);
			*out = (nvs_handle_t)i;
			return ESP_OK;
		}
	}
	return ESP_FAIL;
}

static const char *ns_of(nvs_handle_t h)
{
	if (h == 0u || h > FAKE_NS) {
		return "";
	}
	return s_handle_ns[h];
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
	const struct entry *e;

	if (fake_nvs_get_rc != ESP_OK) {
		return fake_nvs_get_rc;
	}
	if (name_too_long(key)) {
		return ESP_ERR_NVS_NOT_FOUND;
	}
	e = find(ns_of(h), key);
	if (e == NULL) {
		return ESP_ERR_NVS_NOT_FOUND;
	}
	/* ESP-IDF answers a NULL out with the stored length, which is how a
	 * caller sizes its buffer before the real read. */
	if (out == NULL) {
		*len = e->len;
		return ESP_OK;
	}
	if (*len < e->len) {
		*len = e->len;
		return ESP_ERR_NVS_INVALID_LENGTH;
	}
	memcpy(out, e->val, e->len);
	*len = e->len;
	return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
	struct entry *e;

	if (fake_nvs_set_rc != ESP_OK) {
		return fake_nvs_set_rc;
	}
	if (name_too_long(key)) {
		return ESP_ERR_NVS_KEY_TOO_LONG;
	}
	if (len > FAKE_VAL) {
		return ESP_FAIL;
	}
	e = claim(ns_of(h), key);
	if (e == NULL) {
		return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
	}
	memcpy(e->val, val, len);
	e->len = len;
	return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
	struct entry *e;

	if (fake_nvs_erase_rc != ESP_OK) {
		return fake_nvs_erase_rc;
	}
	e = find(ns_of(h), key);
	if (e == NULL) {
		return ESP_ERR_NVS_NOT_FOUND;
	}
	memset(e, 0, sizeof(*e));
	return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t h)
{
	const char *ns = ns_of(h);

	if (fake_nvs_erase_rc != ESP_OK) {
		return fake_nvs_erase_rc;
	}
	/* One namespace, not the partition: every other namespace survives, the
	 * way the real call behaves and the way a factory reset of one store
	 * must behave. */
	for (size_t i = 0; i < FAKE_KEYS; i++) {
		if (s_entries[i].used && strcmp(s_entries[i].ns, ns) == 0) {
			memset(&s_entries[i], 0, sizeof(s_entries[i]));
		}
	}
	return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
	(void)h;
	return fake_nvs_commit_rc;
}

void nvs_close(nvs_handle_t h)
{
	if (h != 0u && h <= FAKE_NS) {
		s_handle_ns[h][0] = '\0';
	}
}
