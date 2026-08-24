/* sdkfake nvs.h — in-RAM NVS blob store: real namespaces and keys, with
 * failure injection. Implemented in fake_nvs.c. */
#ifndef SDKFAKE_NVS_H
#define SDKFAKE_NVS_H

#include "esp_err.h"

/* Same values as ESP-IDF's nvs.h: the longest namespace or key name NVS stores
 * is one short of these (15), NUL included. The fake enforces the cap the way the
 * target does, because ignoring it is how an over-long namespace passed this suite
 * and then failed every store on hardware. */
#define NVS_KEY_NAME_MAX_SIZE 16
#define NVS_NS_NAME_MAX_SIZE  NVS_KEY_NAME_MAX_SIZE

typedef uint32_t nvs_handle_t;

typedef enum {
	NVS_READONLY,
	NVS_READWRITE,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
/* Erases one NAMESPACE, not the partition -- what a factory reset of a single
 * store needs, and what the kv backend uses. */
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
void nvs_close(nvs_handle_t h);

/* ---- test controls (fake_nvs.c) ---- */
extern esp_err_t fake_nvs_init_rc;      /* nvs_flash_init result (ESP_OK) */
extern esp_err_t fake_nvs_init_rc_once; /* one-shot init result, then init_rc */
extern esp_err_t fake_nvs_open_rc;      /* forced nvs_open result (ESP_OK = off) */
extern esp_err_t fake_nvs_get_rc;       /* forced nvs_get_blob result */
extern esp_err_t fake_nvs_set_rc;       /* forced nvs_set_blob result */
extern esp_err_t fake_nvs_commit_rc;    /* forced nvs_commit result */
extern esp_err_t fake_nvs_erase_rc;     /* forced nvs_erase_key/_all result */
extern int fake_nvs_erase_calls;        /* nvs_flash_erase() count */
void fake_nvs_reset(void);              /* wipe store + clear all knobs */
/* Seed and read back one record. Namespace and key are explicit: the store holds
 * many now, so "the stored blob" is no longer a thing a test can mean. */
void fake_nvs_preload(const char *ns, const char *key, const void *blob, size_t len);
size_t fake_nvs_stored(const char *ns, const char *key, void *out, size_t cap);
int fake_nvs_key_count(const char *ns); /* live records in one namespace */

#endif
