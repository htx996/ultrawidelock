/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_side.c — differential-RSSI baseline, temporal filter, unlock gate.
 */

#include "ultrawidelock_side.h"

#include <string.h>

#define RSSI_ABSENT INT16_MIN
#define VEL_UNKNOWN INT32_MIN

void ultrawidelock_side_defaults(struct ultrawidelock_side_cfg *cfg)
{
	if (cfg == NULL) {
		return;
	}
	memset(cfg, 0, sizeof(*cfg));
	cfg->rssi_outside_margin_db = 6;
	cfg->rssi_threshold_band_db = 3;
	cfg->min_pkts_per_anchor = 3;
	cfg->quorum_mask =
		(uint8_t)(ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE | ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE);
	cfg->agree_windows = 3;
	cfg->dwell_ms = 400;
	/*
	 * Must exceed the interval at which features actually arrive, or the
	 * decision is stale before the next one lands and the gate can only ever
	 * pass in the single instant it commits.
	 *
	 * MEASURED 2026-08-11, first working walk-up: BLE witnesses summarise
	 * over WITNESS_WINDOW_MS (2000), so SF1 arrives about every 2 s. At 1500
	 * every window that did not itself re-commit came back
	 * ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE (0x40) -- five refusals in a row on the
	 * approach, and the grant landed only because one window happened to
	 * re-commit while the phone was still inside the unlock radius.
	 *
	 * 4500 covers two feed periods plus margin, so a single dropped window
	 * does not close the gate. It is deliberately below the caller-side feed
	 * watchdog (SIDE_FEED_WATCHDOG_MS, 5000): a feed that is merely late is
	 * this check's business, one that has stopped is the watchdog's.
	 *
	 * This does not widen the real responsiveness/security tradeoff, which is
	 * dominated by agree_windows x the feed period (about 6 s) -- a committed
	 * side already cannot flip faster than that. Shortening the witness
	 * window would be the better fix, but the filtered packet count is 3-8
	 * per 2 s against min_pkts_per_anchor, so halving it starves the quorum.
	 */
	cfg->evidence_fresh_ms = 4500;
	/*
	 * MEASURED 2026-08-11, symmetric witnesses ~1.5 m either side of the
	 * plane: 49% of windows landed in the dead band, because the unlock is
	 * decided at ~100 cm from the lock and that is where the two anchors are
	 * equidistant. OUTSIDE could only be committed while approaching, and
	 * nothing near the door could refresh committed_ms, so the grant
	 * survived or expired purely on walking speed -- 37 refusals, 16 of them
	 * EVIDENCE_STALE, against 2 grants on the same walk.
	 *
	 * 8 s covers a normal walk-up from the last confident OUTSIDE to the
	 * door. It is cancelled the instant an INSIDE is committed (see the hold
	 * test), which is the event that actually matters, and INSIDE_CONTRADICT
	 * already suppresses a lone INSIDE spike from flipping a real OUTSIDE.
	 */
	cfg->outside_hold_ms = 8000;
	/*
	 * The confidence at exactly rssi_outside_margin_db, so the margin is the
	 * ONE knob that sets how far outside is far enough. It used to be 70,
	 * which sounds like an independent sanity floor and is not: confidence is
	 * 60 + (|oi| - margin) * 4, so 70 silently demanded 2.5 dB beyond the
	 * margin and turned a declared 6 dB threshold into a real 9 dB one.
	 *
	 * MEASURED 2026-08-11: windows classified OUTSIDE with no fault flags at
	 * all were refused -- "withheld: side=2 conf=60 flags=0x00" -- while the
	 * documented margin said they should pass. Two thresholds multiplying is
	 * how a gate ends up rejecting evidence its own configuration calls good.
	 *
	 * Tighten by raising rssi_outside_margin_db, which says what it means in
	 * dB. Do not re-raise this: it would reintroduce the same hidden offset.
	 */
	cfg->confidence_min = 60;
	cfg->classifier_ver = 1;
	cfg->calibration_ver = 1;
}

