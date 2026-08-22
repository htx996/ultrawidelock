/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_side.h — safety-oriented INSIDE/OUTSIDE/THRESHOLD/UNKNOWN gate
 *       for passive approach unlock.
 *
 * Separate from ultrawidelock_fusion_may_predict(): that legacy helper fail-opens on
 * UNKNOWN and withholds on OUTSIDE, because it was written to degrade to
 * single-anchor behaviour when a satellite is quiet. Passive unlock has the
 * opposite dangerous error (actual INSIDE classified OUTSIDE), so this module
 * fail-closes: only a fresh, confident OUTSIDE with quorum and no inside
 * contradiction may release a passive unlock.
 *
 * Deliberate unlock paths (NFC Express Mode, Apple Home commands, mechanical
 * operation) must not call this gate.
 */

#ifndef ULTRAWIDELOCK_SIDE_H
#define ULTRAWIDELOCK_SIDE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Classifier / policy side labels. THRESHOLD extends enum ultrawidelock_side. */
enum ultrawidelock_side_label {
	ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN = ULTRAWIDELOCK_SIDE_UNKNOWN,
	ULTRAWIDELOCK_SIDE_LABEL_INSIDE = ULTRAWIDELOCK_SIDE_INSIDE,
	ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE = ULTRAWIDELOCK_SIDE_OUTSIDE,
	ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD = 3,
};

/** Temporal approach state around the door plane. */
enum ultrawidelock_side_motion {
	ULTRAWIDELOCK_SIDE_MOTION_UNKNOWN = 0,
	ULTRAWIDELOCK_SIDE_MOTION_OUTSIDE_FAR,
	ULTRAWIDELOCK_SIDE_MOTION_OUTSIDE_APPROACHING,
	ULTRAWIDELOCK_SIDE_MOTION_OUTSIDE_NEAR,
	ULTRAWIDELOCK_SIDE_MOTION_THRESHOLD,
	ULTRAWIDELOCK_SIDE_MOTION_INSIDE_NEAR,
	ULTRAWIDELOCK_SIDE_MOTION_INSIDE_FAR,
};

/** Bitmask of contributors that fed the latest decision. */
enum ultrawidelock_side_anchor_mask {
	ULTRAWIDELOCK_SIDE_ANCHOR_PRIMARY_UWB = 1u << 0,
	ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE = 1u << 1,
	ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE = 1u << 2,
	ULTRAWIDELOCK_SIDE_ANCHOR_BLE_THRESHOLD = 1u << 3,
	ULTRAWIDELOCK_SIDE_ANCHOR_UWB_SATELLITE = 1u << 4,
	ULTRAWIDELOCK_SIDE_ANCHOR_DOOR = 1u << 5,
};

/** Compact observation features for one correlated window. */
struct ultrawidelock_side_features {
	uint32_t obs_session_id; /**< ephemeral; bind to credential session locally */
	uint32_t seq;            /**< monotonic per obs_session_id */
	int64_t now_ms;          /**< lock monotonic clock */

	int32_t uwb_range_mm;     /**< authenticated primary range; <0 = absent */
	int32_t uwb_vel_mm_s;     /**< negative = closing; INT32_MIN = unknown */
	int32_t uwb_range_var_mm; /**< recent variance; <0 = unknown */

	int16_t ble_rssi_inside_dbm;    /**< INT16_MIN = absent */
	int16_t ble_rssi_outside_dbm;   /**< INT16_MIN = absent */
	int16_t ble_rssi_threshold_dbm; /**< INT16_MIN = absent */
	uint8_t ble_pkts_inside;
	uint8_t ble_pkts_outside;
	uint8_t ble_pkts_threshold;

	int32_t uwb_peer_mm; /**< secondary UWB range; <0 = absent */

	/**
	 * Polarity from a two-anchor fusion, when a caller has one.
	 *
	 * THE "explicitly configured fusion caller" the rule baseline refers to.
	 * Two UWB anchors give a real differential -- sign(d_inside - d_outside)
	 * -- and which anchor is which is a mounting fact the fusion layer holds,
	 * so this is not the "guess inside from the primary lock alone" that the
	 * baseline refuses to do. It is the same kind of evidence differential
	 * BLE provides, from better instruments.
	 *
	 * ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN (0) = absent, and absent must stay
	 * indistinguishable from "no opinion": a fusion that went stale or failed
	 * its triangle gate reports UNKNOWN, which withholds rather than permits.
	 */
	uint8_t fusion_side; /**< ULTRAWIDELOCK_SIDE_LABEL_*; 0 = absent */
	uint8_t fusion_conf; /**< 0..100; ignored when fusion_side is UNKNOWN */

	uint8_t door_state; /**< ULTRAWIDELOCK_DOOR_* or 0 = unknown */
	uint8_t prev_side;  /**< last confirmed ULTRAWIDELOCK_SIDE_LABEL_* */
	uint16_t since_confirmed_ms;

	uint8_t classifier_ver;
	uint8_t calibration_ver;
	uint8_t anchor_health_mask; /**< bits of enum ultrawidelock_side_anchor_mask that are healthy */
	uint8_t flags;              /**< ULTRAWIDELOCK_SIDE_F_* */
};

