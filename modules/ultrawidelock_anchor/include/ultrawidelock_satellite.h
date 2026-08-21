/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_satellite.h — the freshness gate around a second anchor's report.
 *
 * ultrawidelock_fusion.h answers "which side of the door is this phone on" given two
 * distances measured at the same moment. This file is what makes that question
 * safe to ask on a real door, where the second distance arrives over a link
 * that can be slow, lossy or absent.
 *
 * Three rules, all about the satellite NOT being there: a report older than
 * `stale_ms` is not a report; no fresh report means UNKNOWN, and UNKNOWN
 * PERMITS prediction (a quiet satellite degrades to today's behaviour, never to
 * a door that will not open); only a POSITIVE outside verdict or a failed
 * triangle test withholds -- absence is not evidence, and an unconfigured
 * baseline counts as absence. `self_is_inside` is a config field because which
 * anchor is which is a mounting fact; backwards, it inverts the verdict.
 */
#ifndef ULTRAWIDELOCK_SATELLITE_H
#define ULTRAWIDELOCK_SATELLITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_fusion.h"

/** How long a satellite report stays usable. See the header comment. */
#define ULTRAWIDELOCK_SATELLITE_STALE_MS_DEFAULT 1500u

/**
 * How many of THIS node's recent measurements to keep, so a peer report that
 * arrives a block or two late still has something of ours to pair with.
 *
 * Eight because that is the window the freshness gate already permits: 1500 ms
 * of stale_ms against a 192 ms ranging block is 7.8 blocks, so a shorter ring
 * would throw away pairs the staleness rule says are still good, and a longer
 * one would hold samples that rule has already retired. At 16 bytes an entry
 * the whole ring is 128 bytes.
 *
 * The alternative -- widening the block match instead of remembering -- is not
 * available. One block of slack is 192 mm at 1.0 m/s and 269 mm at 1.4, against
 * a tolerance of 90. That is the arithmetic that retired block-parity
 * alternation, and it would destroy the very measurement this gate protects.
 */
#define ULTRAWIDELOCK_SATELLITE_RING 8u

/** One of this node's own measurements, kept so a late report can find it. */
struct ultrawidelock_satellite_sample {
	uint32_t block; /**< initiator's ranging block this was measured in */
	int32_t mm;     /**< this node's distance to the phone */
	int64_t ms;     /**< when it was latched, on this node's clock */
	bool have;
};

/**
 * Latest report from the second anchor, plus everything needed to judge it.
 * Caller-owned; this module allocates nothing and starts no threads.
 */
struct ultrawidelock_satellite {
	struct ultrawidelock_fusion_cfg cfg; /**< baseline, tolerance, dead band */
	int64_t last_ms;           /**< when the stored report arrived */
	int32_t peer_mm;           /**< satellite's distance to the phone */
	uint32_t peer_block;       /**< ranging block that distance was measured in */
	/** This node's own recent measurements, newest at @c ring_next - 1. */
	struct ultrawidelock_satellite_sample ring[ULTRAWIDELOCK_SATELLITE_RING];
	uint8_t ring_next;
	uint32_t stale_ms;         /**< older than this and peer_mm is ignored */
	bool self_is_inside;       /**< true if THIS node is the inside anchor */
	bool have;                 /**< false until the first report */
};

/**
 * @param s              Caller-owned state.
 * @param cfg            Geometry config; copied, not retained by pointer.
 * @param stale_ms       0 selects ULTRAWIDELOCK_SATELLITE_STALE_MS_DEFAULT.
 * @param self_is_inside True if the node running this code is mounted on the
 *                       inside of the door. Read the header before choosing.
 */
void ultrawidelock_satellite_init(struct ultrawidelock_satellite *s,
				  const struct ultrawidelock_fusion_cfg *cfg, uint32_t stale_ms,
				  bool self_is_inside);

/**
 * Store a report from the second anchor.
 *
 * @param peer_mm    Satellite's measured distance to the phone, millimetres.
 *                   Negative is rejected outright rather than stored and
 *                   rejected later, so a decode bug cannot masquerade as a
 *                   stale link.
 * @param peer_block The initiator's ranging block that distance was measured
 *                   in. This is the timebase alignment this header used to say
 *                   stage C owed, and it discharges it better than the clock
 *                   alignment that wording implied: pairing two anchors needs
 *                   to know the readings describe the same instant, not what
 *                   time that instant was. Both anchors read this integer off
 *                   the initiator's own frames, so equality is exact.
 * @param now_ms     Monotonic milliseconds on THIS node's clock. Now only a
 *                   staleness backstop against a stalled link; @p peer_block
 *                   is what establishes that a pair is same-round.
 */
