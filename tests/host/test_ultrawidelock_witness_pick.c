/**
 * @file test_ultrawidelock_witness_pick.c — picking the phone by trajectory,
 *       never by address.
 */

#include "test.h"

#include "ultrawidelock_witness_pick.h"

#include <string.h>

#define PHONE 0x00AAAAAAu
#define TV    0x00BBBBBBu
#define TAG   0x00CCCCCCu

/* One outside-witness window: the phone at `phone_dbm`, plus two static
 * household advertisers that never move. */
static struct ultrawidelock_witness_msg win(int8_t phone_dbm, int8_t tv_dbm, int8_t tag_dbm)
{
	struct ultrawidelock_witness_msg m;

	memset(&m, 0, sizeof(m));
	m.ver = ULTRAWIDELOCK_WITNESS_MSG_VER;
	m.role = ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE;
	m.window_ms = 2000u;
	m.n_tuples = 3u;
	m.tuples[0].hash24 = PHONE;
	m.tuples[0].mean_dbm = phone_dbm;
	m.tuples[0].n_pkts = 6u;
	m.tuples[1].hash24 = TV;
	m.tuples[1].mean_dbm = tv_dbm;
	m.tuples[1].n_pkts = 8u;
	m.tuples[2].hash24 = TAG;
	m.tuples[2].mean_dbm = tag_dbm;
	m.tuples[2].n_pkts = 4u;
	return m;
}

static void test_walkup_picks_the_mover(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: a walk-up identifies itself");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* Closing from 6 m to 1 m; the phone gets louder, the furniture does
	 * not. Static advertisers wander by 1 dB, inside the noise floor. */
	m = win(-84, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	T_OK("pick.none_yet", !ultrawidelock_witness_pick_best(&p, &got));

	m = win(-76, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-68, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-60, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 1500);

	T_OK("pick.found", ultrawidelock_witness_pick_best(&p, &got));
	T_EQ("pick.is_phone", got, PHONE);
}

static void test_static_room_picks_nothing(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: a quiet room decides nothing");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* Range closes, but no advertiser tracks it: the credential's phone is
	 * simply not advertising anything these witnesses can hear. */
	for (int i = 0; i < 6; i++) {
		m = win(-70, -70, -75);
		ultrawidelock_witness_pick_feed(&p, &m, 6000 - i * 800);
	}
	T_OK("pick.silent", !ultrawidelock_witness_pick_best(&p, &got));
}

static void test_two_movers_are_an_ambiguity(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: two things approaching is not a pick");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* Two advertisers rise together -- two people walking up, or one phone
	 * emitting from two advertising sets. Ambiguity must not resolve. */
	m = win(-84, -84, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	m = win(-76, -76, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-68, -68, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-60, -60, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 1500);

	T_OK("pick.ambiguous", !ultrawidelock_witness_pick_best(&p, &got));
}

static void test_receding_is_not_approaching(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: anti-correlation scores against");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* Getting quieter as the range closes: whatever this is, it is not
	 * moving with the phone the lock is ranging. */
	m = win(-60, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	m = win(-68, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-76, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-84, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 1500);

	T_OK("pick.anticorrelated", !ultrawidelock_witness_pick_best(&p, &got));
}

static void test_guards(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: guards and reset");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* No range means no correlation is possible; the window is dropped
	 * rather than scored against a guessed distance. */
	m = win(-84, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, -1);
	m = win(-60, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, -1);
	T_OK("pick.no_range", !ultrawidelock_witness_pick_best(&p, &got));

	/* The inside witness sees an approach as a fall, so scoring it here
	 * would pick the wrong sign. It is refused outright. */
	ultrawidelock_witness_pick_init(&p, NULL);
	for (int i = 0; i < 4; i++) {
		m = win((int8_t)(-84 + i * 8), -70, -75);
		m.role = ULTRAWIDELOCK_WITNESS_ROLE_INSIDE;
		ultrawidelock_witness_pick_feed(&p, &m, 6000 - i * 1500);
	}
	T_OK("pick.inside_ignored", !ultrawidelock_witness_pick_best(&p, &got));

	ultrawidelock_witness_pick_feed(NULL, &m, 1000);
	ultrawidelock_witness_pick_feed(&p, NULL, 1000);
	T_OK("pick.null_state", !ultrawidelock_witness_pick_best(NULL, &got));

	/* A pick belongs to one approach and must not outlive it. */
	ultrawidelock_witness_pick_init(&p, NULL);
	m = win(-84, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	m = win(-76, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-68, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-60, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 1500);
	T_OK("pick.before_reset", ultrawidelock_witness_pick_best(&p, &got));
	ultrawidelock_witness_pick_reset(&p);
	T_OK("pick.after_reset", !ultrawidelock_witness_pick_best(&p, &got));
}

void test_ultrawidelock_witness_pick(void)
{
	test_walkup_picks_the_mover();
	test_static_room_picks_nothing();
	test_two_movers_are_an_ambiguity();
	test_receding_is_not_approaching();
	test_guards();
}
