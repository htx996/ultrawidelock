/**
 * @file test_ultrawidelock_witness_msg.c — WV2 codec and per-witness replay state.
 */

#include "test.h"

#include "ultrawidelock_witness_msg.h"

#include <string.h>

static struct ultrawidelock_witness_msg base_msg(void)
{
	struct ultrawidelock_witness_msg m;

	memset(&m, 0, sizeof(m));
	m.ver = ULTRAWIDELOCK_WITNESS_MSG_VER;
	m.role = ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE;
	m.boot_id = 0x11223344u;
	m.ctr = 7u;
	m.echo_nonce = 0x0123456789ABCDEFull;
	m.window_ms = 2000u;
	m.n_tuples = 2u;
	m.tuples[0].hash24 = 0x00ABCDEFu;
	m.tuples[0].mean_dbm = -61;
	m.tuples[0].n_pkts = 6u;
	m.tuples[1].hash24 = 0x00123456u;
	m.tuples[1].mean_dbm = -80;
	m.tuples[1].n_pkts = 3u;
	return m;
}

static void test_roundtrip(void)
{
	struct ultrawidelock_witness_msg m = base_msg();
	struct ultrawidelock_witness_msg out;
	uint8_t buf[ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN];
	size_t n;

	t_group("witness_msg: round trip");

	n = ultrawidelock_witness_msg_encode(&m, buf, sizeof(buf));
	T_EQ("enc.len", n,
	     ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN + 2u * ULTRAWIDELOCK_WITNESS_MSG_TUPLE_LEN);
	T_OK("dec.ok", ultrawidelock_witness_msg_decode(buf, n, &out));
	T_EQ("dec.ver", out.ver, ULTRAWIDELOCK_WITNESS_MSG_VER);
	T_EQ("dec.role", out.role, ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE);
	T_EQ("dec.boot", out.boot_id, 0x11223344u);
	T_EQ("dec.ctr", out.ctr, 7);
	T_OK("dec.nonce", out.echo_nonce == 0x0123456789ABCDEFull);
	T_EQ("dec.window", out.window_ms, 2000);
	T_EQ("dec.n", out.n_tuples, 2);
	T_EQ("dec.t0.hash", out.tuples[0].hash24, 0x00ABCDEFu);
	T_EQ("dec.t0.dbm", out.tuples[0].mean_dbm, -61);
	T_EQ("dec.t0.n", out.tuples[0].n_pkts, 6);
	T_EQ("dec.t1.dbm", out.tuples[1].mean_dbm, -80);

	/* Zero tuples is a legal report: the witness heard nothing worth
	 * summarising, which is evidence of silence and never of a side. */
	m.n_tuples = 0u;
	n = ultrawidelock_witness_msg_encode(&m, buf, sizeof(buf));
	T_EQ("enc.empty.len", n, ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN);
	T_OK("dec.empty", ultrawidelock_witness_msg_decode(buf, n, &out));
	T_EQ("dec.empty.n", out.n_tuples, 0);
}

static void test_reject(void)
{
	struct ultrawidelock_witness_msg m = base_msg();
	struct ultrawidelock_witness_msg out;
	uint8_t buf[ULTRAWIDELOCK_WITNESS_MSG_MAX_LEN + 4];
	size_t n;

	t_group("witness_msg: malformed input");

	n = ultrawidelock_witness_msg_encode(&m, buf, sizeof(buf));
	T_OK("enc.ok", n > 0);

	T_OK("dec.null_buf", !ultrawidelock_witness_msg_decode(NULL, n, &out));
	T_OK("dec.null_out", !ultrawidelock_witness_msg_decode(buf, n, NULL));
	T_OK("dec.short", !ultrawidelock_witness_msg_decode(buf, n - 1u, &out));
	/* Trailing slack is a disagreement about the format, not spare room. */
	T_OK("dec.trailing", !ultrawidelock_witness_msg_decode(buf, n + 1u, &out));

	buf[0] = 99u;
	T_OK("dec.bad_ver", !ultrawidelock_witness_msg_decode(buf, n, &out));
	buf[0] = ULTRAWIDELOCK_WITNESS_MSG_VER;

	buf[1] = 0u; /* ROLE_UNKNOWN never travels */
	T_OK("dec.bad_role", !ultrawidelock_witness_msg_decode(buf, n, &out));
	buf[1] = 7u;
	T_OK("dec.unknown_role", !ultrawidelock_witness_msg_decode(buf, n, &out));
	buf[1] = ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE;

	buf[ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN - 1u] = ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES + 1u;
	T_OK("dec.too_many", !ultrawidelock_witness_msg_decode(buf, n, &out));

	t_group("witness_msg: encode guards");
	m.n_tuples = ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES + 1u;
	T_EQ("enc.too_many", ultrawidelock_witness_msg_encode(&m, buf, sizeof(buf)), 0);
	m = base_msg();
	m.role = ULTRAWIDELOCK_WITNESS_ROLE_UNKNOWN;
	T_EQ("enc.bad_role", ultrawidelock_witness_msg_encode(&m, buf, sizeof(buf)), 0);
	m = base_msg();
	T_EQ("enc.no_cap", ultrawidelock_witness_msg_encode(&m, buf, 4u), 0);
	T_EQ("enc.null", ultrawidelock_witness_msg_encode(NULL, buf, sizeof(buf)), 0);
}

