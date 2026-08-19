/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_witness_core.h — what a witness accumulates during one
 *       window, and which advertisers make it into the report.
 *
 * The witness firmware's whole decision is "of everything I heard in the last
 * two seconds, which eight does the lock get to see". That decision is here,
 * platform-free and host-tested, because it is the one place a witness can
 * silently starve the lock of the evidence it needs: an advertiser that misses
 * the cut is indistinguishable, at the lock, from one that was never there.
 *
 * Labels arrive already computed. The keyed hash of an advertiser address is
 * crypto and belongs where the key does, so this module never sees an address
 * and cannot leak one -- it counts and ranks opaque 24-bit labels. See
 * ultrawidelock_witness_msg.h for what those labels are and why the lock
 * cannot invert them.
 *
 * SELECTION IS BY MEAN RSSI, LOUDEST FIRST, and that choice has a bias worth
 * stating: it favours whatever is nearest the witness, which at the OUTSIDE
 * witness during a walk-up is the phone, and at the INSIDE witness is
 * whatever the house keeps near that door. The inside witness is therefore
 * the one that can drop the phone off the end of the report, which is why the
 * tuple count is 8 rather than 4 and why a denser room than that wants the
 * lock to hint the label back. Ranking by packet count instead would favour
 * chatty beacons over a nearby phone, which is worse.
 *
 * Integer arithmetic over caller-owned structs: no allocation, no threads, no
 * platform dependency, no clock of its own.
 */

#ifndef ULTRAWIDELOCK_WITNESS_CORE_H
#define ULTRAWIDELOCK_WITNESS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_witness_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Advertisers tracked within one window.
 *
 * Larger than the tuple count on purpose: the report carries the loudest 8,
 * but which 8 those are is not known until the window closes, so the accounting
 * has to hold more than it will send. 16 covers an ordinary room; past it the
 * quietest tracked slot is evicted, which is the correct thing to lose.
 */
#define ULTRAWIDELOCK_WITNESS_CORE_SLOTS 16u

/** One advertiser's running totals for the current window. */
struct ultrawidelock_witness_core_slot {
	uint32_t hash24;
	int32_t rssi_sum;
	uint16_t n;
	int8_t rssi_peak;
	bool used;
};

/** Caller-owned window accumulator. */
struct ultrawidelock_witness_core {
	struct ultrawidelock_witness_core_slot slot[ULTRAWIDELOCK_WITNESS_CORE_SLOTS];
	uint16_t total_pkts; /**< every packet seen, including evicted ones */
	bool open;
};

/** Reset and open a fresh window. Safe to call on an already-open one. */
void ultrawidelock_witness_core_open(struct ultrawidelock_witness_core *c);

/**
 * Record one advertising packet.
 *
 * @param hash24 Opaque label; only the low 24 bits are used.
 * @param rssi   dBm as the controller reported it.
 *
 * Ignored when the window is not open, so a packet arriving between windows is
 * dropped rather than attributed to the wrong one.
 */
void ultrawidelock_witness_core_note(struct ultrawidelock_witness_core *c, uint32_t hash24,
				     int8_t rssi);

/**
 * Close the window and fill @p msg's tuples with the loudest advertisers.
 *
 * Sets only `n_tuples` and `tuples[]`; the caller owns version, role, counters
 * and the challenge echo, because those are transport facts and this module
 * has no transport.
 *
 * @param min_pkts Slots with fewer packets than this are dropped. One packet is
 *                 a mean with no spread and no way to tell a real advertiser
 *                 from a passer-by, so the default caller passes 2.
 * @return Tuples written.
 */
uint8_t ultrawidelock_witness_core_summarize(struct ultrawidelock_witness_core *c,
					     struct ultrawidelock_witness_msg *msg,
					     uint8_t min_pkts);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_WITNESS_CORE_H */
