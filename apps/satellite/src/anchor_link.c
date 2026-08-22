/* SPDX-License-Identifier: ISC */

/**
 * @file anchor_link.c — the satellite's half of the sealed link.
 *
 * Sends one WV3 report per accepted range: this anchor's own measured distance
 * to the phone, and the ranging block it was measured in. Same socket, same
 * seal and same port as the lock's other peers; the version byte is what tells
 * them apart, so nothing here needs to know about any other message family.
 *
 * The block is not decoration. A distance without the round it belongs to
 * cannot be paired with the lock's own -- the two anchors only mean something
 * together if they are describing the same instant, and the block is the one
 * exact integer both of them read off the initiator's own frames.
 *
 * SENDING IS UNCONDITIONAL AND CARRIES NO AUTHORITY. Every rule that matters is
 * enforced at the lock: the seal proves the key, the counter catches replay,
 * the block decides what this pairs with, and the fusion gate decides what the
 * pair means. A satellite that lies, floods or goes silent costs a passive
 * unlock; it cannot cause one.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/thread.h>
#include <openthread/udp.h>
#include <zephyr/net/openthread.h>

#include "anchor_link.h"
#include "ultrawidelock_link.h"
#include "ultrawidelock_seal.h"
#include "ultrawidelock_witness_msg.h"

LOG_MODULE_REGISTER(anclink, LOG_LEVEL_INF);

/* The envelope's own constants, from the ONE definition of it. Aliased rather
 * than redefined: this file used to carry its own copies, which is how a wire
 * format grows two versions of itself. */
#define KEY_LEN     ULTRAWIDELOCK_SEAL_KEY_LEN
#define ANCHOR_PORT CONFIG_ULTRAWIDELOCK_WITNESS_PORT

static otUdpSocket s_sock;
static bool s_open;
/*
 * Every decision about a datagram -- what to send, what an arriving one is,
 * whether to believe it -- belongs to ultrawidelock_link.h, shared with the
 * ESP-NOW carrier. The key, the boot id, the counter, the echoed challenge and
 * the lock's replay window all live inside it, so the two transports cannot
 * come to different conclusions about the same bytes. What stays in this file
 * is OpenThread: sockets, messages and addresses.
 */
static struct ultrawidelock_link s_link;
/* Where a surviving handoff goes. */
static anchor_link_join_cb s_join_cb;

/**
 * Two different things arrive on this socket, and the link core tells them
 * apart: a 9-byte CHALLENGE is unauthenticated on purpose (a freshness beacon,
 * not a command; echoing the wrong one costs a report its standing and nothing
 * more), while a sealed HANDOFF carries a key this board acts on and must
 * prove the link key and clear the replay window first.
 *
 * Our own WV3 reports come back off the all-nodes address too. They are not
 * mistaken for input: the core demultiplexes on length, and a report is not
 * the length of a handoff.
 */
static void udp_rx(void *ctx, otMessage *msg, const otMessageInfo *info)
{
	uint8_t body[ULTRAWIDELOCK_LINK_MAX_FRAME];
	struct ultrawidelock_join_msg jm;
	uint16_t len;

	ARG_UNUSED(ctx);
	ARG_UNUSED(info);

	len = otMessageGetLength(msg) - otMessageGetOffset(msg);
	if (len == 0u || len > sizeof(body)) {
		return;
	}
	if (otMessageRead(msg, otMessageGetOffset(msg), body, len) != len) {
		return;
	}

	memset(&jm, 0, sizeof(jm));
	switch (ultrawidelock_link_consume(&s_link, body, len, NULL,
					   s_join_cb != NULL ? &jm : NULL)) {
	case ULTRAWIDELOCK_LINK_RX_JOIN:
		s_join_cb(jm.ursk, jm.rcfg, jm.channel, jm.sync_code_index);
		break;
	case ULTRAWIDELOCK_LINK_RX_REPLAYED:
		LOG_WRN("handoff replayed or stale; ignored");
		break;
	default:
		break;
	}
	/* The URSK lives on inside the ranging engine; this copy must not. */
	memset(&jm, 0, sizeof(jm));
}

void anchor_link_set_join_cb(anchor_link_join_cb cb)
{
	s_join_cb = cb;
}

