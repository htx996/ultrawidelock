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
/* struct ultrawidelock_uwb_handoff, whose members the sealed handoff reads --
 * the header only forward-declares it. */
#include <ultrawidelock/uwb.h>
#include "ultrawidelock_witness_msg.h"
#include "ultrawidelock_witness_pick.h"

LOG_MODULE_REGISTER(witness_link, LOG_LEVEL_INF);

#define WITNESS_MAX   CONFIG_ULTRAWIDELOCK_WITNESS_MAX
#define WITNESS_PORT  CONFIG_ULTRAWIDELOCK_WITNESS_PORT
#define NONCE_MS      CONFIG_ULTRAWIDELOCK_WITNESS_NONCE_MS
#define CCM_TAG_LEN   8u
#define CCM_NONCE_LEN 13u
/* From witness_link.h, so the reader build's `ultrawidelock witkey` writes the
 * same name and length this reads. */
#define KEY_LEN       WITNESS_LINK_KEY_LEN

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
static int64_t s_nonce_sent_ms;
static int64_t s_deaf_ms;
/* WV3 sink. NULL on a build with no second anchor, which is why the dispatch
 * checks it: the transport still authenticates and replay-checks the report,
 * it simply has nowhere to put it. */
static witness_link_anchor_cb s_anchor_cb;
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
/* The anchor's OWN credentials. Deliberately not a witness role slot: it is a
 * UWB responder sharing the credential session's keys, and enrolling it through
 * the witness path would tie a live device class to a retired one. */
static uint8_t s_anchor_key[KEY_LEN];
static bool s_anchor_provisioned;
static struct ultrawidelock_witness_seen s_anchor_seen;
#endif
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
	uint8_t body[12];
	uint32_t hint = 0u;

	if (!s_open || ot == NULL) {
		return;
	}
	body[0] = ULTRAWIDELOCK_WITNESS_MSG_VER;
	for (int i = 0; i < 8; i++) {
		body[1 + i] = (uint8_t)(s_nonce >> (56 - 8 * i));
	}

	memset(&info, 0, sizeof(info));
	/* Mesh-local all-nodes: the witnesses are on this network and nothing
	 * outside it can act on a challenge anyway. */
	info.mPeerAddr.mFields.m8[0] = 0xFFu;
	info.mPeerAddr.mFields.m8[1] = 0x03u;
	info.mPeerAddr.mFields.m8[15] = 0x01u;
	info.mPeerPort = WITNESS_PORT;

	/*
	 * The OpenThread API lock, and it is not optional. This runs on the main
	 * thread; OT's own thread is concurrently servicing the radio through
	 * MPSL. Every other caller in this tree takes the lock for exactly this
	 * reason (matter_thread_port.c does it around every call). Callbacks are
	 * the exception -- udp_rx() below already runs with the lock held and
	 * must not take it again.
	 */
	openthread_mutex_lock();
	/* The picked label rides along, so the witnesses can keep it in their
	 * reports even when it would lose the loudness cut -- a pick whose
	 * label one report drops fails quorum, and that was every window of
	 * an approach (measured 2026-08-21). The label is opaque to anyone
	 * without the witness group key, and a forged hint buys at most one
	 * junk tuple per report: inclusion is not authority. Zero means no
	 * pick; a real label hashing to zero loses its hint, one in 16M.
	 * Read under the OT lock: udp_rx() feeds s_pick on the OT thread. */
	(void)ultrawidelock_witness_pick_best(&s_pick, &hint);
	body[9] = (uint8_t)(hint >> 16);
	body[10] = (uint8_t)(hint >> 8);
	body[11] = (uint8_t)hint;
	msg = otUdpNewMessage(ot, NULL);
	if (msg != NULL) {
		if (otMessageAppend(msg, body, sizeof(body)) != OT_ERROR_NONE ||
		    otUdpSend(ot, &s_sock, msg, &info) != OT_ERROR_NONE) {
			otMessageFree(msg); /* send takes ownership on success only */
		}
	}
	openthread_mutex_unlock();
	s_nonce_sent_ms = k_uptime_get();
}

/* Takes the KEY, not a witness slot: the second anchor holds its own key and is
 * not enrolled as a witness, so the seal cannot be tied to that slot type. */
