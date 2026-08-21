/* SPDX-License-Identifier: ISC */

/**
 * @file witness_link.h — sealed BLE witness reports over Thread UDP.
 *
 * Replaces the SF1-over-RTT bench feed (side_feed.h), which needed a debug
 * probe held open for the life of the session and so could never be a path a
 * deployed lock used. This one needs nothing attached: the witnesses report
 * over the Thread network the lock has already joined.
 *
 * Feeds the same inbox side_feed.h publishes to, so the side gate and the
 * inside latch consume witness evidence through one path regardless of which
 * transport delivered it.
 */
#ifndef WITNESS_LINK_H
#define WITNESS_LINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Length of a witness link key.
 *
 * Declared here because the code that WRITES these keys is not in the same
 * image as the code that reads them. The lock image is a Thread build, which
 * sets CONFIG_SHELL=n, so the image that runs the latch can never have a
 * console to type a key into. Enrollment is `ultrawidelock witkey` on the
 * `make reader` build (src/prov_shell.c), and the record survives the reflash
 * to the Thread image in the settings partition.
 *
 * The record is "uwl/wit/k/<role>", role 1..3 (ULTRAWIDELOCK_WITNESS_ROLE_*):
 * "uwl/wit/k/2" is the outside witness. Reader and writer are registered
 * separately in PORTING.md and checked against the code by
 * tests/tooling/port_purity_check.sh, which is what keeps the two ends of a
 * name nothing prints at runtime from drifting apart.
 */
#define WITNESS_LINK_KEY_LEN 16u

/** Open the UDP socket and load witness keys. Safe to call before Thread is up. */
/**
 * Called when a sealed SECOND-ANCHOR report (WV3) is accepted.
 *
 * A callback rather than a direct call into the fusion layer, so the transport
 * stays ignorant of what a distance is for: it authenticates a datagram and
 * hands over what it said. @p ranging_block travels with @p peer_mm because a
 * distance without the round it was measured in cannot be paired with ours.
 *
 * Runs on the OpenThread receive path. Keep it to a store.
 */
typedef void (*witness_link_anchor_cb)(int32_t peer_mm, uint32_t ranging_block, int64_t now_ms);

/** Register the WV3 sink (NULL to clear). Call before witness_link_init(). */
void witness_link_set_anchor_cb(witness_link_anchor_cb cb);

void witness_link_init(void);

/**
 * Publish the authenticated UWB range for the current window.
 *
 * The picker correlates advertiser RSSI against this. Without it no candidate
 * can be scored, so no clear is possible -- which is the correct failure: an
 * approach the lock cannot range is an approach it cannot vouch for.
 *
 * @param range_mm Negative when there is no trusted range.
 */
void witness_link_set_range_mm(int32_t range_mm);

/** Credential session up/down. Rotates the challenge and resets the picker. */
void witness_link_session(bool up);

/** Rotate the challenge nonce if due and re-send it. Call from the main loop. */
void witness_link_tick(int64_t now_ms);

/** True when at least one witness has reported inside the staleness bound. */
bool witness_link_healthy(int64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* WITNESS_LINK_H */
