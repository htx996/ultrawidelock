/**
 * @file test_ultrawidelock_side.c — fail-closed passive unlock policy and RSSI baseline.
 */

#include "test.h"

#include "ultrawidelock_side.h"
#include "ultrawidelock_side_log.h"

#include <string.h>

static struct ultrawidelock_side_features base_feat(void)
{
	struct ultrawidelock_side_features f;

	memset(&f, 0, sizeof(f));
	f.obs_session_id = 0xA11U;
	f.seq = 1;
	f.now_ms = 10000;
	f.uwb_range_mm = 800;
	f.uwb_vel_mm_s = -400;
	f.uwb_range_var_mm = 20;
	f.ble_rssi_inside_dbm = -70;
	f.ble_rssi_outside_dbm = -55;
	f.ble_rssi_threshold_dbm = -60;
	f.ble_pkts_inside = 8;
	f.ble_pkts_outside = 8;
	f.ble_pkts_threshold = 8;
	f.uwb_peer_mm = -1;
	f.classifier_ver = 1;
	f.calibration_ver = 1;
	f.anchor_health_mask =
		(uint8_t)(ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE | ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE |
			  ULTRAWIDELOCK_SIDE_ANCHOR_PRIMARY_UWB);
	return f;
}

static struct ultrawidelock_side_decision feed_n(struct ultrawidelock_side_filter *filt,
						 struct ultrawidelock_side_features *f, int n)
{
	struct ultrawidelock_side_decision d = {0};

	for (int i = 0; i < n; i++) {
		f->seq = (uint32_t)(i + 1);
		f->now_ms = 10000 + (int64_t)i * 200;
		d = ultrawidelock_side_filter_feed(filt, f);
	}
	return d;
}

