/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_dgram.h - the sealed link's datagram transport.
 *
 * WHAT THIS REPLACES. The witness link and the satellite's anchor link both
 * moved their bytes by calling OpenThread directly: otUdpOpen, otUdpBind,
 * otUdpNewMessage, otMessageAppend, otUdpSend, and an otMessage the receive
 * callback had to measure with otMessageGetLength minus otMessageGetOffset
 * before otMessageRead would give it up. Eleven OpenThread symbols and the
 * openthread_mutex, in two application files, for what the protocol above them
 * actually needs: send these bytes to the group, and hand me the bytes that
 * arrive.
 *
 * WHY THERE IS NO ADDRESS IN THIS API. Both call sites send to the same
 * mesh-local all-nodes group (ff03::1) and both ignore the sender: each
 * datagram is sealed under a key, and which key opens it is the only identity
 * this protocol has. An address argument would be a parameter every caller
 * passes the same constant to, and a peer identity the seal above does not
 * trust. A transport with no addressing at all -- ESP-NOW broadcast is the
 * obvious second backend -- fits this contract exactly, which is the sign the
 * shape is right rather than merely smaller.
 *
 * ONE LINK PER IMAGE. Every board in the tree binds exactly one of these: the
 * lock binds it in witness_link.c and multiplexes witness reports and anchor
 * reports on it, the satellite binds it in anchor_link.c. There is no handle
 * because there is nothing to hold two of. A second link is the moment to add
 * one, not before.
 *
 * Backends: Zephyr over OpenThread UDP (ports/zephyr/net/dgram_openthread.c)
 * and a host loopback (tests/host/port/dgram_host.c), which doubles as the test
 * fake.
 *
 * NOT a socket API. No streams, no connect, no partial reads, no address
 * family. A datagram arrives whole or not at all.
 */
#ifndef ULTRAWIDELOCK_DGRAM_H
#define ULTRAWIDELOCK_DGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The largest datagram this link carries, in either direction.
 *
 * The biggest thing sent today is a sealed witness report: 61 bytes encoded
 * (ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN) plus a 13-byte nonce and an 8-byte tag,
 * so 82. The satellite's sealed handoff is 65. The cap is deliberately above
 * both and deliberately not derived from them -- a port seam that had to
 * include the witness protocol's headers to state its own buffer size would
 * have the layering upside down. A consumer whose payload could exceed this
 * should _Static_assert against it, the way the provisioning backends assert
 * against ULTRAWIDELOCK_KV_VALUE_MAX.
 */
#define ULTRAWIDELOCK_DGRAM_MAX 128u

enum ultrawidelock_dgram_result {
	ULTRAWIDELOCK_DGRAM_OK = 0,
	/* The length is zero or over ULTRAWIDELOCK_DGRAM_MAX, or the callback
	 * was NULL. A caller bug, never a network condition. */
	ULTRAWIDELOCK_DGRAM_INVALID = -1,
	/* Sent before a successful open, or after the link went down. */
	ULTRAWIDELOCK_DGRAM_CLOSED = -2,
	/* The network stack refused the datagram: no buffer, no route, not
	 * attached yet. Transient by nature; the caller decides whether this
	 * message is worth repeating. */
	ULTRAWIDELOCK_DGRAM_IO = -3,
};

/**
 * A datagram that arrived, delivered whole.
 *
 * @p data is valid only for the duration of the call; copy what you keep.
 *
 * CALLED ON THE STACK'S OWN THREAD, WITH ITS LOCK HELD. On Zephyr that is the
 * OpenThread thread with openthread_mutex already taken on your behalf, which
 * is why a callback must not call ultrawidelock_dgram_send() -- that lock is
 * not recursive and the board would stop dead in a way the console cannot
 * report. No consumer does; the two links both parse here and send from a work
 * item elsewhere. Keep the body short: the radio is not being serviced while it
 * runs.
 */
typedef void (*ultrawidelock_dgram_rx_fn)(void *ctx, const uint8_t *data, size_t len);

/**
 * Open the link on @p port and start delivering to @p cb.
 *
 * Idempotent in the sense that matters on a board: a second open with the same
 * arguments succeeds without disturbing the first. Returns
 * ULTRAWIDELOCK_DGRAM_OK or a negative ultrawidelock_dgram_result. A failure
 * here is not fatal to the caller -- an unopened link simply never delivers,
 * and every consumer of this seam fails closed when it hears nothing.
 */
int ultrawidelock_dgram_open(uint16_t port, ultrawidelock_dgram_rx_fn cb, void *ctx);

/**
 * Send @p len bytes to the group.
 *
 * Takes whatever lock the backend needs, so it must NOT be called from the
 * receive callback. Returns ULTRAWIDELOCK_DGRAM_OK once the stack has taken
 * ownership of the datagram, which is not a delivery receipt: this link is
 * unacknowledged in both directions by design.
 */
int ultrawidelock_dgram_send(const void *data, size_t len);

/** True once open() has succeeded and the link has not been closed since. */
bool ultrawidelock_dgram_ready(void);

/**
 * Stop delivering and release the socket.
 *
 * Present for the host fake's sake as much as the board's: a test that could
 * not close could not run two cases in one process. Safe on a link that was
 * never opened.
 */
int ultrawidelock_dgram_close(void);

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_DGRAM_H */
