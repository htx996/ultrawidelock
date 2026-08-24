/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_link.h — the sealed peer link's decisions, carrier-free.
 *
 * Everything the sealed link does that is NOT sending bytes: compose a report
 * or a handoff, and decide what an arriving datagram is and whether to believe
 * it. No socket, no radio, no allocation, no threads — the caller owns the
 * struct and supplies the bytes, so this compiles and is tested on the host and
 * runs unchanged over Thread UDP or ESP-NOW.
 *
 * WHY THIS EXISTS. apps/satellite/src/anchor_link.c and
 * apps/dwm3001cdk-lock/src/witness_link.c each grew these decisions inline,
 * woven through their own transport. The ESP32 port needs the same decisions
 * over a different carrier, and reimplementing "is this fresh" per carrier is
 * how two ends of one link stop agreeing. The wire format here is byte-identical
 * to what those two already exchange, deliberately: a mixed bench — nRF lock,
 * ESP32 satellite — must keep working.
 *
 * Both Zephyr apps and the ESP32 satellite use this core. Carrier setup, key
 * persistence and dispatch stay with each consumer; wire and freshness rules
 * live here once, so a mixed bench cannot drift one endpoint at a time.
 *
 * WHAT THIS DOES NOT DECIDE. Whether an accepted report should move a door.
 * A datagram that unseals and clears the replay window is authentic and fresh;
 * pairing it with this node's own measurement of the same ranging block, and
 * everything downstream of that, is ultrawidelock_satellite.h's job.
 */

