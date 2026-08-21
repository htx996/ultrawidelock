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
	s->peer_block = 0u;
	s->ring_next = 0u;
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_RING; i++) {
		s->ring[i].have = false;
		s->ring[i].block = 0u;
		s->ring[i].mm = 0;
		s->ring[i].ms = 0;
	}
}

void ultrawidelock_satellite_observe(struct ultrawidelock_satellite *s, int32_t self_mm,
				     uint32_t self_block, int64_t now_ms)
{
	if (s == NULL || self_mm < 0) {
		return;
	}
	s->ring[s->ring_next].block = self_block;
	s->ring[s->ring_next].mm = self_mm;
	s->ring[s->ring_next].ms = now_ms;
	s->ring[s->ring_next].have = true;
	s->ring_next = (uint8_t)((s->ring_next + 1u) % ULTRAWIDELOCK_SATELLITE_RING);
}

/**
 * Our own measurement for @p block, or NULL.
 *
 * Scans rather than indexes because block numbers are the initiator's and can
 * skip: a block we never latched leaves no entry, so position in the ring says
 * nothing about which block an entry holds.
 */
static const struct ultrawidelock_satellite_sample *
ring_find(const struct ultrawidelock_satellite *s, uint32_t block, int64_t now_ms)
{
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_RING; i++) {
		const struct ultrawidelock_satellite_sample *e = &s->ring[i];
		int64_t age;

		if (!e->have || e->block != block) {
			continue;
		}
		/* Our half must be fresh too. Without this a lock that stopped
		 * ranging keeps offering its last sample for ever, and a peer still
		 * reporting that same block would pair against a measurement that
		 * stopped describing the room some time ago. */
		age = now_ms - e->ms;
		if (age < 0 || age > (int64_t)s->stale_ms) {
			return NULL;
		}
		return e;
	}
	return NULL;
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
ultrawidelock_satellite_verdict(const struct ultrawidelock_satellite *s, int64_t now_ms)
{
	struct ultrawidelock_fusion_verdict none = {ULTRAWIDELOCK_SIDE_UNKNOWN, false, 0};
	const struct ultrawidelock_satellite_sample *mine;
	int32_t self_mm;

	if (!fresh(s, now_ms)) {
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
	 * Block equality settles it exactly, and the REPORT chooses the block: a
	 * datagram that took two blocks to arrive is still perfectly good evidence
	 * about the block it describes, so we look up what we measured then rather
	 * than discarding it for not being about now.
	 */
	mine = ring_find(s, s->peer_block, now_ms);
	if (mine == NULL) {
		return none;
	}
	self_mm = mine->mm;
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

int32_t ultrawidelock_satellite_peer_mm(const struct ultrawidelock_satellite *s, int64_t now_ms)
{
	/* Same freshness test the verdict applies, so a caller cannot see a
	 * distance the verdict has already discarded as too old. */
	if (!fresh(s, now_ms)) {
		return -1;
	}
	return s->peer_mm;
}

bool ultrawidelock_satellite_may_predict(const struct ultrawidelock_satellite *s, int64_t now_ms)
{
	struct ultrawidelock_fusion_verdict v;

	/* No satellite at all, or nothing fresh: today's behaviour, unchanged. */
	if (!fresh(s, now_ms)) {
		return true;
	}
	/*
	 * Tested HERE and not inferred from the verdict, because "no pair" and a
	 * failed triangle both surface as {UNKNOWN, geometry_ok=false} and they
	 * must not share a fate. Having nothing of ours for the reported block is
	 * evidence for nothing, and degrades to single-anchor behaviour. A failed
	 * triangle IS a pair, one no phone position could have produced, and the
	 * header owes a false to exactly that case.
	 */
	if (ring_find(s, s->peer_block, now_ms) == NULL) {
		return true;
	}
	v = ultrawidelock_satellite_verdict(s, now_ms);
	return ultrawidelock_fusion_may_predict(&v);
}
