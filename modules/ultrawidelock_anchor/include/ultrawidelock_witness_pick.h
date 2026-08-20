/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_witness_pick.h — which advertiser is the phone, without
 *       ever learning who the phone is.
 *
 * THE PROBLEM THIS SOLVES. A witness hears every advertiser in the room. To
 * turn that into "inside or outside" the lock has to know which of them is the
 * credential currently ranging with it. The obvious answer -- have the lock
 * hash the BLE connection's peer address and match it against what the
 * witnesses report -- is unsound, and the design used to depend on it:
 *
 *   The phone is the CENTRAL. The address the lock holds is an InitA, an RPA
 *   generated for the initiating role. What the witnesses hear comes from
 *   advertising sets, which carry their own address state and rotate on their
 *   own timers. The Core Spec permits a controller to reuse one RPA across
 *   roles; it does not require it, and nothing Apple publishes promises it. A
 *   design that assumes those two addresses are equal fails in the ordinary
 *   case, not in a corner, and it fails by never matching -- which is at least
 *   the safe direction, but it is still a design that does not work.
 *
 * THE ANSWER. Do not identify the phone. Identify the TRAJECTORY. The lock
 * already holds an authenticated UWB range to the credential, sampled in the
 * same windows the witnesses summarise. Exactly one advertiser in the room is
 * getting louder at the outside witness while that range is falling. Static
 * household advertisers -- a TV, a tag, a neighbour's speaker -- do not
 * correlate with it, because nothing ties their RSSI to this lock's ranging.
 *
 * So: score each candidate label on sign agreement between its RSSI change and
 * the negated range change, window by window. The winner is the phone, for
 * this approach only. No address is transmitted, none is matched, and the lock
 * learns nothing it can persist. Labels come from ultrawidelock_witness_msg.h
 * and are keyed under a group key the witnesses share and the lock does not,
 * so the same advertiser gets the same label at both witnesses -- which is
 * what lets inside be compared against outside once a pick exists.
 *
 * WHAT IT COSTS. The pick needs a walk-up to score against, which is precisely
 * the condition ultrawidelock_latch.h already requires for a clear (a closing
 * trajectory from beyond clear_min_mm). It buys nothing in the veto direction
 * and is not asked to: INSIDE is the resting state and needs no RF at all.
 *
 * FAILURE IS ONE-DIRECTIONAL. No confident pick means no clear means no
 * passive unlock. An RPA rotating mid-approach retires a label and restarts
 * the scoring; that costs one approach. A wrong pick would have to be an
 * advertiser that happens to track this lock's ranging, and it would then have
 * to also read OUTSIDE on the differential for clear_windows in a row.
 *
 * UNMEASURED: every threshold below is set from the geometry argument, not
 * from a capture. Stage P8 owes them numbers from real walk-ups.
 *
 * Integer arithmetic over caller-owned structs: no allocation, no threads, no
 * platform dependency.
 */

#ifndef ULTRAWIDELOCK_WITNESS_PICK_H
#define ULTRAWIDELOCK_WITNESS_PICK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_witness_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Labels tracked at once. Above this the least approach-like is evicted.
 * Sized to hold one full report plus carryover: a table smaller than
 * MSG_MAX_TUPLES is forced to evict on every report even when nothing
 * changed, and in a room with more advertisers than slots that cycled the
 * whole table window to window, so no label ever survived long enough to
 * score (measured 2026-08-20).
 */
#define ULTRAWIDELOCK_WITNESS_PICK_MAX (ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES + 4u)

/** Scoring thresholds. */
struct ultrawidelock_witness_pick_cfg {
	/** Net sign-agreement score the winner must reach. */
	int8_t min_score;
	/** Windows the winner must have been seen in. */
	uint8_t min_windows;
	/**
	 * How far clear of the runner-up the winner must be. Two advertisers
	 * that both track the approach are not a pick, they are an ambiguity,
	 * and ambiguity resolves to "no clear".
	 */
	int8_t min_margin;
	/**
	 * RSSI change below this is noise, not motion: the window is counted
	 * but not scored either way. Sized above the per-window spread of a
	 * stationary advertiser.
	 */
	int16_t rssi_eps_db;
	/** Range change below this is noise, same treatment. */
	int32_t range_eps_mm;
	/**
	 * Two correlated labels whose levels sit within this many dB of each
	 * other are treated as ONE emitter rather than as competition.
	 *
	 * A handset does not advertise once. iOS runs several advertising sets
	 * concurrently for different subsystems, and with extended advertising
	 * each may carry its own address -- so one approaching phone can produce
	 * several labels that all correlate perfectly with the range. Without
	 * this, min_margin reads that as an ambiguity and refuses to pick, which
	 * would block every clear permanently rather than occasionally.
	 *
	 * Two genuinely different movers are at different distances from the
	 * witness and so at different levels; two sets from one handset share an
	 * antenna and sit within a few dB. UNMEASURED: the width is an estimate
	 * and stage P0 owes it a capture from a real handset.
	 */
	int16_t cluster_db;
};

