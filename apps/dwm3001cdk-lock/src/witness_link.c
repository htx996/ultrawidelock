/* SPDX-License-Identifier: ISC */

/**
 * @file witness_link.c — sealed WV2 witness reports over Thread UDP.
 *
 * Three jobs, in order of how much they matter:
 *
 * 1. Refuse anything that is not a fresh report from an enrolled witness.
 *    AES-CCM under a per-witness key, a monotonic counter per witness boot,
 *    and -- for evidence that could CLEAR the inside veto -- an echo of the
 *    lock's current challenge nonce. A witness holds no authority: every rule
 *    is enforced here, and the worst a forged or flooding witness achieves is
 *    a door that does not open passively.
 *
 * 2. Work out which advertiser in the room is the credential, without ever
 *    learning who the credential is. See ultrawidelock_witness_pick.h: the
 *    address the lock holds from the BLE connection is an InitA and the
 *    addresses the witnesses hear come from advertising sets, so matching one
 *    against the other is unsound. Trajectory correlation replaces it.
 *
 * 3. Hand the paired inside/outside window to the same inbox the RTT bench
 *    feed publishes to, so the side gate and the latch consume evidence
 *    through one path whatever delivered it.
 *
 * Runs entirely on OpenThread's callback and the main loop; starts no thread.
 */

#include "witness_link.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include <openthread/instance.h>
#include <openthread/message.h>
#include <openthread/udp.h>
#include <zephyr/net/openthread.h>

#include <psa/crypto.h>

#include "side_feed.h"
#include "ultrawidelock_witness_msg.h"
#include "ultrawidelock_witness_pick.h"

LOG_MODULE_REGISTER(witness_link, LOG_LEVEL_INF);

#define WITNESS_MAX   CONFIG_ULTRAWIDELOCK_WITNESS_MAX
#define WITNESS_PORT  CONFIG_ULTRAWIDELOCK_WITNESS_PORT
#define NONCE_MS      CONFIG_ULTRAWIDELOCK_WITNESS_NONCE_MS
#define CCM_TAG_LEN   8u
#define CCM_NONCE_LEN 13u
#define KEY_LEN       16u

/* One report must fit a single 802.15.4 frame with room for the seal. */
#define SEALED_MAX (ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN + CCM_TAG_LEN + CCM_NONCE_LEN)

/** One enrolled witness. Keys arrive at enrollment and live only in RAM. */
struct witness_slot {
	uint8_t key[KEY_LEN];
	bool provisioned;
	struct ultrawidelock_witness_seen seen;

	/* Newest accepted report, kept until its pair arrives. */
	struct ultrawidelock_witness_msg msg;
	int64_t msg_ms;
	bool have;
	bool nonce_ok; /**< echoed the CURRENT challenge */
};

static struct witness_slot s_wit[WITNESS_MAX];
static otUdpSocket s_sock;
static bool s_open;

static struct ultrawidelock_witness_pick s_pick;
static int32_t s_range_mm = -1;
static uint64_t s_nonce;
static int64_t s_nonce_ms;
static bool s_session;

/* Reports older than this are not reports. Matches the satellite module's
 * default and sits below the side gate's own evidence_fresh_ms. */
#define WITNESS_STALE_MS 4000

static struct witness_slot *slot_for_role(uint8_t role)
{
	/* Role indexes the table directly: INSIDE=1, OUTSIDE=2, THRESHOLD=3.
	 * One slot per role is deliberate -- a second board claiming a role
	 * that is already reporting is a misconfiguration or an attack, and
	 * either way the newest report for a role is the only useful one. */
	if (role < 1u || role > WITNESS_MAX) {
		return NULL;
	}
	return &s_wit[role - 1u];
}

