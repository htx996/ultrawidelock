/**
 * @file test_ultrawidelock_satellite.c — ultrawidelock_satellite suite: the freshness gate, the
 * mounting flag that decides which anchor is which, and the block matching that
 * decides whether there is a pair at all.
 *
 * ultrawidelock_fusion is already covered. What this suite is for is the ways the
 * gate around it can be wrong in a way no geometry test would catch: deciding
 * with a stale distance, deciding with the two anchors swapped, and deciding
 * with two distances that were never measured in the same ranging round.
 */
#include "test.h"

#include "ultrawidelock_satellite.h"

/* Most cases here describe ONE ranging round, so they share a block. The
 * pairing cases below choose different ones deliberately. */
#define RND_BLK 7u

/* Anchors 1.2 m apart, matching test_ultrawidelock_fusion.c so the two suites agree on
 * what the geometry means. */
static const struct ultrawidelock_fusion_cfg k_cfg = {
	.baseline_mm = 1200,
	.tol_mm = 90,
	.deadband_mm = 60,
};

static void absent_satellite_changes_nothing(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: no satellite means today's behaviour, not a locked door");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	/* We are measuring; nobody is reporting. This is the state a board is in
	 * for its whole life until the transport exists, so it had better be the
	 * permissive one. */
	ultrawidelock_satellite_observe(&s, 800, RND_BLK, 10000);

	T_OK("absent.predicts", ultrawidelock_satellite_may_predict(&s, 10000));
	T_OK("absent.unknown",
	     ultrawidelock_satellite_verdict(&s, 10000).side == ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_OK("absent.not_ok", !ultrawidelock_satellite_verdict(&s, 10000).geometry_ok);

	/* And a NULL satellite, which is how a build without one behaves. */
	T_OK("absent.null_predicts", ultrawidelock_satellite_may_predict(NULL, 10000));
	ultrawidelock_satellite_observe(NULL, 800, RND_BLK, 10000); /* must not fault */
}

static void fresh_report_decides(void)
{
	struct ultrawidelock_satellite s;
	struct ultrawidelock_fusion_verdict v;

	t_group("sat: a fresh report is used");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	/* Phone nearer the outside anchor: self (inside) 1400, peer 800. */
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);

	v = ultrawidelock_satellite_verdict(&s, 10000);
	T_OK("fresh.ok", v.geometry_ok);
	T_EQ("fresh.outside", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_OK("fresh.withholds", !ultrawidelock_satellite_may_predict(&s, 10000));

	/* And the other way round, in a new round so neither half is reused. */
	ultrawidelock_satellite_observe(&s, 800, RND_BLK + 1u, 10000);
	ultrawidelock_satellite_report(&s, 1400, RND_BLK + 1u, 10000);
	T_EQ("fresh.inside", ultrawidelock_satellite_verdict(&s, 10000).side,
	     ULTRAWIDELOCK_SIDE_INSIDE);
	T_OK("fresh.inside_predicts", ultrawidelock_satellite_may_predict(&s, 10000));
}

/*
 * THE FAILURE THIS SUITE EXISTS FOR. Identical numbers, opposite mounting.
 * Getting self_is_inside backwards does not crash, does not log, and does not
 * fail any geometry test -- it silently withholds from the householder and
 * grants to the person on the doorstep.
 */
static void mounting_flag_inverts_the_verdict(void)
{
	struct ultrawidelock_satellite in, out;

	t_group("sat: self_is_inside is the difference between right and backwards");

	ultrawidelock_satellite_init(&in, &k_cfg, 1500u, true);
	ultrawidelock_satellite_init(&out, &k_cfg, 1500u, false);
	ultrawidelock_satellite_observe(&in, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_observe(&out, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_report(&in, 800, RND_BLK, 10000);
	ultrawidelock_satellite_report(&out, 800, RND_BLK, 10000);

	/* Same self_mm, same peer_mm, same block. Only the mounting differs. */
	T_EQ("mount.inside_says", ultrawidelock_satellite_verdict(&in, 10000).side,
	     ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_EQ("mount.outside_says", ultrawidelock_satellite_verdict(&out, 10000).side,
	     ULTRAWIDELOCK_SIDE_INSIDE);
	T_OK("mount.opposite_predict", ultrawidelock_satellite_may_predict(&in, 10000) !=
					      ultrawidelock_satellite_may_predict(&out, 10000));
}

static void stale_report_is_not_a_report(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: an old distance is discarded, not trusted");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);

	/* Inside the window: still decides, still withholds. */
	T_OK("stale.at_1499", !ultrawidelock_satellite_may_predict(&s, 11499));
	T_OK("stale.at_1500", !ultrawidelock_satellite_may_predict(&s, 11500));

	/* Past it: the verdict evaporates and prediction resumes. The phone has
	 * not moved; only the evidence has expired. */
	T_OK("stale.at_1501", ultrawidelock_satellite_may_predict(&s, 11501));
	T_EQ("stale.side_gone", ultrawidelock_satellite_verdict(&s, 11501).side,
	     ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_OK("stale.not_ok", !ultrawidelock_satellite_verdict(&s, 11501).geometry_ok);

	/* A fresh round revives it. */
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK + 1u, 11501);
	ultrawidelock_satellite_report(&s, 800, RND_BLK + 1u, 11501);
	T_OK("stale.revived", !ultrawidelock_satellite_may_predict(&s, 11501));
}

/*
 * OUR half can go stale too. A lock that stopped ranging must not keep offering
 * its last measurement for ever: a peer still reporting that same block would
 * otherwise pair against a distance that stopped describing the room long ago.
 */
static void our_own_sample_expires(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: our half of the pair ages out as well as theirs");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK, 10000);
	/* The peer keeps reporting the same block, arriving fresh each time. */
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 11400);
	T_OK("selfstale.inside_window", !ultrawidelock_satellite_may_predict(&s, 11400));

	ultrawidelock_satellite_report(&s, 800, RND_BLK, 11600);
	T_OK("selfstale.ours_expired", ultrawidelock_satellite_may_predict(&s, 11600));
	T_OK("selfstale.not_ok", !ultrawidelock_satellite_verdict(&s, 11600).geometry_ok);
}

/*
 * A report timestamped in the future is a clock that moved, not a fast link.
 * Treating it as fresh would pin one stale distance as valid for as long as the
 * offset lasted, which is the one way a staleness window can fail open.
 */
static void future_report_is_refused(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: a report from the future is unusable, not eternal");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK, 50000);
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 50000);

	T_OK("future.predicts", ultrawidelock_satellite_may_predict(&s, 10000));
	T_EQ("future.unknown", ultrawidelock_satellite_verdict(&s, 10000).side,
	     ULTRAWIDELOCK_SIDE_UNKNOWN);
	/* Once the clock catches up it is usable again. */
	T_OK("future.recovers", !ultrawidelock_satellite_may_predict(&s, 50000));
}

static void impossible_geometry_withholds(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: a pair no phone position can produce is evidence, and withholds");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	/* 200 and 300 cannot span a 1200 baseline. */
	ultrawidelock_satellite_observe(&s, 200, RND_BLK, 10000);
	ultrawidelock_satellite_report(&s, 300, RND_BLK, 10000);
	T_OK("bad.not_ok", !ultrawidelock_satellite_verdict(&s, 10000).geometry_ok);
	T_OK("bad.withholds", !ultrawidelock_satellite_may_predict(&s, 10000));
}

/*
 * A report for a block we have no measurement of is the ABSENCE of a pair, not
 * a suspicious one. It must permit, or a satellite that merely fell behind
 * becomes indistinguishable from one reporting an intruder, and the door goes
 * down every time the link hiccups.
 */
static void unmatched_block_is_not_a_pair(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: two anchors must be describing the same ranging round");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK, 10000);

	/* Same numbers that withhold when the blocks agree... */
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);
	T_OK("blk.same_withholds", !ultrawidelock_satellite_may_predict(&s, 10000));

	/*
	 * ...and one block apart, everything else identical. Freshness cannot
	 * catch this: stale_ms 1500 against a 192 ms block is 7.8 blocks, so the
	 * skew still reads as perfectly fresh. Only the block says otherwise.
	 */
	ultrawidelock_satellite_report(&s, 800, RND_BLK + 1u, 10000);
	T_OK("blk.mismatch_not_ok", !ultrawidelock_satellite_verdict(&s, 10000).geometry_ok);
	T_EQ("blk.mismatch_unknown", ultrawidelock_satellite_verdict(&s, 10000).side,
	     ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_OK("blk.mismatch_permits", ultrawidelock_satellite_may_predict(&s, 10000));
}

/*
 * The ring is what lets a LATE report still be used. A datagram that took two
 * blocks to arrive is still perfectly good evidence about the block it
 * describes, so the answer is to remember, never to widen the match: one block
 * of slack is 192 mm at 1.0 m/s against a tolerance of 90.
 */
static void a_late_report_still_finds_its_pair(void)
{
	struct ultrawidelock_satellite s;
	uint32_t i;

	t_group("sat: a report that arrives blocks late still pairs correctly");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	/* We keep ranging while the report is in flight. */
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_observe(&s, 1300, RND_BLK + 1u, 10192);
	ultrawidelock_satellite_observe(&s, 1200, RND_BLK + 2u, 10384);

	/* The report for the OLDEST of those finally lands. */
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10384);
	T_OK("late.pairs", !ultrawidelock_satellite_may_predict(&s, 10384));
	T_EQ("late.uses_that_block", ultrawidelock_satellite_verdict(&s, 10384).side,
	     ULTRAWIDELOCK_SIDE_OUTSIDE);

	/* Older than the ring holds, and there is nothing left to pair with. The
	 * ring is sized to the staleness window, so falling out of it and going
	 * stale are the same event rather than two different cliffs. */
	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	for (i = 0u; i < ULTRAWIDELOCK_SATELLITE_RING + 1u; i++) {
		ultrawidelock_satellite_observe(&s, 1400, RND_BLK + i, 10000);
	}
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);
	T_OK("late.evicted_permits", ultrawidelock_satellite_may_predict(&s, 10000));
}

