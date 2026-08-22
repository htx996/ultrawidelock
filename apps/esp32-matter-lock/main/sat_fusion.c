/* SPDX-License-Identifier: ISC */

/*
 * sat_fusion.c — the lock's half of the two-anchor inside/outside gate.
 *
 * The ESP-IDF twin of what apps/dwm3001cdk-lock/src/main.c does around
 * ultrawidelock_satellite_set_*: take sealed reports from the second anchor,
 * pair each with our own measurement of the SAME ranging block, and answer one
 * question for the approach loop.
 *
 * WHAT IS NOT HERE. Any decision about the door. This file answers "does the
 * geometry object"; the approach controller decides whether to unlock. Keeping
 * those apart is why an UNKNOWN verdict can safely mean "no opinion" rather
 * than needing a policy of its own.
 */

#include "sat_fusion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nvs.h"

#include "ultrawidelock_fusion.h"
#include "ultrawidelock_port.h" /* ultrawidelock_uptime_ms */
#include "ultrawidelock_satellite.h"
#include "ultrawidelock_satlink.h"
#include <ultrawidelock/uwb.h> /* the handoff listener */

/* The anchor is on the same side as the lock only on a bench where both boards
 * sit on one side of the walkway. A bool Kconfig is defined-or-absent on
 * ESP-IDF, so it is normalised once here rather than at the call site. */
#ifdef CONFIG_ULTRAWIDELOCK_ANCHOR_SELF_INSIDE
#define SAT_SELF_INSIDE true
#else
#define SAT_SELF_INSIDE false
#endif

static const char *TAG = "satfuse";

/* NVS. The baseline is measured at install and must survive a reflash, or
 * every triangle test is sized for a geometry nobody has. Both names are in
 * PORTING.md's storage table and asserted against NVS's caps here. */
#define SATFUSE_NS "satfuse"
#define SATFUSE_BL "bl"
_Static_assert(sizeof(SATFUSE_NS) - 1 <= NVS_NS_NAME_MAX_SIZE - 1,
	       "NVS namespace name is longer than NVS allows (NVS_NS_NAME_MAX_SIZE - 1)");
_Static_assert(sizeof(SATFUSE_BL) - 1 <= NVS_KEY_NAME_MAX_SIZE - 1,
	       "NVS key name is longer than NVS allows (NVS_KEY_NAME_MAX_SIZE - 1)");

static struct ultrawidelock_satellite_set s_set;
static bool s_up;

/*
 * The frontier bias must stay several sigma below the baseline or noise decides
 * every verdict: at bias == baseline the INSIDE locus degenerates. Derived from
 * the baseline rather than configured beside it so a recalibration cannot leave
 * the two describing different geometries. What the configured pair fixes is
 * the gap between the frontier and the inside anchor -- BASELINE - BIAS -- so a
 * new baseline keeps that gap rather than the raw bias, and at the configured
 * baseline this returns the configured bias exactly. Mirrors baseline_bias() in
 * apps/dwm3001cdk-lock/src/main.c.
 */
static int32_t baseline_bias(int32_t baseline_mm)
{
	const int32_t gap = CONFIG_ULTRAWIDELOCK_ANCHOR_BASELINE_MM -
			    CONFIG_ULTRAWIDELOCK_ANCHOR_BOUNDARY_BIAS_MM;

	if (CONFIG_ULTRAWIDELOCK_ANCHOR_BOUNDARY_BIAS_MM == 0) {
		return 0;
	}
	return baseline_mm > gap ? baseline_mm - gap : 0;
}

/*
 * The separations a two-anchor install can actually have, in mm. Below 300 the
 * two anchors are close enough that the delta is noise; above 10000 the number
 * is a typo, not a doorway. The same window apps/dwm3001cdk-lock/src/main.c
 * applies to every route a baseline can arrive by -- an unchecked one sizes the
 * triangle test for a geometry that does not exist and the verdict it returns
 * means nothing.
 */
static bool baseline_sane(int32_t mm)
{
	return mm >= 300 && mm <= 10000;
}

/*
 * Role 1's slot only. A separation measured against one board is not the
 * separation to another, so roles 2 and 3 keep their configured baselines --
 * writing this number into their slots would size their triangle test for the
 * wrong geometry.
 */
