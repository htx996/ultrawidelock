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
 * Runs entirely on the datagram link's receive callback and the main loop;
 * starts no thread. The transport itself is ultrawidelock_dgram.h -- this file
 * names no OpenThread symbol and takes no OpenThread lock. What it sends is a
 * sealed blob to the group; who carries it is the port's business.
 */

#include "witness_link.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "ultrawidelock_dgram.h"
#include "ultrawidelock_kv.h"
#include "ultrawidelock_link.h"

#include "side_feed.h"
/* struct ultrawidelock_uwb_handoff, whose members the sealed handoff reads --
 * the header only forward-declares it. */
#include <ultrawidelock/uwb.h>
#include "ultrawidelock_seal.h"
#include "ultrawidelock_witness_msg.h"
#include "ultrawidelock_witness_pick.h"

LOG_MODULE_REGISTER(witness_link, LOG_LEVEL_INF);

#define WITNESS_MAX   CONFIG_ULTRAWIDELOCK_WITNESS_MAX
#define WITNESS_PORT  CONFIG_ULTRAWIDELOCK_WITNESS_PORT
#define NONCE_MS      CONFIG_ULTRAWIDELOCK_WITNESS_NONCE_MS
/* The envelope's own constants, from the ONE definition of it
 * (ultrawidelock_seal.h). Aliased rather than redefined: this file used to
 * carry its own copies, which is how a wire format grows two versions. */
#define CCM_TAG_LEN   ULTRAWIDELOCK_SEAL_TAG_LEN
#define CCM_NONCE_LEN ULTRAWIDELOCK_SEAL_NONCE_LEN
/* From witness_link.h, so the reader build's `ultrawidelock witkey` writes the
 * same name and length this reads. The seal is sized independently and must
 * agree, or a key would be truncated or over-read on its way into PSA. */
#define KEY_LEN       WITNESS_LINK_KEY_LEN
BUILD_ASSERT(WITNESS_LINK_KEY_LEN == ULTRAWIDELOCK_SEAL_KEY_LEN,
	     "witness link key width must match the seal's");
BUILD_ASSERT(ULTRAWIDELOCK_KV_KEY_LINK_WITNESS_KEY_BASE + ULTRAWIDELOCK_LINK_ROLE_MAX <
		     ULTRAWIDELOCK_KV_KEY_LINK_WITNESS_KEY_LIMIT,
	     "the three witness-role keys must fit their assigned KV range");

/* One report must fit a single 802.15.4 frame with room for the seal. */
#define SEALED_MAX (ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN + CCM_TAG_LEN + CCM_NONCE_LEN)
/* And it must fit one datagram, which ultrawidelock_dgram.h asks every consumer
 * to check for itself: a report over the cap would be refused at send time, on a
 * board, after a tuple count change that looked harmless. */
BUILD_ASSERT(SEALED_MAX <= ULTRAWIDELOCK_DGRAM_MAX,
	     "a sealed witness report no longer fits one datagram");

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
static struct ultrawidelock_link s_anchor_link;
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
	/* Never zero. Zero is this file's "no challenge has been issued" value
	 * -- it is what nonce_ok checks below and what disarms the anchor link
	 * -- so a random draw that happened to be zero would silently retire
	 * the freshness rules instead of rolling them. */
	do {
		sys_rand_get(&s_nonce, sizeof(s_nonce));
	} while (s_nonce == 0u);
	s_nonce_ms = now_ms;
	/* Every stored report now echoes a retired challenge, so none of them
	 * may clear the veto any more. They stay usable in the veto direction,
	 * which is the asymmetry the whole protocol is built on. */
	for (size_t i = 0; i < WITNESS_MAX; i++) {
		s_wit[i].nonce_ok = false;
	}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	/*
	 * The second anchor is judged harder than a witness: a WV3 report is
	 * evidence about geometry, not a vote, so a retired challenge does not
	 * merely cost it standing -- the link core refuses it outright, and
	 * refuses a boot id that changes without a challenge in between. Armed
	 * from here because this is the only place s_nonce changes, so what the
	 * wire carries and what the core requires cannot come apart.
	 */
	ultrawidelock_link_expect_echo(&s_anchor_link, s_nonce);
#endif
}

/*
 * The challenge is not secret and is not authenticated on the way out. It does
 * not need to be: its only job is to be unpredictable and current, so a
 * recorded report cannot be replayed into a later approach. An attacker who
 * can read it still cannot produce a report sealed under a witness key.
 */
