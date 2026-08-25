/* SPDX-License-Identifier: ISC */

/**
 * @file test_ultrawidelock_link.c — the sealed link's decisions, over a loopback.
 *
 * Two ends of one link in one process: a satellite that composes reports and a
 * lock that consumes them, with the bytes handed straight across. That is the
 * whole point — the checks below are about what one end will BELIEVE from the
 * other, and a loopback is the only place that can be asserted without two
 * boards and a radio.
 *
 * THE BACKEND DOES NO CRYPTO (psafake). So "unsealed correctly" here means the
 * envelope's arguments were plumbed through and the frame round-tripped, not
 * that a tag was verified. What that leaves fully testable is the part that
 * actually decides things and has no crypto in it: message demultiplexing by
 * length, the replay window, the challenge echo, and the counter/nonce
 * agreement. Those are the rules a forged or recorded datagram meets first.
 *
 * The one thing a fake backend CANNOT show is a rejection that depends on a
 * real tag, so every "wrong key" case below is driven by the failure knob
 * rather than by a genuinely wrong key. That is stated rather than hidden: it
 * means these tests prove the branch is taken, not that the tag caught it.
 */
#include "test.h"

#include "psafake.h"
#include "ultrawidelock_link.h"

#include <psa/crypto.h>
#include <string.h>

/* psafake derives its filler tag width from the algorithm, so a frame built
 * here is exactly as long as one the firmware builds. That matters more than it
 * looks: this link demultiplexes messages BY length, so a fake that got the
 * width wrong would make every frame unrecognisable to its own receiver and the
 * loopback below would prove nothing. */
#define SEALED_FAKE(plain) (ULTRAWIDELOCK_SEAL_OVERHEAD + (plain))

static const uint8_t KEY[ULTRAWIDELOCK_SEAL_KEY_LEN] = {
	0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
	0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
};
static const uint8_t URSK[ULTRAWIDELOCK_JOIN_URSK_LEN] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
	0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
	0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};
static const uint8_t RCFG[ULTRAWIDELOCK_JOIN_RCFG_LEN] = {
	0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31,
};