void anchor_link_report(int32_t peer_mm, uint32_t ranging_block)
{
	uint8_t sealed[ULTRAWIDELOCK_LINK_MAX_FRAME];
	otInstance *ot = openthread_get_default_instance();
	otMessageInfo info;
	otMessage *msg;
	size_t sealed_len;

	if (!s_open || ot == NULL) {
		return;
	}

	/* Composes the WV3, stamps this boot id and the next counter, echoes the
	 * lock's challenge and seals it -- one call, the same one the ESP-NOW
	 * carrier makes. A negative distance and an unprovisioned link are
	 * refused inside it. */
	sealed_len = ultrawidelock_link_build_report(&s_link, peer_mm, ranging_block, sealed,
						     sizeof(sealed));
	if (sealed_len == 0u) {
		return;
	}

	msg = otUdpNewMessage(ot, NULL);
	if (msg == NULL) {
		return;
	}
	if (otMessageAppend(msg, sealed, (uint16_t)sealed_len) != OT_ERROR_NONE) {
		otMessageFree(msg);
		return;
	}
	memset(&info, 0, sizeof(info));
	/*
	 * Mesh-local all-nodes, matching the witness link's reasoning: this board
	 * is never told the lock's address, so replacing the lock or letting its
	 * address change costs no re-provisioning. Only a holder of the link key
	 * can produce a report, so the broadcast costs a frame and reveals a
	 * distance to nobody who could not already measure one.
	 */
	info.mPeerAddr.mFields.m8[0] = 0xFFu;
	info.mPeerAddr.mFields.m8[1] = 0x03u;
	info.mPeerAddr.mFields.m8[15] = 0x01u;
	info.mPeerPort = ANCHOR_PORT;

	if (otUdpSend(ot, &s_sock, msg, &info) != OT_ERROR_NONE) {
		otMessageFree(msg); /* takes ownership only on success */
	}
}

/**
 * Bring the mesh up on an instance that already holds a dataset.
 *
 * Split out because it is needed twice for different reasons: once when the
 * dataset is typed in, and once at every boot after that. CONFIG_OPENTHREAD_
 * MANUAL_START means nothing starts Thread on its own here, so without the
 * second call the stored dataset is inert and `sat dataset` would have to be
 * retyped after every reset.
 *
 * MUST BE CALLED WITH THE OPENTHREAD MUTEX RELEASED. openthread_run() takes it
 * itself -- the same rule ports/zephyr/matter/matter_thread_port.c:136 spells
 * out. Calling otIp6SetEnabled()/otThreadSetEnabled() under the lock instead
 * deadlocks the caller, and on the shell thread that is indistinguishable from
 * a dead console: the command never returns, the prompt never comes back, and
 * the board looks bricked until it is reset. Cost an hour on 2026-08-21, twice,
 * because the same silence also has a plausible transport explanation.
 */
static int thread_bring_up(void)
{
	return openthread_run();
}

/* Role transitions on the log, because the shell is not always reachable and an
 * anchor that never attaches looks exactly like one that attached and had
 * nothing to say. */
static void role_changed(otChangedFlags flags, void *ctx)
{
	ARG_UNUSED(ctx);

	if ((flags & OT_CHANGED_THREAD_ROLE) == 0u) {
		return;
	}
	LOG_INF("thread role now %d (2=child 3=router 4=leader)",
		(int)otThreadGetDeviceRole(openthread_get_default_instance()));
}

int anchor_link_set_dataset(const uint8_t *tlvs, size_t len)
{
	otInstance *ot = openthread_get_default_instance();
	otOperationalDatasetTlvs ds;
	otError err;

	if (tlvs == NULL || len == 0u || len > sizeof(ds.mTlvs)) {
		return -EINVAL;
	}
	if (ot == NULL) {
		return -ENODEV;
	}

	memset(&ds, 0, sizeof(ds));
	memcpy(ds.mTlvs, tlvs, len);
	ds.mLength = (uint8_t)len;

	/*
	 * The satellite joins the SAME Thread network the lock is on. It is not
	 * a Matter device and joins no fabric: a fabric governs who may invoke
	 * clusters, and this board only needs IPv6 to a peer on the mesh, which
	 * mesh membership alone provides.
	 *
	 * The dataset is typed in rather than commissioned because there is no
	 * commissioner here to talk to, and it is persisted by OpenThread itself
	 * so a reflash without --erase keeps it.
	 */
	openthread_mutex_lock();
	err = otDatasetSetActiveTlvs(ot, &ds);
	openthread_mutex_unlock();

	if (err != OT_ERROR_NONE) {
		LOG_WRN("dataset rejected (ot err %d)", (int)err);
		return -EIO;
	}
	/* Outside the lock. See thread_bring_up(). */
	if (thread_bring_up() != 0) {
		LOG_WRN("dataset stored but OpenThread refused to start");
		return -EIO;
	}
	LOG_INF("dataset accepted (%u B); attaching", (unsigned)len);
	return 0;
}

