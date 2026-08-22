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
	struct ultrawidelock_link lock;
	struct ultrawidelock_anchor_msg am;
	struct ultrawidelock_join_msg jm;
	uint8_t frame[ULTRAWIDELOCK_LINK_MAX_FRAME];
	uint8_t frame2[sizeof(frame)];
	uint8_t chal[ULTRAWIDELOCK_LINK_CHALLENGE_LEN];
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

	ultrawidelock_link_init(&lock, 1u, 0x11223344u);
	T_EQ("lock keyed", ultrawidelock_link_set_key(&lock, KEY, 16u), 0);

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

	t_group("replay: a new boot id resets the window, as it must");
	/*
	 * A satellite that loses power restarts its counter at zero and is
	 * telling the truth. Without this the lock's only options would be
	 * accepting replays forever or locking out a peer that was unplugged.
	 */
	psafake_reset();
	ultrawidelock_link_init(&sat, 2u, 0x99887766u);
	(void)ultrawidelock_link_set_key(&sat, KEY, 16u);
	n = ultrawidelock_link_build_report(&sat, 1111, 50u, frame, sizeof(frame));
	memset(&am, 0, sizeof(am));
	T_EQ("counter 1 under a new boot id is fresh",
	     ultrawidelock_link_consume(&lock, frame, n, &am, NULL),
	     ULTRAWIDELOCK_LINK_RX_REPORT);
	T_EQ("distance survived", am.peer_mm, 1111);

	t_group("challenge: unsealed, and it can only cost a report its standing");
	psafake_reset();
	T_EQ("built", ultrawidelock_link_build_challenge(0x0102030405060708ULL, chal,
							sizeof(chal)),
	     ULTRAWIDELOCK_LINK_CHALLENGE_LEN);
	t_vec("challenge bytes", chal, sizeof(chal), "020102030405060708");
	T_EQ("too small a buffer -> 0", ultrawidelock_link_build_challenge(1u, chal, 8u), 0);
	T_EQ("satellite takes it",
	     ultrawidelock_link_consume(&sat, chal, sizeof(chal), NULL, NULL),
	     ULTRAWIDELOCK_LINK_RX_CHALLENGE);
	T_EQ("echo nonce updated", (long)(sat.echo_nonce & 0xFFFFFFFFu), (long)0x05060708u);
	T_EQ("no key was consulted for it", psafake.import_calls, 0);

	t_group("challenge: the echo travels in the next report");
	psafake_reset();
	n = ultrawidelock_link_build_report(&sat, 1200, 51u, frame, sizeof(frame));
	memset(&am, 0, sizeof(am));
	(void)ultrawidelock_link_consume(&lock, frame, n, &am, NULL);
	T_EQ("echo nonce carried", (long)(am.echo_nonce & 0xFFFFFFFFu), (long)0x05060708u);

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
	psafake_reset();
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