static void nonce_roll(int64_t now_ms)
{
	sys_rand_get(&s_nonce, sizeof(s_nonce));
	s_nonce_ms = now_ms;
	/* Every stored report now echoes a retired challenge, so none of them
	 * may clear the veto any more. They stay usable in the veto direction,
	 * which is the asymmetry the whole protocol is built on. */
	for (size_t i = 0; i < WITNESS_MAX; i++) {
		s_wit[i].nonce_ok = false;
	}
}

/*
 * The challenge is not secret and is not authenticated on the way out. It does
 * not need to be: its only job is to be unpredictable and current, so a
 * recorded report cannot be replayed into a later approach. An attacker who
 * can read it still cannot produce a report sealed under a witness key.
 */
static void nonce_send(void)
{
	otInstance *ot = openthread_get_default_instance();
	otMessageInfo info;
	otMessage *msg;
	uint8_t body[9];

	if (!s_open || ot == NULL) {
		return;
	}
	body[0] = ULTRAWIDELOCK_WITNESS_MSG_VER;
	for (int i = 0; i < 8; i++) {
		body[1 + i] = (uint8_t)(s_nonce >> (56 - 8 * i));
	}

	msg = otUdpNewMessage(ot, NULL);
	if (msg == NULL) {
		return;
	}
	if (otMessageAppend(msg, body, sizeof(body)) != OT_ERROR_NONE) {
		otMessageFree(msg);
		return;
	}
	memset(&info, 0, sizeof(info));
	/* Mesh-local all-nodes: the witnesses are on this network and nothing
	 * outside it can act on a challenge anyway. */
	info.mPeerAddr.mFields.m8[0] = 0xFFu;
	info.mPeerAddr.mFields.m8[1] = 0x03u;
	info.mPeerAddr.mFields.m8[15] = 0x01u;
	info.mPeerPort = WITNESS_PORT;
	if (otUdpSend(ot, &s_sock, msg, &info) != OT_ERROR_NONE) {
		otMessageFree(msg); /* takes ownership on success only */
	}
}

static bool unseal(struct witness_slot *w, const uint8_t *in, size_t in_len, uint8_t *out,
		   size_t out_cap, size_t *out_len)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_status_t st;

	if (in_len <= CCM_NONCE_LEN + CCM_TAG_LEN) {
		return false;
	}
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT);
	psa_set_key_algorithm(&attr, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CCM_TAG_LEN));
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, KEY_LEN * 8u);
	if (psa_import_key(&attr, w->key, KEY_LEN, &key) != PSA_SUCCESS) {
		return false;
	}
	st = psa_aead_decrypt(key, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CCM_TAG_LEN), in,
			      CCM_NONCE_LEN, NULL, 0, in + CCM_NONCE_LEN, in_len - CCM_NONCE_LEN,
			      out, out_cap, out_len);
	(void)psa_destroy_key(key);
	return st == PSA_SUCCESS;
}

/* Build one correlated window from the inside/outside pair and publish it.
 * Absent evidence is published as absent (INT16_MIN / zero counts) rather than
 * withheld: the side gate's quorum rule is what turns that into a refusal, and
 * it can only do so if it is told. */