static void test_raw_differential(void)
{
	struct ultrawidelock_side_cfg cfg;
	struct ultrawidelock_side_features f = base_feat();
	struct ultrawidelock_side_raw raw;

	t_group("side: differential RSSI baseline");
	ultrawidelock_side_defaults(&cfg);

	raw = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("raw.outside", raw.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
	T_OK("raw.oi", raw.outside_minus_inside_db == 15);
	T_OK("raw.conf", raw.confidence >= cfg.confidence_min);

	f.ble_rssi_inside_dbm = -50;
	f.ble_rssi_outside_dbm = -70;
	raw = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("raw.inside", raw.side, ULTRAWIDELOCK_SIDE_LABEL_INSIDE);

	f.ble_rssi_inside_dbm = -60;
	f.ble_rssi_outside_dbm = -58;
	raw = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("raw.threshold_band", raw.side, ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD);

	f.ble_pkts_outside = 1;
	raw = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("raw.no_quorum_pkts", raw.side, ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);

	f = base_feat();
	f.ble_rssi_outside_dbm = INT16_MIN;
	f.ble_pkts_outside = 0;
	raw = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("raw.missing_outside", raw.side, ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);
}

static void test_policy_fail_closed(void)
{
	struct ultrawidelock_side_cfg cfg;
	struct ultrawidelock_side_decision d;

	t_group("side: passive unlock is fail-closed");
	ultrawidelock_side_defaults(&cfg);
	memset(&d, 0, sizeof(d));

	T_OK("null_dec", !ultrawidelock_side_may_passive_unlock(NULL, &cfg));

	d.side = ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE;
	d.confidence = 90;
	T_OK("outside_ok", ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.side = ULTRAWIDELOCK_SIDE_LABEL_INSIDE;
	T_OK("inside_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.side = ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD;
	T_OK("threshold_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
	T_OK("unknown_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.side = ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE;
	d.confidence = 10;
	T_OK("low_conf_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.confidence = 90;
	d.flags = ULTRAWIDELOCK_SIDE_F_QUORUM_FAIL;
	T_OK("quorum_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.flags = ULTRAWIDELOCK_SIDE_F_MULTI_PHONE;
	T_OK("multi_phone_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.flags = ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE;
	T_OK("stale_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	d.flags = ULTRAWIDELOCK_SIDE_F_DEGRADED;
	T_OK("degraded_blocked", !ultrawidelock_side_may_passive_unlock(&d, &cfg));
}

static void test_temporal_no_spike_flip(void)
{
	struct ultrawidelock_side_filter filt;
	struct ultrawidelock_side_cfg cfg;
	struct ultrawidelock_side_features f = base_feat();
	struct ultrawidelock_side_decision d;

	t_group("side: single spike cannot flip INSIDE to OUTSIDE");
	ultrawidelock_side_defaults(&cfg);
	cfg.agree_windows = 3;
	cfg.dwell_ms = 200;
	ultrawidelock_side_filter_init(&filt, &cfg);

	/* Establish INSIDE. */
	f.ble_rssi_inside_dbm = -50;
	f.ble_rssi_outside_dbm = -70;
	d = feed_n(&filt, &f, 5);
	T_EQ("commit.inside", d.side, ULTRAWIDELOCK_SIDE_LABEL_INSIDE);
	T_OK("inside.no_unlock", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	/* One outside-looking spike, still within the evidence window. */
	f.ble_rssi_inside_dbm = -70;
	f.ble_rssi_outside_dbm = -50;
	f.seq = 10;
	f.now_ms = 11200;
	d = ultrawidelock_side_filter_feed(&filt, &f);
	T_OK("spike.not_outside", d.side != ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
	T_EQ("spike.still_inside", d.side, ULTRAWIDELOCK_SIDE_LABEL_INSIDE);
	T_OK("spike.no_unlock", !ultrawidelock_side_may_passive_unlock(&d, &cfg));
}

static void test_outside_approach_unlock(void)
{
	struct ultrawidelock_side_filter filt;
	struct ultrawidelock_side_cfg cfg;
	struct ultrawidelock_side_features f = base_feat();
	struct ultrawidelock_side_decision d;

	t_group("side: confident OUTSIDE may passively unlock");
	ultrawidelock_side_defaults(&cfg);
	cfg.agree_windows = 3;
	cfg.dwell_ms = 200;
	ultrawidelock_side_filter_init(&filt, &cfg);

	d = feed_n(&filt, &f, 5);
	T_EQ("commit.outside", d.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
	T_OK("outside.unlock", ultrawidelock_side_may_passive_unlock(&d, &cfg));
}

static void feat_outside(struct ultrawidelock_side_features *f)
{
	f->ble_rssi_inside_dbm = -70;
	f->ble_rssi_outside_dbm = -55;
	f->ble_pkts_inside = 8;
	f->ble_pkts_outside = 8;
}

static void feat_inside(struct ultrawidelock_side_features *f)
{
	f->ble_rssi_inside_dbm = -55;
	f->ble_rssi_outside_dbm = -70;
	f->ble_pkts_inside = 8;
	f->ble_pkts_outside = 8;
}

static void feat_dead_band(struct ultrawidelock_side_features *f)
{
	f->ble_rssi_inside_dbm = -62;
	f->ble_rssi_outside_dbm = -60;
	f->ble_pkts_inside = 8;
	f->ble_pkts_outside = 8;
}

/** Heard, but under min_pkts_per_anchor: missing data, not evidence. */
static void feat_gap(struct ultrawidelock_side_features *f)
{
	f->ble_pkts_inside = 1;
	f->ble_pkts_outside = 1;
}

static struct ultrawidelock_side_decision feed_at(struct ultrawidelock_side_filter *filt,
				        struct ultrawidelock_side_features *f, uint32_t *seq,
				        int64_t now_ms)
{
	f->seq = ++(*seq);
	f->now_ms = now_ms;
	return ultrawidelock_side_filter_feed(filt, f);
}

/*
 * Walking through the door, in both directions. This is the case the gate got
 * wrong on hardware: it could latch a side and never legitimately leave it, so
 * a grant survived the walk-in and the gate would not re-arm on the way back.
 */
static void test_door_crossing(void)
{
	struct ultrawidelock_side_filter filt;
	struct ultrawidelock_side_cfg cfg;
	struct ultrawidelock_side_features f = base_feat();
	struct ultrawidelock_side_decision d;
	uint32_t seq = 0;
	int64_t t = 10000;

	t_group("side: door crossing commits and re-arms");
	ultrawidelock_side_defaults(&cfg);
	cfg.agree_windows = 3;
	cfg.dwell_ms = 200;
	ultrawidelock_side_filter_init(&filt, &cfg);

	feat_outside(&f);
	for (int i = 0; i < 4; i++) {
		d = feed_at(&filt, &f, &seq, t += 200);
	}
	T_EQ("cross.outside_first", d.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);

	/* One inside window: too little to commit, enough to report. */
	feat_inside(&f);
	d = feed_at(&filt, &f, &seq, t += 200);
	T_OK("cross.contradict_reported",
	     (d.flags & ULTRAWIDELOCK_SIDE_F_INSIDE_CONTRADICT) != 0);
	T_EQ("cross.one_window_no_flip", d.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);

	/* Sustained inside must commit, with no observed THRESHOLD in between. */
	for (int i = 0; i < 3; i++) {
		d = feed_at(&filt, &f, &seq, t += 200);
	}
	T_EQ("cross.walk_in_commits", d.side, ULTRAWIDELOCK_SIDE_LABEL_INSIDE);
	T_OK("cross.inside_no_unlock", !ultrawidelock_side_may_passive_unlock(&d, &cfg));

	/* And back out again: the gate must re-arm. */
	feat_outside(&f);
	d = feed_at(&filt, &f, &seq, t += 200);
	T_EQ("cross.one_window_no_rearm", d.side, ULTRAWIDELOCK_SIDE_LABEL_INSIDE);
	for (int i = 0; i < 3; i++) {
		d = feed_at(&filt, &f, &seq, t += 200);
	}
	T_EQ("cross.walk_out_commits", d.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
	T_OK("cross.outside_unlocks", ultrawidelock_side_may_passive_unlock(&d, &cfg));
}

/*
 * outside_hold_ms carries a committed OUTSIDE across the dead band at the door
 * plane, and one inside-favouring window cancels it.
 */
static void test_outside_hold(void)
{
	struct ultrawidelock_side_filter filt;
	struct ultrawidelock_side_cfg cfg;
	struct ultrawidelock_side_features f = base_feat();
	struct ultrawidelock_side_decision d;
	uint32_t seq = 0;
	int64_t t = 10000;

	t_group("side: outside hold across the dead band");
	ultrawidelock_side_defaults(&cfg);
	cfg.agree_windows = 3;
	cfg.dwell_ms = 200;
	T_OK("hold.enabled", cfg.outside_hold_ms > cfg.evidence_fresh_ms);
	ultrawidelock_side_filter_init(&filt, &cfg);

	feat_outside(&f);
	for (int i = 0; i < 4; i++) {
		d = feed_at(&filt, &f, &seq, t += 200);
	}
	T_EQ("hold.committed", d.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);

	/* Evidence gaps while walking up: no commit refresh, no clear yet. */
	feat_gap(&f);
	d = feed_at(&filt, &f, &seq, t + 2000);

	/* Dead band, past evidence_fresh_ms: the hold keeps OUTSIDE usable. */
	feat_dead_band(&f);
	d = feed_at(&filt, &f, &seq, t + (int64_t)cfg.evidence_fresh_ms + 500);
	T_EQ("hold.survives_dead_band", d.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);

	/* Same walk, but one inside window on the way: the hold must end. */
	ultrawidelock_side_filter_init(&filt, &cfg);
	seq = 0;
	t = 10000;
	feat_outside(&f);
	for (int i = 0; i < 4; i++) {
		d = feed_at(&filt, &f, &seq, t += 200);
	}
	T_EQ("hold.committed2", d.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);

	feat_inside(&f);
	d = feed_at(&filt, &f, &seq, t + 1000);

	feat_dead_band(&f);
	d = feed_at(&filt, &f, &seq, t + (int64_t)cfg.evidence_fresh_ms + 500);
	T_EQ("hold.cancelled_by_inside", d.side, ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);
	T_OK("hold.cancelled_stale", (d.flags & ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE) != 0);
	T_OK("hold.cancelled_no_unlock", !ultrawidelock_side_may_passive_unlock(&d, &cfg));
}

static void test_transition_rules(void)
{
	t_group("side: transition plausibility");
	T_OK("same_ok", ultrawidelock_side_transition_ok(ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE,
							 ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE));
	T_OK("via_threshold", ultrawidelock_side_transition_ok(ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE,
							       ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD));
	T_OK("direct_flip_blocked",
	     !ultrawidelock_side_transition_ok(ULTRAWIDELOCK_SIDE_LABEL_INSIDE,
					       ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE));
	T_OK("unknown_ok", ultrawidelock_side_transition_ok(ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN,
							    ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE));
}

static void test_log_roundtrip(void)
{
	struct ultrawidelock_side_features f = base_feat();
	struct ultrawidelock_side_decision d;
	struct ultrawidelock_side_raw raw;
	struct ultrawidelock_side_log_rec rec;
	struct ultrawidelock_side_cfg cfg;

	t_group("side: binary log pack/check");
	ultrawidelock_side_defaults(&cfg);
	raw = ultrawidelock_side_classify_raw(&cfg, &f);
	memset(&d, 0, sizeof(d));
	d.side = raw.side;
	d.confidence = raw.confidence;
	d.contrib_mask = raw.contrib_mask;
	d.obs_session_id = f.obs_session_id;
	d.seq = f.seq;
	d.classifier_ver = 1;
	d.calibration_ver = 1;

	T_EQ("pack", ultrawidelock_side_log_pack(&f, &d, &raw, &rec), (long)ULTRAWIDELOCK_SIDE_LOG_SIZE);
	T_EQ("check", ultrawidelock_side_log_check(&rec), 0);
	rec.confidence ^= 0xff;
	T_OK("bad_crc", ultrawidelock_side_log_check(&rec) != 0);
}

/*
 * Two-anchor fusion as the polarity source, which is what drives the gate now
 * that the BLE witnesses are retired. The safety-relevant cases are the ones
 * where evidence is MISSING or CONTRADICTORY, so they get the most attention:
 * an absent fusion must not become permission.
 */
static struct ultrawidelock_side_features fusion_feat(uint8_t side)
{
	struct ultrawidelock_side_features f;

	memset(&f, 0, sizeof(f));
	f.obs_session_id = 0xF00U;
	f.seq = 1;
	f.now_ms = 10000;
	f.uwb_range_mm = 900;
	f.uwb_vel_mm_s = -400;
	f.uwb_range_var_mm = 20;
	/* No BLE at all: the retired-witness world. */
	f.ble_rssi_inside_dbm = INT16_MIN;
	f.ble_rssi_outside_dbm = INT16_MIN;
	f.ble_rssi_threshold_dbm = INT16_MIN;
	f.uwb_peer_mm = 1400;
	f.fusion_side = side;
	f.fusion_conf = (side == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) ? 0u : 70u;
	f.classifier_ver = 1;
	f.calibration_ver = 1;
	f.anchor_health_mask = (uint8_t)(ULTRAWIDELOCK_SIDE_ANCHOR_PRIMARY_UWB |
					 ULTRAWIDELOCK_SIDE_ANCHOR_UWB_SATELLITE);
	return f;
}

static void test_fusion_polarity(void)
{
	struct ultrawidelock_side_cfg cfg;
	struct ultrawidelock_side_features f;
	struct ultrawidelock_side_raw r;

	t_group("side: fusion polarity with no BLE");

	ultrawidelock_side_defaults(&cfg);
	cfg.quorum_mask = (uint8_t)(ULTRAWIDELOCK_SIDE_ANCHOR_PRIMARY_UWB |
				    ULTRAWIDELOCK_SIDE_ANCHOR_UWB_SATELLITE);

	f = fusion_feat(ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
	r = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("fusion.outside", r.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
	T_OK("fusion.outside_conf", r.confidence > 0);
	T_OK("fusion.sat_bit",
	     (r.contrib_mask & ULTRAWIDELOCK_SIDE_ANCHOR_UWB_SATELLITE) != 0);

	f = fusion_feat(ULTRAWIDELOCK_SIDE_LABEL_INSIDE);
	r = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("fusion.inside", r.side, ULTRAWIDELOCK_SIDE_LABEL_INSIDE);

	t_group("side: absent fusion is never permission");

	/* The whole point of the gate. No fusion, no BLE -> no side, and the
	 * confidence must not carry over from a previous opinion. */
	f = fusion_feat(ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);
	r = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("fusion.absent_unknown", r.side, ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);
	T_EQ("fusion.absent_conf", r.confidence, 0);

	/* A peer distance that never paired: the satellite bit is still set (we
	 * DID hear from it) but polarity is absent, so still no side. */
	f = fusion_feat(ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);
	f.uwb_peer_mm = 1400;
	r = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("fusion.unpaired_unknown", r.side, ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);

	t_group("side: fusion loses quorum when the satellite is silent");

	f = fusion_feat(ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
	f.uwb_peer_mm = -1;
	f.anchor_health_mask = ULTRAWIDELOCK_SIDE_ANCHOR_PRIMARY_UWB;
	{
		struct ultrawidelock_side_filter filt;
		struct ultrawidelock_side_decision d;

		ultrawidelock_side_filter_init(&filt, &cfg);
		d = ultrawidelock_side_filter_feed(&filt, &f);
		T_OK("fusion.quorum_fail",
		     (d.flags & ULTRAWIDELOCK_SIDE_F_QUORUM_FAIL) != 0);
		T_OK("fusion.quorum_no_unlock",
		     !ultrawidelock_side_may_passive_unlock(&d, &cfg));
	}

	t_group("side: fusion and BLE contradicting means UNKNOWN");

	/* Both sources present and disagreeing must not resolve to either one.
	 * BLE says OUTSIDE (outside stronger than inside); fusion says INSIDE. */
	f = fusion_feat(ULTRAWIDELOCK_SIDE_LABEL_INSIDE);
	f.ble_rssi_inside_dbm = -70;
	f.ble_rssi_outside_dbm = -55;
	f.ble_pkts_inside = 8;
	f.ble_pkts_outside = 8;
	f.anchor_health_mask |= (uint8_t)(ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE |
					  ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE);
	r = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("fusion.contradict", r.side, ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN);
	T_EQ("fusion.contradict_conf", r.confidence, 0);

	/* Agreeing is not a contradiction: both say OUTSIDE. */
	f.fusion_side = ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE;
	r = ultrawidelock_side_classify_raw(&cfg, &f);
	T_EQ("fusion.agree", r.side, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE);
}

void test_ultrawidelock_side(void)
{
	test_fusion_polarity();
	test_raw_differential();
	test_policy_fail_closed();
	test_temporal_no_spike_flip();
	test_outside_approach_unlock();
	test_door_crossing();
	test_outside_hold();
	test_transition_rules();
	test_log_roundtrip();
}
