/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_witness_pick.c — trajectory correlation (implementation).
 */

#include "ultrawidelock_witness_pick.h"

#include <string.h>

#define SCORE_MIN (-32)
#define SCORE_MAX 32

void ultrawidelock_witness_pick_defaults(struct ultrawidelock_witness_pick_cfg *cfg)
{
	if (cfg == NULL) {
		return;
	}
	memset(cfg, 0, sizeof(*cfg));
	/*
	 * Three agreeing steps net of any disagreement. Two would be reachable
	 * by chance from a pair of coin flips; three is not, and the walk-up
	 * that the latch already requires supplies more than three windows.
	 */
	cfg->min_score = 3;
	cfg->min_windows = 3u;
	/*
	 * Two clear of the runner-up. A single shared step between the phone
	 * and some other advertiser that happened to move must not decide it.
	 */
	cfg->min_margin = 2;
	/*
	 * 3 dB. Not a noise fence -- stationary advertisers wander past it, and
	 * UWB range to a standing phone wobbles past 300 mm (both measured
	 * 2026-08-20). Raising the floors to exclude that noise was tried and
	 * silenced the walk itself: far out a 2 s window moves RSSI only
	 * 3-4 dB, close in it moves range under 700 mm, so a 5 dB / 700 mm
	 * floor left nothing that could score. The floors only have to make a
	 * window's SIGN meaningful; separating phone from noise is the
	 * asymmetric penalty's job (see the feed).
	 */
	cfg->rssi_eps_db = 3;
	/* 300 mm. Same reasoning. */
	cfg->range_eps_mm = 300;
	/*
	 * 6 dB. Wide enough to hold one handset's concurrent advertising sets
	 * together, narrow enough that two people arriving at the door a metre
	 * apart still read as two. See the header.
	 */
	cfg->cluster_db = 6;
}

void ultrawidelock_witness_pick_init(struct ultrawidelock_witness_pick *p,
				     const struct ultrawidelock_witness_pick_cfg *cfg)
{
	if (p == NULL) {
		return;
	}
	memset(p, 0, sizeof(*p));
	if (cfg != NULL) {
		p->cfg = *cfg;
	} else {
		ultrawidelock_witness_pick_defaults(&p->cfg);
	}
}

void ultrawidelock_witness_pick_reset(struct ultrawidelock_witness_pick *p)
{
	if (p == NULL) {
		return;
	}
	memset(p->cand, 0, sizeof(p->cand));
	p->evictions = 0u;
}

static struct ultrawidelock_witness_pick_cand *cand_get(struct ultrawidelock_witness_pick *p,
							uint32_t hash24)
{
	struct ultrawidelock_witness_pick_cand *victim = NULL;

	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_PICK_MAX; i++) {
		if (p->cand[i].used && p->cand[i].hash24 == hash24) {
			return &p->cand[i];
		}
	}
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_PICK_MAX; i++) {
		if (!p->cand[i].used) {
			victim = &p->cand[i];
			break;
		}
		/* Evict the label least like an approach: lowest score first,
		 * and on a score tie the LONGEST-seen. Stationary advertisers
		 * never score, so score 0 across many windows is proof of a
		 * label that is not the phone -- while the newest zero may be
		 * the phone just entering range. Evicting by score alone tied
		 * everyone at 0 and cycled the whole table each report in a
		 * room with more advertisers than slots (measured 2026-08-20:
		 * every candidate stuck at windows=1, ~3 evictions per report,
		 * no pick ever), which starved the scoring it exists to feed. */
		if (victim == NULL || p->cand[i].score < victim->score ||
		    (p->cand[i].score == victim->score &&
		     p->cand[i].windows > victim->windows)) {
			victim = &p->cand[i];
		}
	}
	if (victim == NULL) {
		return NULL;
	}
	if (victim->used) {
		p->evictions++;
	}
	memset(victim, 0, sizeof(*victim));
	victim->used = true;
	victim->hash24 = hash24;
	return victim;
}

