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
 * The key is imported and destroyed per call rather than held as a key id. It
 * costs a few microseconds per datagram on a link that carries one report per
 * ranging block, and it buys a backend that holds no key material between
 * calls: nothing to leak in a crash dump, and no id to go stale across a
 * re-provision.
 */

#include "ultrawidelock_seal.h"

#include <string.h>

#include <psa/crypto.h>

/** PSA_ALG_CCM with our shortened tag. One expression, so the two halves cannot disagree. */
#define SEAL_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, ULTRAWIDELOCK_SEAL_TAG_LEN)

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
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	size_t ct_len = 0;
	psa_status_t st;

	if (key == NULL || nonce == NULL || out == NULL) {
		return 0;
	}
	if (cap < ULTRAWIDELOCK_SEAL_NONCE_LEN + plain_len + ULTRAWIDELOCK_SEAL_TAG_LEN) {
		return 0;
	}
	memcpy(out, nonce, ULTRAWIDELOCK_SEAL_NONCE_LEN);

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, SEAL_ALG);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, ULTRAWIDELOCK_SEAL_KEY_LEN * 8u);
	if (psa_import_key(&attr, key, ULTRAWIDELOCK_SEAL_KEY_LEN, &kid) != PSA_SUCCESS) {
		return 0;
	}
	st = psa_aead_encrypt(kid, SEAL_ALG, out, ULTRAWIDELOCK_SEAL_NONCE_LEN, NULL, 0, plain,
			      plain_len, out + ULTRAWIDELOCK_SEAL_NONCE_LEN,
			      cap - ULTRAWIDELOCK_SEAL_NONCE_LEN, &ct_len);
	(void)psa_destroy_key(kid);
	if (st != PSA_SUCCESS) {
		return 0;
	}
	return ULTRAWIDELOCK_SEAL_NONCE_LEN + ct_len;
}

bool ultrawidelock_unseal(const uint8_t *key, const uint8_t *in, size_t in_len, uint8_t *out,
			  size_t out_cap, size_t *out_len)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t kid = PSA_KEY_ID_NULL;
	psa_status_t st;

	if (key == NULL || in == NULL || out == NULL || out_len == NULL) {
		return false;
	}
	/* Strictly greater: a frame with a nonce and a tag and no ciphertext
	 * carries nothing, and every message on this link is fixed-width. */
	if (in_len <= ULTRAWIDELOCK_SEAL_NONCE_LEN + ULTRAWIDELOCK_SEAL_TAG_LEN) {
		return false;
	}
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, SEAL_ALG);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, ULTRAWIDELOCK_SEAL_KEY_LEN * 8u);
	if (psa_import_key(&attr, key, ULTRAWIDELOCK_SEAL_KEY_LEN, &kid) != PSA_SUCCESS) {
		return false;
	}
	st = psa_aead_decrypt(kid, SEAL_ALG, in, ULTRAWIDELOCK_SEAL_NONCE_LEN, NULL, 0,
			      in + ULTRAWIDELOCK_SEAL_NONCE_LEN,
			      in_len - ULTRAWIDELOCK_SEAL_NONCE_LEN, out, out_cap, out_len);
	(void)psa_destroy_key(kid);
	return st == PSA_SUCCESS;
}
