/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_link.c — the sealed peer link's decisions, carrier-free.
 *
 * The rules here are the ones apps/satellite/src/anchor_link.c and
 * apps/dwm3001cdk-lock/src/witness_link.c already enforce, lifted out of their
 * transports unchanged. Every byte on the wire is what those two exchange
 * today; see ultrawidelock_link.h for why that is a requirement and not a
 * coincidence.
 */

#include "ultrawidelock_link.h"

#include <string.h>

/* Length of a sealed frame carrying an N-byte plaintext. Every message on this
 * link is fixed-width, so an arriving length identifies the message type before
 * anything is decrypted — and a length that matches nothing is discarded
 * without touching a key. */
#define SEALED(plain_len) (ULTRAWIDELOCK_SEAL_OVERHEAD + (plain_len))

/*
 * consume() indexes report_peer[] and report_epoch[] with (role - 1) on the
 * strength of the codec having range-checked the role. That is only sound
 * while the codec's highest role and this module's array bound are the same
 * number, and they are declared in two different headers -- so tie them here
 * rather than leave the next widening to notice on its own.
 */
_Static_assert(ULTRAWIDELOCK_LINK_ROLE_MIN == ULTRAWIDELOCK_WITNESS_ROLE_INSIDE,
	       "the link's lowest satellite role must be the codec's lowest");
_Static_assert(ULTRAWIDELOCK_LINK_ROLE_MAX == ULTRAWIDELOCK_WITNESS_ROLE_THRESHOLD,
	       "per-role array bound must match the highest role the codec admits");
_Static_assert(ULTRAWIDELOCK_LINK_HANDOFF_ROLE > ULTRAWIDELOCK_LINK_ROLE_MAX,
	       "the lock's nonce role must stay outside every satellite's");

void ultrawidelock_link_init(struct ultrawidelock_link *l, uint8_t role, uint32_t boot_id)
{
	if (l == NULL) {
		return;
	}
	memset(l, 0, sizeof(*l));
	l->role = role;
	l->boot_id = boot_id;
}

int ultrawidelock_link_set_key(struct ultrawidelock_link *l, const uint8_t *key, size_t len)
{
	if (l == NULL || key == NULL || len != ULTRAWIDELOCK_SEAL_KEY_LEN) {
		return -1;
	}
	memcpy(l->key, key, ULTRAWIDELOCK_SEAL_KEY_LEN);
	l->provisioned = true;
	return 0;
}

bool ultrawidelock_link_ready(const struct ultrawidelock_link *l)
{
	return l != NULL && l->provisioned;
}

/**
 * Seal @p plain under our key with the nonce for @p nonce_role and the NEXT
 * counter value, advancing the counter only if the whole thing succeeded.
 *
 * The counter must not advance on a failed send. It is a nonce input, and
 * burning values on failures is harmless; what is NOT harmless is the opposite
 * mistake, so the increment and the nonce are computed from one local here
 * rather than from the struct at two different moments.
 */
static size_t seal_next(struct ultrawidelock_link *l, uint8_t nonce_role, const uint8_t *plain,
			size_t plain_len, uint8_t *out, size_t cap)
{
	uint8_t nonce[ULTRAWIDELOCK_SEAL_NONCE_LEN];
	uint32_t next = l->ctr + 1u;
	size_t n;

	ultrawidelock_seal_nonce(nonce_role, l->boot_id, next, nonce);
	n = ultrawidelock_seal(l->key, nonce, plain, plain_len, out, cap);
	if (n != 0u) {
		l->ctr = next;
	}
	return n;
}

size_t ultrawidelock_link_build_report(struct ultrawidelock_link *l, int32_t peer_mm,
				       uint32_t ranging_block, uint8_t *out, size_t cap)
{
	struct ultrawidelock_anchor_msg am;
	uint8_t plain[ULTRAWIDELOCK_ANCHOR_MSG_LEN];
	size_t plain_len;
	size_t n;

	if (l == NULL || out == NULL || !l->provisioned || peer_mm < 0) {
		return 0;
	}
	if (l->role < ULTRAWIDELOCK_LINK_ROLE_MIN || l->role > ULTRAWIDELOCK_LINK_ROLE_MAX) {
		return 0;
	}
	if (cap < SEALED(ULTRAWIDELOCK_ANCHOR_MSG_LEN)) {
		return 0;
	}

	memset(&am, 0, sizeof(am));
	am.ver = ULTRAWIDELOCK_ANCHOR_MSG_VER;
	am.role = l->role;
	am.boot_id = l->boot_id;
	am.ctr = l->ctr + 1u; /* must match the nonce seal_next() builds */
	am.echo_nonce = l->tx_echo_nonce;
	am.ranging_block = (uint16_t)ranging_block;
	am.peer_mm = peer_mm;

	plain_len = ultrawidelock_anchor_msg_encode(&am, plain, sizeof(plain));
	if (plain_len == 0u) {
		return 0;
	}
	n = seal_next(l, l->role, plain, plain_len, out, cap);
	memset(plain, 0, sizeof(plain));
	return n;
}

