/* SPDX-License-Identifier: ISC */

/**
 * @file anchor_link.h — the satellite's half of the sealed link to the lock.
 *
 * One sealed WV3 datagram per accepted range. The lock authenticates it,
 * replay-checks it, and pairs it with its own measurement of the SAME ranging
 * block; nothing here decides anything.
 */

#ifndef ANCHOR_LINK_H
#define ANCHOR_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Open the socket, load the link key, and pick a boot id.
 *
 * Safe to call with Thread down or no key stored: reporting simply stays off,
 * and says so on the console, because an anchor that ranges perfectly and
 * reports nothing looks exactly like one that never booted.
 */
void anchor_link_init(void);

/**
 * Seal and send one report.
 *
 * @param peer_mm       this anchor's distance to the phone, millimetres.
 *                      Negative is dropped rather than sent.
 * @param ranging_block the initiator's block it was measured in. Travels with
 *                      the distance because the lock cannot pair a distance
 *                      whose round it does not know.
 */
void anchor_link_report(int32_t peer_mm, uint32_t ranging_block);

/**
 * Join the lock's Thread network from a raw Active Operational Dataset.
 *
 * Mesh membership only -- this board joins no Matter fabric and needs none: a
 * fabric governs who may invoke clusters, while sending UDP to a peer needs
 * nothing but being on the same mesh. OpenThread persists the dataset, so a
 * reflash without --erase keeps it.
 */
int anchor_link_set_dataset(const uint8_t *tlvs, size_t len);

/** True once attached as child, router or leader. */
bool anchor_link_attached(void);

/** Store the link key (16 bytes) and persist it. */
int anchor_link_set_key(const uint8_t *key, size_t len);

/** True when the socket is open and a key is stored. */
bool anchor_link_ready(void);

#endif /* ANCHOR_LINK_H */
