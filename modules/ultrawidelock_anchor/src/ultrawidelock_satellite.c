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

/**
 * Has a report arrived, and is it recent enough to still describe the room?
 *
 * The baseline is deliberately NOT consulted. This is what calibration needs:
 * the baseline is calibration's OUTPUT, so a gate that hid the readings until
 * one existed would make the number unmeasurable from the readings that
 * determine it. Every judgement about geometry goes through fresh() below.
 */
static bool report_fresh(const struct ultrawidelock_satellite *s, int64_t now_ms)
{
	int64_t age;

	if (s == NULL || !s->have) {
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

static bool fresh(const struct ultrawidelock_satellite *s, int64_t now_ms)
{
	if (s == NULL) {
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
	return report_fresh(s, now_ms);
}

bool ultrawidelock_satellite_pair(const struct ultrawidelock_satellite *s, int64_t now_ms,
				  int32_t *self_mm, int32_t *peer_mm, uint32_t *block)
{
	const struct ultrawidelock_satellite_sample *mine;

	if (s == NULL || self_mm == NULL || peer_mm == NULL || block == NULL) {
		return false;
	}
	if (!report_fresh(s, now_ms)) {
		return false;
	}
	/* The SAME exact-block rule the verdict uses. Calibration skips the
	 * baseline, never the pairing: a difference taken across two rounds is
	 * a difference of two phone positions, and averaging those would size
	 * the geometry from a walk rather than from a stand. */
	mine = ring_find(s, s->peer_block, now_ms);
	if (mine == NULL) {
		return false;
	}
	*self_mm = mine->mm;
	*peer_mm = s->peer_mm;
	*block = s->peer_block;
	return true;
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

bool ultrawidelock_satellite_may_passive_unlock(const struct ultrawidelock_satellite *s,
						int64_t now_ms)
{
	struct ultrawidelock_fusion_verdict v;

	/* The same two degradations may_predict() makes, for the same reasons:
	 * no satellite and no pair for the reported block are both absence, and
	 * absence returns the door to single-anchor behaviour. */
	if (!fresh(s, now_ms)) {
		return true;
	}
	if (ring_find(s, s->peer_block, now_ms) == NULL) {
		return true;
	}
	v = ultrawidelock_satellite_verdict(s, now_ms);
	return ultrawidelock_fusion_may_passive_unlock(&v);
}

/* ── more than one satellite ─────────────────────────────────────────────── */

void ultrawidelock_satellite_set_init(struct ultrawidelock_satellite_set *set,
				      const struct ultrawidelock_fusion_cfg *cfg,
				      uint32_t stale_ms, bool self_is_inside)
{
	if (set == NULL) {
		return;
	}
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_MAX_ROLES; i++) {
		/* A NULL cfg initialises every slot with a zero baseline, which
		 * fresh() already reads as absence rather than as evidence --
		 * the same fail-back an unconfigured single satellite gets. */
		ultrawidelock_satellite_init(&set->peer[i], cfg != NULL ? &cfg[i] : NULL, stale_ms,
					     self_is_inside);
	}
}

void ultrawidelock_satellite_set_report(struct ultrawidelock_satellite_set *set, uint8_t role,
					int32_t peer_mm, uint32_t peer_block, int64_t now_ms)
{
	if (set == NULL || role < 1u || role > ULTRAWIDELOCK_SATELLITE_MAX_ROLES) {
		return;
	}
	ultrawidelock_satellite_report(&set->peer[role - 1u], peer_mm, peer_block, now_ms);
}

void ultrawidelock_satellite_set_observe(struct ultrawidelock_satellite_set *set, int32_t self_mm,
					 uint32_t self_block, int64_t now_ms)
{
	if (set == NULL) {
		return;
	}
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_MAX_ROLES; i++) {
		ultrawidelock_satellite_observe(&set->peer[i], self_mm, self_block, now_ms);
	}
}

struct ultrawidelock_fusion_verdict
ultrawidelock_satellite_set_verdict(const struct ultrawidelock_satellite_set *set, int64_t now_ms)
{
	struct ultrawidelock_fusion_verdict none = {ULTRAWIDELOCK_SIDE_UNKNOWN, false, 0};
	struct ultrawidelock_fusion_verdict definite = {ULTRAWIDELOCK_SIDE_UNKNOWN, false, 0};
	struct ultrawidelock_fusion_verdict abstain = {ULTRAWIDELOCK_SIDE_UNKNOWN, false, 0};
	bool have_definite = false;
	bool have_abstain = false;

	if (set == NULL) {
		return none;
	}
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_MAX_ROLES; i++) {
		struct ultrawidelock_fusion_verdict v =
			ultrawidelock_satellite_verdict(&set->peer[i], now_ms);

		/*
		 * Silent roles are skipped, not counted as a dissenting UNKNOWN.
		 * An uninstalled role and a quiet one are both absence, and
		 * absence that could veto would make adding a third slot change
		 * the answer for a door that was working.
		 */
		if (!v.geometry_ok) {
			continue;
		}
		/*
		 * A role INSIDE its dead band abstains rather than dissents. It
		 * has a good pair and no opinion, which is the ordinary state of
		 * an anchor the phone happens to be on the bisector of -- and
		 * two anchors do not share a bisector, so with a second
		 * satellite installed this is a normal reading, not a fault.
		 * Counting it as a disagreement would make two satellites refuse
		 * where one would have decided.
		 */
		if (v.side == ULTRAWIDELOCK_SIDE_UNKNOWN) {
			if (!have_abstain) {
				abstain = v;
				have_abstain = true;
			}
			continue;
		}
		if (!have_definite) {
			definite = v;
			have_definite = true;
			continue;
		}
		if (v.side != definite.side) {
			/*
			 * Two anchors watching one phone cannot both be right
			 * about opposite sides. Fail closed, in the shape a
			 * failed triangle already uses: UNKNOWN with geometry_ok
			 * false, which the side gate refuses on.
			 */
			return none;
		}
	}
	if (have_definite) {
		return definite;
	}
	/* Every speaking role abstained: report the abstention rather than
	 * absence, because {UNKNOWN, geometry_ok = true} is what a single
	 * satellite in its dead band returns today and the callers separate
	 * those two states. */
	return have_abstain ? abstain : none;
}

bool ultrawidelock_satellite_set_may_predict(const struct ultrawidelock_satellite_set *set,
					     int64_t now_ms)
{
	if (set == NULL) {
		return true;
	}
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_MAX_ROLES; i++) {
		if (!ultrawidelock_satellite_may_predict(&set->peer[i], now_ms)) {
			return false;
		}
	}
	return true;
}

bool ultrawidelock_satellite_set_may_passive_unlock(const struct ultrawidelock_satellite_set *set,
						    int64_t now_ms)
{
	if (set == NULL) {
		return true;
	}
	/* Any role that withholds withholds for the whole set. A satellite that
	 * can see the phone indoors is not outvoted by two that cannot see it at
	 * all -- silence is absence, and absence never outweighs evidence. */
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_MAX_ROLES; i++) {
		if (!ultrawidelock_satellite_may_passive_unlock(&set->peer[i], now_ms)) {
			return false;
		}
	}
	return true;
}

int32_t ultrawidelock_satellite_set_peer_mm(const struct ultrawidelock_satellite_set *set,
					    int64_t now_ms)
{
	if (set == NULL) {
		return -1;
	}
	for (uint8_t i = 0u; i < ULTRAWIDELOCK_SATELLITE_MAX_ROLES; i++) {
		int32_t mm = ultrawidelock_satellite_peer_mm(&set->peer[i], now_ms);

		if (mm >= 0) {
			return mm;
		}
	}
	return -1;
}
