/* SPDX-License-Identifier: ISC */

/**
 * @file anchor_link.c — the satellite's half of the sealed link.
 *
 * Sends one WV3 report per accepted range: this anchor's own measured distance
 * to the phone, and the ranging block it was measured in. Same link, same seal
 * and same port as the lock's other peers; the version byte is what tells them
 * apart, so nothing here needs to know about any other message family.
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

/*
 * The bring-up half only. Moving bytes is ultrawidelock_dgram.h's job now, but
 * starting the mesh is not: a dataset, a device role and otIp6SetEnabled are
 * OpenThread's own lifecycle, they have no counterpart on a transport that is
 * not Thread, and inventing a seam over them would be inventing one nothing
 * else needs. So the socket calls left this file and the stack calls did not.
 */
#include <openthread/dataset.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>
#include <zephyr/net/openthread.h>

#include "anchor_link.h"
#include "ultrawidelock_dgram.h"
#include "ultrawidelock_seal.h"
#include "ultrawidelock_witness_msg.h"

LOG_MODULE_REGISTER(anclink, LOG_LEVEL_INF);

/* The envelope's own constants, from the ONE definition of it. Aliased rather
 * than redefined: this file used to carry its own copies, which is how a wire
 * format grows two versions of itself. */
#define KEY_LEN        ULTRAWIDELOCK_SEAL_KEY_LEN
#define CCM_NONCE_LEN  ULTRAWIDELOCK_SEAL_NONCE_LEN
#define CCM_TAG_LEN    ULTRAWIDELOCK_SEAL_TAG_LEN
#define ANCHOR_PORT    CONFIG_ULTRAWIDELOCK_WITNESS_PORT

static uint8_t s_key[KEY_LEN];
static bool s_provisioned;
/*
 * Fresh per boot, so a counter that legitimately restarts at zero after a power
 * cut is distinguishable from a replay of the reports that preceded it. Without
 * it the lock's only options would be accepting replays or locking out an
 * anchor that lost power.
 */
static uint32_t s_boot_id;
static uint32_t s_ctr;
/* The lock's current challenge, echoed back so it can tell a live report from a
 * recorded one. Zero until one is heard; the lock only challenges while a
 * credential session is up. */
static uint64_t s_echo_nonce;
/* Where a surviving handoff goes, and the lock's replay state for that
 * direction. Separate from anything the report path keeps: the two directions
 * have independent senders, boot ids and counters. */
static anchor_link_join_cb s_join_cb;
static struct ultrawidelock_witness_seen s_lock_seen;

/**
 * Seal one report under the link key.
 *
 * The envelope is ultrawidelock_seal.h's; what stays here is the nonce, because
 * only this file knows which counter value the caller just committed to. The
 * report path pre-increments s_ctr and then builds the nonce from the new
 * value, so the two can never disagree.
 */
static size_t seal(const uint8_t *plain, size_t plain_len, uint8_t *out, size_t cap)
{
	uint8_t nonce[ULTRAWIDELOCK_SEAL_NONCE_LEN];

	ultrawidelock_seal_nonce((uint8_t)CONFIG_ULTRAWIDELOCK_ANCHOR_ROLE, s_boot_id, s_ctr,
				 nonce);
	return ultrawidelock_seal(s_key, nonce, plain, plain_len, out, cap);
}

/** Unseal under the link key: the inverse of seal() above. */
static bool unseal(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
	return ultrawidelock_unseal(s_key, in, in_len, out, out_cap, out_len);
}

/**
 * Two different things arrive on this socket.
 *
 * A 9- or 12-byte CHALLENGE is unauthenticated on purpose -- a freshness beacon, not a
 * command; echoing the wrong one costs a report its standing and nothing more.
 * A sealed HANDOFF is the opposite: it carries a key and this board acts on it,
 * so it must prove the link key and clear the replay window first.
 */
static void link_rx(void *ctx, const uint8_t *body, size_t len)
{
	uint8_t plain[ULTRAWIDELOCK_JOIN_MSG_LEN];
	struct ultrawidelock_join_msg jm;
	size_t plain_len = 0;

	ARG_UNUSED(ctx);

	/*
	 * Flattened and length-checked by the transport, so the otMessage offset
	 * arithmetic this function used to open with is gone. The cap below is
	 * this protocol's own: the largest thing it can be handed is a sealed
	 * handoff, and anything longer is not one whatever the link carried.
	 */
	if (len == 0u || len > CCM_NONCE_LEN + ULTRAWIDELOCK_JOIN_MSG_LEN + CCM_TAG_LEN) {
		return;
	}

	/* 9 B is the bare challenge; 12 B carries the lock's picked label as a
	 * trailer. Both are valid, and the witness reference parse accepts both
	 * (examples/zephyr/ble-witness). This board ignores the trailer -- it
	 * reports a distance, not a label -- but it must still accept the
	 * length, or it hears no challenge at all and echoes a zero nonce. */
	if ((len == 9u || len == 12u) && body[0] == ULTRAWIDELOCK_WITNESS_MSG_VER) {
		s_echo_nonce = 0u;
		for (int i = 0; i < 8; i++) {
			s_echo_nonce = (s_echo_nonce << 8) | (uint64_t)body[1 + i];
		}
		return;
	}

	/* Anything else is only interesting if it is sealed to us. Our own WV3
	 * reports come back off the all-nodes address too and must not be
	 * mistaken for input: they fail the length check below. */
	if (!s_provisioned || s_join_cb == NULL) {
		return;
	}
	if (len != CCM_NONCE_LEN + ULTRAWIDELOCK_JOIN_MSG_LEN + CCM_TAG_LEN) {
		return;
	}
	if (!unseal(body, len, plain, sizeof(plain), &plain_len)) {
		return; /* not sealed under our key, or tampered with */
	}
	if (!ultrawidelock_join_msg_decode(plain, plain_len, &jm)) {
		memset(plain, 0, sizeof(plain));
		return;
	}
	memset(plain, 0, sizeof(plain));

	/*
	 * Replay window, shared with the report direction's implementation so
	 * there is one notion of freshness on this link. A lock that reboots
	 * gets a new boot_id and its counter may restart; without that a power
	 * cut would lock the satellite out of every future session.
	 */
	if (!ultrawidelock_seen_accept_ctr(&s_lock_seen, jm.boot_id, jm.ctr)) {
		LOG_WRN("handoff replayed or stale (ctr %u); ignored", (unsigned)jm.ctr);
		memset(&jm, 0, sizeof(jm));
		return;
	}

	s_join_cb(jm.ursk, jm.rcfg, jm.channel, jm.sync_code_index);
	/* The URSK lives on inside the ranging engine; this copy must not. */
	memset(&jm, 0, sizeof(jm));
}

