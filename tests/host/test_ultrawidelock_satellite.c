/**
 * @file test_ultrawidelock_satellite.c — ultrawidelock_satellite suite: the freshness gate, and the
 * mounting flag that decides which anchor is which.
 *
 * ultrawidelock_fusion is already covered. What this suite is for is the two ways the
 * gate around it can be wrong in a way no geometry test would catch: deciding
 * with a stale distance, and deciding with the two anchors swapped.
 */
#include "test.h"

/* Every pre-existing case here describes ONE ranging round, so they all share a
 * block. The mismatch cases below choose a different one deliberately. */
#define RND_BLK 7u

#include "ultrawidelock_satellite.h"

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

	/* Never reported. This is the state a board is in for its whole life
	 * until Stage C exists, so it had better be the permissive one. */
	T_OK("absent.predicts", ultrawidelock_satellite_may_predict(&s, 800, RND_BLK, 10000));
	T_OK("absent.unknown",
	     ultrawidelock_satellite_verdict(&s, 800, RND_BLK, 10000).side == ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_OK("absent.not_ok", !ultrawidelock_satellite_verdict(&s, 800, RND_BLK, 10000).geometry_ok);

	/* And a NULL satellite, which is how a build without one behaves. */
	T_OK("absent.null_predicts", ultrawidelock_satellite_may_predict(NULL, 800, RND_BLK, 10000));
}

static void fresh_report_decides(void)
{
	struct ultrawidelock_satellite s;
	struct ultrawidelock_fusion_verdict v;

	t_group("sat: a fresh report is used");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	/* Phone nearer the outside anchor: self (inside) 1400, peer 800. */
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);

	v = ultrawidelock_satellite_verdict(&s, 1400, RND_BLK, 10000);
	T_OK("fresh.ok", v.geometry_ok);
	T_EQ("fresh.outside", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_OK("fresh.withholds", !ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 10000));

	/* And the other way round. */
	ultrawidelock_satellite_report(&s, 1400, RND_BLK, 10000);
	T_EQ("fresh.inside", ultrawidelock_satellite_verdict(&s, 800, RND_BLK, 10000).side,
	     ULTRAWIDELOCK_SIDE_INSIDE);
	T_OK("fresh.inside_predicts", ultrawidelock_satellite_may_predict(&s, 800, RND_BLK, 10000));
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
	ultrawidelock_satellite_report(&in, 800, RND_BLK, 10000);
	ultrawidelock_satellite_report(&out, 800, RND_BLK, 10000);

	/* Same self_mm, same peer_mm, same instant. Only the mounting differs. */
	T_EQ("mount.inside_says", ultrawidelock_satellite_verdict(&in, 1400, RND_BLK, 10000).side,
	     ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_EQ("mount.outside_says", ultrawidelock_satellite_verdict(&out, 1400, RND_BLK, 10000).side,
	     ULTRAWIDELOCK_SIDE_INSIDE);
	T_OK("mount.opposite_predict", ultrawidelock_satellite_may_predict(&in, 1400, RND_BLK, 10000) !=
					      ultrawidelock_satellite_may_predict(&out, 1400, RND_BLK, 10000));
}

static void stale_report_is_not_a_report(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: an old distance is discarded, not trusted");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);

	/* Inside the window: still decides, still withholds. */
	T_OK("stale.at_1499", !ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 11499));
	T_OK("stale.at_1500", !ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 11500));

	/* Past it: the verdict evaporates and prediction resumes. The phone has
	 * not moved; only the evidence has expired. */
	T_OK("stale.at_1501", ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 11501));
	T_EQ("stale.side_gone", ultrawidelock_satellite_verdict(&s, 1400, RND_BLK, 11501).side,
	     ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_OK("stale.not_ok", !ultrawidelock_satellite_verdict(&s, 1400, RND_BLK, 11501).geometry_ok);

	/* A fresh report revives it. */
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 11501);
	T_OK("stale.revived", !ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 11501));
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
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 50000);

	T_OK("future.predicts", ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 10000));
	T_EQ("future.unknown", ultrawidelock_satellite_verdict(&s, 1400, RND_BLK, 10000).side,
	     ULTRAWIDELOCK_SIDE_UNKNOWN);
	/* Once the clock catches up it is usable again. */
	T_OK("future.recovers", !ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 50000));
}