static void rejects_bad_input(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: nonsense in, permissive out");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	ultrawidelock_satellite_observe(&s, 1400, RND_BLK, 10000);

	/* A negative peer distance is a decode bug. Refuse to store it, so it
	 * cannot masquerade as a stale link later. */
	ultrawidelock_satellite_report(&s, -1, RND_BLK, 10000);
	T_OK("in.negative_peer_not_stored", ultrawidelock_satellite_may_predict(&s, 10000));

	/* A negative self distance is the caller's bug, and likewise not stored --
	 * so the block it claimed simply has no pair. */
	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	ultrawidelock_satellite_observe(&s, -1, RND_BLK, 10000);
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);
	T_OK("in.negative_self_not_stored", ultrawidelock_satellite_may_predict(&s, 10000));

	/* NULLs must not fault. */
	ultrawidelock_satellite_init(NULL, &k_cfg, 1500u, true);
	ultrawidelock_satellite_report(NULL, 800, RND_BLK, 10000);
	T_OK("in.null_predicts", ultrawidelock_satellite_may_predict(NULL, 10000));

	/* Zero stale_ms selects the default rather than expiring instantly. */
	{
		struct ultrawidelock_satellite d;

		ultrawidelock_satellite_init(&d, &k_cfg, 0u, true);
		ultrawidelock_satellite_observe(&d, 1400, RND_BLK, 10000);
		ultrawidelock_satellite_report(&d, 800, RND_BLK, 10000);
		T_OK("in.zero_stale_is_default",
		     !ultrawidelock_satellite_may_predict(
			     &d, 10000 + ULTRAWIDELOCK_SATELLITE_STALE_MS_DEFAULT));
	}

	/* A NULL cfg zeroes the baseline, which ultrawidelock_fusion rejects -- so the
	 * verdict is never OK and prediction is never withheld on garbage. */
	{
		struct ultrawidelock_satellite n;

		ultrawidelock_satellite_init(&n, NULL, 1500u, true);
		ultrawidelock_satellite_observe(&n, 1400, RND_BLK, 10000);
		ultrawidelock_satellite_report(&n, 800, RND_BLK, 10000);
		T_OK("in.null_cfg_permits", ultrawidelock_satellite_may_predict(&n, 10000));
	}
}

