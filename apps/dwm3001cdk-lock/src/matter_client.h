/* SPDX-License-Identifier: ISC */

/**
 * @file matter_client.h — the lock, telling another lock to open.
 *
 * modules/ultrawidelock_matter carries the pieces: a CASE initiator, a schedule
 * with no clock in it, the Interaction Model's outbound direction, and the
 * binding table that says who to talk to. Every one of them is a pure function
 * over bytes, which is what makes them testable on a host and what leaves them
 * with no caller. This file is the caller.
 *
 * It owns exactly three things the others cannot: the ONE client session, the
 * clock, and the radio. Everything else it asks for.
 *
 *   main.c            a walk-up was granted   -> matter_client_want()
 *   this file         resolve, Sigma1, Sigma3, TimedRequest, InvokeRequest
 *   matter_commission.c  routes the answers back in through the two hooks below
 *
 * RETRANSMISSION, and how far it goes. The HANDSHAKE is covered: a Sigma1 or
 * Sigma3 that goes unacknowledged is resent on an MRP timer, because a dropped
 * datagram there would otherwise cost the whole MATTER_CLIENT_STEP_MS out of a
 * want worth about eight seconds (matter_client_sm.h) -- one loss nearly spends
 * the budget, and two certainly do.
 *
 * The INTERACTION is not. Once the session exists its messages are sealed by
 * matter_exchange, whose message counters this file does not own, so there is
 * nothing here to arm a timer against. A loss there falls inside a much shorter
 * window and the peer retransmits its own half regardless; the step deadline
 * still catches the rest, one whole attempt at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "matter_clusters.h"
#include "matter_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Take the device state this client reads and start idle.
 *
 * @p info is borrowed for the life of the program: the binding table, the
 * fabrics and the vendor id all live in it, and a copy would be a second
 * binding list to keep in step with the one a controller writes.
 */
void matter_client_init(struct matter_device_info *info);

/**
 * The local bolt is now @p unlocked. Bring the bound lock to match.
 *
 * A STATE to reconcile, not an event to forward, and the difference matters:
 * the walk-up opens the door and the departure closes it, so a client that
 * only ever forwarded the opening left the other lock standing open until
 * somebody noticed. Call it on both edges.
 *
 * Reconciled rather than queued. What is remembered is the last state the peer
 * ACCEPTED, so a relock that arrives while the unlock is still in flight is
 * not lost behind it, and a refusal or a dropped session leaves the two still
 * visibly out of step -- which is the condition that makes the next attempt
 * happen rather than a missed edge nobody can see.
 *
 * Returns immediately and never blocks: it is called from the walk-up path,
 * after the local bolt has already moved. A node with no binding, no fabric or
 * no Thread network does nothing at all, which is the normal case.
 */
void matter_client_want(bool unlocked);

/** Rearm a future UnlockDoor without sending LockDoor on this departure. */
void matter_client_rearm_unlock(void);

/**
 * Is @p session_id the client's own secure session?
 *
 * Asked by matter_thread_on_datagram_owned() before it gives up on an encrypted
 * datagram: a session this node INITIATED is in none of the tables that hold
 * the ones it answered, so without this the peer's InvokeResponse is logged as
 * "not ours" and dropped.
 */
bool matter_client_owns_session(uint16_t session_id);

/**
 * Handle one encrypted datagram on the client's session.
 *
 * @param msg the private datagram copy, decrypted in place.
 * @return how many bytes of @p reply to send back, which is the next message of
 *         the interaction -- an InvokeRequest after the peer's StatusResponse,
 *         or an acknowledgement -- or 0.
 */
size_t matter_client_on_secure(uint8_t *msg, size_t len, uint8_t *reply, size_t cap);

/**
 * Handle one UNSECURED datagram that answers a handshake this node started.
 *
 * Sigma2 and the StatusReport that ends CASE both arrive here. The headers are
 * passed already decoded because the caller decoded them to get this far, and
 * decoding them twice is how the two copies drift.
 *
 * @return the framed Sigma3 in @p reply and its length, or 0. Returning 0 is
 *         not an error -- the StatusReport that establishes the session has
 *         nothing to answer with.
 */
size_t matter_client_on_unsecured(const uint8_t *payload, size_t payload_len,
				  const struct matter_msg_header *mh,
				  const struct matter_proto_header *ph, uint8_t *reply, size_t cap);

/**
 * Is @p exchange_id the exchange this node opened for its handshake?
 *
 * The unsecured session has no session id to route by -- it is session 0 for
 * everybody -- so the exchange id is the only thing that separates an answer to
 * this node's Sigma1 from a Sigma1 somebody else is sending IT.
 */
bool matter_client_owns_exchange(uint16_t exchange_id);

#ifdef __cplusplus
}
#endif
