/* SPDX-License-Identifier: ISC */

/**
 * @file test_ultrawidelock_seal.c — the sealed link's AES-CCM envelope.
 *
 * WHAT THIS CAN AND CANNOT PROVE. The backend here is psafake, which does no
 * crypto: it records what it was handed and serves filler. So nothing below
 * asserts that a ciphertext is correct. What it asserts is the whole of what
 * this file's bugs would look like — the wrong nonce bytes, the wrong tag
 * length, associated data that silently became NULL/0 on one side only, a key
 * left alive in the backend, a buffer accepted that a real backend would
 * overrun. Every one of those produces a peer that rejects every datagram, and
 * on a bench that is indistinguishable from a radio that is not there.
 *
 * THE NONCE IS THE POINT. AES-CCM under a repeated (key, nonce) pair leaks the
 * XOR of two plaintexts and forfeits the tag. The layout is pinned byte for
 * byte here, and so is the property the two directions rely on: the lock's
 * 0xFF role prefix cannot collide with a conforming satellite's 1..3.
 */
#include "test.h"

#include "psafake.h"
#include "ultrawidelock_seal.h"

#include <psa/crypto.h> /* the fake's, for the spellings asserted below */
#include <string.h>

/* psafake derives its filler tag width from the algorithm, the way a real
 * backend does, so a sealed frame here is exactly as long as one the firmware
 * produces and the buffer arithmetic below is the firmware's own. */
#define FAKE_TAG ULTRAWIDELOCK_SEAL_TAG_LEN

static const uint8_t KEY[ULTRAWIDELOCK_SEAL_KEY_LEN] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