void ultrawidelock_side_filter_init(struct ultrawidelock_side_filter *f,
				    const struct ultrawidelock_side_cfg *cfg)
{
	if (f == NULL) {
		return;
	}
	memset(f, 0, sizeof(*f));
	if (cfg != NULL) {
		f->cfg = *cfg;
	} else {
		ultrawidelock_side_defaults(&f->cfg);
	}
	f->cand = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
	f->committed = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
	f->motion = ULTRAWIDELOCK_SIDE_MOTION_UNKNOWN;
	f->last_seq = UINT32_MAX;
}

bool ultrawidelock_side_transition_ok(enum ultrawidelock_side_label from,
				      enum ultrawidelock_side_label to)
{
	if (from == to || from == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN ||
	    to == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) {
		return true;
	}
	/* Direct INSIDE <-> OUTSIDE flip without THRESHOLD is not allowed. */
	if ((from == ULTRAWIDELOCK_SIDE_LABEL_INSIDE && to == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE) ||
	    (from == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE && to == ULTRAWIDELOCK_SIDE_LABEL_INSIDE)) {
		return false;
	}
	return true;
}

static bool rssi_present(int16_t rssi, uint8_t pkts, uint8_t min_pkts)
{
	return rssi != RSSI_ABSENT && pkts >= min_pkts;
}

static uint8_t clamp_u8(int v)
{
	if (v < 0) {
		return 0;
	}
	if (v > 100) {
		return 100;
	}
	return (uint8_t)v;
}