static void baseline_apply(int32_t mm, bool save)
{
	struct ultrawidelock_satellite *sat = &s_set.peer[0];
	nvs_handle_t h;

	sat->cfg.baseline_mm = mm;
	sat->cfg.boundary_bias_mm = baseline_bias(mm);
	ESP_LOGI(TAG, "anchor baseline %d mm (frontier bias %d)", (int)mm,
		 (int)sat->cfg.boundary_bias_mm);
	if (!save) {
		return;
	}
	if (nvs_open(SATFUSE_NS, NVS_READWRITE, &h) != ESP_OK) {
		ESP_LOGE(TAG, "baseline not saved");
		return;
	}
	if (nvs_set_i32(h, SATFUSE_BL, mm) != ESP_OK || nvs_commit(h) != ESP_OK) {
		ESP_LOGE(TAG, "baseline not saved");
	}
	nvs_close(h);
}

static void baseline_load(void)
{
	nvs_handle_t h;
	int32_t mm = 0;

	if (nvs_open(SATFUSE_NS, NVS_READONLY, &h) != ESP_OK) {
		return;
	}
	if (nvs_get_i32(h, SATFUSE_BL, &mm) == ESP_OK && baseline_sane(mm)) {
		baseline_apply(mm, false);
	}
	nvs_close(h);
}

/**
 * A sealed, replay-checked distance from the second anchor.
 *
 * STORES ONLY, into that role's slot. Which of our measurements it belongs with
 * is settled later, by its ranging block, against the ring of recent samples --
 * so a report delayed by a block or two still finds its partner instead of
 * being matched against whatever we happen to hold at this instant.
 *
 * Runs on the Wi-Fi task (ESP-NOW receive).
 */
static void on_anchor_report(uint8_t role, int32_t peer_mm, uint16_t ranging_block)
{
	int64_t now = ultrawidelock_uptime_ms();

	ultrawidelock_satellite_set_report(&s_set, role, peer_mm, ranging_block, now);
	/* Logged at the SINK, because an unpaired report and a report that never
	 * arrived are the same silence otherwise, and they have completely
	 * different causes. */
	ESP_LOGD(TAG, "anchor role=%u report blk=%u mm=%d", (unsigned)role,
		 (unsigned)ranging_block, (int)peer_mm);
}

/*
 * Baseline calibration: the median of CAL_N paired readings, taken with the
 * phone held still. The two anchors and the phone are then collinear-ish and
 * |own - peer| IS the anchor separation, which is the one number no firmware
 * can derive and every triangle test is sized from.
 *
 * The median rather than the mean because the NLOS tail is one-sided: a body
 * between phone and anchor only ever adds distance, so a handful of long
 * readings would drag an average out and leave the geometry quietly wrong.
 *
 * Ported from apps/dwm3001cdk-lock/src/main.c, sampling the same two numbers:
 * this node's own range, and ultrawidelock_satellite_set_peer_mm()'s fresh
 * peer distance (the lock's uwb_range_mm and uwb_peer_mm).
 */
#define CAL_N 25u

static int32_t s_cal[CAL_N];
static uint8_t s_cal_n;
static bool s_cal_on;

/** Insertion sort, then the middle sample. Small n, and it shrugs off the tail. */
static int32_t cal_median(int32_t *v, size_t n)
{
	for (size_t i = 1; i < n; i++) {
		int32_t k = v[i];
		size_t j = i;

		for (; j > 0 && v[j - 1] > k; j--) {
			v[j] = v[j - 1];
		}
		v[j] = k;
	}
	return v[n / 2];
}

static void cal_sample(int32_t self_mm)
{
	int32_t peer_mm;
	int32_t m;

	if (!s_cal_on || self_mm < 0) {
		return;
	}
	peer_mm = ultrawidelock_satellite_set_peer_mm(&s_set, ultrawidelock_uptime_ms());
	if (peer_mm < 0) {
		return;
	}

	s_cal[s_cal_n++] = self_mm - peer_mm;
	if (s_cal_n < CAL_N) {
		return;
	}

	m = cal_median(s_cal, CAL_N);
	m = m < 0 ? -m : m;
	s_cal_on = false;
	s_cal_n = 0;
	/* A median outside the window means the phone was not held past one
	 * anchor -- most often it sat between them, where the difference is
	 * near zero. Refusing beats writing a geometry nothing was measured at. */
	if (!baseline_sane(m)) {
		ESP_LOGW(TAG, "baseline cal failed (median %d mm): not held past an anchor?",
			 (int)m);
		return;
	}
	baseline_apply(m, true);
}

void sat_fusion_observe(int32_t self_mm, uint32_t self_block, int64_t now_ms)
{
	if (s_up) {
		ultrawidelock_satellite_set_observe(&s_set, self_mm, self_block, now_ms);
		cal_sample(self_mm);
	}
}

