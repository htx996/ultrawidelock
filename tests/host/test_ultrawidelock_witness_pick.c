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

	/* Two advertisers rise together at clearly different levels: two people
	 * walking up, one nearer than the other. Ambiguity must not resolve. */
	m = win(-84, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	m = win(-76, -62, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-68, -54, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-60, -46, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 1500);

	T_OK("pick.ambiguous", !ultrawidelock_witness_pick_best(&p, &got));
}

static void test_one_handset_many_advertising_sets(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: one handset, several advertising sets");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* iOS advertises from several sets at once, and with extended
	 * advertising each may carry its own address -- so one approaching
	 * phone shows up as several labels that all correlate. They share an
	 * antenna, so they sit within a few dB. If that read as an ambiguity
	 * the lock would refuse every clear forever, which is a worse failure
	 * than the one min_margin exists to prevent. */
	m = win(-84, -82, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	m = win(-76, -74, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-68, -66, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-60, -58, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 1500);

	T_OK("sets.picked", ultrawidelock_witness_pick_best(&p, &got));
	/* Either label is the same handset, so either is a correct answer --
	 * what matters is that the room's furniture did not win. */
	T_OK("sets.is_a_mover", got == PHONE || got == TV);
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

static void test_crowded_room_still_picks_the_mover(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_pick_stats st;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: a crowded room does not starve the pick");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* Every window carries a full report: the phone plus seven bystander
	 * labels drawn round-robin from a pool larger than the candidate
	 * table. Under score-only eviction everyone ties at zero, the whole
	 * table cycles window to window, and no label ever survives to its
	 * second sighting -- the phone included, so nothing can ever score. */
	for (int i = 0; i < 6; i++) {
		memset(&m, 0, sizeof(m));
		m.ver = ULTRAWIDELOCK_WITNESS_MSG_VER;
		m.role = ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE;
		m.window_ms = 2000u;
		m.n_tuples = ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES;
		m.tuples[0].hash24 = PHONE;
		m.tuples[0].mean_dbm = (int8_t)(-84 + i * 8);
		m.tuples[0].n_pkts = 6u;
		for (uint8_t j = 1; j < ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES; j++) {
			uint32_t k = ((uint32_t)i * 7u + j) % 21u;

			m.tuples[j].hash24 = 0x00C00000u + k;
			m.tuples[j].mean_dbm = (int8_t)(-70 - (int32_t)(k % 5u));
			m.tuples[j].n_pkts = 4u;
		}
		ultrawidelock_witness_pick_feed(&p, &m, 6000 - i * 900);
	}

	T_OK("crowd.picked", ultrawidelock_witness_pick_best(&p, &got));
	T_EQ("crowd.is_phone", got, PHONE);
	/* The pool is larger than the table, so evictions did happen; the pick
	 * survived them because proven-stationary labels went first. */
	ultrawidelock_witness_pick_stats(&p, &st);
	T_OK("crowd.evicted_someone", st.evictions > 0u);
	T_EQ("crowd.best_is_phone", st.best_hash24, PHONE);
}

static void test_rotated_address_ghost_is_retired(void)
{
	struct ultrawidelock_witness_pick p;
	struct ultrawidelock_witness_msg m;
	uint32_t got = 0u;

	t_group("witness_pick: a rotated address does not haunt the pick");
	ultrawidelock_witness_pick_init(&p, NULL);

	/* The handset earns the pick under one advertising address... */
	m = win(-84, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	m = win(-76, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-68, -70, -75);
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-60, -71, -74);
	ultrawidelock_witness_pick_feed(&p, &m, 1500);
	T_OK("ghost.picked", ultrawidelock_witness_pick_best(&p, &got));
	T_EQ("ghost.is_phone", got, PHONE);

	/* ...then rotates it. The caller sees the label gone from every
	 * report and retires it; its banked score must not outrank the
	 * successor address for the rest of the approach. */
	ultrawidelock_witness_pick_retire(&p, PHONE);
	T_OK("ghost.gone", !ultrawidelock_witness_pick_best(&p, &got));

	/* The successor label (TAG's slot reused as the new address here)
	 * correlates just as the old one did, and may now win. */
	m = win(-84, -70, -60);
	m.tuples[0].hash24 = 0x00DDDDDDu;
	ultrawidelock_witness_pick_feed(&p, &m, 6000);
	m = win(-76, -71, -68);
	m.tuples[0].hash24 = 0x00DDDDDDu;
	ultrawidelock_witness_pick_feed(&p, &m, 4500);
	m = win(-68, -70, -76);
	m.tuples[0].hash24 = 0x00DDDDDDu;
	ultrawidelock_witness_pick_feed(&p, &m, 3000);
	m = win(-60, -71, -84);
	m.tuples[0].hash24 = 0x00DDDDDDu;
	ultrawidelock_witness_pick_feed(&p, &m, 1500);
	T_OK("ghost.successor_picked", ultrawidelock_witness_pick_best(&p, &got));
	T_EQ("ghost.successor_is_new_label", got, 0x00DDDDDDu);
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
	test_one_handset_many_advertising_sets();
	test_crowded_room_still_picks_the_mover();
	test_rotated_address_ghost_is_retired();
	test_receding_is_not_approaching();
	test_guards();
}