static void publish_pair(int64_t now_ms)
{
	struct witness_slot *in = slot_for_role(ULTRAWIDELOCK_WITNESS_ROLE_INSIDE);
	struct witness_slot *out = slot_for_role(ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE);
	struct witness_slot *th = slot_for_role(ULTRAWIDELOCK_WITNESS_ROLE_THRESHOLD);
	const struct ultrawidelock_witness_tuple *t;
	struct ultrawidelock_side_features f;
	uint32_t hash = 0u;
	static uint32_t seq;

	if (in == NULL || out == NULL || !in->have || !out->have) {
		return;
	}
	if ((now_ms - in->msg_ms) > WITNESS_STALE_MS ||
	    (now_ms - out->msg_ms) > WITNESS_STALE_MS) {
		return;
	}
	if (!ultrawidelock_witness_pick_best(&s_pick, &hash)) {
		return; /* nothing in the room is moving with the ranged phone */
	}

	memset(&f, 0, sizeof(f));
	f.obs_session_id = 1u;
	f.seq = ++seq;
	f.now_ms = now_ms;
	f.uwb_range_mm = s_range_mm;
	f.uwb_vel_mm_s = INT32_MIN;
	f.uwb_range_var_mm = -1;
	f.uwb_peer_mm = -1;
	f.ble_rssi_inside_dbm = INT16_MIN;
	f.ble_rssi_outside_dbm = INT16_MIN;
	f.ble_rssi_threshold_dbm = INT16_MIN;
	f.classifier_ver = 1u;
	f.calibration_ver = 1u;

	t = ultrawidelock_witness_msg_find(&in->msg, hash);
	if (t != NULL) {
		f.ble_rssi_inside_dbm = t->mean_dbm;
		f.ble_pkts_inside = t->n_pkts;
		f.anchor_health_mask |= ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE;
	}
	t = ultrawidelock_witness_msg_find(&out->msg, hash);
	if (t != NULL) {
		f.ble_rssi_outside_dbm = t->mean_dbm;
		f.ble_pkts_outside = t->n_pkts;
		f.anchor_health_mask |= ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE;
	}
	if (th != NULL && th->have && (now_ms - th->msg_ms) <= WITNESS_STALE_MS) {
		t = ultrawidelock_witness_msg_find(&th->msg, hash);
		if (t != NULL) {
			f.ble_rssi_threshold_dbm = t->mean_dbm;
			f.ble_pkts_threshold = t->n_pkts;
			f.anchor_health_mask |= ULTRAWIDELOCK_SIDE_ANCHOR_BLE_THRESHOLD;
		}
	}
	if (s_range_mm >= 0) {
		f.anchor_health_mask |= ULTRAWIDELOCK_SIDE_ANCHOR_PRIMARY_UWB;
	}
	/*
	 * A window whose evidence rests on a retired challenge is marked
	 * degraded, which the side gate refuses to release a passive unlock on.
	 * It still classifies, so it can still contradict -- that is the whole
	 * asymmetry: stale evidence may close a door, never open one.
	 */
	if (!in->nonce_ok || !out->nonce_ok) {
		f.flags |= ULTRAWIDELOCK_SIDE_F_DEGRADED;
	}

	side_feed_push(&f);
	in->have = false;
	out->have = false;
}

static void udp_rx(void *ctx, otMessage *msg, const otMessageInfo *info)
{
	uint8_t sealed[SEALED_MAX];
	uint8_t plain[ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN];
	struct ultrawidelock_witness_msg wm;
	struct witness_slot *w;
	size_t plain_len = 0;
	uint16_t len;
	int64_t now;

	ARG_UNUSED(ctx);
	ARG_UNUSED(info);

	len = otMessageGetLength(msg) - otMessageGetOffset(msg);
	if (len == 0u || len > sizeof(sealed)) {
		return;
	}
	if (otMessageRead(msg, otMessageGetOffset(msg), sealed, len) != len) {
		return;
	}
	now = k_uptime_get();

	/*
	 * The role is inside the sealed body, so which key to try is not known
	 * until one of them works. With at most three witnesses, trying each is
	 * cheaper than a plaintext role byte outside the seal would be
	 * dangerous: an unauthenticated selector is an attacker's free choice.
	 */
	for (size_t i = 0; i < WITNESS_MAX; i++) {
		if (!s_wit[i].provisioned) {
			continue;
		}
		if (unseal(&s_wit[i], sealed, len, plain, sizeof(plain), &plain_len)) {
			goto opened;
		}
	}
	return;

opened:
	if (!ultrawidelock_witness_msg_decode(plain, plain_len, &wm)) {
		return;
	}
	w = slot_for_role(wm.role);
	if (w == NULL || !w->provisioned) {
		return;
	}
	if (!ultrawidelock_witness_seen_accept(&w->seen, &wm)) {
		LOG_WRN("witness role=%u replay (ctr=%u)", (unsigned)wm.role, (unsigned)wm.ctr);
		return;
	}

	w->msg = wm;
	w->msg_ms = now;
	w->have = true;
	w->nonce_ok = (wm.echo_nonce == s_nonce) && (s_nonce != 0u);

	if (wm.role == ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE) {
		ultrawidelock_witness_pick_feed(&s_pick, &wm, s_range_mm);
	}
	publish_pair(now);
}