/** One tracked label. */
struct ultrawidelock_witness_pick_cand {
	uint32_t hash24;
	int16_t last_dbm;
	int32_t last_range_mm;
	int8_t score;
	uint8_t windows;
	bool used;
	bool have_prev;
};

/** Per-approach scoring state. Never persisted: a pick is valid for one walk-up. */
struct ultrawidelock_witness_pick {
	struct ultrawidelock_witness_pick_cfg cfg;
	struct ultrawidelock_witness_pick_cand cand[ULTRAWIDELOCK_WITNESS_PICK_MAX];
	/** Used slots discarded to admit a new label; reset clears it. A busy
	 *  room can cycle labels through the table faster than any of them can
	 *  accumulate score, and only this counter makes that visible. */
	uint32_t evictions;
};

/**
 * Why the current state does or does not pick — the numbers behind
 * ultrawidelock_witness_pick_best(), for diagnostics only.
 */
struct ultrawidelock_witness_pick_stats {
	uint32_t best_hash24;
	uint32_t evictions;
	int8_t best_score;
	/** Best cluster-aware rival score; SCORE floor (-32) when no rival. */
	int8_t runner_score;
	/** |last_dbm difference| between best and that rival; 0 when no rival.
	 *  A rival tracking best at a gap just past cluster_db is one handset's
	 *  second advertising set, not a second mover. */
	int16_t runner_gap_db;
	uint8_t best_windows;
	uint8_t n_cand;
	bool have;
};

/** Fill @p cfg with the documented defaults. */
void ultrawidelock_witness_pick_defaults(struct ultrawidelock_witness_pick_cfg *cfg);

/** Initialise; @p cfg NULL uses defaults. */
void ultrawidelock_witness_pick_init(struct ultrawidelock_witness_pick *p,
				     const struct ultrawidelock_witness_pick_cfg *cfg);

/** Discard all scoring. Call on session open and on session close. */
void ultrawidelock_witness_pick_reset(struct ultrawidelock_witness_pick *p);

/**
 * Score one window.
 *
 * @param msg      The OUTSIDE witness's report for this window. The outside
 *                 witness is the one that sees an approach as a rise; feeding
 *                 the inside witness here would score the mirror image and
 *                 pick the wrong sign.
 * @param range_mm Authenticated UWB range in the same window; negative = no
 *                 range, in which case the window is ignored entirely rather
 *                 than scored against a guess.
 */
void ultrawidelock_witness_pick_feed(struct ultrawidelock_witness_pick *p,
				     const struct ultrawidelock_witness_msg *msg,
				     int32_t range_mm);

/**
 * The current best label.
 *
 * @param hash24 Out; written only when true is returned.
 * @return true only when one candidate meets min_score, min_windows, and
 *         min_margin over every other candidate.
 */
bool ultrawidelock_witness_pick_best(const struct ultrawidelock_witness_pick *p, uint32_t *hash24);

/** The numbers ultrawidelock_witness_pick_best() decides on, for diagnostics. */
void ultrawidelock_witness_pick_stats(const struct ultrawidelock_witness_pick *p,
				      struct ultrawidelock_witness_pick_stats *st);

/**
 * Forget one label. For when the picked label stops appearing in the
 * witnesses' reports: the handset rotated its advertising address, and the
 * label is now a ghost whose banked score would otherwise outrank -- and so
 * block -- the same handset's new label for the rest of the approach. Only the
 * caller can see the reports, so only the caller can make this call.
 */
void ultrawidelock_witness_pick_retire(struct ultrawidelock_witness_pick *p, uint32_t hash24);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_WITNESS_PICK_H */
