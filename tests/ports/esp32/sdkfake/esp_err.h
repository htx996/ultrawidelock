/* sdkfake esp_err.h — error codes + ESP_ERROR_CHECK (aborts on failure, as on target). */
#ifndef SDKFAKE_ESP_ERR_H
#define SDKFAKE_ESP_ERR_H

#include "sdkfake.h"

typedef int esp_err_t;

#define ESP_OK                        0
#define ESP_FAIL                      (-1)
#define ESP_ERR_INVALID_STATE         0x103
#define ESP_ERR_NVS_BASE              0x1100
#define ESP_ERR_NVS_NOT_FOUND         (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE   (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_KEY_TOO_LONG      (ESP_ERR_NVS_BASE + 0x09)
#define ESP_ERR_NVS_INVALID_LENGTH    (ESP_ERR_NVS_BASE + 0x0a)
#define ESP_ERR_NVS_NO_FREE_PAGES     (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)

#define ESP_ERROR_CHECK(x)                                                     \
	do {                                                                   \
		esp_err_t err_rc_ = (x);                                       \
		if (err_rc_ != ESP_OK) {                                       \
			fprintf(stderr, "ESP_ERROR_CHECK failed: 0x%x (%s:%d)\n", \
				err_rc_, __FILE__, __LINE__);                  \
			abort();                                               \
		}                                                              \
	} while (0)

#endif
