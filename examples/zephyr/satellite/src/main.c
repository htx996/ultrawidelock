/* SPDX-License-Identifier: ISC */

/*
 * Satellite responder (docs/second-anchor.md stage B).
 *
 * The whole ranging engine is the module's: ultrawidelock_uwb_start_cred() brings up
 * the radio, stands up the permanent Pre-POLL listen, and runs the responder
 * round from there (pre-poll recovery means no time sync is needed — the
 * first decrypted Pre-POLL carries Poll_STS_Index). This file only supplies
 * what BLE negotiation supplies on the lock: the session keys and PHY, pasted
 * from the lock's SAT-HANDOFF log line:
 *
 *   sat join <ursk-hex64> <rcfg-hex34> <channel> <sync-code>
 *
 * Everything else the engine needs is inside rcfg[17]: session id (bytes
 * 4..7), STS_Index0 (8..11), responder count (12), RAN multiplier (13),
 * slots per round (14), chaps per slot (15).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_OPENTHREAD)
#include "anchor_link.h"
#endif
#include <zephyr/sys/printk.h>
#include <psa/crypto.h>

#include <ultrawidelock/uwb.h>

#define SAT_URSK_LEN 32u
#define SAT_RCFG_LEN 17u

/* The cfg struct keeps pointers, and the engine reads the URSK again on every
 * Pre-POLL decode, so both live for the session, not the shell command. */