static void nonce_send(void)
{
	uint8_t body[ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN];
	uint32_t hint = 0u;
	size_t body_len;

	if (!ultrawidelock_dgram_ready()) {
		return;
	}
	/* The picked label rides along, so the witnesses can keep it in their
	 * reports even when it would lose the loudness cut -- a pick whose
	 * label one report drops fails quorum, and that was every window of
	 * an approach (measured 2026-08-21). The label is opaque to anyone
	 * without the witness group key, and a forged hint buys at most one
	 * junk tuple per report: inclusion is not authority. Zero means no
	 * pick; a real label hashing to zero loses its hint, one in 16M.
	 *
	 * UNGUARDED, and it was already half-unguarded before the transport
	 * moved to ultrawidelock_dgram.h. This read used to sit inside the
	 * OpenThread API lock, which udp_rx() also holds, so it happened to
	 * exclude the writer -- but witness_link_session() has always read and
	 * reset s_pick from the main thread with no lock at all, so the
	 * discipline was never complete. The seam owns its own locking and does
	 * not lend it out. Closing this properly means deciding which thread
	 * owns s_pick, which is a change to this file's threading model rather
	 * than to its transport; it is deliberately not folded in here. The
	 * exposure is one torn hint on a challenge, and a wrong hint costs a
	 * witness one window. */
	(void)ultrawidelock_witness_pick_best(&s_pick, &hint);
	body_len = ultrawidelock_link_build_challenge_hint(s_nonce, hint, body, sizeof(body));
	if (body_len == 0u) {
		return;
	}
	(void)ultrawidelock_dgram_send(body, body_len);
	s_nonce_sent_ms = k_uptime_get();
}

