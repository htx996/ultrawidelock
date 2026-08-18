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
	 * 3 dB. Stationary advertisers wander by a couple of dB window to
	 * window from multipath alone; below this a change is not motion.
	 */
	cfg->rssi_eps_db = 3;
	/* 300 mm. Below this the phone has not meaningfully moved. */
	cfg->range_eps_mm = 300;
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
		/* Evict the weakest: the label least like an approach. */
		if (victim == NULL || p->cand[i].score < victim->score) {
			victim = &p->cand[i];
		}
	}
	if (victim == NULL) {
		return NULL;
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
		 * signature of the advertiser that is moving with the phone. */
		if ((d_rssi > 0 && d_range < 0) || (d_rssi < 0 && d_range > 0)) {
			if (c->score < SCORE_MAX) {
				c->score++;
			}
		} else if (c->score > SCORE_MIN) {
			c->score--;
		}
	}
}

bool ultrawidelock_witness_pick_best(const struct ultrawidelock_witness_pick *p, uint32_t *hash24)
{
	const struct ultrawidelock_witness_pick_cand *best = NULL;
	int32_t runner = SCORE_MIN;

	if (p == NULL) {
		return false;
	}
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_PICK_MAX; i++) {
		const struct ultrawidelock_witness_pick_cand *c = &p->cand[i];

		if (!c->used) {
			continue;
		}
		if (best == NULL || c->score > best->score) {
			if (best != NULL) {
				runner = best->score;
			}
			best = c;
		} else if ((int32_t)c->score > runner) {
			runner = c->score;
		}
	}
	if (best == NULL) {
		return false;
	}
	if (best->score < p->cfg.min_score || best->windows < p->cfg.min_windows) {
		return false;
	}
	if ((int32_t)best->score - runner < (int32_t)p->cfg.min_margin) {
		return false;
	}
	if (hash24 != NULL) {
		*hash24 = best->hash24;
	}
	return true;
}