static uint8_t s_ursk[SAT_URSK_LEN];
static uint8_t s_rcfg[SAT_RCFG_LEN];

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/** @brief Parse exactly 2*len hex chars into out; -1 on bad length or char. */
static int hex_parse(const char *s, uint8_t *out, size_t len)
{
	if (strlen(s) != 2u * len) {
		return -1;
	}
	for (size_t i = 0; i < len; i++) {
		int hi = hex_nibble(s[2 * i]);
		int lo = hex_nibble(s[2 * i + 1]);

		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

static int cmd_sat_join(const struct shell *sh, size_t argc, char **argv)
{
	struct ultrawidelock_uwb_cred_cfg cfg = {0};
	int rc;

	ARG_UNUSED(argc);
	if (hex_parse(argv[1], s_ursk, SAT_URSK_LEN) != 0) {
		shell_error(sh, "ursk: want %u hex chars", 2u * SAT_URSK_LEN);
		return -EINVAL;
	}
	if (hex_parse(argv[2], s_rcfg, SAT_RCFG_LEN) != 0) {
		shell_error(sh, "rcfg: want %u hex chars", 2u * SAT_RCFG_LEN);
		return -EINVAL;
	}
	/* rcfg[12] is the responder count baked into the SaltedHash. If it does
	 * not match this build, every derived STS diverges and nothing decodes —
	 * refuse loudly instead of listening to silence. */
	if (s_rcfg[12] != CONFIG_ULTRAWIDELOCK_NUM_RESPONDERS) {
		shell_error(sh, "rcfg says %u responders, this build is %u — rebuild one side",
			    s_rcfg[12], CONFIG_ULTRAWIDELOCK_NUM_RESPONDERS);
		return -EINVAL;
	}

	cfg.session_id = ((uint32_t)s_rcfg[4] << 24) | ((uint32_t)s_rcfg[5] << 16) |
			 ((uint32_t)s_rcfg[6] << 8) | s_rcfg[7];
	cfg.sts_index0 = ((uint32_t)s_rcfg[8] << 24) | ((uint32_t)s_rcfg[9] << 16) |
			 ((uint32_t)s_rcfg[10] << 8) | s_rcfg[11];
	cfg.block_duration_ms = (uint32_t)s_rcfg[13] * 96u;
	cfg.slot_per_round = s_rcfg[14];
	cfg.slot_duration_rstu = (uint16_t)(s_rcfg[15] * 400u);
	cfg.channel = (uint8_t)strtoul(argv[3], NULL, 10);
	cfg.sync_code_index = (uint8_t)strtoul(argv[4], NULL, 10);
	cfg.ursk = s_ursk;
	cfg.ranging_config = s_rcfg;
	cfg.rc_len = SAT_RCFG_LEN;

	/* Re-join is the normal case (a new session per walk-up); tear the old
	 * listen down first. A never-started stop is a no-op. */
	ultrawidelock_uwb_stop();
	rc = ultrawidelock_uwb_start_cred(&cfg);
	if (rc != 0) {
		shell_error(sh, "start_cred rc=%d", rc);
		return rc;
	}
	shell_print(sh, "SAT joined sid=0x%08x sts0=0x%08x spr=%u ch=%u code=%u resp=%u/%u",
		    cfg.session_id, cfg.sts_index0, cfg.slot_per_round, cfg.channel,
		    cfg.sync_code_index, CONFIG_ULTRAWIDELOCK_RESPONDER_INDEX,
		    CONFIG_ULTRAWIDELOCK_NUM_RESPONDERS);
	return 0;
}

static int cmd_sat_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ultrawidelock_uwb_stop();
	shell_print(sh, "SAT stopped");
	return 0;
}

LOG_MODULE_REGISTER(sat, LOG_LEVEL_INF);

#if defined(CONFIG_OPENTHREAD)
/*
 * The link key. Typed here rather than baked in, because a key in the image is
 * a key in the repository -- and it must match the lock's `anckey` byte for
 * byte or every report is discarded as unopenable, which at the lock looks
 * exactly like an anchor that never booted.
 */
static int cmd_sat_key(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t key[16];
	size_t hex_len;
	int rc;

	ARG_UNUSED(argc);

	hex_len = strlen(argv[1]);
	if (hex_len != 2u * sizeof(key) ||
	    hex2bin(argv[1], hex_len, key, sizeof(key)) != sizeof(key)) {
		shell_error(sh, "key must be exactly %u hex characters",
			    (unsigned)(2u * sizeof(key)));
		return -EINVAL;
	}
	rc = anchor_link_set_key(key, sizeof(key));
	if (rc != 0) {
		shell_error(sh, "storing the key rc=%d", rc);
		return rc;
	}
	shell_print(sh, "stored; reports %s",
		    anchor_link_ready() ? "will be sent" : "wait for the Thread link");
	return 0;
}
#endif

#if defined(CONFIG_OPENTHREAD)
/* The lock's Active Operational Dataset, as raw TLV hex. Mesh membership only:
 * this board joins no Matter fabric and does not need one to send UDP. */
static int cmd_sat_dataset(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t tlvs[254];
	size_t hex_len;
	size_t n;
	int rc;

	ARG_UNUSED(argc);

	hex_len = strlen(argv[1]);
	if ((hex_len % 2u) != 0u || hex_len / 2u > sizeof(tlvs)) {
		shell_error(sh, "dataset must be an even number of hex chars, <= %u bytes",
			    (unsigned)sizeof(tlvs));
		return -EINVAL;
	}
	n = hex2bin(argv[1], hex_len, tlvs, sizeof(tlvs));
	if (n != hex_len / 2u) {
		shell_error(sh, "not hex");
		return -EINVAL;
	}
	rc = anchor_link_set_dataset(tlvs, n);
	if (rc != 0) {
		shell_error(sh, "dataset rejected rc=%d", rc);
		return rc;
	}
	shell_print(sh, "dataset set (%u B); attaching", (unsigned)n);
	return 0;
}

/* One line, because three separate things have to be true before a single
 * report can reach the lock and they fail in ways that look identical. */
static int cmd_sat_link(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "key %s | thread %s | socket %s",
		    anchor_link_ready() ? "yes" : "NO",
		    anchor_link_attached() ? "attached" : "NOT attached",
		    anchor_link_ready() ? "open" : "check");
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sat_cmds,
	SHELL_CMD_ARG(join, NULL, "join <ursk-hex64> <rcfg-hex34> <channel> <sync-code>",
		      cmd_sat_join, 5, 0),
	SHELL_CMD_ARG(stop, NULL, "stop ranging and quiesce the radio", cmd_sat_stop, 1, 0),
#if defined(CONFIG_OPENTHREAD)
	SHELL_CMD_ARG(key, NULL, "set the sealed-link key: `key <hex32>` (same bytes as the "
		      "lock's `anckey`)", cmd_sat_key, 2, 0),
	SHELL_CMD_ARG(dataset, NULL, "join the lock's Thread network: `dataset <tlv-hex>`",
		      cmd_sat_dataset, 2, 0),
	SHELL_CMD_ARG(link, NULL, "sealed-link status", cmd_sat_link, 1, 0),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(sat, &sat_cmds, "satellite responder (stage B)", NULL);

int main(void)
{
	psa_status_t st = psa_crypto_init();

	if (st != PSA_SUCCESS) {
		printk("SAT psa_crypto_init failed (%d)\n", (int)st);
		return 0;
	}
#if defined(CONFIG_OPENTHREAD)
	/* After psa_crypto_init: the link seals with PSA and would fail every
	 * report if it opened first. */
	anchor_link_init();
#endif
	printk("SAT satellite responder %u/%u — waiting for `sat join`\n",
	       CONFIG_ULTRAWIDELOCK_RESPONDER_INDEX, CONFIG_ULTRAWIDELOCK_NUM_RESPONDERS);

	/* Range reporter: the engine's own logs are budgeted per session, so
	 * poll the generation counter and print every latched range — this line
	 * against the tape is stage B's second pass criterion. */
	uint32_t last_gen = ultrawidelock_uwb_range_generation();

	for (;;) {
		uint32_t gen = ultrawidelock_uwb_range_generation();
		int32_t cm;

		if (gen != last_gen && ultrawidelock_uwb_last_range_cm(&cm)) {
			uint32_t blk = 0u;

			/* Same line the lock prints, so the two captures can be
			 * JOINED ON BLOCK afterwards and subtracted at an instant
			 * where the true difference is known. Deliberately here in
			 * the main loop and not in the ranging callback: the
			 * per-frame trace is throttled per BOOT, so it stops after
			 * the first few blocks and never overlaps the other
			 * anchor's capture. This one prints for every range. */
			if (ultrawidelock_uwb_trusted_range_block_cm(&cm, &blk)) {
				/* LOG_INF, not printk. The shell's serial backend
				 * installs itself as the printk hook, so printk
				 * lands on the UART it shares with the prompt --
				 * which shreds the line -- regardless of what
				 * zephyr,console is set to. The logging subsystem
				 * has its own backend, so this reaches RTT intact
				 * while `sat join` keeps arriving on the UART. */
				LOG_INF("pair sid=%08x blk=%u mm=%d",
					(unsigned)ultrawidelock_uwb_session_id(),
					(unsigned)blk, (int)(cm * 10));
#if defined(CONFIG_OPENTHREAD)
				/* The report the lock actually acts on. The log
				 * line above stays: it is what the bench joins
				 * against the lock's, and it keeps working when
				 * the link is down. */
				anchor_link_report(cm * 10, blk);
#endif
			}
			last_gen = gen;
			printk("SAT range %d cm (gen %u)\n", (int)cm, (unsigned)gen);
		}
		k_sleep(K_MSEC(200));
	}
	return 0;
}