bool sat_fusion_may_predict(int64_t now_ms)
{
	/* Before init, permit: this is the single-anchor lock's behaviour, and
	 * it is what an image with no satellite mounted must keep doing. */
	return !s_up || ultrawidelock_satellite_set_may_predict(&s_set, now_ms);
}

void sat_fusion_send_handoff(const uint8_t *ursk, size_t ursk_len, const uint8_t *rcfg,
			     size_t rcfg_len, uint8_t channel, uint8_t sync_code_index)
{
	if (!s_up) {
		return;
	}
	/* A size the codec cannot carry means this build and the ranging engine
	 * disagree about the wire format. Sending a truncated key would put the
	 * satellite on a schedule nothing else is using. */
	if (ursk == NULL || rcfg == NULL || ursk_len != ULTRAWIDELOCK_JOIN_URSK_LEN ||
	    rcfg_len != ULTRAWIDELOCK_JOIN_RCFG_LEN) {
		ESP_LOGW(TAG, "handoff not sent: ursk %u B rcfg %u B, expected %u/%u",
			 (unsigned)ursk_len, (unsigned)rcfg_len,
			 (unsigned)ULTRAWIDELOCK_JOIN_URSK_LEN,
			 (unsigned)ULTRAWIDELOCK_JOIN_RCFG_LEN);
		return;
	}
	ultrawidelock_satlink_send_handoff(ursk, rcfg, channel, sync_code_index);
}

/**
 * The credential session just started; tell the satellite how to join it.
 *
 * Runs on the credential thread BEFORE the radio starts, so this can land
 * before UWB_Time0 -- which is the point. It is also why nothing here blocks:
 * the seal and one ESP-NOW broadcast, then out.
 */
static void on_uwb_handoff(const struct ultrawidelock_uwb_handoff *h)
{
	if (h == NULL) {
		return;
	}
	sat_fusion_send_handoff(h->ursk, h->ursk_len, h->rcfg, h->rcfg_len, h->channel,
				h->sync_code_index);
}

/* ---- console ------------------------------------------------------------ */

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

static int cmd_anckey(int argc, char **argv)
{
	uint8_t key[ULTRAWIDELOCK_SEAL_KEY_LEN];
	int rc;

	if (argc != 2 || strlen(argv[1]) != 2u * sizeof(key)) {
		printf("usage: sat_anckey <hex%u>\n", (unsigned)(2u * sizeof(key)));
		return 1;
	}
	for (size_t i = 0; i < sizeof(key); i++) {
		int hi = hex_nibble(argv[1][2 * i]);
		int lo = hex_nibble(argv[1][2 * i + 1]);

		if (hi < 0 || lo < 0) {
			printf("key: not hex\n");
			return 1;
		}
		key[i] = (uint8_t)((hi << 4) | lo);
	}
	rc = ultrawidelock_satlink_set_key(key, sizeof(key));
	memset(key, 0, sizeof(key));
	if (rc != 0) {
		printf("link key not accepted\n");
		return 1;
	}
	printf("anchor link key stored\n");
	return 0;
}

static int cmd_baseline(int argc, char **argv)
{
	int32_t mm;

	if (argc != 2) {
		printf("usage: sat_baseline <mm>|cal   (anchor separation, measured at install)\n");
		return 1;
	}
	/* `cal` measures it instead of being told it: hold the phone still, a
	 * metre past one anchor and roughly in line with both, and the median
	 * of the next CAL_N paired readings becomes the baseline. */
	if (strcmp(argv[1], "cal") == 0) {
		s_cal_n = 0;
		s_cal_on = true;
		printf("baseline cal: hold the phone still ~1 m past one anchor (%u readings)\n",
		       (unsigned)CAL_N);
		return 0;
	}
	mm = (int32_t)strtol(argv[1], NULL, 10);
	if (!baseline_sane(mm)) {
		printf("baseline must be 300..10000 mm (got %d)\n", (int)mm);
		return 1;
	}
	baseline_apply(mm, true);
	return 0;
}

