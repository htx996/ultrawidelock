/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_fusion.h — two-anchor fusion: which side of the door, and whether
 *       the two distances are geometrically possible at all.
 *
 * The sign of (d_inside - d_outside) says which side of the anchors' bisector
 * the phone is on -- which means "which side of the door" only if the anchors
 * are mounted symmetrically about the door plane; no code substitutes for that.
 * The triangle inequality over the known baseline gates asymmetric manipulation
 * and impossible pairs. It does NOT catch a symmetric relay, and needs not:
 * inflating both links makes the phone look further, which fails the unlock
 * radius on its own; reduction is defended by time-of-flight + STS, not this.
 * A consistency layer ON TOP of per-link integrity consensus, never instead --
 * feed it only distances that already passed their own link's gate.
 */

#ifndef ULTRAWIDELOCK_FUSION_H
#define ULTRAWIDELOCK_FUSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Which side of the anchor baseline's perpendicular bisector the phone is on. */
enum ultrawidelock_side {
	ULTRAWIDELOCK_SIDE_UNKNOWN = 0, /**< inside the dead band, or the pair was rejected. */
	ULTRAWIDELOCK_SIDE_INSIDE,
	ULTRAWIDELOCK_SIDE_OUTSIDE,
};

/** Fixed install geometry plus the two tolerances, both sized from measured jitter. */
struct ultrawidelock_fusion_cfg {
	/** Anchor separation, mm. Known and fixed: measure it once, at install. */
	int32_t baseline_mm;
	/**
	 * Slack on the triangle inequality, mm. Size at 3 sigma of the measured
	 * per-link jitter, so a legitimate pair essentially never trips it --
	 * this gate exists to catch impossible geometry, not ordinary noise.
	 */
	int32_t tol_mm;
	/**
	 * Half-width of the UNKNOWN band about the bisector, mm. Size at about
	 * 2 sigma: within it the two distances are not far enough apart for
	 * their difference to have a reliable sign, and saying so beats
	 * guessing. A phone standing exactly in the doorway is genuinely
	 * ambiguous and should read that way.
	 */
	int32_t deadband_mm;
	/**
	 * Shift of the side boundary toward the inside anchor, mm. 0 keeps the
	 * boundary on the perpendicular bisector, which is the door plane when
	 * the anchors straddle the door -- the shipping install. A positive
	 * value moves the frontier so INSIDE begins (baseline - bias) / 2 in
	 * front of the inside anchor on the axis: for a bench where both
	 * anchors sit on the same side of the walkway, that puts "inside"
	 * behind the lock itself. It must stay several sigma below the
	 * baseline; at bias == baseline the INSIDE locus degenerates to the
	 * ray behind the anchor and noise decides every verdict.
	 */
	int32_t boundary_bias_mm;
};

/** The verdict, with the evidence that produced it rather than just a yes or no. */
struct ultrawidelock_fusion_verdict {
	enum ultrawidelock_side side;
	/** False when the triangle inequality failed; @p side is then UNKNOWN. */
	bool geometry_ok;
	/** d_inside - d_outside, mm. Negative means nearer the inside anchor. */
	int32_t delta_mm;
};

/**
 * @brief Fuse one pair of distances taken from the same ranging round.
 *
 * @param cfg Install geometry and tolerances.
 * @param d_inside_mm Distance from the anchor on the inside of the door.
 * @param d_outside_mm Distance from the anchor on the outside.
 *
 * The two distances MUST be from the same round. Pairing samples taken at
 * different moments while the phone is moving produces a geometrically
 * impossible pair, which this will correctly reject -- and a stream of
 * rejections whose real cause is a pairing bug reads as a hardware fault.
 */
struct ultrawidelock_fusion_verdict
ultrawidelock_fusion_eval(const struct ultrawidelock_fusion_cfg *cfg, int32_t d_inside_mm,
			  int32_t d_outside_mm);

/**
 * @brief Whether a verdict may release a PREDICTIVE unlock.
 *
 * True for INSIDE and, deliberately, for UNKNOWN.
 *
 * UNKNOWN permits it because the alternative is worse. A satellite that has
 * gone quiet, a phone in the doorway, or a fresh boot all produce UNKNOWN, and
 * a door that refuses to open early in those cases has degraded from "quick"
 * to "locked out" for a reason its owner cannot see. Degrading to today's
 * single-anchor behaviour is the correct failure.
 *
 * Only a positive OUTSIDE verdict, or a failed triangle test, withholds it --
 * the two cases where there is real evidence that opening early is wrong.
 *
 * This gates prediction ONLY. The threshold unlock, which means someone is
 * actually standing at the door, is never gated on geometry: a mis-tuned floor
 * that refuses to open a door locks a human out of their house, and that
 * asymmetry is the same one that argues for the per-link STS gate.
 */
bool ultrawidelock_fusion_may_predict(const struct ultrawidelock_fusion_verdict *v);

#endif /* ULTRAWIDELOCK_FUSION_H */
