/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_latch.h — the inside veto: a lock that will not open
 *       passively for a phone it believes is indoors.
 *
 * WHAT THIS IS NOT: not the bolt, not the mechanical latch, not any hardware.
 * It is one persistent record per credential saying "this phone is believed
 * inside", and its resting state is INSIDE. Losing power, radio, witnesses or
 * storage cannot move it off INSIDE, because moving off INSIDE takes positive
 * evidence and absence is not evidence.
 *
 * WHY IT EXISTS. ultrawidelock_side.h classifies which side of the door a phone
 * is on, continuously, from differential BLE RSSI. MEASURED 2026-08-11: 49% of
 * windows landed in the dead band at the door plane, which is exactly where
 * the approach controller asks to unlock -- 37 refusals against 2 grants on
 * one walk. A continuous classifier is the wrong shape for this problem. The
 * latch changes the shape: the classifier is consulted only during a live
 * walk-up, at a distance where it is unambiguous, and its answer clears the
 * veto for that one approach rather than being re-asked at the door.
 *
 * THE ASYMMETRY THIS ENCODES. Failing to open for someone outside costs them a
 * retry, an NFC tap, or the app. Opening for a phone sitting on the hall table
 * is the failure the whole module exists to prevent. So every uncertainty
 * resolves to "do not open", and the clear is deliberately hard:
 *
 *   1. a live credential session, with the peer address the lock already holds
 *   2. a recorded crossing OPPORTUNITY since this credential was last confirmed
 *      inside -- a moment the phone could physically have left. RF alone can
 *      never clear the veto; the door must also have been openable.
 *   3. `clear_windows` consecutive confident-OUTSIDE windows, the first of them
 *      taken at or beyond `clear_min_mm` -- a closing trajectory from far out,
 *      not a single reading at the door
 *   4. the last of those windows no older than `clear_valid_ms`
 *
 * and then the grant itself immediately re-latches INSIDE. There is no
 * persistent "cleared" state to go stale, be replayed, or survive a reboot:
 * the veto is always on, and every passive unlock proves its way past it from
 * scratch. `clear_valid_ms` is what carries a proven approach across the dead
 * band -- the thing `ultrawidelock_side_cfg::outside_hold_ms` was reaching for,
 * as bounded state rather than as a widened confidence window.
 *
 * WHAT IS NOT GATED. Deliberate unlock paths -- NFC Express Mode, Apple Home
 * commands, the mechanical key -- never consult this module and must not. They
 * are also the universal recovery: after storage loss, a factory boot, a new
 * credential, or an exit through a door nothing observed, one deliberate
 * unlock re-seeds the record and normal walk-up behaviour returns.
 *
 * PRIVACY. Records are keyed by @ref ultrawidelock_latch_rec::cred_id, a value
 * the caller derives from install-local credential material (fabric index and
 * credential slot). No address, IRK, or other phone identifier is stored here
 * or recoverable from what is stored.
 *
 * Integer arithmetic over caller-owned structs: no allocation, no threads, no
 * platform dependency, and the host suite is the whole correctness story.
 */

#ifndef ULTRAWIDELOCK_LATCH_H
#define ULTRAWIDELOCK_LATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_side.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Credentials tracked at once. Beyond this, see ultrawidelock_latch_note_grant(). */
#define ULTRAWIDELOCK_LATCH_MAX_CREDS 8u

/** Passed as cred_id to apply an event to every credential. See note_opportunity(). */
#define ULTRAWIDELOCK_LATCH_CRED_ANY 0xFFFFFFFFu

/** Why a passive unlock was refused. Zero means it was not. */
#define ULTRAWIDELOCK_LATCH_R_NO_SESSION     0x01u /**< no live session for this credential */
#define ULTRAWIDELOCK_LATCH_R_NO_RECORD      0x02u /**< record table full; fails closed */
#define ULTRAWIDELOCK_LATCH_R_NO_OPPORTUNITY 0x04u /**< no chance to have left since entry */
#define ULTRAWIDELOCK_LATCH_R_DWELL          0x08u /**< still inside the post-entry dwell */
#define ULTRAWIDELOCK_LATCH_R_WINDOWS        0x10u /**< not enough agreeing OUTSIDE windows */
#define ULTRAWIDELOCK_LATCH_R_STALE          0x20u /**< the agreeing windows have aged out */