static int cmd_status(int argc, char **argv)
{
	int64_t now = ultrawidelock_uptime_ms();
	struct ultrawidelock_fusion_verdict fv;

	(void)argc;
	(void)argv;
	fv = ultrawidelock_satellite_set_verdict(&s_set, now);
	printf("anchor link:  %s\n", ultrawidelock_satlink_ready() ? "up, keyed" : "not reporting");
	printf("baseline:     %d mm (bias %d)\n", (int)s_set.peer[0].cfg.baseline_mm,
	       (int)s_set.peer[0].cfg.boundary_bias_mm);
	printf("peer distance: %d mm\n", (int)ultrawidelock_satellite_set_peer_mm(&s_set, now));
	printf("side:         %s%s\n",
	       fv.side == ULTRAWIDELOCK_SIDE_INSIDE
		       ? "INSIDE"
		       : (fv.side == ULTRAWIDELOCK_SIDE_OUTSIDE ? "OUTSIDE" : "UNKNOWN"),
	       fv.geometry_ok ? "" : " (triangle rejected)");
	/* The sign is the whole measurement; print it so a bench walk can be
	 * checked against ground truth without decoding the verdict. */
	printf("delta:        %d mm (negative = nearer the inside anchor)\n", (int)fv.delta_mm);
	printf("may predict:  %s\n", sat_fusion_may_predict(now) ? "yes" : "no (geometry says inside)");
	return 0;
}

static void console_register(void)
{
	const esp_console_cmd_t cmds[] = {
		{.command = "sat_anckey",
		 .help = "set the anchor link key (same bytes as the satellite's): sat_anckey <hex32>",
		 .hint = NULL,
		 .func = cmd_anckey},
		{.command = "sat_baseline",
		 .help = "anchor separation: <mm> to set it, `cal` to measure it",
		 .hint = NULL,
		 .func = cmd_baseline},
		{.command = "sat_status",
		 .help = "two-anchor link, baseline and current side verdict",
		 .hint = NULL,
		 .func = cmd_status},
	};

	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		if (esp_console_cmd_register(&cmds[i]) != ESP_OK) {
			ESP_LOGE(TAG, "could not register %s", cmds[i].command);
		}
	}
}

void sat_fusion_init(void)
{
	/*
	 * One geometry per role: the baseline is the distance to THAT satellite,
	 * and the tolerances are properties of the ranging, so they are shared.
	 * Roles 2 and 3 default to a zero baseline, which this tree already reads
	 * as "no satellite mounted" -- so a one-satellite lock is configured
	 * exactly as it is today and behaves exactly as it does today.
	 */
	const struct ultrawidelock_fusion_cfg cfg[ULTRAWIDELOCK_SATELLITE_MAX_ROLES] = {
		{
			.baseline_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_BASELINE_MM,
			.tol_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_TOL_MM,
			.deadband_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_DEADBAND_MM,
			.boundary_bias_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_BOUNDARY_BIAS_MM,
		},
		{
			.baseline_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_BASELINE_2_MM,
			.tol_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_TOL_MM,
			.deadband_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_DEADBAND_MM,
			.boundary_bias_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_BOUNDARY_BIAS_MM,
		},
		{
			.baseline_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_BASELINE_3_MM,
			.tol_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_TOL_MM,
			.deadband_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_DEADBAND_MM,
			.boundary_bias_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_BOUNDARY_BIAS_MM,
		},
	};

	ultrawidelock_satellite_set_init(&s_set, cfg, CONFIG_ULTRAWIDELOCK_ANCHOR_STALE_MS,
					 SAT_SELF_INSIDE);

	/* Before the carrier comes up, so a report cannot arrive with the sink
	 * still unset. */
	ultrawidelock_satlink_set_report_cb(on_anchor_report);
	/* The handoff role: this node is the lock. The value matters twice --
	 * it is the 0xFF nonce prefix that keeps the lock's sealed frames
	 * disjoint from every satellite's, and it is how the carrier knows this
	 * end must ANSWER channel probes rather than send them (the satellite
	 * scans Wi-Fi channels for us; we are pinned to the AP's). */
	if (ultrawidelock_satlink_init(ULTRAWIDELOCK_LINK_HANDOFF_ROLE) != 0) {
		ESP_LOGE(TAG, "sealed link did not come up; single-anchor behaviour");
	}
	/* The satellite cannot range until it holds this session's keys, and a
	 * human relaying them by console is the bench workaround this replaces. */
	ultrawidelock_uwb_set_handoff_listener(on_uwb_handoff);
	baseline_load();
	console_register();
	s_up = true;
	/* The round shape, said out loud: a count mismatch with the satellite
	 * build diverges every derived STS and the only symptom is silence, so
	 * both consoles print their number for the bench to compare. */
	ESP_LOGI(TAG, "two-anchor gate armed (baseline %d mm, round %d/%d)",
		 (int)s_set.peer[0].cfg.baseline_mm, CONFIG_ULTRAWIDELOCK_RESPONDER_INDEX,
		 CONFIG_ULTRAWIDELOCK_NUM_RESPONDERS);
}