#ifndef ULTRAWIDELOCK_LINK_H
#define ULTRAWIDELOCK_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_seal.h"
#include "ultrawidelock_witness_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * THE LOCK'S NONCE PREFIX, and the whole reason this constant is shared.
 *
 * ONE key travels in BOTH directions: the satellite seals WV3 reports with it
 * and the lock seals WV4 handoffs with it. Under AES-CCM a repeated
 * (key, nonce) pair leaks the XOR of two plaintexts and forges the tag, so the
 * two senders' nonce spaces must be provably disjoint, not merely unlikely to
 * collide.
 *
 * They are disjoint in byte 0. A satellite writes its ROLE there, and a role is
 * constrained to 1..3 (enum ultrawidelock_witness_role, and
 * ULTRAWIDELOCK_ANCHOR_ROLE's `range 1 3`). 0xFF is outside that range, so no
 * conforming anchor can emit it and no counter or boot id either side picks can
 * bring the two nonces together.
 *
 * If a role is ever widened past 3, this constant must move with it.
 */
#define ULTRAWIDELOCK_LINK_HANDOFF_ROLE 0xFFu

/** Valid satellite roles. These bounds and the handoff role above are one
 * nonce-space contract: widening one requires reviewing the other. */
#define ULTRAWIDELOCK_LINK_ROLE_MIN 1u
#define ULTRAWIDELOCK_LINK_ROLE_MAX 3u

/**
 * The lock's challenge beacon: version byte then 8 bytes of nonce, in the
 * clear. Unauthenticated on purpose — it is a freshness beacon, not a command.
 * Echoing the wrong one costs a report its standing and can make nothing
 * happen, which is why it needs no seal.
 */
#define ULTRAWIDELOCK_LINK_CHALLENGE_LEN 9u

/** A challenge with the lock's opaque 24-bit picked-label hint appended. */
#define ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN 12u

/**
 * The largest sealed frame this link can produce: the WV4 handoff, which
 * carries a 32-byte URSK and a 17-byte ranging config.
 *
 * Size every carrier buffer with this. A carrier whose payload ceiling is below
 * it cannot carry this link — check it at compile time where the carrier is
 * chosen, not at run time when a handoff is already overdue.
 */
#define ULTRAWIDELOCK_LINK_MAX_FRAME (ULTRAWIDELOCK_SEAL_OVERHEAD + ULTRAWIDELOCK_JOIN_MSG_LEN)

/** What an arriving datagram turned out to be. */
enum ultrawidelock_link_rx {
	/** Not ours: a length no message on this link has, or our own broadcast
	 *  coming back. The ordinary case on a shared carrier, and not an error. */
	ULTRAWIDELOCK_LINK_RX_IGNORED = 0,
	/** The lock's freshness beacon. The OUTGOING echo nonce has been
	 *  updated; nothing this end believes has changed. */
	ULTRAWIDELOCK_LINK_RX_CHALLENGE,
	/** A fresh sealed WV3 report. The @p am out-parameter is populated. */
	ULTRAWIDELOCK_LINK_RX_REPORT,
	/** A fresh sealed WV4 handoff. The @p jm out-parameter is populated. */
	ULTRAWIDELOCK_LINK_RX_JOIN,
	/** Right length, wrong key — or tampered with. The two are not
	 *  distinguished, and must not be reported apart: which one it was is
	 *  what an attacker probing keys wants to learn. */
	ULTRAWIDELOCK_LINK_RX_UNSEALED,
	/** Sealed under our key but the codec refused the plaintext. Means the
	 *  two ends disagree about the format, not that an attacker is present. */
	ULTRAWIDELOCK_LINK_RX_MALFORMED,
	/**
	 * Authentic, but not fresh. One of three: the counter did not advance,
	 * the report echoed a challenge this end is not asking for, or a boot id
	 * changed inside a single challenge epoch. They are one outcome on
	 * purpose -- telling them apart tells a prober which of its frames was
	 * closest. Only @p am's role and counter survive, for the diagnostic.
	 */
	ULTRAWIDELOCK_LINK_RX_REPLAYED,
};

/**
 * One end of the link. Caller-owned; zero it, then ultrawidelock_link_init().
 *
 * The key lives here in RAM. Where it is kept between boots is the caller's
 * problem and differs per platform (the portable KV seam, or ESP-IDF NVS in
 * the legacy consumer).
 */
struct ultrawidelock_link {
	uint8_t key[ULTRAWIDELOCK_SEAL_KEY_LEN];
	bool provisioned;
	/** OURS: which side of the door this node is, 1..3. The lock uses
	 *  ULTRAWIDELOCK_LINK_HANDOFF_ROLE when it seals a handoff. */
	uint8_t role;
	/**
	 * OURS, fresh per boot. It is what makes a counter that legitimately
	 * restarts at zero after a power cut distinguishable from a replay of
	 * everything that preceded it. Without it the far end's only options
	 * would be accepting replays or locking out a peer that lost power.
	 */
	uint32_t boot_id;
	/** OURS. Pre-incremented on every send; the nonce is built from the new
	 *  value, so the two can never disagree. */
	uint32_t ctr;
	/**
	 * OUTGOING. The challenge last heard, echoed in our next report so the
	 * receiver can tell a live report from a recorded one. Zero until one is
	 * heard, and learned from an UNAUTHENTICATED frame — so it is only ever
	 * an input to what we send, never to what we believe.
	 */
	uint64_t tx_echo_nonce;
	/**
	 * INCOMING, and deliberately not the same field. This is the challenge
	 * WE issued and therefore the one an arriving report must echo; it is
	 * set only through ultrawidelock_link_expect_echo(). Sharing one field
	 * with tx_echo_nonce would let anyone who can broadcast a challenge
	 * choose our freshness epoch, which on a shared carrier -- where a lock
	 * hears the satellites' channel probes -- is a frame away.
	 *
	 * Zero means UNARMED: no epoch has been issued, so reports are judged on
	 * the counter window alone. That is the behaviour of a node that never
	 * challenges, and it is what the link did before the epoch existed.
	 */
	uint64_t expected_echo_nonce;
	/** THE SATELLITES' replay windows, one per authenticated role. A single
	 *  window is unsafe: alternating boot IDs from two roles would continually
	 *  reset it and make an old report from either role fresh again. */
	struct ultrawidelock_witness_seen report_peer[ULTRAWIDELOCK_LINK_ROLE_MAX];
	/**
	 * The epoch each role's window last advanced under, so a boot id change
	 * is admitted once per challenge and not once per frame. A changed boot
	 * id is legitimate and must reset the counter window; that same reset is
	 * how a captured old boot rolls the window backwards. Requiring a fresh
	 * challenge in between costs a genuinely rebooted satellite one challenge
	 * interval and costs a recording attack the thing it cannot produce.
	 */
	uint64_t report_epoch[ULTRAWIDELOCK_LINK_ROLE_MAX];
	/** THE LOCK's replay window at a satellite. WV4 has one lock sender and no
	 *  role field, so it must not share a role-indexed WV3 window. */
	struct ultrawidelock_witness_seen join_peer;
};

/**
 * Prepare a link end. Clears every field, then sets the two that identify this
 * node. Does NOT provision a key: reporting stays off until one arrives, which
 * is the safe direction.
 */
void ultrawidelock_link_init(struct ultrawidelock_link *l, uint8_t role, uint32_t boot_id);

/**
 * Install the link key.
 *
 * @return 0, or -1 if @p len is not ULTRAWIDELOCK_SEAL_KEY_LEN. A wrong-sized
 *         key is refused rather than padded: a padded key seals datagrams
 *         nothing can open, and the symptom is a peer that is simply never
 *         heard from.
 */
int ultrawidelock_link_set_key(struct ultrawidelock_link *l, const uint8_t *key, size_t len);

/** True once a key is installed and this end can seal anything. */
bool ultrawidelock_link_ready(const struct ultrawidelock_link *l);

/**
 * Compose one sealed WV3 report: this node's distance to the phone and the
 * ranging block it was measured in.
 *
 * The block travels with the distance because the far end cannot pair a
 * distance whose round it does not know — see ultrawidelock_satellite.h, where
 * mispairing shows up as triangle rejections that read as a hardware fault.
 *
 * @param peer_mm negative is refused rather than sent; there is no such
 *                distance and encoding one would put a nonsense value inside a
 *                valid seal, where the far end must then defend against it.
 * @return bytes written, or 0 if there is no key, @p peer_mm is negative, or
 *         @p cap is too small. Advances the send counter only on success, so a
 *         refused report does not burn a nonce.
 */
size_t ultrawidelock_link_build_report(struct ultrawidelock_link *l, int32_t peer_mm,
				       uint32_t ranging_block, uint8_t *out, size_t cap);

/**
 * Compose one sealed WV4 session handoff: the URSK and ranging config that let
 * the far end join the phone's ranging session.
 *
 * The lock's direction. Sealed under the same key with the 0xFF role prefix —
 * see ULTRAWIDELOCK_LINK_HANDOFF_ROLE for why that byte is load-bearing.
 *
 * @param ursk ULTRAWIDELOCK_JOIN_URSK_LEN bytes, @param rcfg
 *        ULTRAWIDELOCK_JOIN_RCFG_LEN bytes. Borrowed for the call only.
 * @return bytes written, or 0. Advances the send counter only on success.
 */
size_t ultrawidelock_link_build_join(struct ultrawidelock_link *l, const uint8_t *ursk,
				     const uint8_t *rcfg, uint8_t channel,
				     uint8_t sync_code_index, uint8_t *out, size_t cap);

/**
 * Compose the lock's unsealed challenge beacon into @p out.
 *
 * @return ULTRAWIDELOCK_LINK_CHALLENGE_LEN, or 0 if @p cap is too small.
 */
size_t ultrawidelock_link_build_challenge(uint64_t nonce, uint8_t *out, size_t cap);

/**
 * Compose the 12-byte challenge variant used by the lock: the ordinary nonce
 * followed by the low 24 bits of @p hint in network byte order. A satellite
 * ignores the trailer; BLE witnesses use it to retain the picked label.
 *
 * @return ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN, or 0 if @p cap is too small.
 */
size_t ultrawidelock_link_build_challenge_hint(uint64_t nonce, uint32_t hint, uint8_t *out,
				       size_t cap);

/**
 * Arm this end's freshness epoch: the nonce an arriving WV3 report must echo.
 *
 * Call it with the nonce that was just broadcast, from the ONE place that rolls
 * it, so the value a report is judged against and the value on the wire cannot
 * come apart. Passing 0 disarms, which returns the report path to the counter
 * window alone.
 *
 * Deliberately a setter and not a side effect of consume(): a challenge is
 * unauthenticated, so what we EXPECT may only ever be set locally. See
 * expected_echo_nonce.
 */
void ultrawidelock_link_expect_echo(struct ultrawidelock_link *l, uint64_t nonce);

/**
 * Decide what one arriving datagram is, and whether to believe it.
 *
 * On ULTRAWIDELOCK_LINK_RX_REPORT the report is in @p am; on _RX_JOIN the
 * handoff is in @p jm. Either may be NULL if this end does not expect that
 * direction. Such a datagram is reported as IGNORED without opening it or
 * moving a replay window; a later delivery to a real sink can still land.
 *
 * ORDER MATTERS AND IS NOT NEGOTIABLE: seal first, then decode, then freshness.
 * Checking freshness before the seal would let anyone who can forge a counter
 * move the replay window, which is exactly the window's job to prevent.
 *
 * Freshness for a WV3 report is the challenge epoch and then the counter
 * window, in that order, and the epoch half only applies once this end has
 * armed one with ultrawidelock_link_expect_echo(). Unarmed, the counter window
 * decides alone.
 *
 * @p jm carries a URSK on success. The caller must clear it once consumed.
 */
enum ultrawidelock_link_rx ultrawidelock_link_consume(struct ultrawidelock_link *l,
						      const uint8_t *in, size_t len,
						      struct ultrawidelock_anchor_msg *am,
						      struct ultrawidelock_join_msg *jm);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_LINK_H */