void ultrawidelock_witness_pick_feed(struct ultrawidelock_witness_pick *p,
				     const struct ultrawidelock_witness_msg *msg, int32_t range_mm)
{
	if (p == NULL || msg == NULL || range_mm < 0) {
		return;
	}
	if (msg->role != ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE) {
		return;
	}

	for (uint8_t i = 0; i < ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES; i++) {
		const struct ultrawidelock_witness_tuple *t =
			ultrawidelock_witness_msg_at(msg, i);
		struct ultrawidelock_witness_pick_cand *c;
		int32_t d_rssi, d_range;

		if (t == NULL) {
			continue;
		}
		c = cand_get(p, t->hash24);
		if (c == NULL) {
			continue;
		}
		if (c->windows < 0xFFu) {
			c->windows++;
		}
		if (!c->have_prev) {
			c->have_prev = true;
			c->last_dbm = t->mean_dbm;
			c->last_range_mm = range_mm;
			continue;
		}

		d_rssi = (int32_t)t->mean_dbm - (int32_t)c->last_dbm;
		d_range = range_mm - c->last_range_mm;
		c->last_dbm = t->mean_dbm;
		c->last_range_mm = range_mm;

		/* Below either noise floor the window says nothing about
		 * motion. Counted, not scored -- scoring it either way would
		 * turn multipath into evidence. */
		if (d_rssi < 0) {
			if (-d_rssi < p->cfg.rssi_eps_db) {
				continue;
			}
		} else if (d_rssi < p->cfg.rssi_eps_db) {
			continue;
		}
		if (d_range < 0) {
			if (-d_range < p->cfg.range_eps_mm) {
				continue;
			}
		} else if (d_range < p->cfg.range_eps_mm) {
			continue;
		}

		/* Closing range with rising RSSI, or opening with falling: the
		 * signature of the advertiser that is moving with the phone.
		 *
		 * Disagreement costs double. The phone's RSSI tracks the range
		 * by construction, so it almost never lands here; a noise label
		 * clears both floors with a random sign, agreeing half the
		 * time. At +1/-1 that is a driftless random walk, and measured
		 * 2026-08-20 bystanders walked to score 3 and erased the
		 * winner's margin. At +1/-2 noise drifts at -0.5 per scored
		 * window and sinks, while a real approach still climbs. */
		if ((d_rssi > 0 && d_range < 0) || (d_rssi < 0 && d_range > 0)) {
			if (c->score < SCORE_MAX) {
				c->score++;
			}
		} else {
			c->score = (c->score - 2 < SCORE_MIN) ? SCORE_MIN
							      : (int8_t)(c->score - 2);
		}
	}
}

void ultrawidelock_witness_pick_stats(const struct ultrawidelock_witness_pick *p,
				      struct ultrawidelock_witness_pick_stats *st)
{
	const struct ultrawidelock_witness_pick_cand *best = NULL;
	int32_t runner = SCORE_MIN;
	uint8_t n = 0u;

	if (st == NULL) {
		return;
	}
	memset(st, 0, sizeof(*st));
	st->runner_score = SCORE_MIN;
	if (p == NULL) {
		return;
	}
	st->evictions = p->evictions;
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_PICK_MAX; i++) {
		const struct ultrawidelock_witness_pick_cand *c = &p->cand[i];

		if (!c->used) {
			continue;
		}
		n++;
		if (best == NULL || c->score > best->score) {
			best = c;
		}
	}
	st->n_cand = n;
	if (best == NULL) {
		return;
	}
	st->have = true;
	st->best_hash24 = best->hash24;
	st->best_score = best->score;
	st->best_windows = best->windows;

	/* Runner-up, counting only labels that could be a DIFFERENT emitter.
	 * One handset's concurrent advertising sets correlate just as well as
	 * its main one and sit at nearly the same level; treating those as
	 * competition would refuse every pick. See cluster_db. */
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_PICK_MAX; i++) {
		const struct ultrawidelock_witness_pick_cand *c = &p->cand[i];
		int32_t d;

		if (!c->used || c == best) {
			continue;
		}
		d = (int32_t)c->last_dbm - (int32_t)best->last_dbm;
		if (d < 0) {
			d = -d;
		}
		if (c->score >= p->cfg.min_score && d <= (int32_t)p->cfg.cluster_db) {
			continue; /* same emitter as best, not a rival */
		}
		if ((int32_t)c->score > runner) {
			runner = c->score;
			st->runner_gap_db = (int16_t)d;
		}
	}
	st->runner_score = (int8_t)runner;
}