size_t ultrawidelock_link_build_join(struct ultrawidelock_link *l, const uint8_t *ursk,
				     const uint8_t *rcfg, uint8_t channel,
				     uint8_t sync_code_index, uint8_t *out, size_t cap)
{
	struct ultrawidelock_join_msg jm;
	uint8_t plain[ULTRAWIDELOCK_JOIN_MSG_LEN];
	size_t plain_len;
	size_t n;

	if (l == NULL || out == NULL || ursk == NULL || rcfg == NULL || !l->provisioned) {
		return 0;
	}
	if (cap < SEALED(ULTRAWIDELOCK_JOIN_MSG_LEN)) {
		return 0;
	}

	memset(&jm, 0, sizeof(jm));
	jm.ver = ULTRAWIDELOCK_JOIN_MSG_VER;
	jm.boot_id = l->boot_id;
	jm.ctr = l->ctr + 1u; /* must match the nonce seal_next() builds */
	memcpy(jm.ursk, ursk, ULTRAWIDELOCK_JOIN_URSK_LEN);
	memcpy(jm.rcfg, rcfg, ULTRAWIDELOCK_JOIN_RCFG_LEN);
	jm.channel = channel;
	jm.sync_code_index = sync_code_index;

	plain_len = ultrawidelock_join_msg_encode(&jm, plain, sizeof(plain));
	if (plain_len == 0u) {
		memset(&jm, 0, sizeof(jm));
		return 0;
	}
	n = seal_next(l, ULTRAWIDELOCK_LINK_HANDOFF_ROLE, plain, plain_len, out, cap);
	/* The URSK is in both of these. Neither may outlive the call. */
	memset(plain, 0, sizeof(plain));
	memset(&jm, 0, sizeof(jm));
	return n;
}

size_t ultrawidelock_link_build_challenge(uint64_t nonce, uint8_t *out, size_t cap)
{
	if (out == NULL || cap < ULTRAWIDELOCK_LINK_CHALLENGE_LEN) {
		return 0;
	}
	out[0] = ULTRAWIDELOCK_WITNESS_MSG_VER;
	for (int i = 0; i < 8; i++) {
		out[1 + i] = (uint8_t)(nonce >> (56 - 8 * i));
	}
	return ULTRAWIDELOCK_LINK_CHALLENGE_LEN;
}

size_t ultrawidelock_link_build_challenge_hint(uint64_t nonce, uint32_t hint, uint8_t *out,
				       size_t cap)
{
	if (out == NULL || cap < ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN) {
		return 0;
	}
	(void)ultrawidelock_link_build_challenge(nonce, out, cap);
	out[9] = (uint8_t)(hint >> 16);
	out[10] = (uint8_t)(hint >> 8);
	out[11] = (uint8_t)hint;
	return ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN;
}

/**
 * Refuse a report that unsealed and decoded but is not fresh.
 *
 * Clears the out-parameter and keeps only role and counter, the two
 * non-sensitive fields the carriers' existing replay diagnostic prints. The
 * distance never survives a rejection: a caller that forgets to check the
 * return value must not find a believable number waiting for it.
 */
static enum ultrawidelock_link_rx report_rejected(struct ultrawidelock_anchor_msg *am)
{
	uint8_t role = am->role;
	uint32_t ctr = am->ctr;

	memset(am, 0, sizeof(*am));
	am->role = role;
	am->ctr = ctr;
	return ULTRAWIDELOCK_LINK_RX_REPLAYED;
}

void ultrawidelock_link_expect_echo(struct ultrawidelock_link *l, uint64_t nonce)
{
	if (l == NULL) {
		return;
	}
	l->expected_echo_nonce = nonce;
}

enum ultrawidelock_link_rx ultrawidelock_link_consume(struct ultrawidelock_link *l,
						      const uint8_t *in, size_t len,
						      struct ultrawidelock_anchor_msg *am,
						      struct ultrawidelock_join_msg *jm)
{
	uint8_t plain[ULTRAWIDELOCK_JOIN_MSG_LEN];
	size_t plain_len = 0;
	enum ultrawidelock_link_rx rc;

	if (l == NULL || in == NULL) {
		return ULTRAWIDELOCK_LINK_RX_IGNORED;
	}

