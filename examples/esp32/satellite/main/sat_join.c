/* SPDX-License-Identifier: ISC */

/*
 * sat_join.c — configure and start the responder from raw session parameters.
 *
 * A faithful port of apps/satellite/src/main.c's sat_join_apply(): the same
 * fields pulled from the same rcfg offsets, in the same order, with the same
 * refusal. That matters more than it reads — the two boards must derive the
 * same STS from the same 17 bytes, and every one of these offsets is an
 * agreement with the phone, not a local choice.
 *
 * Everything rcfg[17] carries: session id (4..7), STS_Index0 (8..11),
 * responder count (12), RAN multiplier (13), slots per round (14), chaps per
 * slot (15).
 */

#include "sat_join.h"

#include <string.h>

#include "esp_log.h"

#include <ultrawidelock/uwb.h>

/* SAT_NUM_RESPONDERS comes from main/CMakeLists.txt, which reads the SAME
 * CMake variable the project hands the engine component. One number, set in
 * one place: cred_round_config.h itself is private to the engine, so the app
 * cannot include it, and a hand-copied literal here is exactly the desync that
 * header exists to prevent. */
#ifndef SAT_NUM_RESPONDERS
#error "SAT_NUM_RESPONDERS must come from the build (see main/CMakeLists.txt)"
#endif

static const char *TAG = "sat";

/* The cfg struct keeps pointers, and the engine reads the URSK again on every
 * Pre-POLL decode, so both live for the session, not the command. */
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

int sat_hex_parse(const char *s, uint8_t *out, size_t len)
{
	if (s == NULL || out == NULL || strlen(s) != 2u * len) {
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

int sat_join_apply(const uint8_t *ursk, const uint8_t *rcfg, uint8_t channel,
		   uint8_t sync_code_index, uint32_t *sid_out, const char **why)
{
	struct ultrawidelock_uwb_cred_cfg cfg = {0};
	int rc;

	if (ursk == NULL || rcfg == NULL) {
		*why = "no session parameters";
		return -1;
	}

	/* rcfg[12] is the responder count baked into the SaltedHash. If it does
	 * not match this build, every derived STS diverges and nothing decodes —
	 * refuse loudly instead of listening to silence. */
	if (rcfg[12] != SAT_NUM_RESPONDERS) {
		*why = "rcfg responder count disagrees with this build";
		return -1;
	}

	if (ursk != s_ursk) {
		memcpy(s_ursk, ursk, SAT_URSK_LEN);
	}
	if (rcfg != s_rcfg) {
		memcpy(s_rcfg, rcfg, SAT_RCFG_LEN);
	}

	cfg.session_id = ((uint32_t)s_rcfg[4] << 24) | ((uint32_t)s_rcfg[5] << 16) |
			 ((uint32_t)s_rcfg[6] << 8) | s_rcfg[7];
	cfg.sts_index0 = ((uint32_t)s_rcfg[8] << 24) | ((uint32_t)s_rcfg[9] << 16) |
			 ((uint32_t)s_rcfg[10] << 8) | s_rcfg[11];
	cfg.block_duration_ms = (uint32_t)s_rcfg[13] * 96u;
	cfg.slot_per_round = s_rcfg[14];
	cfg.slot_duration_rstu = (uint16_t)(s_rcfg[15] * 400u);
	cfg.channel = channel;
	cfg.sync_code_index = sync_code_index;
	cfg.ursk = s_ursk;
	cfg.ranging_config = s_rcfg;
	cfg.rc_len = SAT_RCFG_LEN;

	/* Re-join is the normal case (a new session per walk-up); tear the old
	 * listen down first. A never-started stop is a no-op. */
	ultrawidelock_uwb_stop();
	rc = ultrawidelock_uwb_start_cred(&cfg);
	if (rc != 0) {
		*why = "start_cred refused the configuration";
		return rc;
	}
	if (sid_out != NULL) {
		*sid_out = cfg.session_id;
	}
	ESP_LOGD(TAG, "joined sid=0x%08x", (unsigned)cfg.session_id);
	return 0;
}