/*
 * The seal and its inverse are ultrawidelock_seal.h's, shared with the
 * satellite: ONE AES-CCM envelope, so the two ends cannot drift apart about
 * what a sealed datagram looks like. Both take the KEY, not a witness slot --
 * the second anchor holds its own key and is not enrolled as a witness, so the
 * seal cannot be tied to that slot type.
 */

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
void witness_link_send_handoff(const struct ultrawidelock_uwb_handoff *h)
{
	uint8_t sealed[ULTRAWIDELOCK_LINK_MAX_FRAME];
	size_t sealed_len;
	uint32_t sent_ctr;

	if (h == NULL || !ultrawidelock_dgram_ready() ||
	    !ultrawidelock_link_ready(&s_anchor_link)) {
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

	sealed_len = ultrawidelock_link_build_join(&s_anchor_link, h->ursk, h->rcfg, h->channel,
						    h->sync_code_index, sealed, sizeof(sealed));
	sent_ctr = s_anchor_link.ctr;
	if (sealed_len == 0u) {
		return;
	}

	/*
	 * To the group, like everything else on this link: the lock is never told
	 * the satellite's address, so replacing the satellite costs no
	 * re-provisioning. Only a key holder can read the handoff.
	 *
	 * Called from the credential thread, not from the receive callback, so
	 * taking the transport's lock is allowed -- ultrawidelock_dgram.h states
	 * that rule once, where it used to be a comment in each sender.
	 */
	(void)ultrawidelock_dgram_send(sealed, sealed_len);
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

static void link_rx(void *ctx, const uint8_t *sealed, size_t len)
{
	uint8_t plain[ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN];
	struct ultrawidelock_witness_msg wm;
	struct witness_slot *w;
	size_t plain_len = 0;
	int64_t now;
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	struct ultrawidelock_anchor_msg am = {0};
	struct ultrawidelock_join_msg jm = {0};
	enum ultrawidelock_link_rx anchor_rx;
#endif

	ARG_UNUSED(ctx);

	/*
	 * The datagram arrives flattened and already length-checked against
	 * ULTRAWIDELOCK_DGRAM_MAX, so the otMessage offset arithmetic this
	 * function used to open with is gone. The cap that remains is this
	 * protocol's own: a report longer than the seal can produce is not a
	 * report, whatever the transport was willing to carry.
	 */
	if (len == 0u || len > SEALED_MAX) {
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
		if (ultrawidelock_unseal(s_wit[i].key, sealed, len, plain, sizeof(plain),
					 &plain_len)) {
			goto opened;
		}
	}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	/* The second anchor's own key, tried after the witnesses and enrolled
	 * separately from them. Its WV3/WV4 decisions live in the carrier-free
	 * link core; the WV2 loop above remains unchanged. */
	anchor_rx = ultrawidelock_link_consume(&s_anchor_link, sealed, len, &am, &jm);
	/* This lock does not consume WV4, but its own multicast can loop back.
	 * Clear the decoded URSK before dispatching the non-sensitive result. */
	memset(&jm, 0, sizeof(jm));
	switch (anchor_rx) {
	case ULTRAWIDELOCK_LINK_RX_REPORT:
		if (s_anchor_cb != NULL) {
			s_anchor_cb(am.role, am.peer_mm, am.ranging_block, now);
		}
		return;
	case ULTRAWIDELOCK_LINK_RX_REPLAYED:
		/* A replayed WV4 is our own multicast returning. The report form
		 * retains role/counter solely for this existing diagnostic. */
		if (am.role != 0u) {
			LOG_WRN("anchor role=%u replay (ctr=%u)", (unsigned)am.role,
				(unsigned)am.ctr);
		}
		return;
	case ULTRAWIDELOCK_LINK_RX_JOIN:
	case ULTRAWIDELOCK_LINK_RX_CHALLENGE:
	case ULTRAWIDELOCK_LINK_RX_MALFORMED:
		return;
	case ULTRAWIDELOCK_LINK_RX_UNSEALED:
	case ULTRAWIDELOCK_LINK_RX_IGNORED:
	default:
		break;
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
	/* Ordinary WV2 handling stays here: its key selection, decoder, replay
	 * window, nonce standing, picker feed and error behavior are unchanged. */
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

void witness_link_set_anchor_cb(witness_link_anchor_cb cb)
{
	s_anchor_cb = cb;
}

void witness_link_init(void)
{
	unsigned provisioned = 0u;
	int kv_rc;
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	uint8_t anchor_key[KEY_LEN];
	size_t anchor_key_len = sizeof(anchor_key);
	uint32_t boot_id;
#endif

	ultrawidelock_witness_pick_init(&s_pick, NULL);

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	/* Never zero: the satellite reads a change of boot id as permission to
	 * accept a counter that went backwards, and zero is what an
	 * uninitialised one looks like. */
	do {
		boot_id = sys_rand32_get();
	} while (boot_id == 0u);
	/* BEFORE the first nonce_roll(), which arms this struct. link_init()
	 * clears every field, so the other order would leave the anchor link
	 * unarmed until the next challenge. */
	ultrawidelock_link_init(&s_anchor_link, ULTRAWIDELOCK_LINK_HANDOFF_ROLE, boot_id);
#endif
	nonce_roll(k_uptime_get());

	/* Numeric keys keep the consumers independent of Zephyr's settings tree.
	 * Only the three semantic witness roles have assigned records; a configured
	 * fourth slot remains deliberately unprovisionable. */
	kv_rc = ultrawidelock_kv_init();
	if (kv_rc == ULTRAWIDELOCK_KV_OK) {
		for (size_t i = 0; i < WITNESS_MAX && i < ULTRAWIDELOCK_LINK_ROLE_MAX; i++) {
			size_t key_len = KEY_LEN;

			if (ultrawidelock_kv_get(
				    (uint16_t)(ULTRAWIDELOCK_KV_KEY_LINK_WITNESS_KEY_BASE + i + 1u),
				    s_wit[i].key, &key_len) == ULTRAWIDELOCK_KV_OK &&
			    key_len == KEY_LEN) {
				s_wit[i].provisioned = true;
			}
		}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
		if (ultrawidelock_kv_get(ULTRAWIDELOCK_KV_KEY_LINK_ANCHOR_KEY, anchor_key,
					  &anchor_key_len) == ULTRAWIDELOCK_KV_OK) {
			(void)ultrawidelock_link_set_key(&s_anchor_link, anchor_key, anchor_key_len);
		}
#endif
	}
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR_LINK)
	memset(anchor_key, 0, sizeof(anchor_key));
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
	if (ultrawidelock_dgram_open(WITNESS_PORT, link_rx, NULL) == ULTRAWIDELOCK_DGRAM_OK) {
		LOG_INF("witness link on port %u (%u enrolled)", (unsigned)WITNESS_PORT,
			provisioned);
	} else {
		/* Not fatal, and not retried here. A link that never opened
		 * delivers nothing, and everything below fails closed on
		 * silence: the latch stays shut, which is the safe end. */
		LOG_ERR("witness link could not open port %u", (unsigned)WITNESS_PORT);
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
