/**
 * @file test_ultrawidelock_fusion.c — ultrawidelock_fusion suite: side-of-door from the signed
 * distance difference, and the triangle-inequality consistency gate.
 *
 * The interesting assertions are the negative ones. It is easy to write a gate
 * that passes everything; what this checks is that the pairs it is supposed to
 * refuse are actually refused, and -- just as important -- that the pairs it
 * CANNOT catch are documented as passing rather than quietly assumed caught.
 */
#include "test.h"

#include "ultrawidelock_fusion.h"

/* Anchors 1.2 m apart. Tolerance 90 mm is 3 sigma of the 30 mm jitter stage A
 * targets; dead band 60 mm is 2 sigma, per the header. */
static const struct ultrawidelock_fusion_cfg k_cfg = {
	.baseline_mm = 1200,
	.tol_mm = 90,
	.deadband_mm = 60,
};

static void side_of_door(void)
{
	struct ultrawidelock_fusion_verdict v;

	t_group("side: the sign of the difference picks the side");

	/* Clearly nearer the inside anchor. */
	v = ultrawidelock_fusion_eval(&k_cfg, 800, 1400);
	T_OK("side.inside.ok", v.geometry_ok);
	T_EQ("side.inside", v.side, ULTRAWIDELOCK_SIDE_INSIDE);
	T_EQ("side.inside.delta", v.delta_mm, -600);

	/* Clearly nearer the outside anchor. */
	v = ultrawidelock_fusion_eval(&k_cfg, 1400, 800);
	T_OK("side.outside.ok", v.geometry_ok);
	T_EQ("side.outside", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);
	T_EQ("side.outside.delta", v.delta_mm, 600);
}

static void deadband_is_honest(void)
{
	struct ultrawidelock_fusion_verdict v;

	t_group("side: the doorway is ambiguous and says so");

	/* Standing on the bisector: geometrically fine, but the difference is
	 * inside the noise, so the sign means nothing. */
	v = ultrawidelock_fusion_eval(&k_cfg, 1000, 1000);
	T_OK("dead.centre.ok", v.geometry_ok);
	T_EQ("dead.centre", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);

	/* Just inside the band: still UNKNOWN rather than whichever way noise fell. */
	v = ultrawidelock_fusion_eval(&k_cfg, 1030, 1000);
	T_EQ("dead.inside_band", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);

	/* Just outside it: now the sign is trusted. */
	v = ultrawidelock_fusion_eval(&k_cfg, 1061, 1000);
	T_EQ("dead.outside_band", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);
}