void ultrawidelock_witness_pick_retire(struct ultrawidelock_witness_pick *p, uint32_t hash24)
{
	if (p == NULL) {
		return;
	}
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_PICK_MAX; i++) {
		if (p->cand[i].used && p->cand[i].hash24 == hash24) {
			memset(&p->cand[i], 0, sizeof(p->cand[i]));
			return;
		}
	}
}

uint32_t ultrawidelock_witness_pick_succeed(struct ultrawidelock_witness_pick *p,
					    uint32_t old_hash24,
					    const struct ultrawidelock_witness_msg *msg)
{
	struct ultrawidelock_witness_pick_cand *old = NULL;
	struct ultrawidelock_witness_pick_cand *succ = NULL;
	int32_t succ_d = 0;

	if (p == NULL || msg == NULL) {
		return 0u;
	}
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_PICK_MAX; i++) {
		if (p->cand[i].used && p->cand[i].hash24 == old_hash24) {
			old = &p->cand[i];
			break;
		}
	}
	if (old == NULL) {
		return 0u;
	}
	for (uint8_t i = 0; i < ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES; i++) {
		const struct ultrawidelock_witness_tuple *t =
			ultrawidelock_witness_msg_at(msg, i);
		struct ultrawidelock_witness_pick_cand *c = NULL;
		int32_t d;

		if (t == NULL || t->hash24 == old_hash24) {
			continue;
		}
		for (size_t j = 0; j < ULTRAWIDELOCK_WITNESS_PICK_MAX; j++) {
			if (p->cand[j].used && p->cand[j].hash24 == t->hash24) {
				c = &p->cand[j];
				break;
			}
		}
		/* The caller retires after three both-absent publishes, so a
		 * genuine successor has been heard for three or four windows.
		 * Anything older coexisted with the old label and is a
		 * different device, whatever its level. */
		if (c == NULL || c->windows > 6u) {
			continue;
		}
		d = (int32_t)t->mean_dbm - (int32_t)old->last_dbm;
		if (d < 0) {
			d = -d;
		}
		if (d > (int32_t)p->cfg.cluster_db) {
			continue;
		}
		if (succ == NULL || d < succ_d) {
			succ = c;
			succ_d = d;
		}
	}
	if (succ == NULL) {
		return 0u;
	}
	/* Same radio, new name: the score and the observation history follow
	 * the emitter. Its own last_dbm/last_range tracking stays -- that
	 * continuity is real and fresher than the ghost's. */
	succ->score = old->score;
	succ->windows = old->windows;
	memset(old, 0, sizeof(*old));
	return succ->hash24;
}

bool ultrawidelock_witness_pick_best(const struct ultrawidelock_witness_pick *p, uint32_t *hash24)
{
	struct ultrawidelock_witness_pick_stats st;

	if (p == NULL) {
		return false;
	}
	ultrawidelock_witness_pick_stats(p, &st);
	if (!st.have) {
		return false;
	}
	if (st.best_score < p->cfg.min_score || st.best_windows < p->cfg.min_windows) {
		return false;
	}
	if ((int32_t)st.best_score - (int32_t)st.runner_score < (int32_t)p->cfg.min_margin) {
		return false;
	}
	if (hash24 != NULL) {
		*hash24 = st.best_hash24;
	}
	return true;
}
