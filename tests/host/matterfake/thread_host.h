/* SPDX-License-Identifier: ISC */

/**
 * @file thread_host.h — the radio and the resolver, as recording doubles.
 *
 * matter_client.c is the one file in the client path that talks to the network,
 * and the two calls it makes -- matter_thread_send_to() and
 * matter_thread_resolve() -- are the whole of that surface. Faking them here
 * turns the client into an ordinary testable object: the suite drives it with
 * the virtual clock, reads back the bytes it tried to send, and decides for
 * itself whether the bound lock answers, answers late, or does not exist.
 *
 * The resolve is deliberately NOT answered inline. On target the callback
 * arrives later, on OpenThread's own thread; a fake that called back from
 * inside matter_thread_resolve() would test a shape the firmware never sees.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "matter_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Everything one datagram handed to matter_thread_send_to(). */
struct matterfake_tx {
	struct matter_thread_peer peer;
	uint8_t buf[1024];
	size_t len;
};

/** Forget every recorded call, every armed resolve and every injected fault. */
void matterfake_thread_reset(void);

/** How many datagrams the code under test has sent since the reset. */
unsigned matterfake_tx_count(void);

/** The most recent datagram, or NULL when none was sent. */
const struct matterfake_tx *matterfake_last_tx(void);

/** Datagram @p i in send order, or NULL when @p i is past the end. */
const struct matterfake_tx *matterfake_tx_at(unsigned i);

/**
 * Make the next @p n sends fail with MATTER_E_STATE, as a downed socket does.
 * @p n of 0 clears the fault.
 */
void matterfake_fail_next_sends(unsigned n);

/** Is a resolve outstanding, waiting for matterfake_resolve_answer()? */
bool matterfake_resolve_pending(void);

/** The instance name of the outstanding resolve, or NULL when there is none. */
const char *matterfake_resolve_name(void);

/** How many resolves have been started since the reset. */
unsigned matterfake_resolve_count(void);

/** Make the next matter_thread_resolve() call fail outright rather than arm. */
void matterfake_fail_next_resolve(void);

/**
 * Answer the outstanding resolve, the way OpenThread's thread would.
 *
 * @param peer where the node is, or NULL for "there is no such node", which is
 *        an answer and not a failure -- an unplugged lock looks exactly so.
 *        Does nothing when no resolve is outstanding.
 */
void matterfake_resolve_answer(const struct matter_thread_peer *peer);

/** A usable peer address, so a suite does not have to invent one each time. */
void matterfake_some_peer(struct matter_thread_peer *out);

#ifdef __cplusplus
}
#endif