static int witness_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				void *cb_arg)
{
	unsigned long idx;
	char *end;

	/* "k/<role>" -> the link key for that role. Written once at enrollment;
	 * absent means that witness is not enrolled, and an un-enrolled witness
	 * is silence, which fails closed. */
	if (len != KEY_LEN || strncmp(name, "k/", 2) != 0) {
		return -ENOENT;
	}
	idx = strtoul(name + 2, &end, 10);
	if (*end != '\0' || idx < 1u || idx > WITNESS_MAX) {
		return -ENOENT;
	}
	if (read_cb(cb_arg, s_wit[idx - 1u].key, KEY_LEN) != (ssize_t)KEY_LEN) {
		return -EINVAL;
	}
	s_wit[idx - 1u].provisioned = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(witness, "uwl/wit", NULL, witness_settings_set, NULL, NULL);

void witness_link_init(void)
{
	otInstance *ot = openthread_get_default_instance();
	otSockAddr bind_addr;
	unsigned provisioned = 0u;

	ultrawidelock_witness_pick_init(&s_pick, NULL);
	nonce_roll(k_uptime_get());

	for (size_t i = 0; i < WITNESS_MAX; i++) {
		if (s_wit[i].provisioned) {
			provisioned++;
		}
	}
	if (provisioned == 0u) {
		/* Nothing enrolled. The socket still opens so an enrollment can
		 * land, but no report will ever unseal, so the latch stays shut
		 * -- the intended state for an uncommissioned lock. */
		LOG_WRN("no witnesses enrolled; passive unlock stays withheld");
	}
	if (ot == NULL) {
		return;
	}
	memset(&bind_addr, 0, sizeof(bind_addr));
	bind_addr.mPort = WITNESS_PORT;
	if (otUdpOpen(ot, &s_sock, udp_rx, NULL) == OT_ERROR_NONE &&
	    otUdpBind(ot, &s_sock, &bind_addr, OT_NETIF_THREAD) == OT_ERROR_NONE) {
		s_open = true;
		LOG_INF("witness link on UDP %u (%u enrolled)", (unsigned)WITNESS_PORT,
			provisioned);
	} else {
		LOG_ERR("witness link could not bind UDP %u", (unsigned)WITNESS_PORT);
	}
}

void witness_link_set_range_mm(int32_t range_mm)
{
	s_range_mm = range_mm;
}

void witness_link_session(bool up)
{
	if (up == s_session) {
		return;
	}
	s_session = up;
	/* Scoring belongs to one approach. Carrying it across sessions would
	 * let a candidate proven by one walk-up decide a different one. */
	ultrawidelock_witness_pick_reset(&s_pick);
	nonce_roll(k_uptime_get());
	if (up) {
		nonce_send();
	}
}

void witness_link_tick(int64_t now_ms)
{
	if (!s_session) {
		return; /* no approach in progress, nothing to challenge for */
	}
	if ((now_ms - s_nonce_ms) < (int64_t)NONCE_MS) {
		return;
	}
	nonce_roll(now_ms);
	nonce_send();
}

bool witness_link_healthy(int64_t now_ms)
{
	for (size_t i = 0; i < WITNESS_MAX; i++) {
		if (s_wit[i].have && (now_ms - s_wit[i].msg_ms) <= WITNESS_STALE_MS) {
			return true;
		}
	}
	return false;
}