/** Feature / decision flags. */
#define ULTRAWIDELOCK_SIDE_F_DEGRADED           0x01u
#define ULTRAWIDELOCK_SIDE_F_MULTI_PHONE        0x02u
#define ULTRAWIDELOCK_SIDE_F_SESSION_MISMATCH   0x04u
#define ULTRAWIDELOCK_SIDE_F_REPLAY             0x08u
#define ULTRAWIDELOCK_SIDE_F_VERSION_MISMATCH   0x10u
#define ULTRAWIDELOCK_SIDE_F_INSIDE_CONTRADICT  0x20u
#define ULTRAWIDELOCK_SIDE_F_EVIDENCE_STALE     0x40u
#define ULTRAWIDELOCK_SIDE_F_QUORUM_FAIL        0x80u

/** Tunables for the rule baseline and temporal filter. */
struct ultrawidelock_side_cfg {
	int16_t rssi_outside_margin_db; /**< outside_minus_inside must clear this */
	int16_t rssi_threshold_band_db; /**< |outside-threshold| below this => THRESHOLD */
	uint8_t min_pkts_per_anchor;    /**< below this, that anchor is absent */
	uint8_t quorum_mask;            /**< required healthy bits */
	uint8_t agree_windows;          /**< consecutive agreeing windows to commit */
	uint16_t dwell_ms;              /**< minimum dwell in a motion state */
	uint16_t evidence_fresh_ms;     /**< max age of evidence for a decision */
	/**
	 * How long a committed OUTSIDE stays usable across the dead band, when
	 * no INSIDE has been committed since. Two symmetric witnesses are by
	 * construction equidistant AT the door plane, which is exactly where the
	 * approach controller asks to unlock -- so the gate must decide while the
	 * credential is still walking up and then survive the crossing. 0 = off.
	 */
	uint16_t outside_hold_ms;
	uint8_t confidence_min;         /**< 0..100; passive unlock floor */
	uint8_t classifier_ver;
	uint8_t calibration_ver;
};

/** One classified window, before temporal filtering. */
struct ultrawidelock_side_raw {
	enum ultrawidelock_side_label side;
	uint8_t confidence; /**< 0..100 */
	uint8_t contrib_mask;
	int16_t outside_minus_inside_db;
	int16_t threshold_minus_inside_db;
	int16_t outside_minus_threshold_db;
};

/** Auditable passive-unlock decision. */
struct ultrawidelock_side_decision {
	enum ultrawidelock_side_label side;
	enum ultrawidelock_side_motion motion;
	uint8_t confidence;
	uint8_t contrib_mask;
	uint8_t flags;
	uint8_t classifier_ver;
	uint8_t calibration_ver;
	uint32_t obs_session_id;
	uint32_t seq;
	int64_t decided_ms;
};

/** Temporal filter + previous-side memory. Caller-owned. */
struct ultrawidelock_side_filter {
	struct ultrawidelock_side_cfg cfg;
	enum ultrawidelock_side_label cand;
	enum ultrawidelock_side_label committed;
	enum ultrawidelock_side_motion motion;
	uint8_t cand_n;
	uint8_t confidence;
	/* Confidence of the window that COMMITTED the current side, not of
	 * whatever arrived last. The decision reports `committed` for its side,
	 * so reporting an unrelated later sample's confidence beside it describes
	 * two different moments -- and the caller checks them together. */
	uint8_t committed_conf;
	uint8_t contrib_mask;
	uint8_t flags;
	uint32_t obs_session_id;
	uint32_t last_seq;
	int64_t cand_since_ms;
	int64_t committed_ms;
	int64_t last_outside_ms;
	int64_t last_inside_ms;
};

/** Fill cfg with conservative defaults. */
void ultrawidelock_side_defaults(struct ultrawidelock_side_cfg *cfg);

/** Initialise filter; @p cfg NULL uses defaults. */
void ultrawidelock_side_filter_init(struct ultrawidelock_side_filter *f,
				    const struct ultrawidelock_side_cfg *cfg);

/**
 * Rule-based differential-RSSI (+ optional UWB pair) baseline.
 *
 * Does not mutate filter state. Returns UNKNOWN when evidence is insufficient
 * or contradictory; never invents OUTSIDE from a single absolute RSSI.
 */
struct ultrawidelock_side_raw
ultrawidelock_side_classify_raw(const struct ultrawidelock_side_cfg *cfg,
				const struct ultrawidelock_side_features *feat);

/**
 * Feed one correlated window through the temporal filter.
 *
 * Applies hysteresis, consecutive-agreement, dwell, confidence decay, evidence
 * expiry, and physically plausible transitions. A single RSSI spike cannot flip
 * INSIDE to OUTSIDE.
 */
struct ultrawidelock_side_decision
ultrawidelock_side_filter_feed(struct ultrawidelock_side_filter *f,
			       const struct ultrawidelock_side_features *feat);

/**
 * Passive unlock gate. True only for confident OUTSIDE with fresh evidence,
 * quorum, matching versions, and no contradiction / replay / degrade flags.
 *
 * False for INSIDE, THRESHOLD, UNKNOWN, and every fail-closed condition.
 */
bool ultrawidelock_side_may_passive_unlock(const struct ultrawidelock_side_decision *d,
				 const struct ultrawidelock_side_cfg *cfg);

/** True when @p from -> @p to is a physically plausible door-plane transition. */
bool ultrawidelock_side_transition_ok(enum ultrawidelock_side_label from,
				      enum ultrawidelock_side_label to);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_SIDE_H */
