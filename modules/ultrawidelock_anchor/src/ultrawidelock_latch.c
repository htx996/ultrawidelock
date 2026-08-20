/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_latch.c — the inside veto (implementation).
 */

#include "ultrawidelock_latch.h"
#include "ultrawidelock_side_log.h"

#include <string.h>

#define LATCH_BLOB_VER 1u

void ultrawidelock_latch_defaults(struct ultrawidelock_latch_cfg *cfg)
{
	if (cfg == NULL) {
		return;
	}
	memset(cfg, 0, sizeof(*cfg));
	/*
	 * 60 s covers the walk from the door to wherever the phone lands. It
	 * only has to outlast the crossing itself: during it the phone is at
	 * the plane, and MEASURED 2026-08-11 that is where the differential is
	 * in its dead band roughly half the time.
	 */
	cfg->entry_dwell_ms = 60000u;
	/*
	 * Three, matching ultrawidelock_side_cfg::agree_windows, so a clear
	 * needs the same weight of agreement the side filter needed to commit.
	 * The two run in series, not in parallel: this counts decisions the
	 * side filter had already committed.
	 */
	cfg->clear_windows = 3u;
	/*
	 * 3 m. Far enough out that the inside and outside witnesses are not
	 * close to equidistant, so the sign of the differential means what it
	 * says. UNMEASURED as a distance: it is set from the geometry argument
	 * in ultrawidelock_fusion.h, not from a capture, and stage P8 owes it a
	 * number from real walk-ups.
	 */
	cfg->clear_min_mm = 3000;
	/*
	 * 10 s from the last agreeing window. A normal walk-up from 3 m to the
	 * door is a few seconds, so this covers it with margin while keeping
	 * the proven approach clearly bounded -- long enough to cross the dead
	 * band, far too short to still be valid next time anyone walks past.
	 */
	cfg->clear_valid_ms = 10000u;
	cfg->opportunity_valid_ms = 0u; /* never expires; see the header */
}

void ultrawidelock_latch_init(struct ultrawidelock_latch *l,
			      const struct ultrawidelock_latch_cfg *cfg)
{
	if (l == NULL) {
		return;
	}
	memset(l, 0, sizeof(*l));
	if (cfg != NULL) {
		l->cfg = *cfg;
	} else {
		ultrawidelock_latch_defaults(&l->cfg);
	}
}

static struct ultrawidelock_latch_rec *rec_find(struct ultrawidelock_latch *l, uint32_t cred_id)
{
	for (size_t i = 0; i < ULTRAWIDELOCK_LATCH_MAX_CREDS; i++) {
		if ((l->rec[i].flags & ULTRAWIDELOCK_LATCH_F_USED) != 0u &&
		    l->rec[i].cred_id == cred_id) {
			return &l->rec[i];
		}
	}
	return NULL;
}

static const struct ultrawidelock_latch_rec *rec_find_const(const struct ultrawidelock_latch *l,
							    uint32_t cred_id)
{
	return rec_find((struct ultrawidelock_latch *)l, cred_id);
}

/* Free slot, or the credential confirmed inside longest ago. Evicting the
 * stalest record costs that credential one deliberate unlock; evicting the
 * freshest would drop the one most likely to be mid-approach. */
static struct ultrawidelock_latch_rec *rec_alloc(struct ultrawidelock_latch *l, uint32_t cred_id,
						 int64_t now_ms)
{
	struct ultrawidelock_latch_rec *victim = NULL;

	for (size_t i = 0; i < ULTRAWIDELOCK_LATCH_MAX_CREDS; i++) {
		if ((l->rec[i].flags & ULTRAWIDELOCK_LATCH_F_USED) == 0u) {
			victim = &l->rec[i];
			break;
		}
		if (victim == NULL ||
		    l->rec[i].confirmed_inside_ms < victim->confirmed_inside_ms) {
			victim = &l->rec[i];
		}
	}
	if (victim == NULL) {
		return NULL;
	}
	memset(victim, 0, sizeof(*victim));
	victim->cred_id = cred_id;
	victim->confirmed_inside_ms = now_ms;
	victim->flags = ULTRAWIDELOCK_LATCH_F_USED;
	return victim;
}

void ultrawidelock_latch_session_open(struct ultrawidelock_latch *l, uint32_t cred_id)
{
	if (l == NULL) {
		return;
	}
	l->active = true;
	l->active_cred = cred_id;
	l->agree_n = 0u;
	l->last_outside_ms = 0;
}

void ultrawidelock_latch_session_close(struct ultrawidelock_latch *l)
{
	if (l == NULL) {
		return;
	}
	l->active = false;
	l->active_cred = 0u;
	l->agree_n = 0u;
	l->last_outside_ms = 0;
}

