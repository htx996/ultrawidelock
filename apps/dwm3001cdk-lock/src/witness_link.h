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

/** Open the UDP socket and load witness keys. Safe to call before Thread is up. */
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
