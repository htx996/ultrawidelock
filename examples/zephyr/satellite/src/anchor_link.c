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

#include <openthread/message.h>
#include <openthread/udp.h>
#include <zephyr/net/openthread.h>

#include <psa/crypto.h>

#include "anchor_link.h"
#include "ultrawidelock_witness_msg.h"

LOG_MODULE_REGISTER(anclink, LOG_LEVEL_INF);

#define KEY_LEN        16u
#define CCM_NONCE_LEN  13u
#define CCM_TAG_LEN    8u
#define ANCHOR_PORT    CONFIG_ULTRAWIDELOCK_WITNESS_PORT

static otUdpSocket s_sock;
static bool s_open;
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

/** Seal a payload: nonce ‖ ciphertext ‖ tag, exactly what the lock unseals. */
static size_t seal(const uint8_t *plain, size_t plain_len, uint8_t *out, size_t cap)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CCM_TAG_LEN);
	size_t ct_len = 0;
	psa_status_t st;

	if (cap < CCM_NONCE_LEN + plain_len + CCM_TAG_LEN) {
		return 0;
	}
	/*
	 * Explicit nonce, and it must never repeat under one key: role, boot_id,
	 * counter, then zeros. The counter is what makes it unique within a boot
	 * and the boot_id across boots.
	 */
	memset(out, 0, CCM_NONCE_LEN);
	out[0] = (uint8_t)CONFIG_ULTRAWIDELOCK_ANCHOR_ROLE;
	out[1] = (uint8_t)(s_boot_id >> 24);
	out[2] = (uint8_t)(s_boot_id >> 16);
	out[3] = (uint8_t)(s_boot_id >> 8);
	out[4] = (uint8_t)s_boot_id;
	out[5] = (uint8_t)(s_ctr >> 24);
	out[6] = (uint8_t)(s_ctr >> 16);
	out[7] = (uint8_t)(s_ctr >> 8);
	out[8] = (uint8_t)s_ctr;

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, alg);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, KEY_LEN * 8u);
	if (psa_import_key(&attr, s_key, KEY_LEN, &key) != PSA_SUCCESS) {
		return 0;
	}
	st = psa_aead_encrypt(key, alg, out, CCM_NONCE_LEN, NULL, 0, plain, plain_len,
			      out + CCM_NONCE_LEN, cap - CCM_NONCE_LEN, &ct_len);
	(void)psa_destroy_key(key);
	if (st != PSA_SUCCESS) {
		return 0;
	}
	return CCM_NONCE_LEN + ct_len;
}

/**
 * The lock's challenge. Unauthenticated on purpose: it is a freshness beacon,
 * not a command. Echoing the wrong one costs this report its standing; it
 * cannot make the lock do anything.
 */
static void udp_rx(void *ctx, otMessage *msg, const otMessageInfo *info)
{
	uint8_t body[12];
	uint16_t len;

	ARG_UNUSED(ctx);
	ARG_UNUSED(info);

	len = otMessageGetLength(msg) - otMessageGetOffset(msg);
	if (len != 9u) {
		return; /* not a challenge; the lock's reports are not for us */
	}
	if (otMessageRead(msg, otMessageGetOffset(msg), body, len) != len) {
		return;
	}
	if (body[0] != ULTRAWIDELOCK_WITNESS_MSG_VER) {
		return;
	}
	s_echo_nonce = 0u;
	for (int i = 0; i < 8; i++) {
		s_echo_nonce = (s_echo_nonce << 8) | (uint64_t)body[1 + i];
	}
}

void anchor_link_report(int32_t peer_mm, uint32_t ranging_block)
{
	struct ultrawidelock_anchor_msg am;
	uint8_t plain[ULTRAWIDELOCK_ANCHOR_MSG_LEN];
	uint8_t sealed[CCM_NONCE_LEN + ULTRAWIDELOCK_ANCHOR_MSG_LEN + CCM_TAG_LEN];
	otInstance *ot = openthread_get_default_instance();
	otMessageInfo info;
	otMessage *msg;
	size_t plain_len;
	size_t sealed_len;

	if (!s_open || !s_provisioned || ot == NULL || peer_mm < 0) {
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
	return s_open && s_provisioned;
}

void anchor_link_init(void)
{
	otInstance *ot;

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
	openthread_mutex_lock();
	if (otUdpOpen(ot, &s_sock, udp_rx, NULL) == OT_ERROR_NONE) {
		otSockAddr addr = {0};

		addr.mPort = ANCHOR_PORT;
		s_open = (otUdpBind(ot, &s_sock, &addr, OT_NETIF_THREAD) == OT_ERROR_NONE);
	}
	openthread_mutex_unlock();

	if (!s_open) {
		LOG_WRN("anchor link socket did not open");
	} else if (!s_provisioned) {
		/* Loud, because it fails closed and silently otherwise: an
		 * un-provisioned anchor ranges perfectly and reports nothing,
		 * which at the lock is indistinguishable from a board that never
		 * booted. */
		LOG_WRN("anchor link has no key; run `sat key <hex32>`");
	}
}