void ultrawidelock_latch_note_grant(struct ultrawidelock_latch *l, uint32_t cred_id,
				    int64_t now_ms)
{
	struct ultrawidelock_latch_rec *r;

	if (l == NULL || cred_id == ULTRAWIDELOCK_LATCH_CRED_ANY) {
		return;
	}
	r = rec_find(l, cred_id);
	if (r == NULL) {
		r = rec_alloc(l, cred_id, now_ms);
		if (r == NULL) {
			return;
		}
	}
	r->confirmed_inside_ms = now_ms;

	/*
	 * The door opened, and the lock cannot tell an entry from an exit: the
	 * same tap unlocks it whether the holder is coming in or going out. So
	 * this credential is asserted INSIDE *and* granted the opportunity,
	 * and the two are not in tension -- the dwell is what separates them.
	 * For entry_dwell_ms after the grant, R_DWELL vetoes regardless; after
	 * it, the opportunity stands and the evidence decides.
	 *
	 * It used to clear the flag here, on the reasoning that a credential
	 * being asserted inside cannot also have crossed. That is true at the
	 * instant of the grant and false a minute later, and it had a
	 * consequence the tests never caught because they set the flag by hand:
	 * with a single enrolled credential NOTHING set it, so passive unlock
	 * was refused forever with R_NO_OPPORTUNITY. A latch that never opens
	 * is not a safe latch, it is an unmeasurable one -- it withholds
	 * correctly indoors for a reason that has nothing to do with where the
	 * phone is.
	 *
	 * What this gives up is stated: for one credential the opportunity is
	 * no longer independent evidence, and the whole discrimination rests on
	 * the dwell plus the N agreeing OUTSIDE windows with a UWB range that
	 * started beyond clear_min_mm. That is safety-table row 10, which was
	 * already the residual whenever a second credential opened the door.
	 */
	r->opportunity_ms = now_ms;
	r->flags = ULTRAWIDELOCK_LATCH_F_USED | ULTRAWIDELOCK_LATCH_F_OPPORTUNITY;

	/* Every OTHER credential gained a chance to cross the same door. They
	 * get no confirmed_inside_ms refresh, so no dwell applies to them. */
	for (size_t i = 0; i < ULTRAWIDELOCK_LATCH_MAX_CREDS; i++) {
		struct ultrawidelock_latch_rec *o = &l->rec[i];

		if (o == r || (o->flags & ULTRAWIDELOCK_LATCH_F_USED) == 0u) {
			continue;
		}
		o->flags |= ULTRAWIDELOCK_LATCH_F_OPPORTUNITY;
		o->opportunity_ms = now_ms;
	}

	l->agree_n = 0u;
	l->last_outside_ms = 0;
}

void ultrawidelock_latch_note_opportunity(struct ultrawidelock_latch *l, uint32_t cred_id,
					  int64_t now_ms)
{
	if (l == NULL) {
		return;
	}
	if (cred_id == ULTRAWIDELOCK_LATCH_CRED_ANY) {
		for (size_t i = 0; i < ULTRAWIDELOCK_LATCH_MAX_CREDS; i++) {
			if ((l->rec[i].flags & ULTRAWIDELOCK_LATCH_F_USED) == 0u) {
				continue;
			}
			l->rec[i].flags |= ULTRAWIDELOCK_LATCH_F_OPPORTUNITY;
			l->rec[i].opportunity_ms = now_ms;
		}
		return;
	}
	struct ultrawidelock_latch_rec *r = rec_find(l, cred_id);

	if (r == NULL) {
		r = rec_alloc(l, cred_id, now_ms);
		if (r == NULL) {
			return;
		}
	}
	r->flags |= ULTRAWIDELOCK_LATCH_F_OPPORTUNITY;
	r->opportunity_ms = now_ms;
}

void ultrawidelock_latch_note_window(struct ultrawidelock_latch *l, uint32_t cred_id,
				     enum ultrawidelock_side_label side, int32_t range_mm,
				     int64_t now_ms)
{
	if (l == NULL || !l->active || l->active_cred != cred_id) {
		return;
	}
	if (side == ULTRAWIDELOCK_SIDE_LABEL_INSIDE) {
		/* A contradiction outweighs the agreements before it. */
		l->agree_n = 0u;
		l->last_outside_ms = 0;
		return;
	}
	if (side != ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE) {
		return; /* UNKNOWN / THRESHOLD: neither helps nor harms */
	}
	if (l->agree_n == 0u) {
		/* A run must START far out, where the sign of the differential
		 * is trustworthy. An unknown range cannot start one. */
		if (range_mm < 0 || range_mm < l->cfg.clear_min_mm) {
			return;
		}
	}
	if (l->agree_n < 0xFFu) {
		l->agree_n++;
	}
	l->last_outside_ms = now_ms;
}

