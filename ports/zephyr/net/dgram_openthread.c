/* SPDX-License-Identifier: ISC */

/*
 * dgram_openthread.c - the Zephyr backend of ultrawidelock_dgram.h, on
 * OpenThread UDP.
 *
 * Everything OpenThread-shaped about the sealed link lives here now: the
 * socket, the message allocation, the mesh-local all-nodes destination, the
 * length-minus-offset dance a received otMessage needs, and the API lock. The
 * two link files above this used to carry a copy of each.
 *
 * THE LOCK IS THE POINT OF THIS FILE, not the socket. OpenThread's API is not
 * thread-safe and its own thread is concurrently servicing the radio through
 * MPSL; every call from an application thread has to hold openthread_mutex.
 * Both link files took it by hand around their sends and both carried a comment
 * warning that the receive callback already runs with it held and must not take
 * it again. That is exactly the kind of rule that survives review twice and
 * then loses on the third file, so it is one rule in one place: send() locks,
 * the callback runs under the lock OpenThread already holds, and
 * ultrawidelock_dgram.h says so where a consumer will read it.
 */
#if defined(__ZEPHYR__)

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/openthread.h>

#include <openthread/message.h>
#include <openthread/udp.h>

#include "ultrawidelock_dgram.h"

static otUdpSocket s_sock;
static ultrawidelock_dgram_rx_fn s_cb;
static void *s_ctx;
static bool s_open;

/*
 * Mesh-local all-nodes. The witnesses and the satellite are on this network,
 * and nothing outside it can act on a datagram it cannot unseal anyway. Built
 * once here rather than three fields assigned at each call site, which is how
 * both link files spelled it.
 */
static void group_addr(otMessageInfo *info, uint16_t port)
{
	memset(info, 0, sizeof(*info));
	info->mPeerAddr.mFields.m8[0] = 0xFFu;
	info->mPeerAddr.mFields.m8[1] = 0x03u;
	info->mPeerAddr.mFields.m8[15] = 0x01u;
	info->mPeerPort = port;
}

/*
 * OpenThread hands over an otMessage, not bytes. The payload starts at the
 * message's offset and runs to its length, and reading it anywhere else is how
 * a header ends up parsed as a body. Flattened here so no consumer has to know
 * that, and dropped rather than truncated when it will not fit: a short read of
 * a sealed datagram fails its tag check and would look like a key mismatch.
 */
static void udp_rx(void *ctx, otMessage *msg, const otMessageInfo *info)
{
	uint8_t buf[ULTRAWIDELOCK_DGRAM_MAX];
	uint16_t len;

	ARG_UNUSED(ctx);
	ARG_UNUSED(info);

	if (s_cb == NULL) {
		return;
	}
	len = otMessageGetLength(msg) - otMessageGetOffset(msg);
	if (len == 0u || len > sizeof(buf)) {
		return;
	}
	if (otMessageRead(msg, otMessageGetOffset(msg), buf, len) != len) {
		return;
	}
	s_cb(s_ctx, buf, len);
}

int ultrawidelock_dgram_open(uint16_t port, ultrawidelock_dgram_rx_fn cb, void *ctx)
{
	otInstance *ot = openthread_get_default_instance();
	otSockAddr bind_addr;
	otError err;

	if (cb == NULL) {
		return ULTRAWIDELOCK_DGRAM_INVALID;
	}
	if (s_open) {
		return ULTRAWIDELOCK_DGRAM_OK;
	}
	if (ot == NULL) {
		return ULTRAWIDELOCK_DGRAM_IO;
	}
	/*
	 * Set before the open, not after. otUdpOpen can deliver on OpenThread's
	 * thread the moment it returns, and a callback that arrived between the
	 * open and the assignment would be dropped by the NULL check above --
	 * rarely, and only under traffic, which is the worst way to lose one.
	 */
	s_cb = cb;
	s_ctx = ctx;

	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.mPort = port;

	openthread_mutex_lock();
	err = otUdpOpen(ot, &s_sock, udp_rx, NULL);
	if (err == OT_ERROR_NONE) {
		err = otUdpBind(ot, &s_sock, &bind_addr, OT_NETIF_THREAD);
		if (err != OT_ERROR_NONE) {
			/* Opened but not bound is a socket nobody can reach and
			 * nobody will close. Give it back. */
			(void)otUdpClose(ot, &s_sock);
		}
	}
	openthread_mutex_unlock();

	if (err != OT_ERROR_NONE) {
		s_cb = NULL;
		s_ctx = NULL;
		return ULTRAWIDELOCK_DGRAM_IO;
	}
	s_open = true;
	return ULTRAWIDELOCK_DGRAM_OK;
}

int ultrawidelock_dgram_send(const void *data, size_t len)
{
	otInstance *ot = openthread_get_default_instance();
	otMessageInfo info;
	otMessage *msg;
	otError err;

	if (data == NULL || len == 0u || len > ULTRAWIDELOCK_DGRAM_MAX) {
		return ULTRAWIDELOCK_DGRAM_INVALID;
	}
	if (!s_open) {
		return ULTRAWIDELOCK_DGRAM_CLOSED;
	}
	if (ot == NULL) {
		return ULTRAWIDELOCK_DGRAM_IO;
	}
	group_addr(&info, s_sock.mSockName.mPort);

	openthread_mutex_lock();
	msg = otUdpNewMessage(ot, NULL);
	if (msg == NULL) {
		openthread_mutex_unlock();
		return ULTRAWIDELOCK_DGRAM_IO;
	}
	err = otMessageAppend(msg, data, (uint16_t)len);
	if (err == OT_ERROR_NONE) {
		err = otUdpSend(ot, &s_sock, msg, &info);
	}
	if (err != OT_ERROR_NONE) {
		/* Send takes ownership on success ONLY. Freeing after a
		 * successful send is a double free; not freeing after a failed
		 * one leaks a buffer per attempt, and this link retries. */
		otMessageFree(msg);
	}
	openthread_mutex_unlock();

	return err == OT_ERROR_NONE ? ULTRAWIDELOCK_DGRAM_OK : ULTRAWIDELOCK_DGRAM_IO;
}

bool ultrawidelock_dgram_ready(void)
{
	return s_open;
}

int ultrawidelock_dgram_close(void)
{
	otInstance *ot = openthread_get_default_instance();

	if (!s_open) {
		return ULTRAWIDELOCK_DGRAM_OK;
	}
	/*
	 * Cleared before the close, for the mirror of the reason open() sets it
	 * after: a datagram already queued can reach udp_rx while otUdpClose
	 * runs, and a consumer that asked to be closed should not get one more.
	 */
	s_open = false;
	s_cb = NULL;
	s_ctx = NULL;

	if (ot == NULL) {
		return ULTRAWIDELOCK_DGRAM_OK;
	}
	openthread_mutex_lock();
	(void)otUdpClose(ot, &s_sock);
	openthread_mutex_unlock();
	return ULTRAWIDELOCK_DGRAM_OK;
}

#endif /* __ZEPHYR__ */
