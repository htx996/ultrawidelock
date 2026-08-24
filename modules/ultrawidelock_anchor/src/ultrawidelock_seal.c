/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_seal.c — AES-CCM seal/unseal for the sealed peer link.
 *
 * Lifted verbatim from the two copies it replaces (anchor_link.c's seal/unseal
 * and witness_link.c's seal_key/unseal_key), so the bytes on the wire are the
 * bytes both nRF boards already exchange. A mixed bench — nRF lock, ESP32
 * satellite — depends on that being true, and nothing here may be "improved"
 * without changing it on both ends at once.
 *
 * Key lifetime and provider selection belong to the primitive seam. This
 * layer owns only the wire envelope and delegates AES-128-CCM to that seam, so
 * portable link code never reaches a platform crypto API directly.
 */

#include "ultrawidelock_seal.h"
#include "ultrawidelock_prim.h"

#include <string.h>

_Static_assert(ULTRAWIDELOCK_SEAL_KEY_LEN == 16u,
	       "the primitive provider implements AES-128-CCM");
_Static_assert(ULTRAWIDELOCK_SEAL_TAG_LEN <= ULTRAWIDELOCK_CCM_TAG,
	       "the sealed-link tag must fit the primitive provider");

void ultrawidelock_seal_nonce(uint8_t role, uint32_t boot_id, uint32_t ctr, uint8_t *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, ULTRAWIDELOCK_SEAL_NONCE_LEN);
	out[0] = role;
	out[1] = (uint8_t)(boot_id >> 24);
	out[2] = (uint8_t)(boot_id >> 16);
	out[3] = (uint8_t)(boot_id >> 8);
	out[4] = (uint8_t)boot_id;
	out[5] = (uint8_t)(ctr >> 24);
	out[6] = (uint8_t)(ctr >> 16);
	out[7] = (uint8_t)(ctr >> 8);
	out[8] = (uint8_t)ctr;
}

size_t ultrawidelock_seal(const uint8_t *key, const uint8_t *nonce, const uint8_t *plain,
			  size_t plain_len, uint8_t *out, size_t cap)
{
	size_t ct_len = 0;

	if (key == NULL || nonce == NULL || out == NULL) {
		return 0;
	}
	if (cap < ULTRAWIDELOCK_SEAL_NONCE_LEN + plain_len + ULTRAWIDELOCK_SEAL_TAG_LEN) {
		return 0;
	}
	memcpy(out, nonce, ULTRAWIDELOCK_SEAL_NONCE_LEN);
	if (ultrawidelock_aes128_ccm_encrypt(
		    key, out, ULTRAWIDELOCK_SEAL_NONCE_LEN, plain, plain_len,
		    ULTRAWIDELOCK_SEAL_TAG_LEN, out + ULTRAWIDELOCK_SEAL_NONCE_LEN,
		    cap - ULTRAWIDELOCK_SEAL_NONCE_LEN, &ct_len) != 0) {
		return 0;
	}
	return ULTRAWIDELOCK_SEAL_NONCE_LEN + ct_len;
}

bool ultrawidelock_unseal(const uint8_t *key, const uint8_t *in, size_t in_len, uint8_t *out,
			  size_t out_cap, size_t *out_len)
{
	size_t plain_len = 0;

	if (key == NULL || in == NULL || out == NULL || out_len == NULL) {
		return false;
	}
	/* Strictly greater: a frame with a nonce and a tag and no ciphertext
	 * carries nothing, and every message on this link is fixed-width. */
	if (in_len <= ULTRAWIDELOCK_SEAL_NONCE_LEN + ULTRAWIDELOCK_SEAL_TAG_LEN) {
		return false;
	}
	if (ultrawidelock_aes128_ccm_decrypt(
		    key, in, ULTRAWIDELOCK_SEAL_NONCE_LEN,
		    in + ULTRAWIDELOCK_SEAL_NONCE_LEN,
		    in_len - ULTRAWIDELOCK_SEAL_NONCE_LEN, ULTRAWIDELOCK_SEAL_TAG_LEN, out,
		    out_cap, &plain_len) != 0) {
		return false;
	}
	*out_len = plain_len;
	return true;
}