/** Record flags. */
#define ULTRAWIDELOCK_LATCH_F_USED        0x01u
/**
 * The credential has had a chance to cross the door since it was last
 * confirmed inside. Set by any grant, for every record -- see
 * ultrawidelock_latch_note_grant(). Not independent evidence when only one
 * credential is enrolled; the dwell and the window run carry the discrimination
 * there.
 */
#define ULTRAWIDELOCK_LATCH_F_OPPORTUNITY 0x02u

/** Tunables. All defaults are conservative; raising them is always safe. */
struct ultrawidelock_latch_cfg {
	/**
	 * After any grant, how long before this credential may even be
	 * considered for another passive unlock. Covers the walk-in: during it
	 * the phone is crossing the plane and every classification is noise.
	 */
	uint32_t entry_dwell_ms;
	/** Consecutive confident-OUTSIDE windows required to clear. */
	uint8_t clear_windows;
	/**
	 * The FIRST agreeing window must be taken at or beyond this range, so a
	 * clear always begins far from the door where the differential is
	 * unambiguous. Later windows in the run may be closer -- that is the
	 * approach happening.
	 */
	int32_t clear_min_mm;
	/**
	 * How long a completed run of agreeing windows stays usable. This is
	 * what carries a proven approach across the dead band at the plane.
	 * Bounded on purpose: it is evidence with an expiry, not a mode.
	 */
	uint32_t clear_valid_ms;
	/**
	 * How long a crossing opportunity stays valid. 0 = never expires, and
	 * that is the default: a phone that left three days ago really did have
	 * a chance to leave, and expiring the fact would manufacture refusals
	 * without preventing anything. The knob exists for installs that want a
	 * tighter story.
	 */
	uint32_t opportunity_valid_ms;
};

/** One credential's persistent belief. Serialised; keep it POD. */
struct ultrawidelock_latch_rec {
	uint32_t cred_id;             /**< derived, non-identifying */
	int64_t confirmed_inside_ms;  /**< when INSIDE was last asserted */
	int64_t opportunity_ms;       /**< when the crossing opportunity was recorded */
	uint8_t flags;                /**< ULTRAWIDELOCK_LATCH_F_* */
};

/**
 * Latch state. The record array persists; everything below @ref active is
 * per-approach and deliberately NOT persisted -- progress toward a clear must
 * not survive a reboot, a session change, or the credential walking away.
 */
struct ultrawidelock_latch {
	struct ultrawidelock_latch_cfg cfg;
	struct ultrawidelock_latch_rec rec[ULTRAWIDELOCK_LATCH_MAX_CREDS];

	bool active;          /**< a credential session is open */
	uint32_t active_cred; /**< which one */
	uint8_t agree_n;      /**< consecutive confident-OUTSIDE windows so far */
	int64_t last_outside_ms; /**< when the most recent one landed */
};

/** Fill @p cfg with the conservative defaults documented above. */
void ultrawidelock_latch_defaults(struct ultrawidelock_latch_cfg *cfg);

/** Initialise; @p cfg NULL uses defaults. Every credential starts INSIDE. */
void ultrawidelock_latch_init(struct ultrawidelock_latch *l,
			      const struct ultrawidelock_latch_cfg *cfg);

/**
 * Open a credential session. Resets clear progress: evidence never carries
 * from one session to the next, because it never carries from one phone to
 * another and the lock cannot always tell those apart.
 */
void ultrawidelock_latch_session_open(struct ultrawidelock_latch *l, uint32_t cred_id);

/** Close the session and discard clear progress. */
void ultrawidelock_latch_session_close(struct ultrawidelock_latch *l);