void test_ultrawidelock_seal(void)
{
	uint8_t nonce[ULTRAWIDELOCK_SEAL_NONCE_LEN];
	uint8_t nonce2[ULTRAWIDELOCK_SEAL_NONCE_LEN];
	uint8_t plain[24];
	uint8_t out[ULTRAWIDELOCK_SEAL_NONCE_LEN + sizeof(plain) + FAKE_TAG];
	uint8_t back[sizeof(plain) + FAKE_TAG];
	size_t n;
	size_t back_len;
	bool ok;

	for (size_t i = 0; i < sizeof(plain); i++) {
		plain[i] = (uint8_t)(0xA0u + i);
	}

	t_group("nonce: exact layout");
	memset(nonce, 0xEE, sizeof(nonce));
	ultrawidelock_seal_nonce(2u, 0x01020304u, 0x05060708u, nonce);
	t_vec("nonce role=2 boot=01020304 ctr=05060708", nonce, sizeof(nonce),
	      "02010203040506070800000000");
	/* The tail is zeroed HERE, not left to the caller: an uninitialised tail
	 * is a nonce that repeats only sometimes, which is the hardest version of
	 * this bug to ever see. The 0xEE pre-fill above is what makes that
	 * checkable rather than assumed. */
	T_EQ("nonce tail zeroed (byte 9)", nonce[9], 0);
	T_EQ("nonce tail zeroed (byte 12)", nonce[12], 0);

	t_group("nonce: the two directions cannot collide");
	/*
	 * ONE key, two senders. They are disjoint in byte 0 and nowhere else:
	 * the satellite writes its role (apps/satellite/Kconfig pins
	 * ULTRAWIDELOCK_ANCHOR_ROLE to `range 1 3`) and the lock writes 0xFF.
	 * Worst case for the rest of the nonce — same boot id, same counter —
	 * and they must still differ.
	 */
	ultrawidelock_seal_nonce(3u, 0u, 0u, nonce);
	ultrawidelock_seal_nonce(0xFFu, 0u, 0u, nonce2);
	T_OK("lock and satellite nonces differ at worst case",
	     memcmp(nonce, nonce2, sizeof(nonce)) != 0);
	T_EQ("and they differ in byte 0", nonce[0] != nonce2[0], 1);

	t_group("nonce: NULL is survivable");
	ultrawidelock_seal_nonce(1u, 1u, 1u, NULL); /* must not fault */
	T_OK("NULL out did not fault", 1);

	t_group("seal: argument plumbing");
	psafake_reset();
	ultrawidelock_seal_nonce(1u, 0xDEADBEEFu, 7u, nonce);
	n = ultrawidelock_seal(KEY, nonce, plain, sizeof(plain), out, sizeof(out));
	T_EQ("sealed length", n, ULTRAWIDELOCK_SEAL_NONCE_LEN + sizeof(plain) + FAKE_TAG);
	T_EQ("one encrypt", psafake.aead_enc_calls, 1);
	T_EQ("alg is CCM with the shortened tag", psafake.last_alg,
	     PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, ULTRAWIDELOCK_SEAL_TAG_LEN));
	T_EQ("nonce length", psafake.last_nonce_len, ULTRAWIDELOCK_SEAL_NONCE_LEN);
	/* No associated data, on purpose and on BOTH sides. A seal with AAD and
	 * an unseal without it verifies nothing and rejects everything. */
	T_EQ("no associated data", psafake.last_aad_len, 0);
	T_EQ("plaintext length", psafake.last_in_len, sizeof(plain));
	T_EQ("key type is AES", psafake.attr_type, PSA_KEY_TYPE_AES);
	T_EQ("key bits", psafake.attr_bits, ULTRAWIDELOCK_SEAL_KEY_LEN * 8u);
	T_EQ("usage is encrypt", psafake.attr_usage, PSA_KEY_USAGE_ENCRYPT);
	T_EQ("key material forwarded whole", psafake.key_len, ULTRAWIDELOCK_SEAL_KEY_LEN);
	T_OK("key bytes forwarded unchanged",
	     memcmp(psafake.key, KEY, ULTRAWIDELOCK_SEAL_KEY_LEN) == 0);
	/* The backend must not be left holding the link key. */
	T_EQ("key destroyed", psafake.destroy_calls, 1);

	t_group("seal: the nonce goes out in the clear, first");
	T_OK("output starts with the nonce", memcmp(out, nonce, sizeof(nonce)) == 0);

	t_group("seal: refuses rather than truncates");
	psafake_reset();
	/* Exactly one byte short of what a REAL backend needs. The firmware sizes
	 * its buffers from ULTRAWIDELOCK_SEAL_OVERHEAD, so this is the boundary
	 * that matters even though the fake's tag is wider. */
	n = ultrawidelock_seal(KEY, nonce, plain, sizeof(plain), out,
			       ULTRAWIDELOCK_SEAL_NONCE_LEN + sizeof(plain) +
				       ULTRAWIDELOCK_SEAL_TAG_LEN - 1u);
	T_EQ("cap one byte short -> 0", n, 0);
	T_EQ("and no key was imported", psafake.import_calls, 0);
	T_EQ("and nothing was encrypted", psafake.aead_enc_calls, 0);

	psafake_reset();
	T_EQ("NULL key -> 0", ultrawidelock_seal(NULL, nonce, plain, sizeof(plain), out,
						sizeof(out)), 0);
	T_EQ("NULL nonce -> 0", ultrawidelock_seal(KEY, NULL, plain, sizeof(plain), out,
						  sizeof(out)), 0);
	T_EQ("NULL out -> 0", ultrawidelock_seal(KEY, nonce, plain, sizeof(plain), NULL,
						 sizeof(out)), 0);
	T_EQ("no backend call on any NULL", psafake.import_calls, 0);

	t_group("seal: a failing backend yields no frame");
	psafake_reset();
	psafake.import_ret = -1;
	T_EQ("import failure -> 0",
	     ultrawidelock_seal(KEY, nonce, plain, sizeof(plain), out, sizeof(out)), 0);
	psafake_reset();
	psafake.aead_enc_ret = -1;
	T_EQ("encrypt failure -> 0",
	     ultrawidelock_seal(KEY, nonce, plain, sizeof(plain), out, sizeof(out)), 0);
	/* Even on the failure path the key must not survive in the backend. */
	T_EQ("key still destroyed on failure", psafake.destroy_calls, 1);

	t_group("unseal: argument plumbing");
	psafake_reset();
	n = ultrawidelock_seal(KEY, nonce, plain, sizeof(plain), out, sizeof(out));
	psafake_reset();
	back_len = 0;
	ok = ultrawidelock_unseal(KEY, out, n, back, sizeof(back), &back_len);
	T_EQ("unseal accepts what seal produced", ok, 1);
	T_EQ("one decrypt", psafake.aead_dec_calls, 1);
	T_EQ("alg matches the seal's exactly", psafake.last_alg,
	     PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, ULTRAWIDELOCK_SEAL_TAG_LEN));
	T_EQ("nonce length", psafake.last_nonce_len, ULTRAWIDELOCK_SEAL_NONCE_LEN);
	T_EQ("no associated data", psafake.last_aad_len, 0);
	/* The nonce is stripped before the ciphertext is handed over — an
	 * off-by-13 here decrypts the nonce as ciphertext and never verifies. */
	T_EQ("ciphertext length excludes the nonce", psafake.last_in_len,
	     n - ULTRAWIDELOCK_SEAL_NONCE_LEN);
	T_EQ("usage is decrypt", psafake.attr_usage, PSA_KEY_USAGE_DECRYPT);
	T_EQ("key destroyed", psafake.destroy_calls, 1);

	t_group("unseal: round trip returns the plaintext");
	T_EQ("plaintext length", back_len, sizeof(plain));
	T_OK("plaintext bytes", memcmp(back, plain, sizeof(plain)) == 0);

	t_group("unseal: refuses a frame with no room for a payload");
	psafake_reset();
	/* Nonce + tag and nothing else carries nothing, and every message on this
	 * link is fixed-width. Strictly greater, so the empty case is refused. */
	T_EQ("exactly overhead -> false",
	     ultrawidelock_unseal(KEY, out, ULTRAWIDELOCK_SEAL_OVERHEAD, back, sizeof(back),
				  &back_len), 0);
	T_EQ("one byte under overhead -> false",
	     ultrawidelock_unseal(KEY, out, ULTRAWIDELOCK_SEAL_OVERHEAD - 1u, back,
				  sizeof(back), &back_len), 0);
	T_EQ("zero length -> false",
	     ultrawidelock_unseal(KEY, out, 0, back, sizeof(back), &back_len), 0);
	T_EQ("and no key was imported", psafake.import_calls, 0);

	T_EQ("overhead+1 reaches the backend",
	     ultrawidelock_unseal(KEY, out, ULTRAWIDELOCK_SEAL_OVERHEAD + 1u, back,
				  sizeof(back), &back_len), 1);
	T_EQ("import happened for it", psafake.import_calls, 1);

	psafake_reset();
	T_EQ("NULL key -> false",
	     ultrawidelock_unseal(NULL, out, n, back, sizeof(back), &back_len), 0);
	T_EQ("NULL in -> false",
	     ultrawidelock_unseal(KEY, NULL, n, back, sizeof(back), &back_len), 0);
	T_EQ("NULL out -> false",
	     ultrawidelock_unseal(KEY, out, n, NULL, sizeof(back), &back_len), 0);
	T_EQ("NULL out_len -> false",
	     ultrawidelock_unseal(KEY, out, n, back, sizeof(back), NULL), 0);
	T_EQ("no backend call on any NULL", psafake.import_calls, 0);

	t_group("unseal: a failing backend is a rejection, not a pass");
	psafake_reset();
	psafake.import_ret = -1;
	T_EQ("import failure -> false",
	     ultrawidelock_unseal(KEY, out, n, back, sizeof(back), &back_len), 0);
	psafake_reset();
	psafake.aead_dec_ret = -1;
	T_EQ("tag failure -> false",
	     ultrawidelock_unseal(KEY, out, n, back, sizeof(back), &back_len), 0);
	T_EQ("key still destroyed on failure", psafake.destroy_calls, 1);

	t_group("envelope: the constants the firmware sizes buffers with");
	T_EQ("key len", ULTRAWIDELOCK_SEAL_KEY_LEN, 16);
	T_EQ("nonce len", ULTRAWIDELOCK_SEAL_NONCE_LEN, 13);
	T_EQ("tag len", ULTRAWIDELOCK_SEAL_TAG_LEN, 8);
	T_EQ("overhead is their sum", ULTRAWIDELOCK_SEAL_OVERHEAD, 21);

	psafake_reset();
}