static void test_find(void)
{
	struct ultrawidelock_witness_msg m = base_msg();

	t_group("witness_msg: label lookup");

	T_OK("find.hit", ultrawidelock_witness_msg_find(&m, 0x00ABCDEFu) != NULL);
	T_OK("find.miss", ultrawidelock_witness_msg_find(&m, 0x00FFFFFFu) == NULL);
	T_OK("find.null", ultrawidelock_witness_msg_find(NULL, 0u) == NULL);

	/* An empty slot is not an advertiser heard at 0 dBm. */
	m.tuples[0].n_pkts = 0u;
	T_OK("find.zero_pkts", ultrawidelock_witness_msg_find(&m, 0x00ABCDEFu) == NULL);
	T_OK("at.zero_pkts", ultrawidelock_witness_msg_at(&m, 0u) == NULL);
	T_OK("at.ok", ultrawidelock_witness_msg_at(&m, 1u) != NULL);
	T_OK("at.oob", ultrawidelock_witness_msg_at(&m, 9u) == NULL);
}

static void test_replay(void)
{
	struct ultrawidelock_witness_seen seen;
	struct ultrawidelock_witness_msg m = base_msg();

	t_group("witness_msg: replay state");
	memset(&seen, 0, sizeof(seen));

	m.ctr = 5u;
	T_OK("seen.first", ultrawidelock_witness_seen_accept(&seen, &m));
	m.ctr = 6u;
	T_OK("seen.forward", ultrawidelock_witness_seen_accept(&seen, &m));
	T_OK("seen.repeat", !ultrawidelock_witness_seen_accept(&seen, &m));
	m.ctr = 4u;
	T_OK("seen.backward", !ultrawidelock_witness_seen_accept(&seen, &m));
	/* A rejected report must not have moved the state. */
	T_EQ("seen.ctr_held", seen.ctr, 6);

	/* A witness that lost power gets a new boot_id, so its counter may
	 * legitimately restart. Without this a power cut would lock it out. */
	m.boot_id = 0x99999999u;
	m.ctr = 0u;
	T_OK("seen.reboot", ultrawidelock_witness_seen_accept(&seen, &m));
	T_EQ("seen.reboot_ctr", seen.ctr, 0);
	T_OK("seen.null", !ultrawidelock_witness_seen_accept(NULL, &m));
}

/*
 * WV4, the lock's session handoff. Tested harder than its size suggests because
 * it is the one message the SATELLITE acts on: a decode that silently accepts a
 * truncated or mis-versioned frame hands the radio a key it should not have.
 */
static struct ultrawidelock_join_msg base_join(void)
{
	struct ultrawidelock_join_msg j;
	size_t i;

	memset(&j, 0, sizeof(j));
	j.ver = ULTRAWIDELOCK_JOIN_MSG_VER;
	j.boot_id = 0xDEADBEEFu;
	j.ctr = 42u;
	for (i = 0; i < ULTRAWIDELOCK_JOIN_URSK_LEN; i++) {
		j.ursk[i] = (uint8_t)(0xA0u + i);
	}
	for (i = 0; i < ULTRAWIDELOCK_JOIN_RCFG_LEN; i++) {
		j.rcfg[i] = (uint8_t)(0x10u + i);
	}
	j.channel = 9u;
	j.sync_code_index = 10u;
	return j;
}

