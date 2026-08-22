/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_seal.h — the sealed link's AES-CCM envelope, once.
 *
 * ONE definition of what a sealed datagram looks like on the wire:
 *
 *     nonce (13 B) ‖ ciphertext (plain_len B) ‖ tag (8 B)
 *
 * This existed twice — apps/satellite/src/anchor_link.c and
 * apps/dwm3001cdk-lock/src/witness_link.c each carried a copy — and the ESP32
 * satellite would have made three. Two copies of an envelope are two chances
 * for the nonce layout, the tag length or the associated-data choice to drift,
 * and the failure that drift produces is a peer whose every report is silently
 * rejected: indistinguishable, from the bench, from a radio that is not there.
 *
 * WHAT IS NOT HERE, deliberately. Freshness. This layer proves the key and
 * nothing else. The counter, the boot id, the replay window and the challenge
 * echo are the caller's, because only the caller knows which peer a datagram
 * claims to be from — see ultrawidelock_witness_seen in ultrawidelock_witness_msg.h.
 * A datagram that unseals is authentic, not fresh.
 *
 * NONCE UNIQUENESS is a caller obligation and the sharpest edge here. AES-CCM
 * under a repeated (key, nonce) pair leaks the XOR of the two plaintexts and
 * forfeits the tag's unforgeability, so ultrawidelock_seal_nonce() composes the
 * one layout both ends agree on — role, boot id, counter, zeros — and the
 * caller must never seal twice under one counter value. Every existing sender
 * pre-increments its counter and then builds the nonce from the new value.
 *
 * Platform-free apart from psa/crypto.h, the same seam ultrawidelock_prim_psa.c
 * sits on: PSA on nRF, and the mbedTLS-PSA provider on ESP32.
 */

#ifndef ULTRAWIDELOCK_SEAL_H
#define ULTRAWIDELOCK_SEAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Link key width. AES-128, as every peer key on this link is sized. */
#define ULTRAWIDELOCK_SEAL_KEY_LEN 16u

/** Explicit nonce, sent in the clear ahead of the ciphertext. */
#define ULTRAWIDELOCK_SEAL_NONCE_LEN 13u

/**
 * Shortened CCM tag. 8 bytes, not 16: these datagrams must fit one 802.15.4
 * frame, and a forgery attempt costs an attacker a round trip against a
 * receiver that rate-limits nothing but accepts nothing either.
 */
#define ULTRAWIDELOCK_SEAL_TAG_LEN 8u

/** Bytes a seal adds to its plaintext. Size buffers with this, not with 21. */
#define ULTRAWIDELOCK_SEAL_OVERHEAD (ULTRAWIDELOCK_SEAL_NONCE_LEN + ULTRAWIDELOCK_SEAL_TAG_LEN)

/**
 * Compose the nonce both ends agree on: role, boot id, counter, then zeros.
 *
 * The counter makes it unique within a boot and the boot id across boots, so a
 * peer that loses power and restarts its counter at zero is still distinct from
 * a replay of what it sent before. @p out must have room for
 * ULTRAWIDELOCK_SEAL_NONCE_LEN bytes; the trailing bytes are zeroed here rather
 * than left to the caller, because an uninitialised tail is a nonce that
 * repeats only sometimes.
 */
void ultrawidelock_seal_nonce(uint8_t role, uint32_t boot_id, uint32_t ctr, uint8_t *out);

/**
 * Seal @p plain under @p key with @p nonce.
 *
 * @param key   ULTRAWIDELOCK_SEAL_KEY_LEN bytes.
 * @param nonce ULTRAWIDELOCK_SEAL_NONCE_LEN bytes, copied into @p out.
 * @param out   receives nonce ‖ ciphertext ‖ tag.
 * @param cap   must be at least plain_len + ULTRAWIDELOCK_SEAL_OVERHEAD.
 *
 * @return bytes written, or 0 on any failure — too small a buffer, a key the
 *         backend rejects, or an encrypt that did not complete. Never a partial
 *         frame: a caller that checks for 0 cannot send half a datagram.
 */
size_t ultrawidelock_seal(const uint8_t *key, const uint8_t *nonce, const uint8_t *plain,
			  size_t plain_len, uint8_t *out, size_t cap);

/**
 * Unseal @p in under @p key: the exact inverse of ultrawidelock_seal().
 *
 * @param in      nonce ‖ ciphertext ‖ tag, as received.
 * @param out_len receives the plaintext length on success; untouched on failure.
 *
 * @return true only if the tag verified. False covers both "not sealed under
 *         this key" and "tampered with", and the caller must not distinguish
 *         them to the outside world: which one it was is exactly what an
 *         attacker probing keys wants to learn.
 */
bool ultrawidelock_unseal(const uint8_t *key, const uint8_t *in, size_t in_len, uint8_t *out,
			  size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_SEAL_H */
