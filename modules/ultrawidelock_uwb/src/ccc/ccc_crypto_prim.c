/* SPDX-License-Identifier: ISC */

/** @file ccc_crypto_prim.c — CCC/Matter AES-ECB adapter over ultrawidelock_prim. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "ccc_kdf.h"
#include "ultrawidelock_prim.h"

int crypto_aes_ecb_encrypt(const uint8_t *key, size_t key_bits, const uint8_t in[16],
			   uint8_t out[16])
{
	if (key == NULL || in == NULL || out == NULL ||
	    (key_bits != 128u && key_bits != 256u)) {
		return -EINVAL;
	}

	return ultrawidelock_aes_ecb_encrypt(key, key_bits, in, out) == 0 ? 0 : -EIO;
}