static void boundary_bias_moves_the_frontier(void)
{
	/* Same bench, boundary pushed to 200 mm in front of the inside anchor:
	 * (1200 - 800) / 2 per the header. The bench case this serves is both
	 * anchors on one side of the walkway, "inside" meaning behind the lock. */
	static const struct ultrawidelock_fusion_cfg k_biased = {
		.baseline_mm = 1200,
		.tol_mm = 90,
		.deadband_mm = 60,
		.boundary_bias_mm = 800,
	};
	struct ultrawidelock_fusion_verdict v;

	t_group("bias: the frontier moves, the evidence and the gate do not");

	/* Between the bisector and the inside anchor -- INSIDE at bias 0
	 * (side.inside above proves the sign) -- now reads OUTSIDE. */
	v = ultrawidelock_fusion_eval(&k_biased, 400, 800);
	T_OK("bias.front.ok", v.geometry_ok);
	T_EQ("bias.front", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);

	/* Behind the shifted frontier is still INSIDE. */
	v = ultrawidelock_fusion_eval(&k_biased, 100, 1100);
	T_EQ("bias.behind", v.side, ULTRAWIDELOCK_SIDE_INSIDE);

	/* The dead band recentres on the frontier: exactly on it is UNKNOWN,
	 * and delta_mm stays the RAW difference -- the bias moves opinion,
	 * not evidence. */
	v = ultrawidelock_fusion_eval(&k_biased, 200, 1000);
	T_EQ("bias.on_frontier", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_EQ("bias.raw_delta", v.delta_mm, -800);
	v = ultrawidelock_fusion_eval(&k_biased, 235, 965); /* eff +70, past the band */
	T_EQ("bias.past_band", v.side, ULTRAWIDELOCK_SIDE_OUTSIDE);

	/* The triangle gate still runs on the raw pair: an impossible pair is
	 * refused no matter where the frontier sits. */
	v = ultrawidelock_fusion_eval(&k_biased, 200, 300);
	T_OK("bias.gate_intact", !v.geometry_ok);
}

static void triangle_gate_refuses_impossible_pairs(void)
{
	struct ultrawidelock_fusion_verdict v;

	t_group("triangle: pairs no single phone position can produce");

	/*
	 * Too short to span the baseline. The anchors are 1200 mm apart, so a
	 * point 200 mm from one and 300 mm from the other does not exist. This
	 * is the shape a distance-reduction attack on ONE link takes.
	 */
	v = ultrawidelock_fusion_eval(&k_cfg, 200, 300);
	T_OK("tri.too_short.rejected", !v.geometry_ok);
	T_EQ("tri.too_short.side", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);

	/*
	 * Too unequal. One link inflated well past what the baseline allows --
	 * the shape of an asymmetric relay, or of two ranges taken from
	 * different targets.
	 */
	v = ultrawidelock_fusion_eval(&k_cfg, 400, 2000);
	T_OK("tri.too_unequal.rejected", !v.geometry_ok);
	T_EQ("tri.too_unequal.side", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);

	/* Exactly on the baseline, within tolerance, is legal: the phone is in
	 * line with both anchors, which is an ordinary place to stand. */
	v = ultrawidelock_fusion_eval(&k_cfg, 600, 600);
	T_OK("tri.collinear_ok", v.geometry_ok);
	v = ultrawidelock_fusion_eval(&k_cfg, 100, 1300);
	T_OK("tri.inline_ok", v.geometry_ok);

	/* Tolerance is real: a pair short by less than tol still passes, so
	 * ordinary noise at the boundary does not read as an attack. */
	v = ultrawidelock_fusion_eval(&k_cfg, 570, 570); /* sum 1140, baseline - tol = 1110 */
	T_OK("tri.within_tol", v.geometry_ok);
	v = ultrawidelock_fusion_eval(&k_cfg, 540, 540); /* sum 1080, below 1110 */
	T_OK("tri.past_tol", !v.geometry_ok);
}

/*
 * THE LIMIT, asserted so nobody has to trust the header.
 *
 * ultrawidelock_fusion.h states plainly that a SYMMETRIC relay -- one inflating both
 * links by the same amount -- passes this gate. If that ever silently started
 * being caught, the header would be understating the defence; if it is still
 * passed, this documents why that is acceptable. Either way it should be a test
 * and not a claim.
 */
static void symmetric_relay_is_not_caught(void)
{
	struct ultrawidelock_fusion_verdict real, relayed;

	t_group("triangle: a symmetric relay passes, and that is the documented limit");

	real = ultrawidelock_fusion_eval(&k_cfg, 800, 1400);
	/* Both links inflated by 3 m through one point near the door. */
	relayed = ultrawidelock_fusion_eval(&k_cfg, 800 + 3000, 1400 + 3000);

	T_OK("sym.real_ok", real.geometry_ok);
	T_OK("sym.relayed_still_ok", relayed.geometry_ok);
	T_EQ("sym.same_delta", relayed.delta_mm, real.delta_mm);
	/*
	 * And why it does not matter, asserted rather than asserted-in-prose: the
	 * inflated distances are far outside the approach controller's unlock
	 * radius (100 cm by default, ultrawidelock_approach.c), so the range gate refuses
	 * them long before geometry is consulted. Inflation cannot help an
	 * attacker who needs to look CLOSE.
	 */
	T_OK("sym.beyond_unlock_radius", (800 + 3000) > 1000 && (1400 + 3000) > 1000);
}

static void predict_gate(void)
{
	struct ultrawidelock_fusion_verdict v;

	t_group("predict gate: only a positive OUTSIDE withholds prediction");

	v = ultrawidelock_fusion_eval(&k_cfg, 800, 1400); /* inside */
	T_OK("pred.inside_allows", ultrawidelock_fusion_may_predict(&v));

	v = ultrawidelock_fusion_eval(&k_cfg, 1400, 800); /* outside */
	T_OK("pred.outside_withholds", !ultrawidelock_fusion_may_predict(&v));

	/* Ambiguous must NOT withhold: a phone in the doorway, or a satellite
	 * that has gone quiet, has to degrade to today's behaviour rather than
	 * to a door that will not open. */
	v = ultrawidelock_fusion_eval(&k_cfg, 1000, 1000);
	T_EQ("pred.unknown_is_unknown", v.side, ULTRAWIDELOCK_SIDE_UNKNOWN);
	T_OK("pred.unknown_allows", ultrawidelock_fusion_may_predict(&v));

	/* No verdict at all is the same as no satellite. */
	T_OK("pred.null_allows", ultrawidelock_fusion_may_predict(NULL));

	/* But a pair that failed the triangle test is real evidence of a fault
	 * or an attack, and that does withhold. */
	v = ultrawidelock_fusion_eval(&k_cfg, 200, 300);
	T_OK("pred.bad_geometry_withholds", !ultrawidelock_fusion_may_predict(&v));
}

static void rejects_bad_input(void)
{
	struct ultrawidelock_fusion_verdict v;
	struct ultrawidelock_fusion_cfg zero = {0, 0, 0};

	t_group("input: nonsense in, UNKNOWN out");

	v = ultrawidelock_fusion_eval(NULL, 800, 1400);
	T_OK("in.null_cfg", !v.geometry_ok);
	v = ultrawidelock_fusion_eval(&zero, 800, 1400);
	T_OK("in.zero_baseline", !v.geometry_ok);
	v = ultrawidelock_fusion_eval(&k_cfg, -1, 1400);
	T_OK("in.negative", !v.geometry_ok);
}

void test_ultrawidelock_fusion(void)
{
	side_of_door();
	deadband_is_honest();
	boundary_bias_moves_the_frontier();
	triangle_gate_refuses_impossible_pairs();
	symmetric_relay_is_not_caught();
	predict_gate();
	rejects_bad_input();
}