static void test_join(void)
{
	struct ultrawidelock_join_msg j = base_join();
	struct ultrawidelock_join_msg out;
	uint8_t buf[ULTRAWIDELOCK_JOIN_MSG_LEN + 4u];
	size_t n;

	t_group("join_msg: round trip");

	n = ultrawidelock_join_msg_encode(&j, buf, sizeof(buf));
	T_EQ("join.encode_len", (int)n, ULTRAWIDELOCK_JOIN_MSG_LEN);
	T_OK("join.is_join", ultrawidelock_msg_is_join(buf, n));
	T_OK("join.not_anchor", !ultrawidelock_msg_is_anchor(buf, n));

	memset(&out, 0, sizeof(out));
	T_OK("join.decode", ultrawidelock_join_msg_decode(buf, n, &out));
	T_EQ("join.ver", out.ver, ULTRAWIDELOCK_JOIN_MSG_VER);
	T_EQ("join.boot_id", (int)out.boot_id, (int)j.boot_id);
	T_EQ("join.ctr", (int)out.ctr, (int)j.ctr);
	T_EQ("join.channel", out.channel, j.channel);
	T_EQ("join.sync_code", out.sync_code_index, j.sync_code_index);
	T_OK("join.ursk", memcmp(out.ursk, j.ursk, ULTRAWIDELOCK_JOIN_URSK_LEN) == 0);
	T_OK("join.rcfg", memcmp(out.rcfg, j.rcfg, ULTRAWIDELOCK_JOIN_RCFG_LEN) == 0);

	t_group("join_msg: rejects");

	/* Exact length, both directions: a handoff carrying a key must not be
	 * decoded from a frame whose size the two sides disagree about. */
	T_OK("join.short", !ultrawidelock_join_msg_decode(buf, ULTRAWIDELOCK_JOIN_MSG_LEN - 1u,
							 &out));
	T_OK("join.long", !ultrawidelock_join_msg_decode(buf, ULTRAWIDELOCK_JOIN_MSG_LEN + 1u,
							&out));
	buf[0] = ULTRAWIDELOCK_ANCHOR_MSG_VER;
	T_OK("join.wrong_ver", !ultrawidelock_join_msg_decode(buf, ULTRAWIDELOCK_JOIN_MSG_LEN,
							     &out));
	T_OK("join.is_join_false", !ultrawidelock_msg_is_join(buf, ULTRAWIDELOCK_JOIN_MSG_LEN));
	buf[0] = ULTRAWIDELOCK_JOIN_MSG_VER;

	T_OK("join.null_buf", !ultrawidelock_join_msg_decode(NULL, ULTRAWIDELOCK_JOIN_MSG_LEN,
							    &out));
	T_OK("join.null_out", !ultrawidelock_join_msg_decode(buf, ULTRAWIDELOCK_JOIN_MSG_LEN,
							    NULL));
	T_EQ("join.encode_cap", (int)ultrawidelock_join_msg_encode(&j, buf,
								   ULTRAWIDELOCK_JOIN_MSG_LEN - 1u),
	     0);
	T_EQ("join.encode_null", (int)ultrawidelock_join_msg_encode(NULL, buf, sizeof(buf)), 0);

	t_group("join_msg: version namespace");

	/* One version byte, one decoder. A WV3 report must never decode as a
	 * handoff and vice versa, or the wrong consumer acts on it. */
	{
		struct ultrawidelock_anchor_msg a;
		uint8_t abuf[ULTRAWIDELOCK_ANCHOR_MSG_LEN];
		size_t an;

		memset(&a, 0, sizeof(a));
		a.ver = ULTRAWIDELOCK_ANCHOR_MSG_VER;
		a.role = ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE;
		a.ranging_block = 12u;
		a.peer_mm = 1500;
		an = ultrawidelock_anchor_msg_encode(&a, abuf, sizeof(abuf));
		T_EQ("join.anchor_len", (int)an, ULTRAWIDELOCK_ANCHOR_MSG_LEN);
		T_OK("join.anchor_not_join", !ultrawidelock_msg_is_join(abuf, an));
		T_OK("join.anchor_no_decode", !ultrawidelock_join_msg_decode(abuf, an, &out));
	}
}

void test_ultrawidelock_witness_msg(void)
{
	test_roundtrip();
	test_reject();
	test_find();
	test_replay();
	test_join();
}
