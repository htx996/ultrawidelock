/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_satlink.h — the sealed anchor link over ESP-NOW.
 *
 * The carrier half of what apps/satellite/src/anchor_link.c and
 * apps/dwm3001cdk-lock/src/witness_link.c do over Thread UDP. Every decision —
 * what to send, what an arriving datagram is, whether to believe it — belongs
 * to ultrawidelock_link.h and is shared with those; this file moves bytes and
 * keeps the key.
 *
 * WHY ESP-NOW. The Thread transport cannot come along: the ESP32-S3 has no
 * 802.15.4 radio. ESP-NOW carries a datagram between two boards with no AP, no
 * association and no IP stack, which is the whole of what this link needs. The
 * sealed frame is byte-identical to the Thread one — the carrier is a pipe, and
 * a mixed bench (nRF lock, ESP32 satellite) stays possible because of it.
 *
 * ESP-NOW's own crypto is deliberately NOT used. The seal is ours, end to end,
 * under a key the two boards share and the carrier never sees; turning on
 * ESP-NOW encryption as well would add a second key to provision and protect
 * nothing that is not already protected.
 *
 * ONE ROLE PER IMAGE. A satellite reports and receives handoffs; a lock
 * receives reports and sends handoffs. Both use this file; which callbacks are
 * registered is what makes an image one or the other.
 */

#ifndef ULTRAWIDELOCK_SATLINK_H
#define ULTRAWIDELOCK_SATLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A fresh, authenticated report from the far anchor.
 *
 * Runs on the Wi-Fi task, like the Thread port's equivalent runs on the
 * OpenThread RX thread. Do the least possible here.
 *
 * @param role the sender's mounting side, from inside the seal.
 * @param ranging_block the block the distance was measured in. The lock cannot
 *        pair a distance whose round it does not know.
 */
typedef void (*ultrawidelock_satlink_report_cb)(uint8_t role, int32_t peer_mm,
						uint16_t ranging_block);

/**
 * A fresh, authenticated session handoff from the lock.
 *
 * @param ursk ULTRAWIDELOCK_JOIN_URSK_LEN bytes, @param rcfg
 *        ULTRAWIDELOCK_JOIN_RCFG_LEN bytes — borrowed for the call only; the
 *        copies here are wiped as soon as it returns.
 *
 * The only message this board ACTS on rather than merely records, so what
 * protects it is the key and the counter.
 */
typedef void (*ultrawidelock_satlink_join_cb)(const uint8_t *ursk, const uint8_t *rcfg,
					      uint8_t channel, uint8_t sync_code_index);

/** Register the report sink (the lock's direction). Set before init. */
void ultrawidelock_satlink_set_report_cb(ultrawidelock_satlink_report_cb cb);

/** Register the handoff sink (the satellite's direction). Set before init. */
void ultrawidelock_satlink_set_join_cb(ultrawidelock_satlink_join_cb cb);

/**
 * Bring up Wi-Fi and ESP-NOW, load the stored link key, pick a boot id.
 *
 * Safe to call with no key stored: the link comes up and reports stay off,
 * and it says so on the console — an anchor that ranges perfectly and reports
 * nothing looks exactly like one that never booted.
 *
 * On a satellite this also starts the Wi-Fi channel hunt: ESP-NOW only
 * crosses between boards sharing a channel, the lock sits wherever its AP
 * put it, and the satellite sweeps until the lock answers a probe (see the
 * CHANNEL DISCOVERY comment in the .c). The found channel persists in NVS.
 *
 * @param role this node's mounting side, 1..3
 *        (enum ultrawidelock_witness_role) -- a MOUNTING FACT: getting it
 *        backwards fails no test and silently inverts the side verdict -- or
 *        ULTRAWIDELOCK_LINK_HANDOFF_ROLE on the lock, which answers channel
 *        probes and never retunes the radio it shares with Matter.
 * @return 0 on success, or an esp_err_t.
 */
int ultrawidelock_satlink_init(uint8_t role);

/**
 * Where the channel hunt stands: the channel the lock was found on, 0 while
 * still hunting, -1 where no hunt runs (the lock, or a borrowed radio).
 */
int ultrawidelock_satlink_channel(void);

/** Store the 16-byte link key in NVS and install it. @return 0 or -1. */
int ultrawidelock_satlink_set_key(const uint8_t *key, size_t len);

/** True once ESP-NOW is up and a key is installed. */
bool ultrawidelock_satlink_ready(void);

/**
 * Seal and broadcast one report.
 *
 * @param peer_mm this anchor's distance to the phone. Negative is dropped
 *        rather than sent.
 */
void ultrawidelock_satlink_report(int32_t peer_mm, uint32_t ranging_block);

/** Seal and broadcast one session handoff (the lock's direction). */
void ultrawidelock_satlink_send_handoff(const uint8_t *ursk, const uint8_t *rcfg, uint8_t channel,
					uint8_t sync_code_index);

/** Broadcast the unsealed freshness beacon (the lock's direction). */
void ultrawidelock_satlink_challenge(uint64_t nonce);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_SATLINK_H */