void ultrawidelock_satellite_report(struct ultrawidelock_satellite *s, int32_t peer_mm,
				    uint32_t peer_block, int64_t now_ms);

/**
 * Evaluate the side of the door, given this node's own distance to the phone.
 *
 * The self distance is NOT a parameter: it is looked up in the ring by the
 * stored report's block. Which measurement of ours to fuse is decided by the
 * peer's report, not by the caller, so letting a caller pass one is how a pair
 * from two different rounds gets assembled.
 *
 * @return A verdict with `geometry_ok == false` and side UNKNOWN when there is
 *         no fresh report, or the ring holds nothing for that report's block;
 *         otherwise ultrawidelock_fusion_eval()'s answer with the two distances
 *         placed according to `self_is_inside`.
 */
struct ultrawidelock_fusion_verdict
ultrawidelock_satellite_verdict(const struct ultrawidelock_satellite *s, int64_t now_ms);

/**
 * Record one of THIS node's measurements, so a later report can pair with it.
 *
 * @param self_mm    This node's distance to the phone, millimetres. Negative is
 *                   ignored, matching ultrawidelock_satellite_report().
 * @param self_block The ranging block it was measured in. Must come from the
 *                   same latch as @p self_mm -- a block that does not describe
 *                   the distance it is stored beside defeats the whole point,
 *                   because the equality check then passes on a wrong pair.
 * @param now_ms     Monotonic milliseconds on this node's clock.
 */
void ultrawidelock_satellite_observe(struct ultrawidelock_satellite *s, int32_t self_mm,
				     uint32_t self_block, int64_t now_ms);

/**
 * The one question the approach controller asks: may prediction proceed?
 *
 * @return false ONLY on a SAME-BLOCK fresh report that puts the phone outside,
 *         or on a same-block fresh pair that no single phone position could
 *         produce. True when there is no satellite, no fresh report, no
 *         same-block pair, or nothing to object to -- see rule 2 in the header.
 *
 * A block mismatch returns true rather than false on purpose: it is the absence
 * of a pair, not a suspicious one, and withholding on it would let a satellite
 * that merely fell a block behind look identical to one reporting an intruder.
 */
bool ultrawidelock_satellite_may_predict(const struct ultrawidelock_satellite *s, int64_t now_ms);

/**
 * The second anchor's own distance, if a fresh one is held.
 *
 * For callers that must report WHAT WAS MEASURED alongside the verdict --
 * a health mask, a log line, a feature vector. It is deliberately not part of
 * the verdict: a distance that exists but could not be paired is exactly the
 * state an operator needs to see, and folding it into UNKNOWN hides it.
 *
 * @return millimetres, or -1 when there is no report or it is older than
 *         @c stale_ms. Never a stale value: a caller cannot tell one from a
 *         live one, and this feeds a gate.
 */
int32_t ultrawidelock_satellite_peer_mm(const struct ultrawidelock_satellite *s, int64_t now_ms);

/* ── more than one satellite ─────────────────────────────────────────────── */

/**
 * How many satellite roles a lock can ingest at once.
 *
 * THREE, and the number is not free: it is `range 1 3` on
 * ULTRAWIDELOCK_ANCHOR_ROLE (apps/nrf5340dk-satellite/Kconfig), it is enum
 * ultrawidelock_witness_role, and it is what makes 0xFF safe as the lock's own
 * nonce prefix on the sealed link (HANDOFF_NONCE_ROLE, witness_link.c). Those
 * four have to move together or the AES-CCM nonce spaces stop being disjoint,
 * so widening this alone is not a widening -- it is a hole.
 *
 * Deliberately NOT CONFIG_ULTRAWIDELOCK_WITNESS_MAX, which looks like the same
 * number and is not: that one is `range 2 4` and counts the retired BLE witness
 * slots, so indexing this array with it would either waste a slot or run off
 * the end of a role that has nowhere to live.
 */
#define ULTRAWIDELOCK_SATELLITE_MAX_ROLES 3u

/**
 * Every satellite the lock listens to, one slot per role.
 *
 * ONE DEPLOYED SATELLITE IS THE CASE THIS SHIPS FOR, and with one reporter
 * every function below reduces exactly to the single-peer function it wraps.
 * The array exists so that a second and third board is a role Kconfig plus a
 * key rather than a patch to this file: without it, two satellites sharing the
 * lock's single struct overwrite each other's distance between blocks, and the
 * fusion then reads role 1's measurement as if role 2 had made it -- which does
 * not fail any test, it silently inverts the side verdict.
 *
 * Each slot keeps its OWN copy of this node's sample ring, which is the same
 * data three times over (about 470 B of the lock's .bss at three roles). Bought
 * deliberately: sharing one ring would mean reaching into struct
 * ultrawidelock_satellite from here, and that struct's pairing rule -- the
 * report chooses which of our samples it pairs with -- is the part of this
 * module that has been on hardware. Slots stay independent so nothing about
 * one peer's freshness can reach another's.
 */