struct ultrawidelock_side_raw
ultrawidelock_side_classify_raw(const struct ultrawidelock_side_cfg *cfg,
				const struct ultrawidelock_side_features *feat)
{
	struct ultrawidelock_side_raw raw;
	struct ultrawidelock_side_cfg local;
	const struct ultrawidelock_side_cfg *c = cfg;
	bool have_in, have_out, have_th;
	int oi, ti, ot;
	uint8_t mask = 0;
	uint8_t conf = 0;

	memset(&raw, 0, sizeof(raw));
	raw.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
	raw.outside_minus_inside_db = 0;
	raw.threshold_minus_inside_db = 0;
	raw.outside_minus_threshold_db = 0;

	if (feat == NULL) {
		return raw;
	}
	if (c == NULL) {
		ultrawidelock_side_defaults(&local);
		c = &local;
	}

	if ((feat->flags & (ULTRAWIDELOCK_SIDE_F_MULTI_PHONE | ULTRAWIDELOCK_SIDE_F_SESSION_MISMATCH |
			    ULTRAWIDELOCK_SIDE_F_REPLAY | ULTRAWIDELOCK_SIDE_F_VERSION_MISMATCH |
			    ULTRAWIDELOCK_SIDE_F_DEGRADED | ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE)) != 0) {
		return raw;
	}

	if (feat->uwb_range_mm >= 0) {
		mask |= ULTRAWIDELOCK_SIDE_ANCHOR_PRIMARY_UWB;
	}

	have_in = rssi_present(feat->ble_rssi_inside_dbm, feat->ble_pkts_inside,
			       c->min_pkts_per_anchor);
	have_out = rssi_present(feat->ble_rssi_outside_dbm, feat->ble_pkts_outside,
				c->min_pkts_per_anchor);
	have_th = rssi_present(feat->ble_rssi_threshold_dbm, feat->ble_pkts_threshold,
			       c->min_pkts_per_anchor);

	if (have_in) {
		mask |= ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE;
	}
	if (have_out) {
		mask |= ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE;
	}
	if (have_th) {
		mask |= ULTRAWIDELOCK_SIDE_ANCHOR_BLE_THRESHOLD;
	}

	/*
	 * Secondary UWB peer range is recorded in the contrib mask when present.
	 * Polarity comes from differential BLE, or from the fusion caller below.
	 * What is still refused is guessing which millimetre is "inside" from the
	 * primary lock ALONE -- that is how INSIDE→OUTSIDE errors start, and no
	 * amount of range on one anchor fixes it.
	 */
	if (feat->uwb_peer_mm >= 0) {
		mask |= ULTRAWIDELOCK_SIDE_ANCHOR_UWB_SATELLITE;
	}

	/*
	 * Two-anchor fusion polarity, when the caller has it.
	 *
	 * Taken as a real side rather than a hint because it is a differential
	 * between two anchors whose mounting the fusion layer knows, which is the
	 * same shape of evidence as differential BLE. It is applied FIRST so the
	 * BLE block below sees it as an already-held opinion and its existing
	 * disagreement checks (raw.side already OUTSIDE / already INSIDE) fire
	 * against it unchanged -- the two sources contradicting each other must
	 * mean UNKNOWN, and that logic is better reused than re-derived.
	 *
	 * A caller with no fusion, a stale one, or one whose triangle gate failed
	 * passes UNKNOWN and nothing here changes.
	 */
	if (feat->fusion_side == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE ||
	    feat->fusion_side == ULTRAWIDELOCK_SIDE_LABEL_INSIDE ||
	    feat->fusion_side == ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD) {
		raw.side = (enum ultrawidelock_side_label)feat->fusion_side;
		conf = feat->fusion_conf > 100u ? 100u : feat->fusion_conf;
	}

	if (have_in && have_out) {
		oi = (int)feat->ble_rssi_outside_dbm - (int)feat->ble_rssi_inside_dbm;
		raw.outside_minus_inside_db = (int16_t)oi;

		if (have_th) {
			ti = (int)feat->ble_rssi_threshold_dbm -
			     (int)feat->ble_rssi_inside_dbm;
			ot = (int)feat->ble_rssi_outside_dbm -
			     (int)feat->ble_rssi_threshold_dbm;
			raw.threshold_minus_inside_db = (int16_t)ti;
			raw.outside_minus_threshold_db = (int16_t)ot;

			if (oi > -c->rssi_threshold_band_db &&
			    oi < c->rssi_threshold_band_db &&
			    (ot > -c->rssi_threshold_band_db &&
			     ot < c->rssi_threshold_band_db)) {
				/* Near the lintel: both outside and inside are
				 * comparable and threshold is also close. */
				raw.side = ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD;
				conf = clamp_u8(55 + (c->rssi_threshold_band_db - (oi < 0 ? -oi : oi)) * 5);
				raw.contrib_mask = mask;
				raw.confidence = conf;
				return raw;
			}
		}

		if (oi >= c->rssi_outside_margin_db) {
			/* Stronger outside than inside: OUTSIDE candidate. */
			if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_INSIDE) {
				/* BLE and UWB disagree: safety UNKNOWN. */
				raw.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
				raw.confidence = 0;
				raw.contrib_mask = mask;
				return raw;
			}
			raw.side = ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE;
			conf = clamp_u8(60 + (oi - c->rssi_outside_margin_db) * 4);
		} else if (oi <= -c->rssi_outside_margin_db) {
			if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE) {
				raw.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
				raw.confidence = 0;
				raw.contrib_mask = mask;
				return raw;
			}
			raw.side = ULTRAWIDELOCK_SIDE_LABEL_INSIDE;
			conf = clamp_u8(60 + (-oi - c->rssi_outside_margin_db) * 4);
		} else if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) {
			raw.side = ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD;
			conf = 40;
		}
	} else if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) {
		/* Insufficient BLE quorum: never invent a side from absolute RSSI. */
		raw.contrib_mask = mask;
		raw.confidence = 0;
		return raw;
	}

	raw.contrib_mask = mask;
	raw.confidence = conf;
	return raw;
}