/* ── more than one satellite ─────────────────────────────────────────────── */

/*
 * The set's geometry. Role 1 is the shipping case and keeps k_cfg's baseline so
 * the single-satellite assertions below can be compared line for line with the
 * suite above. Role 2 is a second board 1.6 m away -- a DIFFERENT baseline,
 * because that is the whole reason the config is per role. Role 3 is left
 * uninstalled (baseline 0), which is what every deployment in this tree has.
 */
static const struct ultrawidelock_fusion_cfg k_set_cfg[ULTRAWIDELOCK_SATELLITE_MAX_ROLES] = {
	{.baseline_mm = 1200, .tol_mm = 90, .deadband_mm = 60},
	{.baseline_mm = 1600, .tol_mm = 90, .deadband_mm = 60},
	{.baseline_mm = 0, .tol_mm = 90, .deadband_mm = 60},
};

/*
 * The case that actually ships. One satellite reporting into a set must be
 * indistinguishable from one satellite reporting into a single struct -- if it
 * is not, this port changed the behaviour of a working door.
 */
static void one_role_is_todays_behaviour(void)
{
	struct ultrawidelock_satellite_set set;
	struct ultrawidelock_satellite one;

	t_group("set: one satellite behaves exactly as it does today");

	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_init(&one, &k_cfg, 1500u, true);

	/* Nobody reporting: the permissive state, and the state a board is in
	 * for its whole life until a satellite is mounted. */
	ultrawidelock_satellite_set_observe(&set, 800, RND_BLK, 10000);
	ultrawidelock_satellite_observe(&one, 800, RND_BLK, 10000);
	T_OK("set.one.absent_predicts", ultrawidelock_satellite_set_may_predict(&set, 10000));
	T_OK("set.one.absent_not_ok",
	     !ultrawidelock_satellite_set_verdict(&set, 10000).geometry_ok);
	T_EQ("set.one.absent_peer_mm", ultrawidelock_satellite_set_peer_mm(&set, 10000), -1);

	/* Role 1 reports the pair that withholds. */
	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_init(&one, &k_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_observe(&one, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 1u, 800, RND_BLK, 10000);
	ultrawidelock_satellite_report(&one, 800, RND_BLK, 10000);

	T_EQ("set.one.same_side", ultrawidelock_satellite_set_verdict(&set, 10000).side,
	     ultrawidelock_satellite_verdict(&one, 10000).side);
	T_EQ("set.one.same_delta", ultrawidelock_satellite_set_verdict(&set, 10000).delta_mm,
	     ultrawidelock_satellite_verdict(&one, 10000).delta_mm);
	T_OK("set.one.same_geometry_ok",
	     ultrawidelock_satellite_set_verdict(&set, 10000).geometry_ok ==
		     ultrawidelock_satellite_verdict(&one, 10000).geometry_ok);
	T_OK("set.one.same_predict",
	     ultrawidelock_satellite_set_may_predict(&set, 10000) ==
		     ultrawidelock_satellite_may_predict(&one, 10000));
	T_EQ("set.one.same_peer_mm", ultrawidelock_satellite_set_peer_mm(&set, 10000),
	     ultrawidelock_satellite_peer_mm(&one, 10000));
	T_EQ("set.one.outside", ultrawidelock_satellite_set_verdict(&set, 10000).side,
	     ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_OK("set.one.withholds", !ultrawidelock_satellite_set_may_predict(&set, 10000));
}

/*
 * THE ONE-SATELLITE ASSUMPTION THIS SUITE EXISTS FOR.
 *
 * Two boards report in the SAME ranging block. Before the roles were kept
 * apart, both distances landed in one slot and whichever arrived second was
 * fused as though the other board had measured it -- no test failed, the side
 * verdict simply inverted. Here role 2's much closer distance must not be able
 * to masquerade as role 1's.
 */
static void two_roles_report_in_one_block(void)
{
	struct ultrawidelock_satellite_set set;
	struct ultrawidelock_fusion_verdict v;

	t_group("set: two satellites reporting into the same block stay apart");

	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);

	/* Both agree the phone is nearer the outer anchors: 1400 against 800 and
	 * against 700, one block, two boards. */
	ultrawidelock_satellite_set_report(&set, 1u, 800, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 2u, 700, RND_BLK, 10000);

	v = ultrawidelock_satellite_set_verdict(&set, 10000);
	T_OK("set.two.geometry_ok", v.geometry_ok);
	T_EQ("set.two.outside", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);
	/* Role 1's pair, not role 2's: the lowest-numbered deciding role, so the
	 * number a one-satellite lock reported does not move when a second board
	 * is added beside it. */
	T_EQ("set.two.delta_is_role1", v.delta_mm, 600);
	T_EQ("set.two.peer_mm_is_role1", ultrawidelock_satellite_set_peer_mm(&set, 10000), 800);
	T_OK("set.two.withholds", !ultrawidelock_satellite_set_may_predict(&set, 10000));

	/* Role 2 alone, with role 1 silent, must decide on its OWN baseline.
	 * 1400/700 is INSIDE-of-nothing on a 1600 baseline the same way it is on
	 * 1200 -- what matters is that the slot answering is the one that was
	 * reported to. */
	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 2u, 700, RND_BLK, 10000);
	v = ultrawidelock_satellite_set_verdict(&set, 10000);
	T_OK("set.role2_alone.geometry_ok", v.geometry_ok);
	T_EQ("set.role2_alone.outside", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_EQ("set.role2_alone.delta", v.delta_mm, 700);
	T_EQ("set.role2_alone.peer_mm", ultrawidelock_satellite_set_peer_mm(&set, 10000), 700);
}

/* Three roles, all installed, all reporting the same round. */
static void three_roles_report_in_one_block(void)
{
	struct ultrawidelock_satellite_set set;
	struct ultrawidelock_fusion_verdict v;
	const struct ultrawidelock_fusion_cfg all_three[ULTRAWIDELOCK_SATELLITE_MAX_ROLES] = {
		{.baseline_mm = 1200, .tol_mm = 90, .deadband_mm = 60},
		{.baseline_mm = 1600, .tol_mm = 90, .deadband_mm = 60},
		{.baseline_mm = 900, .tol_mm = 90, .deadband_mm = 60},
	};

	t_group("set: three satellites is the ceiling and it holds");

	ultrawidelock_satellite_set_init(&set, all_three, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 1u, 800, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 2u, 700, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 3u, 900, RND_BLK, 10000);

	v = ultrawidelock_satellite_set_verdict(&set, 10000);
	T_OK("set.three.geometry_ok", v.geometry_ok);
	T_EQ("set.three.outside", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_OK("set.three.withholds", !ultrawidelock_satellite_set_may_predict(&set, 10000));

	/* All three agreeing the other way permits, and does so on every role's
	 * own arithmetic rather than the first one's. */
	ultrawidelock_satellite_set_init(&set, all_three, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 800, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 1u, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 2u, 1500, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 3u, 1300, RND_BLK, 10000);
	v = ultrawidelock_satellite_set_verdict(&set, 10000);
	T_EQ("set.three.inside", v.side, ULTRAWIDELOCK_SIDE_INSIDE);
	T_OK("set.three.inside_predicts", ultrawidelock_satellite_set_may_predict(&set, 10000));
}

/*
 * Two anchors watching one phone cannot both be right about opposite sides, so
 * the set refuses rather than picking a winner. A majority vote is exactly the
 * wrong instinct here: one of the two devices may be the one lying.
 */
static void disagreeing_roles_fail_closed(void)
{
	struct ultrawidelock_satellite_set set;
	struct ultrawidelock_fusion_verdict v;

	t_group("set: two satellites naming opposite sides is refused, not voted on");

	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	/* Role 1: 1400 inside vs 800 outside -> OUTSIDE. */
	ultrawidelock_satellite_set_report(&set, 1u, 800, RND_BLK, 10000);
	/* Role 2: 1400 inside vs 2000 outside -> INSIDE. Both pairs pass their
	 * own triangle test; they simply cannot both describe one phone. */
	ultrawidelock_satellite_set_report(&set, 2u, 2000, RND_BLK, 10000);

	v = ultrawidelock_satellite_set_verdict(&set, 10000);
	T_OK("set.split.not_ok", !v.geometry_ok);
	T_EQ("set.split.unknown", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);
	/* And the AND over may_predict still withholds, because role 1 has real
	 * evidence the phone is outside. The two gates fail closed separately:
	 * neither is allowed to rescue the other. */
	T_OK("set.split.withholds", !ultrawidelock_satellite_set_may_predict(&set, 10000));
}

/*
 * A role sitting in its dead band has a good pair and no opinion. Two anchors
 * do not share a bisector, so with a second satellite installed this is an
 * ORDINARY reading -- and if it counted as a disagreement, adding a satellite
 * would make a door that worked refuse.
 */
static void a_dead_band_role_abstains(void)
{
	struct ultrawidelock_satellite_set set;
	struct ultrawidelock_fusion_verdict v;

	t_group("set: a satellite in its dead band abstains, it does not veto");

	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	/* Role 1: |1400 - 1370| = 30, inside the 60 mm dead band -> UNKNOWN with
	 * a good pair. */
	ultrawidelock_satellite_set_report(&set, 1u, 1370, RND_BLK, 10000);
	/* Role 2: a confident INSIDE. */
	ultrawidelock_satellite_set_report(&set, 2u, 2000, RND_BLK, 10000);

	v = ultrawidelock_satellite_set_verdict(&set, 10000);
	T_OK("set.abstain.geometry_ok", v.geometry_ok);
	T_EQ("set.abstain.uses_the_deciding_role", v.side, ULTRAWIDELOCK_SIDE_INSIDE);
	T_OK("set.abstain.predicts", ultrawidelock_satellite_set_may_predict(&set, 10000));

	/* Every speaking role abstaining reports the abstention -- {UNKNOWN,
	 * geometry_ok = true} -- which is what one satellite in its dead band
	 * returns today, and is NOT the same state as nobody reporting. */
	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 1u, 1370, RND_BLK, 10000);
	v = ultrawidelock_satellite_set_verdict(&set, 10000);
	T_OK("set.abstain.only_ok", v.geometry_ok);
	T_EQ("set.abstain.only_unknown", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_OK("set.abstain.only_predicts", ultrawidelock_satellite_set_may_predict(&set, 10000));
}

/*
 * Absence must stay absence however it arises: a role nobody mounted, a role
 * whose board went quiet, and a role whose report was for another block are all
 * "no satellite", and none of them may withhold. This is rule 2 of the header,
 * asserted once per way of being absent.
 */
static void absent_roles_never_withhold(void)
{
	struct ultrawidelock_satellite_set set;

	t_group("set: an uninstalled or quiet role permits, it does not veto");

	/* Role 3 is uninstalled (baseline 0) and reports anyway -- a board left
	 * over from a previous mounting, say. A zero baseline is absence, so the
	 * report cannot become evidence. */
	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 3u, 800, RND_BLK, 10000);
	T_OK("set.absent.uninstalled_predicts",
	     ultrawidelock_satellite_set_may_predict(&set, 10000));
	T_OK("set.absent.uninstalled_not_ok",
	     !ultrawidelock_satellite_set_verdict(&set, 10000).geometry_ok);
	T_EQ("set.absent.uninstalled_no_peer_mm",
	     ultrawidelock_satellite_set_peer_mm(&set, 10000), -1);

	/* Role 1 deciding while role 2 is silent: the silent one must not dilute
	 * the answer. This is the one-satellite deployment with a slot spare. */
	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 1u, 800, RND_BLK, 10000);
	T_EQ("set.absent.quiet_role_ignored",
	     ultrawidelock_satellite_set_verdict(&set, 10000).side, ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_OK("set.absent.quiet_role_withholds",
	     !ultrawidelock_satellite_set_may_predict(&set, 10000));

	/* Role 2 reporting a block we never latched is not a pair, and a
	 * non-pair permits -- even while role 1 has a good one. */
	ultrawidelock_satellite_set_report(&set, 2u, 700, RND_BLK + 5u, 10000);
	T_EQ("set.absent.other_block_ignored",
	     ultrawidelock_satellite_set_verdict(&set, 10000).side, ULTRAWIDELOCK_SIDE_OUTSIDE);

	/* Every role stale: back to today's behaviour, all the way. */
	T_OK("set.absent.all_stale_predicts",
	     ultrawidelock_satellite_set_may_predict(&set, 11501));
	T_OK("set.absent.all_stale_not_ok",
	     !ultrawidelock_satellite_set_verdict(&set, 11501).geometry_ok);
}

/* Roles off the end, and a NULL set, which is how a build with no satellite
 * transport behaves. Dropped rather than clamped: a role outside 1..3 has no
 * disjoint nonce space with the lock, so it is not a peer this link can have. */
static void set_rejects_bad_input(void)
{
	struct ultrawidelock_satellite_set set;

	t_group("set: out-of-range roles are dropped, not folded onto role 1");

	ultrawidelock_satellite_set_init(&set, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);

	ultrawidelock_satellite_set_report(&set, 0u, 800, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, ULTRAWIDELOCK_SATELLITE_MAX_ROLES + 1u, 800,
					   RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 255u, 800, RND_BLK, 10000);
	T_OK("set.bad.role_dropped", ultrawidelock_satellite_set_may_predict(&set, 10000));
	T_EQ("set.bad.no_peer_mm", ultrawidelock_satellite_set_peer_mm(&set, 10000), -1);

	/* A negative distance is refused by the slot, exactly as it is for a
	 * single satellite: a decode bug must not read as a stale link. */
	ultrawidelock_satellite_set_report(&set, 1u, -1, RND_BLK, 10000);
	T_OK("set.bad.negative_not_stored", ultrawidelock_satellite_set_may_predict(&set, 10000));

	/* NULL everywhere must not fault, and must permit. */
	ultrawidelock_satellite_set_init(NULL, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_observe(NULL, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(NULL, 1u, 800, RND_BLK, 10000);
	T_OK("set.bad.null_predicts", ultrawidelock_satellite_set_may_predict(NULL, 10000));
	T_OK("set.bad.null_not_ok", !ultrawidelock_satellite_set_verdict(NULL, 10000).geometry_ok);
	T_EQ("set.bad.null_peer_mm", ultrawidelock_satellite_set_peer_mm(NULL, 10000), -1);

	/* A NULL cfg leaves every baseline at zero, which is absence rather than
	 * a permanently failing triangle -- the same fail-back the single-peer
	 * init gives. */
	ultrawidelock_satellite_set_init(&set, NULL, 1500u, true);
	ultrawidelock_satellite_set_observe(&set, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&set, 1u, 800, RND_BLK, 10000);
	T_OK("set.bad.null_cfg_permits", ultrawidelock_satellite_set_may_predict(&set, 10000));
}

/*
 * The mounting flag is one fact about THIS board, so it applies to every slot.
 * Backwards, it inverts every role's verdict at once rather than some of them,
 * which is the only sane failure: half-inverted geometry would read as two
 * satellites disagreeing and hide a config error behind a fail-closed.
 */
static void mounting_flag_applies_to_every_role(void)
{
	struct ultrawidelock_satellite_set in;
	struct ultrawidelock_satellite_set out;

	t_group("set: which side THIS board is on applies to all roles at once");

	ultrawidelock_satellite_set_init(&in, k_set_cfg, 1500u, true);
	ultrawidelock_satellite_set_init(&out, k_set_cfg, 1500u, false);
	ultrawidelock_satellite_set_observe(&in, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_observe(&out, 1400, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&in, 1u, 800, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&in, 2u, 700, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&out, 1u, 800, RND_BLK, 10000);
	ultrawidelock_satellite_set_report(&out, 2u, 700, RND_BLK, 10000);

	T_EQ("set.mount.inside_says", ultrawidelock_satellite_set_verdict(&in, 10000).side,
	     ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_EQ("set.mount.outside_says", ultrawidelock_satellite_set_verdict(&out, 10000).side,
	     ULTRAWIDELOCK_SIDE_INSIDE);
	/* Both roles flipped together, so the two sets still each hold an
	 * internally consistent opinion rather than a refusal. */
	T_OK("set.mount.both_decide",
	     ultrawidelock_satellite_set_verdict(&in, 10000).geometry_ok &&
		     ultrawidelock_satellite_set_verdict(&out, 10000).geometry_ok);
	T_OK("set.mount.opposite_predict",
	     ultrawidelock_satellite_set_may_predict(&in, 10000) !=
		     ultrawidelock_satellite_set_may_predict(&out, 10000));
}

void test_ultrawidelock_satellite(void)
{
	absent_satellite_changes_nothing();
	fresh_report_decides();
	mounting_flag_inverts_the_verdict();
	stale_report_is_not_a_report();
	our_own_sample_expires();
	future_report_is_refused();
	impossible_geometry_withholds();
	unmatched_block_is_not_a_pair();
	a_late_report_still_finds_its_pair();
	rejects_bad_input();
	one_role_is_todays_behaviour();
	two_roles_report_in_one_block();
	three_roles_report_in_one_block();
	disagreeing_roles_fail_closed();
	a_dead_band_role_abstains();
	absent_roles_never_withhold();
	set_rejects_bad_input();
	mounting_flag_applies_to_every_role();
}
