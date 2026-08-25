/* SPDX-License-Identifier: ISC */

/*
 * sat_console.c — the `sat` commands, esp_console in place of the Zephyr shell.
 *
 * Same five verbs apps/satellite's shell registers, so the bench procedure in
 * docs/second-anchor.md reads the same on either board:
 *
 *   sat join <ursk-hex64> <rcfg-hex34> <channel> <sync-code>
 *   sat key  <hex32>
 *   sat stop
 *   sat link
 *
 * `dataset` is gone and has no ESP equivalent: it joined a Thread mesh, and
 * ESP-NOW has no network to join. That is the whole of what the carrier change
 * costs at the console.
 */

#include "sat_join.h"

#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

#include <ultrawidelock/uwb.h>

#include "ultrawidelock_satlink.h"

static const char *TAG = "sat";

static int cmd_join(int argc, char **argv)
{
	uint8_t ursk[SAT_URSK_LEN];
	uint8_t rcfg[SAT_RCFG_LEN];
	const char *why = "";
	uint32_t sid = 0u;
	uint8_t channel;
	uint8_t code;
	int rc;

	if (argc != 5) {
		printf("usage: sat join <ursk-hex%u> <rcfg-hex%u> <channel> <sync-code>\n",
		       2u * SAT_URSK_LEN, 2u * SAT_RCFG_LEN);
		return 1;
	}
	if (sat_hex_parse(argv[1], ursk, SAT_URSK_LEN) != 0) {
		printf("ursk: want %u hex chars\n", 2u * SAT_URSK_LEN);
		return 1;
	}
	if (sat_hex_parse(argv[2], rcfg, SAT_RCFG_LEN) != 0) {
		printf("rcfg: want %u hex chars\n", 2u * SAT_RCFG_LEN);
		return 1;
	}
	channel = (uint8_t)strtoul(argv[3], NULL, 10);
	code = (uint8_t)strtoul(argv[4], NULL, 10);

	rc = sat_join_apply(ursk, rcfg, channel, code, &sid, &why);
	/* The URSK was on the stack and in the typed line; at least drop our copy. */
	memset(ursk, 0, sizeof(ursk));
	if (rc != 0) {
		printf("%s (rc=%d)\n", why, rc);
		return 1;
	}
	printf("SAT joined sid=0x%08x ch=%u code=%u resp=%u/%u\n", (unsigned)sid,
	       (unsigned)channel, (unsigned)code, (unsigned)SAT_RESPONDER_INDEX,
	       (unsigned)SAT_NUM_RESPONDERS);
	return 0;
}

static int cmd_stop(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	ultrawidelock_uwb_stop();
	printf("SAT stopped\n");
	return 0;
}

/*
 * The link key. Typed rather than baked in, because a key in the image is a key
 * in the repository -- and it must match the lock's byte for byte or every
 * report is discarded as unopenable, which at the lock looks exactly like an
 * anchor that never booted.
 */
static int cmd_key(int argc, char **argv)
{
	uint8_t key[ULTRAWIDELOCK_SEAL_KEY_LEN];
	int rc;

	if (argc != 2) {
		printf("usage: sat key <hex%u>\n", 2u * ULTRAWIDELOCK_SEAL_KEY_LEN);
		return 1;
	}
	if (sat_hex_parse(argv[1], key, sizeof(key)) != 0) {
		printf("key: want %u hex chars\n", 2u * ULTRAWIDELOCK_SEAL_KEY_LEN);
		return 1;
	}
	rc = ultrawidelock_satlink_set_key(key, sizeof(key));
	memset(key, 0, sizeof(key));
	if (rc != 0) {
		printf("link key not accepted\n");
		return 1;
	}
	printf("SAT link key stored\n");
	return 0;
}

static int cmd_link(int argc, char **argv)
{
	int ch = ultrawidelock_satlink_channel();

	(void)argc;
	(void)argv;
	printf("SAT link: %s\n", ultrawidelock_satlink_ready()
					 ? "up, keyed — reporting"
					 : "not reporting (no key, or ESP-NOW down)");
	/* The other silent failure: a keyed link on the wrong Wi-Fi channel
	 * reports into the void. Say where the hunt stands. */
	if (ch > 0) {
		printf("SAT link: lock heard on Wi-Fi channel %d\n", ch);
	} else if (ch == 0) {
		printf("SAT link: hunting for the lock's Wi-Fi channel\n");
	}
	return 0;
}

void sat_console_register(void)
{
	/* esp_console has no command tree, so the Zephyr `sat <verb>` subcommands
	 * become four flat commands. The names keep the prefix so the console's
	 * help still groups them where a reader expects. */
	const esp_console_cmd_t flat[] = {
		{.command = "sat_join",
		 .help = "join a session: sat_join <ursk-hex64> <rcfg-hex34> <channel> <sync-code>",
		 .hint = NULL,
		 .func = cmd_join},
		{.command = "sat_key",
		 .help = "set the sealed-link key (same bytes as the lock's): sat_key <hex32>",
		 .hint = NULL,
		 .func = cmd_key},
		{.command = "sat_stop",
		 .help = "stop ranging and quiesce the radio",
		 .hint = NULL,
		 .func = cmd_stop},
		{.command = "sat_link", .help = "sealed-link status", .hint = NULL,
		 .func = cmd_link},
	};

	for (size_t i = 0; i < sizeof(flat) / sizeof(flat[0]); i++) {
		esp_err_t e = esp_console_cmd_register(&flat[i]);

		if (e != ESP_OK) {
			ESP_LOGE(TAG, "could not register %s", flat[i].command);
		}
	}
}
