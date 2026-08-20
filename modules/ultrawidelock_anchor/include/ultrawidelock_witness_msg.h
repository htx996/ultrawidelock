/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_witness_msg.h — WV2, the BLE witness report on the wire.
 *
 * One datagram per summarisation window, witness -> lock, over Thread UDP.
 * This file is the codec and the validation rules ONLY: sealing (AES-CCM under
 * the per-witness link key) belongs to the transport, because the key lives
 * where the PSA backend does and this module has to stay platform-free.
 *
 * Three properties the format is shaped by, in order:
 *
 * 1. NO PHONE IDENTIFIERS, AND THE LOCK NEVER LEARNS ONE. A tuple carries a
 *    24-bit truncation of a hash over an advertiser address under the WITNESS
 *    GROUP key, never the address. The group key is shared by the witnesses
 *    and NOT by the lock, so the label is consistent across the two witnesses
 *    -- which is the whole point, it is what lets one advertiser be compared
 *    inside against outside -- while remaining opaque to the lock and to
 *    anyone on the air.
 *
 *    The lock does not match these against anything it knows. It CANNOT: an
 *    earlier revision of this design had the lock hash the credential
 *    connection's peer address and match, and that is unsound. The phone is
 *    the central, so the address the lock holds is an InitA generated for the
 *    initiating role; the ambient traffic a witness hears comes from
 *    advertising sets with their own address state and their own rotation
 *    timers. The Core Spec permits one RPA across roles but does not require
 *    it, and nothing Apple documents promises it. The lock instead picks which
 *    label is the phone by trajectory correlation against its own
 *    authenticated UWB range -- see ultrawidelock_witness_pick.h.
 *
 * 2. FRESHNESS IS ASYMMETRIC. `ctr` is monotonic per (witness, boot_id) and
 *    catches ordinary replay. `echo_nonce` is the lock's current challenge,
 *    and only reports echoing it may CLEAR an inside veto -- a replayed
 *    genuine "phone is outside" window from yesterday is the one forgery that
 *    would actually open a door, so the clear direction gets a challenge and
 *    the veto direction does not. See ultrawidelock_latch.h.
 *
 * 3. A WITNESS HOLDS NO AUTHORITY. Every rule here is enforced by the lock on
 *    receipt. A witness that lies, replays, or floods can cost a passive
 *    unlock; it cannot cause one.
 *
 * Byte order is big-endian throughout, chosen to match the UWB message codecs
 * in this tree rather than the host. Fixed layout, no alignment assumptions,
 * no allocation: encode/decode are pure functions over caller-owned structs.
 */

#ifndef ULTRAWIDELOCK_WITNESS_MSG_H
#define ULTRAWIDELOCK_WITNESS_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version. WV1 was the ASCII SF1 bench line; this is its replacement. */
#define ULTRAWIDELOCK_WITNESS_MSG_VER 2u

/**
 * Advertiser tuples per report, loudest first.
 *
 * Eight, not four. The binding case is not the outside witness during a
 * walk-up -- the phone is nearly on top of it and is certainly in the top few
 * -- but the INSIDE witness, which hears that same phone through a door and
 * therefore ranks it below whatever else the house is running. If the phone
 * misses the cut there, the pair has no inside reading, quorum fails, and no
 * clear is possible. That fails safe and feels like a broken lock.
 *
 * Eight tuples plus the header is 85 B, still one 802.15.4 frame with room for
 * the seal. Raising the count was considered and rejected: past eight the
 * sealed report no longer fits one frame, and a fragmented report on a
 * sleepy end device loses more windows than the wider cut saves. The
 * lock instead hints its picked label back on the challenge so the witnesses
 * always include it -- see ultrawidelock_witness_core_include().
 */
#define ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES 8u

/** Fixed header bytes: ver, role, boot_id, ctr, echo_nonce, window_ms, n. */
#define ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN 21u

/** Bytes per tuple: hash24, mean_dbm, n_pkts. */
#define ULTRAWIDELOCK_WITNESS_MSG_TUPLE_LEN 5u

/** Largest encoded report, before sealing. */
#define ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN \
	(ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN + \
	 ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES * ULTRAWIDELOCK_WITNESS_MSG_TUPLE_LEN)

/** Which side of the door plane this witness is mounted on. */
enum ultrawidelock_witness_role {
	ULTRAWIDELOCK_WITNESS_ROLE_UNKNOWN = 0,
	ULTRAWIDELOCK_WITNESS_ROLE_INSIDE = 1,
	ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE = 2,
	ULTRAWIDELOCK_WITNESS_ROLE_THRESHOLD = 3,
};