	/* The challenge is unsealed by design and is the only thing read before
	 * the key is consulted. It can set no state but the OUTGOING echo nonce,
	 * and an outgoing echo nonce can only ever cost our own next report its
	 * standing. What we require of an arriving report is expected_echo_nonce,
	 * which nothing on the wire can reach. */
	if ((len == ULTRAWIDELOCK_LINK_CHALLENGE_LEN ||
	     len == ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN) &&
	    in[0] == ULTRAWIDELOCK_WITNESS_MSG_VER) {
		uint64_t n = 0u;

		for (int i = 0; i < 8; i++) {
			n = (n << 8) | (uint64_t)in[1 + i];
		}
		l->tx_echo_nonce = n;
		return ULTRAWIDELOCK_LINK_RX_CHALLENGE;
	}

	if (!l->provisioned) {
		return ULTRAWIDELOCK_LINK_RX_IGNORED;
	}

	/*
	 * Fixed widths pick the message. Our own broadcast coming back off a
	 * shared carrier lands here too and is discarded by the same rule: a
	 * report is not the length of a handoff, so a satellite hearing itself
	 * never mistakes it for input.
	 */
	if (len == SEALED(ULTRAWIDELOCK_ANCHOR_MSG_LEN)) {
		struct ultrawidelock_witness_seen *seen;
		size_t idx;

		if (am == NULL) {
			return ULTRAWIDELOCK_LINK_RX_IGNORED;
		}
		if (!ultrawidelock_unseal(l->key, in, len, plain, sizeof(plain), &plain_len)) {
			return ULTRAWIDELOCK_LINK_RX_UNSEALED;
		}
		if (!ultrawidelock_anchor_msg_decode(plain, plain_len, am)) {
			memset(plain, 0, sizeof(plain));
			return ULTRAWIDELOCK_LINK_RX_MALFORMED;
		}
		memset(plain, 0, sizeof(plain));
		/* anchor_msg_decode() enforces the nonce-safe role range before
		 * populating am, so indexing the per-role state is safe here. */
		idx = (size_t)(am->role - 1u);
		seen = &l->report_peer[idx];
		/*
		 * ORDER: freshness epoch, then the counter window. Both reject the
		 * same way, and both run only after the seal, so neither is
		 * reachable by anyone without the link key.
		 */
		if (l->expected_echo_nonce != 0u) {
			/* Produced during some other epoch, or none: a recording,
			 * however well sealed. */
			if (am->echo_nonce != l->expected_echo_nonce) {
				return report_rejected(am);
			}
			/*
			 * A boot id change resets the counter window, which is
			 * exactly what a captured older boot needs. Admit it once
			 * per epoch: a satellite that really rebooted is believed
			 * on the next challenge, and a replayed old boot is not
			 * believed at all while this one stands.
			 */
			if (seen->have && seen->boot_id != am->boot_id &&
			    l->report_epoch[idx] == l->expected_echo_nonce) {
				return report_rejected(am);
			}
		}
		if (!ultrawidelock_seen_accept_ctr(seen, am->boot_id, am->ctr)) {
			return report_rejected(am);
		}
		l->report_epoch[idx] = l->expected_echo_nonce;
		return ULTRAWIDELOCK_LINK_RX_REPORT;
	}

	if (len == SEALED(ULTRAWIDELOCK_JOIN_MSG_LEN)) {
		if (jm == NULL) {
			return ULTRAWIDELOCK_LINK_RX_IGNORED;
		}
		if (!ultrawidelock_unseal(l->key, in, len, plain, sizeof(plain), &plain_len)) {
			return ULTRAWIDELOCK_LINK_RX_UNSEALED;
		}
		memset(jm, 0, sizeof(*jm));
		rc = ULTRAWIDELOCK_LINK_RX_JOIN;
		if (!ultrawidelock_join_msg_decode(plain, plain_len, jm)) {
			rc = ULTRAWIDELOCK_LINK_RX_MALFORMED;
		} else if (!ultrawidelock_seen_accept_ctr(&l->join_peer, jm->boot_id, jm->ctr)) {
			rc = ULTRAWIDELOCK_LINK_RX_REPLAYED;
		}
		/* Carries a URSK either way: clear the staging buffer, and the
		 * out-parameter too unless the caller is about to use it. */
		memset(plain, 0, sizeof(plain));
		if (rc != ULTRAWIDELOCK_LINK_RX_JOIN) {
			uint32_t ctr = jm->ctr;

			memset(jm, 0, sizeof(*jm));
			/* Preserve the diagnostic counter, never the rejected URSK. */
			if (rc == ULTRAWIDELOCK_LINK_RX_REPLAYED) {
				jm->ctr = ctr;
			}
		}
		return rc;
	}

	return ULTRAWIDELOCK_LINK_RX_IGNORED;
}