/**
 * Record that the door was opened for @p cred_id, by any path at all --
 * passive, NFC, app, or a mechanical operation the lock managed to see.
 *
 * Asserts INSIDE for that credential and discards clear progress. This is the
 * pessimism the whole module rests on: after a door opening the phone
 * plausibly went in, so the lock assumes it did, and refuses for entry_dwell_ms
 * whatever the RF says.
 *
 * It also grants the opportunity, to this credential and to every other one.
 * The lock cannot tell an entry from an exit -- the same tap serves both -- so
 * a door opening is a crossing chance for everybody who was near it. The dwell,
 * not the flag, is what keeps the walk-in from clearing the latch it just set.
 *
 * When the record table is full the oldest confirmed_inside_ms is evicted; the
 * evicted credential then reads as "no record", which fails closed.
 */
void ultrawidelock_latch_note_grant(struct ultrawidelock_latch *l, uint32_t cred_id,
				    int64_t now_ms);

/**
 * Record a moment at which a phone could have crossed the door.
 *
 * @param cred_id ULTRAWIDELOCK_LATCH_CRED_ANY for events with no credential
 *                attached to them -- a sensed door swing has no identity, so
 *                it grants the opportunity to everyone. That breadth is safe
 *                because an opportunity alone clears nothing: conditions 3 and
 *                4 still have to be met by live RF for the specific phone.
 *
 * No production caller today. ultrawidelock_latch_note_grant() is the only
 * opportunity source the firmware wires up; this exists for the sensed door
 * event of stage P7, and for the tests.
 */
void ultrawidelock_latch_note_opportunity(struct ultrawidelock_latch *l, uint32_t cred_id,
					  int64_t now_ms);

/**
 * Feed one classified window from ultrawidelock_side_filter_feed().
 *
 * Only windows for the open session count. A confident OUTSIDE extends the
 * run; an INSIDE resets it to zero, because a contradiction is worth more than
 * the agreements before it; UNKNOWN and THRESHOLD leave it alone, so the dead
 * band neither helps nor destroys an approach already proven from further out.
 *
 * @param side      The filtered decision's side.
 * @param range_mm  Primary UWB range for this window; negative = unknown, which
 *                  cannot start a run.
 */
void ultrawidelock_latch_note_window(struct ultrawidelock_latch *l, uint32_t cred_id,
				     enum ultrawidelock_side_label side, int32_t range_mm,
				     int64_t now_ms);

/**
 * The one question the approach controller asks.
 *
 * @param reason Optional out; receives the ULTRAWIDELOCK_LATCH_R_* bits that
 *               refused, or 0 on permit. Every refusal names itself, because a
 *               gate that silently withholds is indistinguishable from a bug.
 * @return true only when all four clear conditions hold.
 */
bool ultrawidelock_latch_may_passive_unlock(const struct ultrawidelock_latch *l, uint32_t cred_id,
					    int64_t now_ms, uint8_t *reason);

/**
 * Serialise the persistent records (not the per-approach state).
 *
 * @return bytes written, or 0 if @p cap is too small.
 */
size_t ultrawidelock_latch_serialize(const struct ultrawidelock_latch *l, uint8_t *buf, size_t cap);

/**
 * Restore records from ultrawidelock_latch_serialize() output.
 *
 * A short buffer, an unknown version, an impossible count or a bad CRC all
 * leave @p l initialised-and-empty rather than half-restored: every credential
 * then reads INSIDE with no opportunity, which is the safest state and the one
 * a deliberate unlock recovers from.
 *
 * @return true when records were restored, false when @p l was reset instead.
 */
bool ultrawidelock_latch_deserialize(struct ultrawidelock_latch *l,
				     const struct ultrawidelock_latch_cfg *cfg, const uint8_t *buf,
				     size_t len);

/** Bytes ultrawidelock_latch_serialize() needs. */
#define ULTRAWIDELOCK_LATCH_REC_LEN 21u
#define ULTRAWIDELOCK_LATCH_BLOB_LEN \
	(2u + ULTRAWIDELOCK_LATCH_MAX_CREDS * ULTRAWIDELOCK_LATCH_REC_LEN + 2u)

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_LATCH_H */