struct ultrawidelock_satellite_set {
	struct ultrawidelock_satellite peer[ULTRAWIDELOCK_SATELLITE_MAX_ROLES];
};

/**
 * Initialise every slot.
 *
 * @param cfg  Array of ULTRAWIDELOCK_SATELLITE_MAX_ROLES geometry configs, one
 *             per role, index 0 = role 1. PER ROLE because baseline_mm is the
 *             distance from THIS node to THAT satellite, and two satellites are
 *             not in the same place; a shared baseline would size every
 *             triangle test for one of them and mis-size it for the other.
 *             NULL, or a slot with baseline_mm <= 0, is a role that is not
 *             installed -- and absence is already how this module spells "no
 *             satellite", so an uninstalled role permits prediction rather than
 *             withholding on it.
 * @param stale_ms       0 selects ULTRAWIDELOCK_SATELLITE_STALE_MS_DEFAULT.
 * @param self_is_inside Whether THIS node is the inside anchor. One value for
 *                       the whole set: it describes where this board is screwed,
 *                       which cannot depend on who is reporting.
 */
void ultrawidelock_satellite_set_init(struct ultrawidelock_satellite_set *set,
				      const struct ultrawidelock_fusion_cfg *cfg,
				      uint32_t stale_ms, bool self_is_inside);

/**
 * Store a report, from the role that sent it.
 *
 * @param role 1..ULTRAWIDELOCK_SATELLITE_MAX_ROLES. Anything else is dropped
 *             rather than clamped: a role outside the range is a peer this lock
 *             cannot have a disjoint nonce space with, so the safe reading of
 *             it is "not one of mine".
 */
void ultrawidelock_satellite_set_report(struct ultrawidelock_satellite_set *set, uint8_t role,
					int32_t peer_mm, uint32_t peer_block, int64_t now_ms);

/** Record one of THIS node's measurements into every slot. */
void ultrawidelock_satellite_set_observe(struct ultrawidelock_satellite_set *set, int32_t self_mm,
					 uint32_t self_block, int64_t now_ms);

/**
 * The set's side verdict.
 *
 * Roles that produced no same-block pair are silent, exactly as a single quiet
 * satellite is. Of those that did speak:
 *   - a role inside its dead band ABSTAINS: it has a good pair and no opinion,
 *     which is ordinary for an anchor the phone is on the bisector of. It
 *     neither decides nor vetoes;
 *   - roles agreeing on a side: that verdict, from the lowest-numbered one, so
 *     a one-satellite install returns precisely what it returns today;
 *   - two roles naming opposite sides: {UNKNOWN, geometry_ok = false}. Two
 *     correctly mounted anchors looking at one phone cannot reach opposite
 *     answers, so a disagreement is a mounting error, a wrong baseline or a
 *     forged report -- and none of those should be resolved by a majority vote
 *     among devices one of which is lying.
 *
 * With one satellite these rules collapse to the single-peer function: the one
 * role decides, abstains or stays silent, and there is nobody to disagree with.
 */
struct ultrawidelock_fusion_verdict
ultrawidelock_satellite_set_verdict(const struct ultrawidelock_satellite_set *set, int64_t now_ms);

/**
 * Whether prediction may proceed, over the whole set.
 *
 * The AND of the per-role answers: one satellite with real evidence that the
 * phone is outside withholds, whatever the others do or do not say. Absence
 * still permits, per rule 2 -- so a set with nothing installed, nothing fresh
 * or nothing paired is today's single-anchor behaviour, unchanged.
 */
bool ultrawidelock_satellite_set_may_predict(const struct ultrawidelock_satellite_set *set,
					     int64_t now_ms);

/**
 * A fresh peer distance, for the health mask and the log line.
 *
 * The lowest-numbered role holding one; -1 when no role does. One number
 * because its consumer is one feature field (uwb_peer_mm), and the lowest role
 * rather than the nearest so the value does not hop between boards while the
 * phone walks.
 */
int32_t ultrawidelock_satellite_set_peer_mm(const struct ultrawidelock_satellite_set *set,
					    int64_t now_ms);

#endif /* ULTRAWIDELOCK_SATELLITE_H */