void test_ultrawidelock_link(void)
{
	struct ultrawidelock_link sat;
	struct ultrawidelock_link sat2;
	struct ultrawidelock_link lock;
	/* A receiver that never arms an epoch, for the rules that must keep
	 * holding without one. */
	struct ultrawidelock_link unarmed;
	struct ultrawidelock_anchor_msg am;
	struct ultrawidelock_join_msg jm;
	uint8_t frame[ULTRAWIDELOCK_LINK_MAX_FRAME];
	uint8_t frame2[sizeof(frame)];
	uint8_t chal[ULTRAWIDELOCK_LINK_CHALLENGE_LEN];
	uint8_t chal_hint[ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN];
	size_t n;
	size_t n2;

	t_group("frame budget: the carrier must be able to carry this");
	/*
	 * ESP-NOW's payload ceiling is 250 B. The largest thing this link ever
	 * sends is the WV4 handoff, and if that ever exceeds the carrier the
	 * failure is a session handoff that silently never arrives — a satellite
	 * that ranges perfectly and joins nothing. Pin it here as well as at the
	 * carrier, because this is where the number is decided.
	 */
	T_EQ("max frame is the handoff", ULTRAWIDELOCK_LINK_MAX_FRAME,
	     ULTRAWIDELOCK_SEAL_OVERHEAD + ULTRAWIDELOCK_JOIN_MSG_LEN);
	T_EQ("max frame is 81 B", ULTRAWIDELOCK_LINK_MAX_FRAME, 81);
	T_OK("max frame fits ESP-NOW's 250 B", ULTRAWIDELOCK_LINK_MAX_FRAME <= 250);
	T_EQ("a report is 45 B",
	     ULTRAWIDELOCK_SEAL_OVERHEAD + ULTRAWIDELOCK_ANCHOR_MSG_LEN, 45);
	/* The demultiplexer is length-based, so the two must never be equal. */
	T_OK("report and handoff have different sealed lengths",
	     ULTRAWIDELOCK_ANCHOR_MSG_LEN != ULTRAWIDELOCK_JOIN_MSG_LEN);
	T_OK("handoff nonce role is outside every satellite role",
	     ULTRAWIDELOCK_LINK_HANDOFF_ROLE > ULTRAWIDELOCK_LINK_ROLE_MAX);

	t_group("init: nothing is sendable before a key");
	psafake_reset();
	ultrawidelock_link_init(&sat, 2u, 0xAABBCCDDu);
	T_EQ("role recorded", sat.role, 2);
	T_EQ("boot id recorded", sat.boot_id, (long)0xAABBCCDDu);
	T_EQ("counter starts at zero", sat.ctr, 0);
	T_EQ("not ready", ultrawidelock_link_ready(&sat), 0);
	T_EQ("report refused without a key",
	     ultrawidelock_link_build_report(&sat, 1500, 7u, frame, sizeof(frame)), 0);
	T_EQ("handoff refused without a key",
	     ultrawidelock_link_build_join(&sat, URSK, RCFG, 9u, 3u, frame, sizeof(frame)), 0);
	T_EQ("and nothing touched the backend", psafake.import_calls, 0);

	t_group("set_key: a wrong-sized key is refused, not padded");
	T_EQ("15 bytes refused", ultrawidelock_link_set_key(&sat, KEY, 15u), -1);
	T_EQ("17 bytes refused", ultrawidelock_link_set_key(&sat, KEY, 17u), -1);
	T_EQ("0 bytes refused", ultrawidelock_link_set_key(&sat, KEY, 0u), -1);
	T_EQ("NULL refused", ultrawidelock_link_set_key(&sat, NULL, 16u), -1);
	T_EQ("still not ready", ultrawidelock_link_ready(&sat), 0);
	T_EQ("16 bytes accepted", ultrawidelock_link_set_key(&sat, KEY, 16u), 0);
	T_EQ("ready", ultrawidelock_link_ready(&sat), 1);
	{
		struct ultrawidelock_link bad_role;

		ultrawidelock_link_init(&bad_role, ULTRAWIDELOCK_LINK_HANDOFF_ROLE, 1u);
		(void)ultrawidelock_link_set_key(&bad_role, KEY, sizeof(KEY));
		T_EQ("handoff nonce role cannot compose a satellite report",
		     ultrawidelock_link_build_report(&bad_role, 100, 1u, frame, sizeof(frame)), 0);
		T_EQ("refused role burns no nonce", bad_role.ctr, 0);
	}

	ultrawidelock_link_init(&lock, 1u, 0x11223344u);
	T_EQ("lock keyed", ultrawidelock_link_set_key(&lock, KEY, 16u), 0);

	t_group("challenge: issued locally, echoed by the reporter");
	/*
	 * Two nonces, never one. What we ECHO is learned from an unauthenticated
	 * frame; what we REQUIRE is set only by the local caller that broadcast
	 * it. One field for both would let anyone able to send a challenge --
	 * on ESP-NOW, anyone at all -- choose the receiver's freshness epoch.
	 */
	psafake_reset();
	T_EQ("built", ultrawidelock_link_build_challenge(0x0102030405060708ULL, chal,
							sizeof(chal)),
	     ULTRAWIDELOCK_LINK_CHALLENGE_LEN);
	t_vec("challenge bytes", chal, sizeof(chal), "020102030405060708");
	T_EQ("too small a buffer -> 0", ultrawidelock_link_build_challenge(1u, chal, 8u), 0);
	ultrawidelock_link_expect_echo(&lock, 0x0102030405060708ULL);
	T_EQ("satellite takes it",
	     ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL),
	     ULTRAWIDELOCK_LINK_RX_CHALLENGE);
	T_EQ("the reporter's outgoing echo is set", (long)(sat.tx_echo_nonce & 0xFFFFFFFFu),
	     (long)0x05060708u);
	T_EQ("the receiver's expectation is not", (long)sat.expected_echo_nonce, (long)0);
	T_EQ("the receiver expects what it issued",
	     (long)(lock.expected_echo_nonce & 0xFFFFFFFFu), (long)0x05060708u);
	T_EQ("hearing a challenge did not arm the receiver",
	     (long)(lock.tx_echo_nonce & 0xFFFFFFFFu), (long)0);
	T_EQ("no key was consulted for it", psafake.import_calls, 0);

	t_group("report: composed, carried, believed");
	psafake_reset();
	n = ultrawidelock_link_build_report(&sat, 1234, 42u, frame, sizeof(frame));
	T_EQ("sealed report length", n, SEALED_FAKE(ULTRAWIDELOCK_ANCHOR_MSG_LEN));
	T_EQ("counter advanced to 1", sat.ctr, 1);
	memset(&am, 0, sizeof(am));
	T_EQ("lock believes it", ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("distance survived", am.peer_mm, 1234);
	/* The block is what makes the distance pairable at all. */
	T_EQ("ranging block survived", am.ranging_block, 42);
	T_EQ("role survived", am.role, 2);
	T_EQ("boot id survived", am.boot_id, (long)0xAABBCCDDu);
	T_EQ("counter survived", am.ctr, 1);

	t_group("report: the counter in the message matches the nonce's");
	/*
	 * These are built in two different places from two different
	 * expressions, and if they ever disagree the far end's replay window
	 * advances on a counter the nonce never used — which is a nonce reuse
	 * waiting for the next boot with the same id.
	 */
	psafake_reset();
	n = ultrawidelock_link_build_report(&sat, 900, 43u, frame, sizeof(frame));
	T_EQ("counter now 2", sat.ctr, 2);
	/* Byte 5..8 of the nonce, which the seal copies to the head of the frame. */
	T_EQ("nonce counter byte matches", frame[8], 2);
	T_EQ("nonce role byte is ours", frame[0], 2);
	memset(&am, 0, sizeof(am));
	(void)ultrawidelock_link_consume(&lock, frame, n, &am, NULL);
	T_EQ("message counter matches the nonce's", am.ctr, 2);

	t_group("replay: the same frame twice is believed once");
	memset(&am, 0, sizeof(am));
	T_EQ("second delivery refused",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);
	/* A refused report must leave nothing behind for a caller that forgot to
	 * check the return value. */
	T_EQ("and the out-parameter was cleared", am.peer_mm, 0);

	t_group("replay: a counter that goes backwards is refused");
	psafake_reset();
	sat.ctr = 0u; /* pretend an older frame was recorded and replayed */
	n2 = ultrawidelock_link_build_report(&sat, 700, 44u, frame2, sizeof(frame2));
	memset(&am, 0, sizeof(am));
	T_EQ("older counter refused",
	     ultrawidelock_link_consume(&lock, frame2, n2, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);

	t_group("replay: unarmed, a new boot id resets the window, as it must");
	/*
	 * A satellite that loses power restarts its counter at zero and is
	 * telling the truth. A receiver that has issued no challenge has nothing
	 * better to go on, so it must accept that: the alternative is accepting
	 * replays forever or locking out a peer that was unplugged.
	 */
	psafake_reset();
	ultrawidelock_link_init(&unarmed, ULTRAWIDELOCK_LINK_HANDOFF_ROLE, 0x44556677u);
	(void)ultrawidelock_link_set_key(&unarmed, KEY, 16u);
	ultrawidelock_link_init(&sat, 2u, 0x0BADB001u);
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	n = ultrawidelock_link_build_report(&sat, 1010, 49u, frame, sizeof(frame));
	T_EQ("the first boot lands",
	     ultrawidelock_link_consume(&unarmed, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	ultrawidelock_link_init(&sat, 2u, 0x99887766u); /* the power cut */
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	n = ultrawidelock_link_build_report(&sat, 1111, 50u, frame, sizeof(frame));
	memset(&am, 0, sizeof(am));
	T_EQ("counter 1 under a new boot id is fresh",
	     ultrawidelock_link_consume(&unarmed, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("distance survived", am.peer_mm, 1111);

	t_group("replay: armed, a boot switch waits for the next challenge");
	/*
	 * That same reset is how a recording attacks: accept boot A, then boot
	 * B, then a captured frame from A, and the window rolls backwards every
	 * time the boot id changes. An epoch admits the change ONCE per
	 * challenge, so a satellite that really rebooted is believed on the next
	 * beacon and a replayed old boot is never believed while this one
	 * stands.
	 */
	psafake_reset();
	ultrawidelock_link_init(&lock, ULTRAWIDELOCK_LINK_HANDOFF_ROLE, 0x11223344u);
	(void)ultrawidelock_link_set_key(&lock, KEY, 16u);
	T_EQ("epoch 1 built",
	     ultrawidelock_link_build_challenge(0x1112131415161718ULL, chal, sizeof(chal)),
	     ULTRAWIDELOCK_LINK_CHALLENGE_LEN);
	ultrawidelock_link_expect_echo(&lock, 0x1112131415161718ULL);

	ultrawidelock_link_init(&sat, 2u, 0x0B007A11u);
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	T_EQ("boot A takes epoch 1",
	     ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL),
	     ULTRAWIDELOCK_LINK_RX_CHALLENGE);
	n = ultrawidelock_link_build_report(&sat, 1010, 49u, frame, sizeof(frame));
	T_EQ("boot A lands in epoch 1",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);

	ultrawidelock_link_init(&sat, 2u, 0x99887766u); /* another boot, or a lie */
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	T_EQ("boot B takes the SAME epoch",
	     ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL),
	     ULTRAWIDELOCK_LINK_RX_CHALLENGE);
	n2 = ultrawidelock_link_build_report(&sat, 1111, 50u, frame2, sizeof(frame2));
	memset(&am, 0, sizeof(am));
	T_EQ("a boot switch inside one epoch is held",
	     ultrawidelock_link_consume(&lock, frame2, n2, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);
	T_EQ("and its distance was cleared", am.peer_mm, 0);
	T_EQ("while the diagnostic role survives", am.role, 2);

	T_EQ("epoch 2 built",
	     ultrawidelock_link_build_challenge(0x2122232425262728ULL, chal, sizeof(chal)),
	     ULTRAWIDELOCK_LINK_CHALLENGE_LEN);
	ultrawidelock_link_expect_echo(&lock, 0x2122232425262728ULL);
	T_EQ("boot B takes epoch 2",
	     ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL),
	     ULTRAWIDELOCK_LINK_RX_CHALLENGE);
	n2 = ultrawidelock_link_build_report(&sat, 1111, 50u, frame2, sizeof(frame2));
	memset(&am, 0, sizeof(am));
	T_EQ("and lands under it",
	     ultrawidelock_link_consume(&lock, frame2, n2, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("distance survived", am.peer_mm, 1111);
	T_EQ("the new echo travelled with it", (long)(am.echo_nonce & 0xFFFFFFFFu),
	     (long)0x25262728u);
	memset(&am, 0, sizeof(am));
	T_EQ("epoch 1's frame cannot roll the window back",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);
	T_EQ("nor is its distance handed back", am.peer_mm, 0);

	t_group("replay: alternating anchor roles cannot reset each other's window");
	{
		struct ultrawidelock_link role2;
		struct ultrawidelock_link role3;
		struct ultrawidelock_link role_lock;
		uint8_t role2_frame[ULTRAWIDELOCK_LINK_MAX_FRAME];
		uint8_t role3_frame[ULTRAWIDELOCK_LINK_MAX_FRAME];
		size_t role2_len;
		size_t role3_len;

		ultrawidelock_link_init(&role2, 2u, 0x11111111u);
		ultrawidelock_link_init(&role3, 3u, 0x33333333u);
		ultrawidelock_link_init(&role_lock, ULTRAWIDELOCK_LINK_HANDOFF_ROLE,
					0x99999999u);
		(void)ultrawidelock_link_set_key(&role2, KEY, sizeof(KEY));
		(void)ultrawidelock_link_set_key(&role3, KEY, sizeof(KEY));
		(void)ultrawidelock_link_set_key(&role_lock, KEY, sizeof(KEY));
		role2_len = ultrawidelock_link_build_report(&role2, 1200, 70u, role2_frame,
							  sizeof(role2_frame));
		role3_len = ultrawidelock_link_build_report(&role3, 1300, 70u, role3_frame,
							  sizeof(role3_frame));
		T_EQ("role 2 lands", ultrawidelock_link_consume(&role_lock, role2_frame,
								     role2_len, &am, NULL),
		     ULTRAWIDELOCK_LINK_RX_REPORT);
		T_EQ("role 3 lands without replacing role 2's state",
		     ultrawidelock_link_consume(&role_lock, role3_frame, role3_len, &am, NULL),
		     ULTRAWIDELOCK_LINK_RX_REPORT);
		T_EQ("role 2's first frame is still a replay",
		     ultrawidelock_link_consume(&role_lock, role2_frame, role2_len, &am, NULL),
		     ULTRAWIDELOCK_LINK_RX_REPLAYED);
		T_EQ("replay diagnostic keeps its role", am.role, 2);
		T_EQ("replay diagnostic keeps its counter", am.ctr, 1);
		T_EQ("rejected distance remains cleared", am.peer_mm, 0);
	}

	t_group("challenge: the 12-byte picked-label hint is accepted and ignored");
	psafake_reset();
	T_EQ("hinted challenge built",
	     ultrawidelock_link_build_challenge_hint(0x0102030405060708ULL, 0x00A1B2C3u,
						     chal_hint, sizeof(chal_hint)),
	     ULTRAWIDELOCK_LINK_CHALLENGE_HINT_LEN);
	t_vec("hinted challenge bytes", chal_hint, sizeof(chal_hint),
	      "020102030405060708a1b2c3");
	T_EQ("11-byte buffer refused",
	     ultrawidelock_link_build_challenge_hint(1u, 2u, chal_hint,
						     sizeof(chal_hint) - 1u),
	     0);
	sat.tx_echo_nonce = 0u;
	T_EQ("satellite takes the hinted form",
	     ultrawidelock_link_consume(&sat, chal_hint, sizeof(chal_hint), NULL, NULL),
	     ULTRAWIDELOCK_LINK_RX_CHALLENGE);
	T_EQ("trailer did not alter the nonce", (long)(sat.tx_echo_nonce & 0xFFFFFFFFu),
	     (long)0x05060708u);
	T_EQ("no key was consulted for it", psafake.import_calls, 0);

	t_group("challenge: the echo travels in the next report");
	psafake_reset();
	/* The receiver arms the same challenge the hinted form carried, which is
	 * what the lock does: one nonce, broadcast in whichever form, armed from
	 * the single place that rolls it. */
	ultrawidelock_link_expect_echo(&lock, 0x0102030405060708ULL);
	n = ultrawidelock_link_build_report(&sat, 1200, 51u, frame, sizeof(frame));
	memset(&am, 0, sizeof(am));
	T_EQ("believed under the armed challenge",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("echo nonce carried", (long)(am.echo_nonce & 0xFFFFFFFFu), (long)0x05060708u);

	t_group("challenge: an armed receiver refuses a report echoing another one");
	/*
	 * The rule the whole epoch rests on. A perfectly sealed report with a
	 * perfectly advancing counter is still a recording if it answers a
	 * challenge this end has retired.
	 */
	psafake_reset();
	ultrawidelock_link_expect_echo(&lock, 0x3132333435363738ULL);
	n = ultrawidelock_link_build_report(&sat, 1250, 52u, frame, sizeof(frame));
	memset(&am, 0, sizeof(am));
	T_EQ("a stale echo is refused",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);
	T_EQ("with no distance handed back", am.peer_mm, 0);
	(void)ultrawidelock_link_build_challenge(0x3132333435363738ULL, chal, sizeof(chal));
	(void)ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL);
	n = ultrawidelock_link_build_report(&sat, 1250, 52u, frame, sizeof(frame));
	memset(&am, 0, sizeof(am));
	T_EQ("the same reporter is believed once it answers the current one",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("distance survived", am.peer_mm, 1250);

	t_group("handoff: the lock's direction, under the same key");
	psafake_reset();
	n = ultrawidelock_link_build_join(&lock, URSK, RCFG, 9u, 3u, frame, sizeof(frame));
	T_EQ("sealed handoff length", n, SEALED_FAKE(ULTRAWIDELOCK_JOIN_MSG_LEN));
	/*
	 * THE BYTE THAT KEEPS ONE KEY SAFE IN TWO DIRECTIONS. The lock's nonce
	 * starts 0xFF, which no conforming satellite role (1..3) can emit, so
	 * the two senders' nonce spaces cannot meet.
	 */
	T_EQ("lock's nonce role byte is 0xFF", frame[0], 0xFF);
	memset(&jm, 0, sizeof(jm));
	T_EQ("satellite believes it",
	     ultrawidelock_link_consume(&sat, frame, n, NULL, &jm),
	     ULTRAWIDELOCK_LINK_RX_JOIN);
	T_OK("ursk survived", memcmp(jm.ursk, URSK, sizeof(URSK)) == 0);
	T_OK("rcfg survived", memcmp(jm.rcfg, RCFG, sizeof(RCFG)) == 0);
	T_EQ("channel survived", jm.channel, 9);
	T_EQ("sync code index survived", jm.sync_code_index, 3);

	t_group("handoff: replayed once is refused, and leaves no key behind");
	memset(&jm, 0, sizeof(jm));
	T_EQ("second delivery refused",
	     ultrawidelock_link_consume(&sat, frame, n, NULL, &jm),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);
	{
		uint8_t zero[ULTRAWIDELOCK_JOIN_URSK_LEN] = {0};

		T_OK("no URSK left in the out-parameter",
		     memcmp(jm.ursk, zero, sizeof(zero)) == 0);
	}

	t_group("demux: a message with nowhere to go is not believed quietly");
	/*
	 * A satellite has no use for a report and a lock has no use for a
	 * handoff. Passing NULL for that direction must NOT let the datagram
	 * advance the replay window — if it did, the real message that followed
	 * would be rejected as stale.
	 */
	psafake_reset();
	ultrawidelock_link_init(&lock, 1u, 0x11223344u);
	(void)ultrawidelock_link_set_key(&lock, KEY, 16u);
	ultrawidelock_link_init(&sat, 2u, 0x55667788u);
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	n = ultrawidelock_link_build_report(&sat, 1300, 60u, frame, sizeof(frame));
	T_EQ("report with no sink is ignored",
	     ultrawidelock_link_consume(&lock, frame, n, NULL, NULL),
	     ULTRAWIDELOCK_LINK_RX_IGNORED);
	memset(&am, 0, sizeof(am));
	T_EQ("and the window did not move, so the real one still lands",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);

	t_group("replay: armed, the per-role windows stay independent");
	/*
	 * The per-role windows and the epoch are two rules over the same state,
	 * and the epoch stamps ITS bookkeeping per role too. Two satellites
	 * answering one challenge must therefore still not disturb each other:
	 * role 2 landing may not re-open role 1's counter, nor may it re-open
	 * role 1's boot-switch allowance.
	 */
	psafake_reset();
	ultrawidelock_link_init(&lock, ULTRAWIDELOCK_LINK_HANDOFF_ROLE, 0x11223344u);
	(void)ultrawidelock_link_set_key(&lock, KEY, 16u);
	ultrawidelock_link_init(&sat, 1u, 0x10101010u);
	ultrawidelock_link_init(&sat2, 2u, 0x20202020u);
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	(void)ultrawidelock_link_set_key(&sat2, KEY, 16u);
	(void)ultrawidelock_link_build_challenge(0x4142434445464748ULL, chal, sizeof(chal));
	ultrawidelock_link_expect_echo(&lock, 0x4142434445464748ULL);
	(void)ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL);
	(void)ultrawidelock_link_consume(&sat2, chal, sizeof(chal), NULL, NULL);
	n = ultrawidelock_link_build_report(&sat, 900, 70u, frame, sizeof(frame));
	n2 = ultrawidelock_link_build_report(&sat2, 800, 70u, frame2, sizeof(frame2));
	T_EQ("role 1 accepted", ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("role 2 accepted without resetting role 1",
	     ultrawidelock_link_consume(&lock, frame2, n2, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("role 1 replay still rejected after role 2",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);
	/* Role 1 reboots inside this epoch; role 2's acceptance must not have
	 * spent the allowance on role 1's behalf. */
	ultrawidelock_link_init(&sat, 1u, 0x1F1F1F1Fu);
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	(void)ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL);
	n = ultrawidelock_link_build_report(&sat, 950, 71u, frame, sizeof(frame));
	memset(&am, 0, sizeof(am));
	T_EQ("role 1's boot switch is held in this epoch",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPLAYED);
	n2 = ultrawidelock_link_build_report(&sat2, 810, 71u, frame2, sizeof(frame2));
	T_EQ("and role 2 carries on regardless",
	     ultrawidelock_link_consume(&lock, frame2, n2, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);

	t_group("demux: lengths nothing sends are discarded without a key");
	psafake_reset();
	T_EQ("empty", ultrawidelock_link_consume(&lock, frame, 0, &am, &jm),
	     ULTRAWIDELOCK_LINK_RX_IGNORED);
	T_EQ("one byte", ultrawidelock_link_consume(&lock, frame, 1, &am, &jm),
	     ULTRAWIDELOCK_LINK_RX_IGNORED);
	T_EQ("one short of a report",
	     ultrawidelock_link_consume(&lock, frame,
					SEALED_FAKE(ULTRAWIDELOCK_ANCHOR_MSG_LEN) - 1u, &am, &jm),
	     ULTRAWIDELOCK_LINK_RX_IGNORED);
	T_EQ("one over a report",
	     ultrawidelock_link_consume(&lock, frame,
					SEALED_FAKE(ULTRAWIDELOCK_ANCHOR_MSG_LEN) + 1u, &am, &jm),
	     ULTRAWIDELOCK_LINK_RX_IGNORED);
	{
		/* Right length, wrong version byte. Written as bytes rather than
		 * a string literal: "\xff1..." would swallow the digits into the
		 * escape and mean something else entirely. */
		const uint8_t wrong_ver[ULTRAWIDELOCK_LINK_CHALLENGE_LEN] = {
			0xFFu, 1, 2, 3, 4, 5, 6, 7, 8,
		};

		T_EQ("a challenge-length frame with the wrong version",
		     ultrawidelock_link_consume(&lock, wrong_ver, sizeof(wrong_ver), &am, &jm),
		     ULTRAWIDELOCK_LINK_RX_IGNORED);
	}
	T_EQ("no key was consulted for any of them", psafake.import_calls, 0);

	t_group("seal failure is a rejection, never a pass");
	/* On a receiver with no epoch armed, so what is under test is the seal
	 * and the counter window alone. */
	psafake_reset();
	ultrawidelock_link_init(&lock, ULTRAWIDELOCK_LINK_HANDOFF_ROLE, 0x11223344u);
	(void)ultrawidelock_link_set_key(&lock, KEY, 16u);
	n = ultrawidelock_link_build_report(&sat, 1400, 61u, frame, sizeof(frame));
	psafake_reset();
	psafake.aead_dec_ret = -1; /* stands in for a tag the fake cannot check */
	memset(&am, 0, sizeof(am));
	T_EQ("a frame that does not unseal is refused",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_UNSEALED);
	/* And it must not have moved the window on the way past. */
	psafake_reset();
	memset(&am, 0, sizeof(am));
	T_EQ("the window is untouched, so the genuine frame still lands",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);

	t_group("compose: refusals that must not burn a counter");
	psafake_reset();
	{
		uint32_t before = sat.ctr;

		T_EQ("negative distance refused",
		     ultrawidelock_link_build_report(&sat, -1, 62u, frame, sizeof(frame)), 0);
		T_EQ("counter untouched", sat.ctr, (long)before);
		T_EQ("too small a buffer refused",
		     ultrawidelock_link_build_report(&sat, 100, 62u, frame,
						     ULTRAWIDELOCK_SEAL_OVERHEAD +
							     ULTRAWIDELOCK_ANCHOR_MSG_LEN - 1u),
		     0);
		T_EQ("counter still untouched", sat.ctr, (long)before);
		psafake.aead_enc_ret = -1;
		T_EQ("a failing backend refused",
		     ultrawidelock_link_build_report(&sat, 100, 62u, frame, sizeof(frame)), 0);
		T_EQ("counter still untouched after a backend failure", sat.ctr, (long)before);
	}

	t_group("compose: NULL arguments");
	psafake_reset();
	T_EQ("NULL link -> 0",
	     ultrawidelock_link_build_report(NULL, 100, 1u, frame, sizeof(frame)), 0);
	T_EQ("NULL out -> 0",
	     ultrawidelock_link_build_report(&sat, 100, 1u, NULL, sizeof(frame)), 0);
	T_EQ("NULL ursk -> 0",
	     ultrawidelock_link_build_join(&lock, NULL, RCFG, 1u, 1u, frame, sizeof(frame)), 0);
	T_EQ("NULL rcfg -> 0",
	     ultrawidelock_link_build_join(&lock, URSK, NULL, 1u, 1u, frame, sizeof(frame)), 0);
	T_EQ("NULL link on consume",
	     ultrawidelock_link_consume(NULL, frame, 10, &am, &jm),
	     ULTRAWIDELOCK_LINK_RX_IGNORED);
	T_EQ("NULL bytes on consume",
	     ultrawidelock_link_consume(&lock, NULL, 10, &am, &jm),
	     ULTRAWIDELOCK_LINK_RX_IGNORED);

	psafake_reset();
}