static void impossible_geometry_withholds(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: a pair no phone position can produce is evidence, and withholds");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	/* 200 and 300 cannot span a 1200 baseline. */
	ultrawidelock_satellite_report(&s, 300, RND_BLK, 10000);
	T_OK("bad.not_ok", !ultrawidelock_satellite_verdict(&s, 200, RND_BLK, 10000).geometry_ok);
	T_OK("bad.withholds", !ultrawidelock_satellite_may_predict(&s, 200, RND_BLK, 10000));
}

static void rejects_bad_input(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: nonsense in, permissive out");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);

	/* A negative peer distance is a decode bug. Refuse to store it, so it
	 * cannot masquerade as a stale link later. */
	ultrawidelock_satellite_report(&s, -1, RND_BLK, 10000);
	T_OK("in.negative_peer_not_stored", ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 10000));

	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);
	/* A negative self distance is the caller's bug; do not decide on it. */
	T_OK("in.negative_self", ultrawidelock_satellite_may_predict(&s, -1, RND_BLK, 10000));

	/* NULLs must not fault. */
	ultrawidelock_satellite_init(NULL, &k_cfg, 1500u, true);
	ultrawidelock_satellite_report(NULL, 800, RND_BLK, 10000);
	T_OK("in.null_predicts", ultrawidelock_satellite_may_predict(NULL, 800, RND_BLK, 10000));

	/* Zero stale_ms selects the default rather than expiring instantly. */
	{
		struct ultrawidelock_satellite d;

		ultrawidelock_satellite_init(&d, &k_cfg, 0u, true);
		ultrawidelock_satellite_report(&d, 800, RND_BLK, 10000);
		T_OK("in.zero_stale_is_default",
		     !ultrawidelock_satellite_may_predict(&d, 1400, RND_BLK,
						10000 + ULTRAWIDELOCK_SATELLITE_STALE_MS_DEFAULT));
	}

	/* A NULL cfg zeroes the baseline, which ultrawidelock_fusion rejects -- so the
	 * verdict is never OK and prediction is never withheld on garbage. */
	{
		struct ultrawidelock_satellite n;

		ultrawidelock_satellite_init(&n, NULL, 1500u, true);
		ultrawidelock_satellite_report(&n, 800, RND_BLK, 10000);
		T_OK("in.null_cfg_permits", ultrawidelock_satellite_may_predict(&n, 1400, RND_BLK, 10000));
	}
}

static void different_block_is_not_a_pair(void)
{
	struct ultrawidelock_satellite s;

	t_group("sat: two anchors must be describing the same ranging round");

	ultrawidelock_satellite_init(&s, &k_cfg, 1500u, true);
	/* The exact pair that withholds when both anchors mean the same round:
	 * self (inside) 1400 against peer 800 puts the phone OUTSIDE. */
	ultrawidelock_satellite_report(&s, 800, RND_BLK, 10000);
	T_OK("blk.same_withholds", !ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK, 10000));

	/*
	 * One block apart and everything else identical. Freshness cannot catch
	 * this -- stale_ms is 1500 against a 192 ms block, so eight blocks of skew
	 * still reads as fresh -- which is exactly why the block has to be carried
	 * and compared rather than assumed.
	 */
	T_OK("blk.mismatch_not_ok",
	     !ultrawidelock_satellite_verdict(&s, 1400, RND_BLK + 1u, 10000).geometry_ok);
	T_EQ("blk.mismatch_unknown",
	     ultrawidelock_satellite_verdict(&s, 1400, RND_BLK + 1u, 10000).side,
	     ULTRAWIDELOCK_SIDE_UNKNOWN);

	/*
	 * And it must PERMIT rather than withhold. A mismatch is the absence of a
	 * pair, not a suspicious one; withholding on it would make a satellite that
	 * merely fell a block behind indistinguishable from one reporting an
	 * intruder, and would take the door down every time the link hiccuped.
	 */
	T_OK("blk.mismatch_permits",
	     ultrawidelock_satellite_may_predict(&s, 1400, RND_BLK + 1u, 10000));

	/* A failed triangle, however, IS a pair, and still owes a withhold. Both
	 * surface as {UNKNOWN, !geometry_ok}, so they must not share a fate. */
	ultrawidelock_satellite_report(&s, 30000, RND_BLK, 10000);
	T_OK("blk.impossible_still_withholds",
	     !ultrawidelock_satellite_may_predict(&s, 30, RND_BLK, 10000));
}

void test_ultrawidelock_satellite(void)
{
	absent_satellite_changes_nothing();
	different_block_is_not_a_pair();
	fresh_report_decides();
	mounting_flag_inverts_the_verdict();
	stale_report_is_not_a_report();
	future_report_is_refused();
	impossible_geometry_withholds();
	rejects_bad_input();
}