static bool unseal_key(const uint8_t *k, const uint8_t *in, size_t in_len, uint8_t *out,
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
	if (psa_import_key(&attr, k, KEY_LEN, &key) != PSA_SUCCESS) {
		return false;
	}
	st = psa_aead_decrypt(key, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CCM_TAG_LEN), in,
			      CCM_NONCE_LEN, NULL, 0, in + CCM_NONCE_LEN, in_len - CCM_NONCE_LEN,
			      out, out_cap, out_len);
	(void)psa_destroy_key(key);
	return st == PSA_SUCCESS;
}

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
/*
 * THE LOCK'S NONCE PREFIX, and the whole reason this constant exists.
 *
 * The anchor link key is ONE key used in BOTH directions: the satellite seals
 * WV3 reports with it and the lock now seals WV4 handoffs with it. Under
 * AES-CCM a repeated (key, nonce) pair is catastrophic -- it leaks the XOR of
 * two plaintexts and forges the MAC -- so the two senders' nonce spaces must be
 * provably disjoint, not merely unlikely to collide.
 *
 * They are disjoint in byte 0. The satellite writes its ROLE there
 * (anchor_link.c seal()), and apps/nrf5340dk-satellite/Kconfig constrains
 * ULTRAWIDELOCK_ANCHOR_ROLE to `range 1 3`. 0xFF is outside that range and no
 * conforming anchor can ever emit it, so no counter or boot id either side
 * chooses can bring the two nonces together.
 *
 * If a role is ever widened past 3, this constant must move with it.
 */
#define HANDOFF_NONCE_ROLE 0xFFu

/* Fresh per boot for the same reason the satellite's is: it makes a counter
 * that legitimately restarts at zero distinguishable from a replay. */
static uint32_t s_lock_boot_id;
static uint32_t s_handoff_ctr;

