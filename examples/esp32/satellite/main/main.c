/* SPDX-License-Identifier: ISC */

/*
 * Satellite responder on ESP32 + DWM3000EVB.
 *
 * The ESP-IDF twin of apps/satellite/src/main.c, and deliberately the same
 * shape: the whole ranging engine is the module's, and this file only supplies
 * what BLE negotiation supplies on the lock — the session keys and PHY. They
 * arrive either over the sealed link or pasted at the console:
 *
 *   sat_join <ursk-hex64> <rcfg-hex34> <channel> <sync-code>
 *
 * What goes back is one sealed WV3 report per accepted range: this board's own
 * distance to the phone, and the ranging BLOCK it was measured in. The block
 * travels with it because the lock cannot pair a distance whose round it does
 * not know — mispairing shows up as triangle rejections that read as a
 * hardware fault.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_log.h"
#include "linenoise/linenoise.h"
#include "nvs_flash.h"

#include <psa/crypto.h>

#include <ultrawidelock/uwb.h>

#include "sat_join.h"
#include "ultrawidelock_satlink.h"

static const char *TAG = "sat";

/* The sealed handoff arrived and unsealed. Runs on the Wi-Fi task. */
static void on_link_join(const uint8_t *ursk, const uint8_t *rcfg, uint8_t channel,
			 uint8_t sync_code_index)
{
	const char *why = "";
	uint32_t sid = 0u;

	if (sat_join_apply(ursk, rcfg, channel, sync_code_index, &sid, &why) != 0) {
		ESP_LOGW(TAG, "sealed handoff rejected: %s", why);
		return;
	}
	ESP_LOGI(TAG, "SAT joined from the sealed link sid=0x%08x ch=%u code=%u", (unsigned)sid,
		 (unsigned)channel, (unsigned)sync_code_index);
}

static void console_start(void)
{
	esp_console_repl_t *repl = NULL;
	esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

	repl_cfg.prompt = "sat>";
	repl_cfg.task_core_id = 0;
	/* A pasted `sat_join` line is 64 + 34 hex characters plus the verb, so
	 * the default 256-byte command line would truncate the one command this
	 * board exists to receive. */
	repl_cfg.max_cmdline_length = 1024;

	ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl));
	/* Multiline mode and the hints callback make linenoise redraw the whole
	 * prompt on every keystroke, which is unreadable when a 200-character
	 * paste arrives. Same setting the reader app makes, for the same reason. */
	linenoiseSetMultiLine(0);
	linenoiseSetHintsCallback(NULL);

	sat_console_register();
	ESP_ERROR_CHECK(esp_console_register_help_command());
	ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
	psa_status_t st;
	esp_err_t e;

	/* Mute the CCC shim's per-frame STS trace: it fires on the delayed-TX
	 * reply path, where a log line can blow the reply window. WARN keeps its
	 * errors. Same lever the reader app pulls. */
	esp_log_level_set("ccc_shim", ESP_LOG_WARN);

	/* NVS first: the link key lives there and satlink reads it during init. */
	e = nvs_flash_init();
	if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		e = nvs_flash_init();
	}
	ESP_ERROR_CHECK(e);

	/* The link seals with PSA and would fail every report if it came up
	 * first. Same ordering apps/satellite's main() takes. */
	st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		ESP_LOGE(TAG, "psa_crypto_init failed (%d)", (int)st);
		return;
	}

	/* Before the carrier comes up, so a handoff cannot arrive with the sink
	 * still unset — the lock sends one at every session start. */
	ultrawidelock_satlink_set_join_cb(on_link_join);
	if (ultrawidelock_satlink_init((uint8_t)CONFIG_ULTRAWIDELOCK_ANCHOR_ROLE) != 0) {
		/* Not fatal: the board still ranges and still prints, it simply
		 * has nobody to tell. */
		ESP_LOGE(TAG, "sealed link did not come up; ranging only");
	}

	console_start();

	printf("SAT satellite responder %u/%u — waiting for a sealed handoff\n",
	       (unsigned)SAT_RESPONDER_INDEX, (unsigned)SAT_NUM_RESPONDERS);

	/*
	 * Range reporter. Poll the generation counter rather than registering a
	 * range listener: there is one listener slot and the engine owns it.
	 */
	uint32_t last_gen = ultrawidelock_uwb_range_generation();

	for (;;) {
		uint32_t gen = ultrawidelock_uwb_range_generation();
		int32_t cm;

		if (gen != last_gen && ultrawidelock_uwb_last_range_cm(&cm)) {
			uint32_t blk = 0u;

			/*
			 * Only a TRUSTED range is reported. An untrusted one is
			 * still printed below for the bench capture, but it must
			 * never reach the lock's fusion: a single unverified
			 * block is exactly what a relay would inject.
			 */
			if (ultrawidelock_uwb_trusted_range_block_cm(&cm, &blk)) {
				/* The same line the lock prints, so two captures
				 * can be JOINED ON BLOCK afterwards and
				 * subtracted at an instant where the true
				 * difference is known. */
				ESP_LOGI(TAG, "pair sid=%08x blk=%u mm=%d",
					 (unsigned)ultrawidelock_uwb_session_id(), (unsigned)blk,
					 (int)(cm * 10));
				/* The report the lock actually acts on. The log
				 * line above stays: it is what the bench joins
				 * against the lock's, and it keeps working when
				 * the link is down. */
				ultrawidelock_satlink_report(cm * 10, blk);
			}
			last_gen = gen;
			printf("SAT range %d cm (gen %u)\n", (int)cm, (unsigned)gen);
		}
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}
