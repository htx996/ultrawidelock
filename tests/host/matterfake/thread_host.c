/* SPDX-License-Identifier: ISC */

/*
 * thread_host — see thread_host.h. The Thread transport, as far as
 * matter_client.c can tell.
 *
 * A recording double, not a simulator: it keeps what was sent and lets the
 * suite decide what comes back. There is no network model in here on purpose,
 * because a wrong one would be a second implementation of the protocol and the
 * suite would end up testing it instead of the firmware.
 */
#include <string.h>

#include "matterfake/thread_host.h"

#include "matter_status.h"
#include "ultrawidelock_prim.h"

/* Deep enough for a whole attempt: Sigma1, Sigma3, TimedRequest, Invoke, and
 * an ack or two, with room to see a retry on top rather than losing it. */
#define TX_MAX 16u

static struct matterfake_tx s_tx[TX_MAX];
static unsigned s_tx_n;
static unsigned s_fail_sends;

static matter_thread_resolve_fn s_resolve_cb;
static void *s_resolve_ctx;
static char s_resolve_name[64];
static bool s_resolve_armed;
static unsigned s_resolve_n;
static bool s_fail_resolve;

void matterfake_thread_reset(void)
{
	memset(s_tx, 0, sizeof(s_tx));
	s_tx_n = 0u;
	s_fail_sends = 0u;
	s_resolve_cb = NULL;
	s_resolve_ctx = NULL;
	memset(s_resolve_name, 0, sizeof(s_resolve_name));
	s_resolve_armed = false;
	s_resolve_n = 0u;
	s_fail_resolve = false;
}

unsigned matterfake_tx_count(void)
{
	return s_tx_n;
}

const struct matterfake_tx *matterfake_last_tx(void)
{
	if (s_tx_n == 0u) {
		return NULL;
	}
	return matterfake_tx_at(s_tx_n - 1u);
}

const struct matterfake_tx *matterfake_tx_at(unsigned i)
{
	if (i >= s_tx_n || i >= TX_MAX) {
		return NULL;
	}
	return &s_tx[i];
}

void matterfake_fail_next_sends(unsigned n)
{
	s_fail_sends = n;
}

bool matterfake_resolve_pending(void)
{
	return s_resolve_armed;
}

const char *matterfake_resolve_name(void)
{
	return s_resolve_armed ? s_resolve_name : NULL;
}

unsigned matterfake_resolve_count(void)
{
	return s_resolve_n;
}

void matterfake_fail_next_resolve(void)
{
	s_fail_resolve = true;
}

void matterfake_resolve_answer(const struct matter_thread_peer *peer)
{
	matter_thread_resolve_fn cb = s_resolve_cb;
	void *ctx = s_resolve_ctx;

	if (!s_resolve_armed) {
		return;
	}
	/*
	 * Disarmed BEFORE the callback runs. The client is entitled to start
	 * the next resolve from inside this one -- a failed answer is exactly
	 * what makes it want to -- and it would be refused by an outstanding
	 * query that only clears afterwards.
	 */
	s_resolve_armed = false;
	s_resolve_cb = NULL;
	s_resolve_ctx = NULL;

	if (cb != NULL) {
		cb(ctx, peer);
	}
}

void matterfake_some_peer(struct matter_thread_peer *out)
{
	static const uint8_t addr[16] = {0xfd, 0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

	if (out == NULL) {
		return;
	}
	memcpy(out->addr, addr, sizeof(addr));
	out->port = 5540u;
	out->valid = true;
}

/* ---- the two primitives the client asks for directly ---------------------- */

/*
 * Counted, not random, and that is the point: a suite that wants to assert on
 * the bytes a Sigma1 carries cannot do it against real entropy. The real
 * CSPRNG lives in ultrawidelock_prim_psa.c, which the core host binary does not link --
 * every OTHER Matter source takes its entropy as an argument, and matter_client.c
 * is the one that must go and get it.
 *
 * Nothing here is a key. The suite stops at the Sigma1 leaving (see the file
 * comment in test_matter_client.c) precisely because completing a handshake
 * would need arithmetic these stubs deliberately do not do.
 */
static uint8_t s_rng_next;

int ultrawidelock_random(uint8_t *out, size_t len)
{
	if (out == NULL) {
		return -1;
	}
	for (size_t i = 0u; i < len; i++) {
		out[i] = s_rng_next++;
	}
	return 0;
}

int ultrawidelock_ec_p256_keygen(uint8_t priv[ULTRAWIDELOCK_P256_SCALAR],
				 uint8_t pub[ULTRAWIDELOCK_P256_POINT])
{
	if (priv == NULL || pub == NULL) {
		return -1;
	}
	memset(priv, 0x11, ULTRAWIDELOCK_P256_SCALAR);
	/* 0x04 so anything that checks the point FORMAT is satisfied; the
	 * coordinates are filler and are not on the curve. */
	memset(pub, 0x22, ULTRAWIDELOCK_P256_POINT);
	pub[0] = 0x04u;
	return 0;
}

/* ---- the seam itself ------------------------------------------------------ */

int matter_thread_send_to(const struct matter_thread_peer *peer, const uint8_t *msg, size_t len)
{
	struct matterfake_tx *slot;

	if (peer == NULL || msg == NULL) {
		return MATTER_E_INVAL;
	}
	/*
	 * Overflow is dropped rather than wrapped, and counted anyway. A suite
	 * that sends more than TX_MAX has stopped agreeing with this fake about
	 * what an attempt is, and a wrapped ring would show that as a confusing
	 * assertion on the wrong datagram instead of an obvious count.
	 */
	if (s_tx_n < TX_MAX) {
		slot = &s_tx[s_tx_n];
		slot->peer = *peer;
		slot->len = len < sizeof(slot->buf) ? len : sizeof(slot->buf);
		memcpy(slot->buf, msg, slot->len);
	}
	s_tx_n++;
	/*
	 * Recorded BEFORE the injected failure, not instead of it. A suite
	 * asking "did it try to send" and a suite asking "what happened when
	 * the send failed" are the same suite, and a double that swallowed the
	 * attempt could only answer the second.
	 */
	if (s_fail_sends != 0u) {
		s_fail_sends--;
		return MATTER_E_STATE;
	}
	return MATTER_OK;
}

int matter_thread_resolve(const char *instance_name, matter_thread_resolve_fn cb, void *ctx)
{
	size_t n;

	if (instance_name == NULL || cb == NULL) {
		return MATTER_E_INVAL;
	}
	if (s_fail_resolve) {
		s_fail_resolve = false;
		return MATTER_E_STATE;
	}
	/* One query at a time, as the real one documents. */
	if (s_resolve_armed) {
		return MATTER_E_STATE;
	}

	n = strlen(instance_name);
	if (n >= sizeof(s_resolve_name)) {
		n = sizeof(s_resolve_name) - 1u;
	}
	memcpy(s_resolve_name, instance_name, n);
	s_resolve_name[n] = '\0';

	s_resolve_cb = cb;
	s_resolve_ctx = ctx;
	s_resolve_armed = true;
	s_resolve_n++;
	return MATTER_OK;
}