/** One advertiser the witness heard during the window. */
struct ultrawidelock_witness_tuple {
	/**
	 * Low 24 bits of a hash over the advertiser address under the witness
	 * GROUP key. NOT an address and not reversible into one without that
	 * key, which the lock does not hold.
	 *
	 * Its only job is to be the SAME value at both witnesses for the same
	 * advertiser in the same window, so the lock can line inside up against
	 * outside without knowing who either of them is. It is stable only for
	 * as long as the underlying resolvable private address is; a rotation
	 * mid-approach retires a label and the lock's pick restarts, which costs
	 * one approach and never opens a door.
	 *
	 * 24 bits keeps ambient collisions negligible at household scale; a
	 * collision at worst adds one wrong tuple to one window, which the
	 * consecutive-window rule and the paired second witness absorb.
	 */
	uint32_t hash24;
	int8_t mean_dbm;  /**< mean RSSI over the window for this advertiser */
	uint8_t n_pkts;   /**< packets that fed the mean; 0 means the tuple is empty */
};

/** One decoded report. Caller-owned; the codec allocates nothing. */
struct ultrawidelock_witness_msg {
	uint8_t ver;
	uint8_t role; /**< enum ultrawidelock_witness_role */
	/**
	 * Random per witness boot. A witness that reboots gets a new one and
	 * its counter legitimately restarts at zero -- without this field that
	 * restart is indistinguishable from a replay, and the choice would be
	 * between accepting replays and locking out a witness that lost power.
	 */
	uint32_t boot_id;
	uint32_t ctr;        /**< strictly monotonic within (witness, boot_id) */
	uint64_t echo_nonce; /**< most recent challenge the witness heard */
	uint16_t window_ms;  /**< summarisation window this report covers */
	uint8_t n_tuples;
	struct ultrawidelock_witness_tuple tuples[ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES];
};

/**
 * Encode a report.
 *
 * @return bytes written, or 0 if @p msg is malformed or @p cap is too small.
 *         Tuples beyond n_tuples are not encoded; n_tuples above the maximum
 *         is a malformed message rather than a silent truncation.
 */
size_t ultrawidelock_witness_msg_encode(const struct ultrawidelock_witness_msg *msg, uint8_t *buf,
					size_t cap);

/**
 * Decode a report.
 *
 * Rejects: a short buffer, an unknown version, a role outside the enum, a
 * tuple count above the maximum, and a length that does not match the tuple
 * count exactly. A trailing byte is a decode failure, not slack: the datagram
 * is sealed, so extra bytes mean the sender and this decoder disagree about
 * the format, and continuing would be guessing.
 *
 * @return true on success, @p out fully populated.
 */
bool ultrawidelock_witness_msg_decode(const uint8_t *buf, size_t len,
				      struct ultrawidelock_witness_msg *out);

/**
 * Find the tuple matching @p hash24.
 *
 * @return pointer into @p msg, or NULL when the phone was not heard this
 *         window. NULL is the ordinary case and means "no evidence", never
 *         "evidence of absence" -- an unheard phone is silent, not outside.
 */
const struct ultrawidelock_witness_tuple *
ultrawidelock_witness_msg_find(const struct ultrawidelock_witness_msg *msg, uint32_t hash24);

/** Tuple at @p idx, or NULL. Iteration order is loudest-first by convention. */
const struct ultrawidelock_witness_tuple *
ultrawidelock_witness_msg_at(const struct ultrawidelock_witness_msg *msg, uint8_t idx);

/**
 * Per-witness replay state. One of these per enrolled witness, on the lock.
 */
struct ultrawidelock_witness_seen {
	uint32_t boot_id;
	uint32_t ctr;
	bool have;
};

/**
 * Accept-or-reject a decoded report against the replay state, updating it on
 * accept.
 *
 * A new boot_id resets the accepted counter (see boot_id above). Within a
 * boot_id the counter must strictly increase; equal is a replay, lower is a
 * replay, and both are rejected without touching the stored state.
 *
 * @return true if the report is fresh and @p seen has been advanced.
 */
bool ultrawidelock_witness_seen_accept(struct ultrawidelock_witness_seen *seen,
				       const struct ultrawidelock_witness_msg *msg);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_WITNESS_MSG_H */