/** Seal under an explicit key: nonce ‖ ciphertext ‖ tag, what the peer unseals. */
static size_t seal_key(const uint8_t *k, const uint8_t *nonce, const uint8_t *plain,
		       size_t plain_len, uint8_t *out, size_t cap)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_algorithm_t alg = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, CCM_TAG_LEN);
	size_t ct_len = 0;
	psa_status_t st;

	if (cap < CCM_NONCE_LEN + plain_len + CCM_TAG_LEN) {
		return 0;
	}
	memcpy(out, nonce, CCM_NONCE_LEN);

	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attr, alg);
	psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attr, KEY_LEN * 8u);
	if (psa_import_key(&attr, k, KEY_LEN, &key) != PSA_SUCCESS) {
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

void witness_link_send_handoff(const struct ultrawidelock_uwb_handoff *h)
{
	struct ultrawidelock_join_msg jm;
	uint8_t plain[ULTRAWIDELOCK_JOIN_MSG_LEN];
	uint8_t sealed[CCM_NONCE_LEN + ULTRAWIDELOCK_JOIN_MSG_LEN + CCM_TAG_LEN];
	uint8_t nonce[CCM_NONCE_LEN];
	otInstance *ot = openthread_get_default_instance();
	otMessageInfo info;
	otMessage *msg;
	size_t plain_len;
	size_t sealed_len;
	uint32_t sent_ctr;

	if (h == NULL || !s_open || ot == NULL || !s_anchor_provisioned) {
		return;
	}
	if (h->ursk == NULL || h->ursk_len != ULTRAWIDELOCK_JOIN_URSK_LEN ||
	    h->rcfg == NULL || h->rcfg_len != ULTRAWIDELOCK_JOIN_RCFG_LEN) {
		/* A size the codec cannot carry means this build and the ranging
		 * engine disagree about the wire format. Sending a truncated key
		 * would put the satellite on a schedule nothing else is using. */
		LOG_WRN("handoff not sent: ursk %u B rcfg %u B, expected %u/%u",
			(unsigned)h->ursk_len, (unsigned)h->rcfg_len,
			ULTRAWIDELOCK_JOIN_URSK_LEN, ULTRAWIDELOCK_JOIN_RCFG_LEN);
		return;
	}

	memset(&jm, 0, sizeof(jm));
	jm.ver = ULTRAWIDELOCK_JOIN_MSG_VER;
	jm.boot_id = s_lock_boot_id;
	jm.ctr = ++s_handoff_ctr; /* pre-increment: the nonce below must match */
	memcpy(jm.ursk, h->ursk, ULTRAWIDELOCK_JOIN_URSK_LEN);
	memcpy(jm.rcfg, h->rcfg, ULTRAWIDELOCK_JOIN_RCFG_LEN);
	jm.channel = h->channel;
	jm.sync_code_index = h->sync_code_index;

	plain_len = ultrawidelock_join_msg_encode(&jm, plain, sizeof(plain));
	if (plain_len == 0u) {
		return;
	}

	memset(nonce, 0, sizeof(nonce));
	nonce[0] = HANDOFF_NONCE_ROLE;
	nonce[1] = (uint8_t)(s_lock_boot_id >> 24);
	nonce[2] = (uint8_t)(s_lock_boot_id >> 16);
	nonce[3] = (uint8_t)(s_lock_boot_id >> 8);
	nonce[4] = (uint8_t)s_lock_boot_id;
	nonce[5] = (uint8_t)(jm.ctr >> 24);
	nonce[6] = (uint8_t)(jm.ctr >> 16);
	nonce[7] = (uint8_t)(jm.ctr >> 8);
	nonce[8] = (uint8_t)jm.ctr;

	sealed_len = seal_key(s_anchor_key, nonce, plain, plain_len, sealed, sizeof(sealed));
	sent_ctr = jm.ctr;
	/* The URSK is in here; do not leave a copy on the stack for the rest of
	 * the frame. */
	memset(&jm, 0, sizeof(jm));
	memset(plain, 0, sizeof(plain));
	if (sealed_len == 0u) {
		return;
	}

	memset(&info, 0, sizeof(info));
	/* Mesh-local all-nodes, matching every other message on this link: the
	 * lock is never told the satellite's address, so replacing the satellite
	 * costs no re-provisioning. Only a key holder can read the handoff. */
	info.mPeerAddr.mFields.m8[0] = 0xFFu;
	info.mPeerAddr.mFields.m8[1] = 0x03u;
	info.mPeerAddr.mFields.m8[15] = 0x01u;
	info.mPeerPort = WITNESS_PORT;

	/*
	 * Called from the credential thread, not an OT callback, so the API lock
	 * is ours to take -- same rule as nonce_send() above.
	 */
	openthread_mutex_lock();
	msg = otUdpNewMessage(ot, NULL);
	if (msg != NULL) {
		if (otMessageAppend(msg, sealed, (uint16_t)sealed_len) != OT_ERROR_NONE ||
		    otUdpSend(ot, &s_sock, msg, &info) != OT_ERROR_NONE) {
			otMessageFree(msg); /* send takes ownership on success only */
		}
	}
	openthread_mutex_unlock();
	LOG_INF("handoff sent to the second anchor (ctr %u)", (unsigned)sent_ctr);
}
#endif /* CONFIG_ULTRAWIDELOCK_ANCHOR_LINK */

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

	/* Rate-limited: a link that has not yet formed a pair is otherwise
	 * silent, and "no reports at all", "one witness missing" and "pair
	 * fine but the phone not yet correlated" all look identical from the
	 * console. One line every 5 s names the stage that is starving. */
	static int64_t s_pair_log_ms;
	bool say = (now_ms - s_pair_log_ms) > 5000;

	if (in == NULL || out == NULL || !in->have || !out->have) {
		if (say) {
			s_pair_log_ms = now_ms;
			LOG_INF("witness pair: waiting (in=%d out=%d)",
				(in != NULL && in->have) ? 1 : 0,
				(out != NULL && out->have) ? 1 : 0);
		}
		return;
	}
	if ((now_ms - in->msg_ms) > WITNESS_STALE_MS ||
	    (now_ms - out->msg_ms) > WITNESS_STALE_MS) {
		if (say) {
			s_pair_log_ms = now_ms;
			LOG_INF("witness pair: stale (in %d ms, out %d ms)",
				(int)(now_ms - in->msg_ms), (int)(now_ms - out->msg_ms));
		}
		return;
	}
	if (!ultrawidelock_witness_pick_best(&s_pick, &hash)) {
		if (say) {
			struct ultrawidelock_witness_pick_stats st;

			s_pair_log_ms = now_ms;
			ultrawidelock_witness_pick_stats(&s_pick, &st);
			/* Which gate refuses: score, windows, or the rival's
			 * margin. evict counts labels pushed out of the table;
			 * climbing fast means the room has more advertisers
			 * than slots and nothing can accumulate score. */
			LOG_INF("witness pair: no pick (cand=%u best=%d win=%u rival=%d gap=%d evict=%u range=%d)",
				(unsigned)st.n_cand, (int)st.best_score,
				(unsigned)st.best_windows, (int)st.runner_score,
				(int)st.runner_gap_db,
				(unsigned)st.evictions, (int)s_range_mm);
		}
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

	/*
	 * A picked label that NEITHER fresh report carries any more is not weak
	 * signal, it is a retired advertising address: the handset rotated its
	 * RPA and will never use this label again. Left in place, its banked
	 * score outranks the same handset's new label by the pick margin and
	 * blocks re-picking for the rest of the approach (measured 2026-08-21:
	 * phone touching the outside witness, oi_pkts=0/0 for minutes). Three
	 * misses tell it apart from one report's tuple-cut flicker.
	 */
	{
		static uint32_t s_miss_hash;
		static uint8_t s_miss_n;

		if ((f.anchor_health_mask & (ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE |
					     ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE)) == 0u) {
			if (s_miss_hash == hash && s_miss_n < 0xFFu) {
				s_miss_n++;
			} else {
				s_miss_hash = hash;
				s_miss_n = 1u;
			}
			if (s_miss_n >= 3u) {
				uint32_t heir = ultrawidelock_witness_pick_succeed(
					&s_pick, hash, &out->msg);

				if (heir != 0u) {
					LOG_INF("witness pick: label rotated, successor inherits");
				} else {
					LOG_INF("witness pick: label retired (address rotated)");
					ultrawidelock_witness_pick_retire(&s_pick, hash);
				}
				s_miss_n = 0u;
			}
		} else {
			s_miss_n = 0u;
		}
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
		if (unseal_key(s_wit[i].key, sealed, len, plain, sizeof(plain), &plain_len)) {
			goto opened;
		}
	}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	/* The second anchor's own key, tried after the witnesses and enrolled
	 * separately from them. */
	if (s_anchor_provisioned &&
	    unseal_key(s_anchor_key, sealed, len, plain, sizeof(plain), &plain_len)) {
		goto opened;
	}
#endif
	/*
	 * Rate-limited on purpose, and present at all for one reason: a link
	 * key typed differently on the two ends drops every report here in
	 * silence, and silence is indistinguishable from a witness that never
	 * booted. Both fail closed, but only one is fixed by retyping a key.
	 * Says nothing about which key or how it differed.
	 */
	if (now - s_deaf_ms > 10000) {
		s_deaf_ms = now;
		LOG_WRN("witness datagram no enrolled key opened (%u B); check the "
			"link keys match", (unsigned)len);
	}
	return;

opened:
	/*
	 * Demultiplex on the version byte BEFORE choosing a decoder. WV3 is the
	 * second anchor's own measured distance; it rides this link unchanged --
	 * same socket, same seal, same per-role key -- and shares WV2's leading
	 * fields precisely so the replay window below applies to both.
	 */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	if (ultrawidelock_msg_is_anchor(plain, plain_len)) {
		struct ultrawidelock_anchor_msg am;

		if (!ultrawidelock_anchor_msg_decode(plain, plain_len, &am)) {
			return;
		}
		if (!s_anchor_provisioned) {
			return;
		}
		if (!ultrawidelock_seen_accept_ctr(&s_anchor_seen, am.boot_id, am.ctr)) {
			LOG_WRN("anchor role=%u replay (ctr=%u)", (unsigned)am.role,
				(unsigned)am.ctr);
			return;
		}
		if (s_anchor_cb != NULL) {
			s_anchor_cb(am.peer_mm, am.ranging_block, now);
		}
		return;
	}
#endif
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

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
/* Its own subtree, so the anchor's key is not enrolled, listed or erased as
 * part of the witness set. */
static int anchor_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			       void *cb_arg)
{
	if (len != KEY_LEN || strcmp(name, "k") != 0) {
		return -ENOENT;
	}
	if (read_cb(cb_arg, s_anchor_key, KEY_LEN) != (ssize_t)KEY_LEN) {
		return -EINVAL;
	}
	s_anchor_provisioned = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(anchor, "uwl/anc", NULL, anchor_settings_set, NULL, NULL);
#endif

void witness_link_set_anchor_cb(witness_link_anchor_cb cb)
{
	s_anchor_cb = cb;
}

void witness_link_init(void)
{
	otInstance *ot = openthread_get_default_instance();
	otSockAddr bind_addr;
	unsigned provisioned = 0u;

	ultrawidelock_witness_pick_init(&s_pick, NULL);
	nonce_roll(k_uptime_get());

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	/* Never zero: the satellite reads a change of boot id as permission to
	 * accept a counter that went backwards, and zero is what an
	 * uninitialised one looks like. */
	do {
		s_lock_boot_id = sys_rand32_get();
	} while (s_lock_boot_id == 0u);
	s_handoff_ctr = 0u;
#endif

	/*
	 * Load the keys. Registering the handler above does NOT read anything --
	 * a static handler only says who to call, and something has to ask. This
	 * line was missing, so every enrolled key sat in flash unread and the
	 * lock reported "0 enrolled" with the keys right there; the latch's own
	 * load two files over made the omission easy to miss.
	 *
	 * Not settings_load(): the whole store includes the Matter fabric, whose
	 * handlers have already run by now, and re-running them is neither free
	 * nor obviously safe.
	 */
	(void)settings_load_subtree("uwl/wit");
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	(void)settings_load_subtree("uwl/anc");
#endif

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
	openthread_mutex_lock();
	if (otUdpOpen(ot, &s_sock, udp_rx, NULL) == OT_ERROR_NONE &&
	    otUdpBind(ot, &s_sock, &bind_addr, OT_NETIF_THREAD) == OT_ERROR_NONE) {
		openthread_mutex_unlock();
		s_open = true;
		LOG_INF("witness link on UDP %u (%u enrolled)", (unsigned)WITNESS_PORT,
			provisioned);
	} else {
		openthread_mutex_unlock();
		LOG_ERR("witness link could not bind UDP %u", (unsigned)WITNESS_PORT);
	}
}

void witness_link_set_range_mm(int32_t range_mm)
{
	s_range_mm = range_mm;
}

/* How long a gap between credential sessions still counts as the SAME
 * approach. iOS tears the session down on its credential phase deadline and
 * reconnects within seconds, mid-walk; resetting the pick on that flap
 * discards the correlation right when it was about to complete (measured
 * 2026-08-20: session destroyed 22:53:28, recreated 22:53:30, walk lost).
 * Two different walk-ups are separated by minutes, not seconds. */
#define SESSION_CARRY_MS 30000

void witness_link_session(bool up)
{
	static int64_t s_down_ms;
	int64_t now = k_uptime_get();

	if (up == s_session) {
		return;
	}
	s_session = up;
	if (!up) {
		s_down_ms = now;
		nonce_roll(now);
		return;
	}
	/* UNPROVEN scoring belongs to one approach: carrying half-built score
	 * across approaches would let a lucky candidate from one walk-up decide
	 * a different one, so with nothing committed the table resets. A
	 * COMMITTED pick names an address, and the address IS the phone until
	 * it rotates -- retiring on rotation (or on both witnesses dropping it)
	 * is what ends its authority, not the minutes between walk-ups. Without
	 * this every approach re-derived the pick from scratch, which needs
	 * exactly the trajectory a short walk-up does not have (measured
	 * 2026-08-21: the pick took a full pacing pass to rebuild while the
	 * grant it fed was over in 12 s). */
	if ((now - s_down_ms) > SESSION_CARRY_MS &&
	    !ultrawidelock_witness_pick_best(&s_pick, NULL)) {
		ultrawidelock_witness_pick_reset(&s_pick);
	}
	nonce_roll(now);
	nonce_send();
}

void witness_link_tick(int64_t now_ms)
{
	if (!s_session) {
		return; /* no approach in progress, nothing to challenge for */
	}
	if ((now_ms - s_nonce_ms) < (int64_t)NONCE_MS) {
		/* Re-send the CURRENT challenge without rolling it. A challenge
		 * is one unacknowledged UDP multicast through a sleepy child's
		 * parent queue; when that single datagram is lost, every report
		 * stays degraded until the next roll, 30 s away (measured
		 * 2026-08-20: one lost challenge degraded a whole approach).
		 * Resending is free -- the challenge is a freshness beacon, not
		 * a secret, and the witnesses just overwrite the same value. */
		if ((now_ms - s_nonce_sent_ms) >= 3000) {
			nonce_send();
		}
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