void anchor_link_set_join_cb(anchor_link_join_cb cb)
{
	s_join_cb = cb;
}

void anchor_link_report(int32_t peer_mm, uint32_t ranging_block)
{
	struct ultrawidelock_anchor_msg am;
	uint8_t plain[ULTRAWIDELOCK_ANCHOR_MSG_LEN];
	uint8_t sealed[CCM_NONCE_LEN + ULTRAWIDELOCK_ANCHOR_MSG_LEN + CCM_TAG_LEN];
	size_t plain_len;
	size_t sealed_len;

	if (!ultrawidelock_dgram_ready() || !s_provisioned || peer_mm < 0) {
		return;
	}

	memset(&am, 0, sizeof(am));
	am.ver = ULTRAWIDELOCK_ANCHOR_MSG_VER;
	am.role = (uint8_t)CONFIG_ULTRAWIDELOCK_ANCHOR_ROLE;
	am.boot_id = s_boot_id;
	am.ctr = ++s_ctr; /* pre-increment: the nonce below must match this */
	am.echo_nonce = s_echo_nonce;
	am.ranging_block = (uint16_t)ranging_block;
	am.peer_mm = peer_mm;

	plain_len = ultrawidelock_anchor_msg_encode(&am, plain, sizeof(plain));
	if (plain_len == 0u) {
		return;
	}
	sealed_len = seal(plain, plain_len, sealed, sizeof(sealed));
	if (sealed_len == 0u) {
		return;
	}

	/*
	 * To the group, matching the witness link's reasoning: this board is
	 * never told the lock's address, so replacing the lock or letting its
	 * address change costs no re-provisioning. Only a holder of the link key
	 * can produce a report, so the broadcast costs a frame and reveals a
	 * distance to nobody who could not already measure one.
	 *
	 * Runs on the app thread out of the ranging loop in main.c, not in the
	 * receive callback, so the transport may take its lock. That rule is
	 * ultrawidelock_dgram.h's to state now; it used to be a paragraph here
	 * and another one in the witness link, saying the same thing twice.
	 */
	(void)ultrawidelock_dgram_send(sealed, sealed_len);
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
	if (len != KEY_LEN || strcmp(name, "lk") != 0) {
		return -ENOENT;
	}
	if (read_cb(cb_arg, s_key, KEY_LEN) != (ssize_t)KEY_LEN) {
		return -EINVAL;
	}
	s_provisioned = true;
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
	memcpy(s_key, key, KEY_LEN);
	s_provisioned = true;
	return 0;
}

bool anchor_link_ready(void)
{
	return ultrawidelock_dgram_ready() && s_provisioned;
}

void anchor_link_init(void)
{
	otInstance *ot;
	bool commissioned;

	(void)settings_subsys_init();
	(void)settings_load_subtree("sat");

	/* Never zero: a zero boot_id is what an uninitialised variable looks
	 * like, and the lock treats a change of boot_id as permission to accept
	 * a counter that went backwards. */
	do {
		s_boot_id = sys_rand32_get();
	} while (s_boot_id == 0u);
	s_ctr = 0u;

	ot = openthread_get_default_instance();
	if (ot == NULL) {
		LOG_WRN("no Thread instance; anchor reports will not be sent");
		return;
	}
	/*
	 * Outside the OpenThread lock, and it has to be: the transport takes that
	 * lock itself, and taking it here first would be the caller holding it
	 * twice. This is the shape the seam asks for -- a consumer that has no
	 * lock of its own to interleave.
	 */
	(void)ultrawidelock_dgram_open(ANCHOR_PORT, link_rx, NULL);

	openthread_mutex_lock();
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

	if (!ultrawidelock_dgram_ready()) {
		LOG_WRN("anchor link did not open");
	} else if (!s_provisioned) {
		/* Loud, because it fails closed and silently otherwise: an
		 * un-provisioned anchor ranges perfectly and reports nothing,
		 * which at the lock is indistinguishable from a board that never
		 * booted. */
		LOG_WRN("anchor link has no key; run `sat key <hex32>`");
	}
}