bool anchor_link_attached(void)
{
	otInstance *ot = openthread_get_default_instance();
	otDeviceRole role;

	if (ot == NULL) {
		return false;
	}
	openthread_mutex_lock();
	role = otThreadGetDeviceRole(ot);
	openthread_mutex_unlock();
	return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
	       role == OT_DEVICE_ROLE_LEADER;
}

static int anchor_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			       void *cb_arg)
{
	uint8_t key[KEY_LEN];

	if (len != KEY_LEN || strcmp(name, "lk") != 0) {
		return -ENOENT;
	}
	if (read_cb(cb_arg, key, KEY_LEN) != (ssize_t)KEY_LEN) {
		return -EINVAL;
	}
	(void)ultrawidelock_link_set_key(&s_link, key, KEY_LEN);
	memset(key, 0, sizeof(key));
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(anchor_link, "sat", NULL, anchor_settings_set, NULL, NULL);

int anchor_link_set_key(const uint8_t *key, size_t len)
{
	int rc;

	if (key == NULL || len != KEY_LEN) {
		return -EINVAL;
	}
	rc = settings_save_one("sat/lk", key, len);
	if (rc != 0) {
		return rc;
	}
	return ultrawidelock_link_set_key(&s_link, key, len);
}

bool anchor_link_ready(void)
{
	return s_open && ultrawidelock_link_ready(&s_link);
}

void anchor_link_init(void)
{
	otInstance *ot;
	bool commissioned;

	uint32_t boot_id;

	/* Never zero: a zero boot_id is what an uninitialised variable looks
	 * like, and the lock treats a change of boot_id as permission to accept
	 * a counter that went backwards. Before the settings load, which
	 * installs the stored key into the same struct this initialises. */
	do {
		boot_id = sys_rand32_get();
	} while (boot_id == 0u);
	ultrawidelock_link_init(&s_link, (uint8_t)CONFIG_ULTRAWIDELOCK_ANCHOR_ROLE, boot_id);

	(void)settings_subsys_init();
	(void)settings_load_subtree("sat");

	ot = openthread_get_default_instance();
	if (ot == NULL) {
		LOG_WRN("no Thread instance; anchor reports will not be sent");
		return;
	}
	openthread_mutex_lock();
	if (otUdpOpen(ot, &s_sock, udp_rx, NULL) == OT_ERROR_NONE) {
		otSockAddr addr = {0};

		addr.mPort = ANCHOR_PORT;
		s_open = (otUdpBind(ot, &s_sock, &addr, OT_NETIF_THREAD) == OT_ERROR_NONE);
	}
	(void)otSetStateChangedCallback(ot, role_changed, NULL);
	commissioned = otDatasetIsCommissioned(ot);
	openthread_mutex_unlock();

	/*
	 * A dataset typed in on an earlier boot is persisted by OpenThread but
	 * does not start anything: CONFIG_OPENTHREAD_MANUAL_START leaves the
	 * stack down until something asks. Ask here, so the mesh comes back by
	 * itself after a reset or a reflash and the dataset is typed in exactly
	 * once in this board's life.
	 *
	 * Outside the lock, which is released just above. See thread_bring_up().
	 */
	if (commissioned) {
		LOG_INF("stored dataset found; bringing the mesh up (rc %d)",
			thread_bring_up());
	} else {
		LOG_WRN("no Thread dataset; run `sat dataset <tlv-hex>`");
	}

	if (!s_open) {
		LOG_WRN("anchor link socket did not open");
	} else if (!ultrawidelock_link_ready(&s_link)) {
		/* Loud, because it fails closed and silently otherwise: an
		 * un-provisioned anchor ranges perfectly and reports nothing,
		 * which at the lock is indistinguishable from a board that never
		 * booted. */
		LOG_WRN("anchor link has no key; run `sat key <hex32>`");
	}
}
