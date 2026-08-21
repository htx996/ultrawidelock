/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_satellite.c — freshness gate around a second anchor's report.
 */

#include "ultrawidelock_satellite.h"

void ultrawidelock_satellite_init(struct ultrawidelock_satellite *s,
				  const struct ultrawidelock_fusion_cfg *cfg, uint32_t stale_ms,
				  bool self_is_inside)
{
	if (s == NULL) {
		return;
	}
	s->cfg.baseline_mm = 0;
	s->cfg.tol_mm = 0;
	s->cfg.deadband_mm = 0;
	if (cfg != NULL) {
		s->cfg = *cfg;
	}
	s->last_ms = 0;
	s->peer_mm = 0;
	s->stale_ms = stale_ms != 0u ? stale_ms : ULTRAWIDELOCK_SATELLITE_STALE_MS_DEFAULT;
	s->self_is_inside = self_is_inside;
	s->have = false;
}

void ultrawidelock_satellite_report(struct ultrawidelock_satellite *s, int32_t peer_mm,
				    uint32_t peer_block, int64_t now_ms)
{
	if (s == NULL || peer_mm < 0) {
		return;
	}
	s->peer_mm = peer_mm;
	s->peer_block = peer_block;
	s->last_ms = now_ms;
	s->have = true;
}

static bool fresh(const struct ultrawidelock_satellite *s, int64_t now_ms)
{
	int64_t age;

	if (s == NULL || !s->have) {
		return false;
	}
	/*
	 * An unconfigured baseline is absence, not evidence. Without this check
	 * it reads as evidence: ultrawidelock_fusion_eval() rejects a zero baseline, every
	 * pair then fails the triangle test, and a board that was merely
	 * misconfigured silently stops predicting for ever -- with a verdict that
	 * looks exactly like a detected attack. Fail back to "no satellite".
	 */
	if (s->cfg.baseline_mm <= 0) {
		return false;
	}
	age = now_ms - s->last_ms;
	/*
	 * A report from the future is a clock that moved, not a fast satellite.
	 * Treat it as unusable rather than eternally fresh: a backwards jump on
	 * this node would otherwise pin one stale distance as valid for as long
	 * as the offset lasts.
	 */
	if (age < 0) {
		return false;
	}
	return age <= (int64_t)s->stale_ms;
}

struct ultrawidelock_fusion_verdict
ultrawidelock_satellite_verdict(const struct ultrawidelock_satellite *s, int32_t self_mm,
				uint32_t self_block, int64_t now_ms)
{
	struct ultrawidelock_fusion_verdict none = {ULTRAWIDELOCK_SIDE_UNKNOWN, false, 0};

	if (!fresh(s, now_ms) || self_mm < 0) {
		return none;
	}
	/*
	 * ultrawidelock_fusion.h states the two distances MUST come from the same
	 * ranging round, and until now nothing could hold a caller to it: the only
	 * guard was stale_ms, which at 1500 ms against a 192 ms block admits pairs
	 * almost eight blocks apart. A moving phone travels a long way in eight
	 * blocks, so that window was wide enough to manufacture a geometrically
	 * impossible pair out of two perfectly good measurements.
	 *
	 * Block equality settles it exactly. Both anchors read the same block index
	 * off the initiator's own frames, so this is an integer comparison rather
	 * than an estimate, and a pair that passes is same-round by construction.
	 * stale_ms stays as a backstop against a stalled link, not as the primary
	 * guard it was never strong enough to be.
	 */
	if (s->peer_block != self_block) {
		return none;
	}
	/*
	 * ultrawidelock_fusion_eval() takes (inside, outside) in that order and the sign of
	 * the difference is the whole answer, so this swap is the entire
	 * consequence of where the boards are screwed.
	 */
	if (s->self_is_inside) {
		return ultrawidelock_fusion_eval(&s->cfg, self_mm, s->peer_mm);
	}
	return ultrawidelock_fusion_eval(&s->cfg, s->peer_mm, self_mm);
}

bool ultrawidelock_satellite_may_predict(const struct ultrawidelock_satellite *s, int32_t self_mm,
					 uint32_t self_block, int64_t now_ms)
{
	struct ultrawidelock_fusion_verdict v;

	/* No satellite at all, or nothing fresh: today's behaviour, unchanged. */
	if (!fresh(s, now_ms) || self_mm < 0) {
		return true;
	}
	/*
	 * Tested HERE and not inferred from the verdict, because a mismatch and a
	 * failed triangle both surface as {UNKNOWN, geometry_ok=false} and they
	 * must not share a fate. A block mismatch is not a pair at all, so it is
	 * evidence for nothing and degrades to single-anchor behaviour. A failed
	 * triangle IS a pair, one no phone position could have produced, and the
	 * header owes a false to exactly that case.
	 */
	if (s->peer_block != self_block) {
		return true;
	}
	v = ultrawidelock_satellite_verdict(s, self_mm, self_block, now_ms);
	return ultrawidelock_fusion_may_predict(&v);
}