bool ultrawidelock_latch_may_passive_unlock(const struct ultrawidelock_latch *l, uint32_t cred_id,
					    int64_t now_ms, uint8_t *reason)
{
	const struct ultrawidelock_latch_rec *r;
	uint8_t why = 0u;

	if (l == NULL) {
		if (reason != NULL) {
			*reason = ULTRAWIDELOCK_LATCH_R_NO_SESSION;
		}
		return false;
	}
	if (!l->active || l->active_cred != cred_id ||
	    cred_id == ULTRAWIDELOCK_LATCH_CRED_ANY) {
		why |= ULTRAWIDELOCK_LATCH_R_NO_SESSION;
	}

	r = rec_find_const(l, cred_id);
	if (r == NULL) {
		/* Never seen, or evicted. Fails closed: one deliberate unlock
		 * creates the record and normal behaviour returns. */
		why |= ULTRAWIDELOCK_LATCH_R_NO_RECORD;
	} else {
		bool have_opp = (r->flags & ULTRAWIDELOCK_LATCH_F_OPPORTUNITY) != 0u;

		if (have_opp && l->cfg.opportunity_valid_ms != 0u &&
		    (now_ms - r->opportunity_ms) > (int64_t)l->cfg.opportunity_valid_ms) {
			have_opp = false;
		}
		if (!have_opp) {
			why |= ULTRAWIDELOCK_LATCH_R_NO_OPPORTUNITY;
		}
		if ((now_ms - r->confirmed_inside_ms) < (int64_t)l->cfg.entry_dwell_ms) {
			why |= ULTRAWIDELOCK_LATCH_R_DWELL;
		}
	}

	if (l->agree_n < l->cfg.clear_windows) {
		why |= ULTRAWIDELOCK_LATCH_R_WINDOWS;
	} else if (l->cfg.clear_valid_ms != 0u &&
		   (now_ms - l->last_outside_ms) > (int64_t)l->cfg.clear_valid_ms) {
		why |= ULTRAWIDELOCK_LATCH_R_STALE;
	}

	if (reason != NULL) {
		*reason = why;
	}
	return why == 0u;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
	       (uint32_t)p[3];
}

static void put_i64(uint8_t *p, int64_t v)
{
	uint64_t u = (uint64_t)v;

	put_u32(p, (uint32_t)(u >> 32));
	put_u32(p + 4, (uint32_t)u);
}

static int64_t get_i64(const uint8_t *p)
{
	uint64_t u = ((uint64_t)get_u32(p) << 32) | (uint64_t)get_u32(p + 4);

	return (int64_t)u;
}

size_t ultrawidelock_latch_serialize(const struct ultrawidelock_latch *l, uint8_t *buf, size_t cap)
{
	uint8_t *p;
	uint8_t n = 0u;
	size_t len;
	uint16_t crc;

	if (l == NULL || buf == NULL || cap < ULTRAWIDELOCK_LATCH_BLOB_LEN) {
		return 0;
	}
	p = buf;
	*p++ = LATCH_BLOB_VER;
	p++; /* count, back-filled */

	for (size_t i = 0; i < ULTRAWIDELOCK_LATCH_MAX_CREDS; i++) {
		const struct ultrawidelock_latch_rec *r = &l->rec[i];

		if ((r->flags & ULTRAWIDELOCK_LATCH_F_USED) == 0u) {
			continue;
		}
		put_u32(p, r->cred_id);
		p += 4;
		put_i64(p, r->confirmed_inside_ms);
		p += 8;
		put_i64(p, r->opportunity_ms);
		p += 8;
		*p++ = r->flags;
		n++;
	}
	buf[1] = n;
	len = (size_t)(p - buf);
	crc = ultrawidelock_side_log_crc16(buf, len);
	*p++ = (uint8_t)(crc >> 8);
	*p++ = (uint8_t)crc;
	return len + 2u;
}

bool ultrawidelock_latch_deserialize(struct ultrawidelock_latch *l,
				     const struct ultrawidelock_latch_cfg *cfg, const uint8_t *buf,
				     size_t len)
{
	uint8_t n;
	size_t need;
	uint16_t want, got;
	const uint8_t *p;

	ultrawidelock_latch_init(l, cfg);
	if (l == NULL || buf == NULL || len < 4u) {
		return false;
	}
	if (buf[0] != LATCH_BLOB_VER) {
		return false;
	}
	n = buf[1];
	if (n > ULTRAWIDELOCK_LATCH_MAX_CREDS) {
		return false;
	}
	need = 2u + (size_t)n * ULTRAWIDELOCK_LATCH_REC_LEN;
	if (len != need + 2u) {
		return false;
	}
	got = (uint16_t)(((uint16_t)buf[need] << 8) | buf[need + 1u]);
	want = ultrawidelock_side_log_crc16(buf, need);
	if (got != want) {
		return false;
	}

	p = buf + 2;
	for (uint8_t i = 0; i < n; i++) {
		struct ultrawidelock_latch_rec *r = &l->rec[i];

		r->cred_id = get_u32(p);
		p += 4;
		r->confirmed_inside_ms = get_i64(p);
		p += 8;
		r->opportunity_ms = get_i64(p);
		p += 8;
		/* Only flags this version defines survive; an unknown bit from a
		 * future writer must not silently become F_OPPORTUNITY here. */
		r->flags = (uint8_t)(*p++ & (ULTRAWIDELOCK_LATCH_F_USED |
					     ULTRAWIDELOCK_LATCH_F_OPPORTUNITY));
		r->flags |= ULTRAWIDELOCK_LATCH_F_USED;
	}
	return true;
}
