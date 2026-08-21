/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_fusion.c — two-anchor side-of-door fusion (implementation).
 */

#include "ultrawidelock_fusion.h"

struct ultrawidelock_fusion_verdict
ultrawidelock_fusion_eval(const struct ultrawidelock_fusion_cfg *cfg, int32_t d_inside_mm,
			  int32_t d_outside_mm)
{
	struct ultrawidelock_fusion_verdict v = {ULTRAWIDELOCK_SIDE_UNKNOWN, false, 0};
	int32_t diff, sum, adiff, eff, aeff;

	if (cfg == NULL || cfg->baseline_mm <= 0 || d_inside_mm < 0 || d_outside_mm < 0) {
		return v;
	}

	diff = d_inside_mm - d_outside_mm;
	sum = d_inside_mm + d_outside_mm;
	adiff = diff < 0 ? -diff : diff;
	v.delta_mm = diff;

	/*
	 * The triangle inequality, both ways round. The phone and the two
	 * anchors are three points; the anchor separation is the one side that
	 * is known, so the two measured sides have to be able to close on it.
	 *
	 *   sum   >= baseline - tol : the two are long enough to reach across
	 *   adiff <= baseline + tol : they are not so unequal that no point exists
	 *
	 * Both use the SAME tolerance because both failures mean the same thing
	 * -- a pair no single phone position can produce.
	 */
	if (sum < cfg->baseline_mm - cfg->tol_mm) {
		return v; /* geometry_ok stays false */
	}
	if (adiff > cfg->baseline_mm + cfg->tol_mm) {
		return v;
	}
	v.geometry_ok = true;

	/*
	 * Sign of the difference is the side, after the configured boundary
	 * shift (zero in the shipping install, where the bisector IS the door
	 * plane). Inside the dead band -- centred on the shifted boundary, not
	 * on the bisector -- the distances are too close for the sign to mean
	 * anything, so the answer is UNKNOWN rather than whichever way the
	 * noise fell: a phone on the frontier is genuinely ambiguous and must
	 * read that way. The triangle gate above stays on the RAW pair; the
	 * bias moves opinion, never what counts as physically possible.
	 */
	eff = diff + cfg->boundary_bias_mm;
	aeff = eff < 0 ? -eff : eff;
	if (aeff <= cfg->deadband_mm) {
		v.side = ULTRAWIDELOCK_SIDE_UNKNOWN;
	} else if (eff < 0) {
		v.side = ULTRAWIDELOCK_SIDE_INSIDE; /* nearer the inside anchor */
	} else {
		v.side = ULTRAWIDELOCK_SIDE_OUTSIDE;
	}
	return v;
}

bool ultrawidelock_fusion_may_predict(const struct ultrawidelock_fusion_verdict *v)
{
	if (v == NULL) {
		return true; /* no verdict at all is the same as no satellite */
	}
	if (!v->geometry_ok) {
		return false;
	}
	return v->side != ULTRAWIDELOCK_SIDE_OUTSIDE;
}