static enum ultrawidelock_side_motion motion_from(enum ultrawidelock_side_label side,
						  int32_t vel_mm_s, int32_t range_mm)
{
	bool closing = (vel_mm_s != VEL_UNKNOWN && vel_mm_s < -100);
	bool far = (range_mm < 0) || (range_mm > 1500);

	switch (side) {
	case ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE:
		if (far) {
			return closing ? ULTRAWIDELOCK_SIDE_MOTION_OUTSIDE_APPROACHING
				       : ULTRAWIDELOCK_SIDE_MOTION_OUTSIDE_FAR;
		}
		return closing ? ULTRAWIDELOCK_SIDE_MOTION_OUTSIDE_APPROACHING
			       : ULTRAWIDELOCK_SIDE_MOTION_OUTSIDE_NEAR;
	case ULTRAWIDELOCK_SIDE_LABEL_INSIDE:
		return far ? ULTRAWIDELOCK_SIDE_MOTION_INSIDE_FAR : ULTRAWIDELOCK_SIDE_MOTION_INSIDE_NEAR;
	case ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD:
		return ULTRAWIDELOCK_SIDE_MOTION_THRESHOLD;
	default:
		return ULTRAWIDELOCK_SIDE_MOTION_UNKNOWN;
	}
}

struct ultrawidelock_side_decision
ultrawidelock_side_filter_feed(struct ultrawidelock_side_filter *f,
			       const struct ultrawidelock_side_features *feat)
{
	struct ultrawidelock_side_decision d;
	struct ultrawidelock_side_raw raw;
	uint8_t flags = 0;
	uint8_t healthy;

	memset(&d, 0, sizeof(d));
	d.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
	d.motion = ULTRAWIDELOCK_SIDE_MOTION_UNKNOWN;

	if (f == NULL || feat == NULL) {
		d.flags = ULTRAWIDELOCK_SIDE_F_DEGRADED;
		return d;
	}

	flags = feat->flags;
	if (feat->classifier_ver != f->cfg.classifier_ver ||
	    feat->calibration_ver != f->cfg.calibration_ver) {
		flags |= ULTRAWIDELOCK_SIDE_F_VERSION_MISMATCH;
	}
	if (f->obs_session_id != 0 && feat->obs_session_id != f->obs_session_id) {
		/* New observation session: reset agreement counters. */
		f->cand_n = 0;
		f->cand = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
		f->last_seq = UINT32_MAX;
	}
	if (f->last_seq != UINT32_MAX && feat->seq != 0 && feat->seq <= f->last_seq) {
		flags |= ULTRAWIDELOCK_SIDE_F_REPLAY;
	}

	healthy = (uint8_t)(feat->anchor_health_mask & f->cfg.quorum_mask);
	if (healthy != f->cfg.quorum_mask) {
		flags |= ULTRAWIDELOCK_SIDE_F_QUORUM_FAIL;
	}

	raw = ultrawidelock_side_classify_raw(&f->cfg, feat);
	if ((flags & (ULTRAWIDELOCK_SIDE_F_MULTI_PHONE | ULTRAWIDELOCK_SIDE_F_SESSION_MISMATCH |
		      ULTRAWIDELOCK_SIDE_F_REPLAY | ULTRAWIDELOCK_SIDE_F_VERSION_MISMATCH |
		      ULTRAWIDELOCK_SIDE_F_DEGRADED | ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE |
		      ULTRAWIDELOCK_SIDE_F_QUORUM_FAIL)) != 0) {
		raw.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
		raw.confidence = 0;
	}

	/*
	 * Evidence that the credential is INSIDE is never discarded, even when it
	 * contradicts a committed OUTSIDE. This used to erase the window outright.
	 * That made the one transition that actually matters -- walking through
	 * the door -- unrepresentable: while committed OUTSIDE, every inside
	 * window became UNKNOWN, so INSIDE could never accumulate, could never
	 * commit, and last_inside_ms could never advance. MEASURED 2026-08-11:
	 * `passive unlock revoked` fired ZERO times across every hardware run,
	 * including runs that ended with the phone demonstrably indoors and the
	 * door still open.
	 *
	 * Stamping last_inside_ms here, before any suppression, is deliberately
	 * asymmetric: one inside-favouring window is enough to cancel the OUTSIDE
	 * hold, because closing the gate on weak evidence is the safe error.
	 * last_outside_ms is still only stamped on a full commit -- opening on one
	 * window is the dangerous error, and it stays behind agree_windows.
	 */
	if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_INSIDE) {
		f->last_inside_ms = feat->now_ms;
		if (f->committed == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE &&
		    !ultrawidelock_side_transition_ok(f->committed, raw.side)) {
			flags |= ULTRAWIDELOCK_SIDE_F_INSIDE_CONTRADICT;
		}
	}

	f->obs_session_id = feat->obs_session_id;
	f->last_seq = feat->seq;
	f->flags = flags;
	f->contrib_mask = raw.contrib_mask;

	/*
	 * A window that simply did not hear enough packets is MISSING DATA, not
	 * evidence against the candidate. classify_raw reports that as UNKNOWN
	 * with confidence 0 and no fault flag set -- distinguishable from the
	 * dead band (THRESHOLD, confidence 40) and from a real fault (flagged
	 * above). Collapsing the two is what made agree_windows unreachable:
	 *
	 * MEASURED 2026-08-11: 35% of windows fell below min_pkts_per_anchor on
	 * one anchor or the other, and each one reset cand_n to 0, so three
	 * CONSECUTIVE agreeing windows almost never happened. The gate refused
	 * fifteen approaches in a row with side=0 while individual windows were
	 * reporting confidence 88 and 100.
	 *
	 * Holding the candidate across a data gap cannot make the decision stale:
	 * committed_ms is untouched here, so evidence_fresh_ms still ages it, and
	 * the caller's feed watchdog still closes the gate if the feed dies.
	 */
	const bool evidence_gap = (raw.side == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) &&
				  raw.confidence == 0 && flags == 0;

	if (!evidence_gap) {
		if (raw.side == f->cand && raw.side != ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) {
			if (f->cand_n < 255) {
				f->cand_n++;
			}
		} else {
			f->cand = raw.side;
			f->cand_n = (raw.side == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) ? 0 : 1;
			f->cand_since_ms = feat->now_ms;
		}
		f->confidence = raw.confidence;
	}

	/*
	 * Sustained agreement IS the transition evidence. The commit used to also
	 * require ultrawidelock_side_transition_ok(), i.e. an observed THRESHOLD commit
	 * between INSIDE and OUTSIDE. That is not observable at walking speed: the
	 * dead band is about a metre wide, crossed in under 2 s, so a single
	 * witness window straddles it and never yields the three consecutive
	 * THRESHOLD windows the rule demanded. The gate could therefore latch a
	 * side and never legitimately leave it -- which is both halves of the
	 * problem, a grant that survives going indoors and a gate that will not
	 * re-arm on coming back out.
	 *
	 * What the rule was protecting against -- one RSSI spike flipping the
	 * side -- is already covered by agree_windows (3) plus dwell_ms, and by
	 * the explicit lone-spike guard below. transition_ok now feeds the
	 * INSIDE_CONTRADICT flag only, so the contradiction is still reported to
	 * the caller instead of silently changing the verdict.
	 */
	if (raw.side != ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN && f->cand_n >= f->cfg.agree_windows &&
	    (feat->now_ms - f->cand_since_ms) >= (int64_t)f->cfg.dwell_ms) {
		f->committed = raw.side;
		f->committed_ms = feat->now_ms;
		f->committed_conf = raw.confidence;
		if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE) {
			f->last_outside_ms = feat->now_ms;
		} else if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_INSIDE) {
			f->last_inside_ms = feat->now_ms;
		}
	} else if (raw.side == ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) {
		/* Confidence decay: do not keep a stale committed OUTSIDE live. */
		if (f->committed != ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN &&
		    (feat->now_ms - f->committed_ms) > (int64_t)f->cfg.evidence_fresh_ms) {
			f->committed = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
			f->confidence = 0;
			f->committed_conf = 0;
		}
	}

	/* A lone spike toward OUTSIDE while committed INSIDE never commits. */
	if (f->committed == ULTRAWIDELOCK_SIDE_LABEL_INSIDE &&
	    raw.side == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE &&
	    f->cand_n < f->cfg.agree_windows) {
		f->committed = ULTRAWIDELOCK_SIDE_LABEL_INSIDE;
	}

	f->motion = motion_from(f->committed, feat->uwb_vel_mm_s, feat->uwb_range_mm);

	d.side = f->committed;
	d.motion = f->motion;
	/*
	 * Report the confidence OF THE SIDE BEING REPORTED. d.side is the
	 * committed label, so pairing it with the last window's confidence
	 * describes two different moments, and ultrawidelock_side_may_passive_unlock
	 * tests them as if they were one.
	 *
	 * MEASURED 2026-08-11: "withheld: side=2 conf=40 flags=0x00" -- the gate
	 * had committed OUTSIDE, and refused it because a later window happened
	 * to land in the dead band, whose confidence is 40. Freshness still
	 * bounds how long a commit stays usable (evidence_fresh_ms below, plus
	 * the caller's feed watchdog); that is the right tool for "too old", not
	 * an unrelated sample's score.
	 */
	d.confidence = (f->committed != ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN) ? f->committed_conf
								: f->confidence;
	d.contrib_mask = f->contrib_mask;
	d.flags = f->flags;
	d.classifier_ver = f->cfg.classifier_ver;
	d.calibration_ver = f->cfg.calibration_ver;
	d.obs_session_id = feat->obs_session_id;
	d.seq = feat->seq;
	d.decided_ms = feat->now_ms;

	if (d.side != ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN &&
	    (feat->now_ms - f->committed_ms) > (int64_t)f->cfg.evidence_fresh_ms) {
		/*
		 * Hold a recent OUTSIDE across the dead band at the door plane.
		 * Deliberately OUTSIDE-only and strictly bounded: the credential
		 * must have been committed OUTSIDE inside outside_hold_ms, and no
		 * INSIDE may have been committed since. The moment inside wins,
		 * last_inside_ms overtakes last_outside_ms and this stops --
		 * which is the whole point, because "has it gone in" is the
		 * question, not "how long since the last good sample".
		 */
		const bool hold_outside =
			f->cfg.outside_hold_ms != 0 &&
			d.side == ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE &&
			f->last_outside_ms > f->last_inside_ms &&
			(feat->now_ms - f->last_outside_ms) <=
				(int64_t)f->cfg.outside_hold_ms;

		if (!hold_outside) {
			d.side = ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN;
			d.flags |= ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE;
			d.confidence = 0;
		}
	}

	return d;
}

bool ultrawidelock_side_may_passive_unlock(const struct ultrawidelock_side_decision *d,
				 const struct ultrawidelock_side_cfg *cfg)
{
	struct ultrawidelock_side_cfg local;
	const struct ultrawidelock_side_cfg *c = cfg;
	uint8_t min_conf;

	if (d == NULL) {
		return false;
	}
	if (c == NULL) {
		ultrawidelock_side_defaults(&local);
		c = &local;
	}
	min_conf = c->confidence_min;

	if (d->side != ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE) {
		return false;
	}
	if (d->confidence < min_conf) {
		return false;
	}
	if ((d->flags & (ULTRAWIDELOCK_SIDE_F_DEGRADED | ULTRAWIDELOCK_SIDE_F_MULTI_PHONE |
			 ULTRAWIDELOCK_SIDE_F_SESSION_MISMATCH | ULTRAWIDELOCK_SIDE_F_REPLAY |
			 ULTRAWIDELOCK_SIDE_F_VERSION_MISMATCH | ULTRAWIDELOCK_SIDE_F_INSIDE_CONTRADICT |
			 ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE | ULTRAWIDELOCK_SIDE_F_QUORUM_FAIL)) != 0) {
		return false;
	}
	return true;
}
