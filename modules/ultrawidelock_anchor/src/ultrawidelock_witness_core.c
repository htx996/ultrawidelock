/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_witness_core.c — window accumulation and ranking.
 */

#include "ultrawidelock_witness_core.h"

#include <string.h>

#define RSSI_FLOOR (-128)

void ultrawidelock_witness_core_open(struct ultrawidelock_witness_core *c)
{
	if (c == NULL) {
		return;
	}
	memset(c, 0, sizeof(*c));
	c->open = true;
}

/* Integer mean, rounded toward the floor. Rounding direction does not matter
 * to ranking -- it matters that it is the SAME rounding the lock's differential
 * later subtracts, so both ends do this one way and only here. */
static int32_t slot_mean(const struct ultrawidelock_witness_core_slot *s)
{
	if (s->n == 0u) {
		return RSSI_FLOOR;
	}
	return s->rssi_sum / (int32_t)s->n;
}

void ultrawidelock_witness_core_note(struct ultrawidelock_witness_core *c, uint32_t hash24,
				     int8_t rssi)
{
	struct ultrawidelock_witness_core_slot *first_free = NULL;
	struct ultrawidelock_witness_core_slot *weakest = NULL;
	struct ultrawidelock_witness_core_slot *target;
	uint32_t h;

	if (c == NULL || !c->open) {
		return;
	}
	h = hash24 & 0x00FFFFFFu;
	if (c->total_pkts < 0xFFFFu) {
		c->total_pkts++;
	}

	/* One pass, three answers: the matching slot if there is one, the first
	 * free slot, and the quietest occupied slot. Keeping the free-slot and
	 * eviction candidates in separate variables is not tidiness -- sharing
	 * one made an occupied slot shadow every free slot after it, so the
	 * second distinct advertiser of a window displaced the first and the
	 * report never held more than one. */
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_CORE_SLOTS; i++) {
		struct ultrawidelock_witness_core_slot *s = &c->slot[i];

		if (!s->used) {
			if (first_free == NULL) {
				first_free = s;
			}
			continue;
		}
		if (s->hash24 == h) {
			if (s->n < 0xFFFFu) {
				s->n++;
				s->rssi_sum += rssi;
			}
			if (rssi > s->rssi_peak) {
				s->rssi_peak = rssi;
			}
			return;
		}
		/* Evict the quietest, never the loudest: the report is ranked by
		 * loudness, so a slot that would not have made the cut is the
		 * one whose loss costs nothing. */
		if (weakest == NULL || slot_mean(s) < slot_mean(weakest)) {
			weakest = s;
		}
	}

	target = (first_free != NULL) ? first_free : weakest;
	if (target == NULL) {
		return;
	}
	if (first_free == NULL) {
		/* Only displace a weaker signal than the arriving one, or a
		 * burst of distant chatter would evict a nearby phone. */
		if (rssi <= (int8_t)slot_mean(target)) {
			return;
		}
	}
	memset(target, 0, sizeof(*target));
	target->used = true;
	target->hash24 = h;
	target->n = 1u;
	target->rssi_sum = rssi;
	target->rssi_peak = rssi;
}

uint8_t ultrawidelock_witness_core_summarize(struct ultrawidelock_witness_core *c,
					     struct ultrawidelock_witness_msg *msg,
					     uint8_t min_pkts)
{
	uint8_t out = 0u;

	if (c == NULL || msg == NULL) {
		return 0u;
	}
	c->open = false;
	msg->n_tuples = 0u;
	memset(msg->tuples, 0, sizeof(msg->tuples));

	/* Selection sort over at most 16 slots, run at most 8 times. Bounded,
	 * allocation-free, and the ordering is what the lock relies on when it
	 * walks tuples loudest-first. */
	while (out < ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES) {
		struct ultrawidelock_witness_core_slot *best = NULL;
		int32_t best_mean = RSSI_FLOOR;

		for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_CORE_SLOTS; i++) {
			struct ultrawidelock_witness_core_slot *s = &c->slot[i];
			int32_t m;

			if (!s->used || s->n < min_pkts) {
				continue;
			}
			m = slot_mean(s);
			if (best == NULL || m > best_mean) {
				best = s;
				best_mean = m;
			}
		}
		if (best == NULL) {
			break;
		}
		msg->tuples[out].hash24 = best->hash24;
		msg->tuples[out].mean_dbm = (int8_t)best_mean;
		msg->tuples[out].n_pkts = (best->n > 255u) ? 255u : (uint8_t)best->n;
		out++;
		best->used = false; /* consumed; not eligible again */
	}
	msg->n_tuples = out;
	return out;
}

bool ultrawidelock_witness_core_include(struct ultrawidelock_witness_core *c,
					struct ultrawidelock_witness_msg *msg, uint32_t hash24)
{
	struct ultrawidelock_witness_core_slot *found = NULL;
	struct ultrawidelock_witness_tuple *t;
	uint32_t h;

	if (c == NULL || msg == NULL) {
		return false;
	}
	h = hash24 & 0x00FFFFFFu;
	if (h == 0u) {
		return false;
	}
	if (ultrawidelock_witness_msg_find(msg, h) != NULL) {
		return true; /* made the cut on its own */
	}
	/* summarize() marks the tuples it consumed unused, so anything still
	 * used here is exactly what missed the cut. */
	for (size_t i = 0; i < ULTRAWIDELOCK_WITNESS_CORE_SLOTS; i++) {
		if (c->slot[i].used && c->slot[i].hash24 == h) {
			found = &c->slot[i];
			break;
		}
	}
	if (found == NULL || found->n == 0u) {
		return false; /* not heard this window; nothing to include */
	}
	if (msg->n_tuples < ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES) {
		t = &msg->tuples[msg->n_tuples];
		msg->n_tuples++;
	} else {
		t = &msg->tuples[ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES - 1u];
	}
	t->hash24 = found->hash24;
	t->mean_dbm = (int8_t)slot_mean(found);
	t->n_pkts = (found->n > 255u) ? 255u : (uint8_t)found->n;
	found->used = false;
	return true;
}
