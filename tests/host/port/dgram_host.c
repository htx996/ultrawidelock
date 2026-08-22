/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_dgram.h on the host. The host backend and the test fake are the
 * same file, for the reason flash_host.c gives: a fake more permissive than the
 * contract hides the bugs the contract exists to catch.
 *
 * So this one enforces what the header promises. A send before open is
 * ULTRAWIDELOCK_DGRAM_CLOSED, not a silently discarded datagram -- the link
 * files fail closed when they hear nothing, so a fake that quietly accepted
 * pre-open sends would let a missing open() pass every test and then ship. A
 * length of zero or over ULTRAWIDELOCK_DGRAM_MAX is INVALID rather than
 * truncated, because a truncated sealed datagram fails its tag check and
 * arrives at the other end looking like a key mismatch, which is a bug report
 * about the wrong thing entirely.
 *
 * THE RECEIVE SIDE IS THE POINT. Nothing on the host runs OpenThread, so the
 * only way a test can exercise the sealed link's parse, replay window and
 * quorum logic is to hand it a datagram. dgram_host_deliver() is that door: it
 * calls the consumer's callback exactly as OpenThread's udp_rx would, with the
 * bytes flattened and the length already checked. Datagrams do NOT loop back to
 * the sender on their own -- the real link's group address means a board does
 * hear its own transmissions, but a test that wants that should say so by
 * delivering what it captured, rather than having a hidden echo appear in every
 * case that only meant to send.
 *
 * A fixed capture ring, not a heap: the host suite should not need an allocator
 * to describe what a radio does.
 */

#include "ultrawidelock_dgram.h"
#include "dgram_host.h"

#include <string.h>

#define CAPTURE_MAX 16u

struct capture {
	uint8_t data[ULTRAWIDELOCK_DGRAM_MAX];
	size_t len;
};

static struct capture s_sent[CAPTURE_MAX];
static size_t s_sent_count;
static ultrawidelock_dgram_rx_fn s_cb;
static void *s_ctx;
static uint16_t s_port;
static bool s_open;

/* Injected results, one shot each, so a test can make the next call fail
 * without having to break the whole link. Mirrors the sdkfake NVS knobs. */
int dgram_host_open_rc;
int dgram_host_send_rc;

int ultrawidelock_dgram_open(uint16_t port, ultrawidelock_dgram_rx_fn cb, void *ctx)
{
	if (cb == NULL) {
		return ULTRAWIDELOCK_DGRAM_INVALID;
	}
	if (dgram_host_open_rc != ULTRAWIDELOCK_DGRAM_OK) {
		int rc = dgram_host_open_rc;

		dgram_host_open_rc = ULTRAWIDELOCK_DGRAM_OK;
		return rc;
	}
	if (s_open) {
		return ULTRAWIDELOCK_DGRAM_OK;
	}
	s_cb = cb;
	s_ctx = ctx;
	s_port = port;
	s_open = true;
	return ULTRAWIDELOCK_DGRAM_OK;
}

int ultrawidelock_dgram_send(const void *data, size_t len)
{
	if (data == NULL || len == 0u || len > ULTRAWIDELOCK_DGRAM_MAX) {
		return ULTRAWIDELOCK_DGRAM_INVALID;
	}
	if (!s_open) {
		return ULTRAWIDELOCK_DGRAM_CLOSED;
	}
	if (dgram_host_send_rc != ULTRAWIDELOCK_DGRAM_OK) {
		int rc = dgram_host_send_rc;

		dgram_host_send_rc = ULTRAWIDELOCK_DGRAM_OK;
		return rc;
	}
	/*
	 * Past the end of the ring the datagram is still SENT, just not kept.
	 * The alternative -- failing the send once the ring fills -- would make
	 * a test's capture depth into link behaviour, and a soak case that sent
	 * twenty reports would start failing for a reason that has nothing to
	 * do with the code under test.
	 */
	if (s_sent_count < CAPTURE_MAX) {
		memcpy(s_sent[s_sent_count].data, data, len);
		s_sent[s_sent_count].len = len;
	}
	s_sent_count++;
	return ULTRAWIDELOCK_DGRAM_OK;
}

bool ultrawidelock_dgram_ready(void)
{
	return s_open;
}

int ultrawidelock_dgram_close(void)
{
	s_open = false;
	s_cb = NULL;
	s_ctx = NULL;
	return ULTRAWIDELOCK_DGRAM_OK;
}

/* --- the test-facing half (dgram_host.h) --- */

void dgram_host_reset(void)
{
	memset(s_sent, 0, sizeof(s_sent));
	s_sent_count = 0u;
	s_cb = NULL;
	s_ctx = NULL;
	s_port = 0u;
	s_open = false;
	dgram_host_open_rc = ULTRAWIDELOCK_DGRAM_OK;
	dgram_host_send_rc = ULTRAWIDELOCK_DGRAM_OK;
}

bool dgram_host_deliver(const void *data, size_t len)
{
	/*
	 * The same checks the OpenThread backend's udp_rx does before it calls
	 * the consumer, and for the same reason: a consumer must never see a
	 * datagram the real backend would have dropped, or the host suite is
	 * testing a link that does not exist. A closed link delivers nothing --
	 * that is what closed means.
	 */
	if (!s_open || s_cb == NULL || data == NULL || len == 0u ||
	    len > ULTRAWIDELOCK_DGRAM_MAX) {
		return false;
	}
	s_cb(s_ctx, (const uint8_t *)data, len);
	return true;
}

size_t dgram_host_sent_count(void)
{
	return s_sent_count;
}

uint16_t dgram_host_port(void)
{
	return s_port;
}

size_t dgram_host_sent(size_t index, void *out, size_t cap)
{
	if (index >= CAPTURE_MAX || index >= s_sent_count || out == NULL) {
		return 0u;
	}
	if (s_sent[index].len > cap) {
		return 0u;
	}
	memcpy(out, s_sent[index].data, s_sent[index].len);
	return s_sent[index].len;
}
