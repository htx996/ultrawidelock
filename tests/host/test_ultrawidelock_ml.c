/**
 * @file test_ultrawidelock_ml.c — certify the shipped LOS/NLOS tree against sklearn.
 *
 * This is not a regression pin. `ultrawidelock_ml_los_vec_expect` is what scikit-learn
 * itself answered for each vector, so a pass means the C on the target agrees
 * with the model that was trained, not merely with its own past behaviour.
 * gen_model.py (tinyml repo) refuses to emit the model unless that agreement is
 * exact over all 6,300 held-out samples; these vectors carry the claim into CI,
 * stratified so every one of the pruned tree's leaves is reached.
 *
 * Only a single tree can be tested this way. A random forest's generated C
 * disagrees with sklearn on 0.92% of samples at emlearn's defaults and 0.06%
 * with leaf_bits=8, which selects soft voting instead of majority voting. 0.06%
 * is still not the exact agreement this file demands, so a forest still has no
 * oracle -- but it misses by 4 samples in 6,300 rather than 58, and the cause is
 * 8-bit leaf quantisation rather than the architecture.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h> /* NULL, for the rejected-destination check */
#include <stdint.h>

#include "test.h"
#include "ultrawidelock_ml.h"
#include "ultrawidelock_ml_log2.h"

#include "data/ultrawidelock_ml_los_lin_vectors.h"
#include "data/ultrawidelock_ml_los_vectors.h"

void test_ultrawidelock_ml(void)
{
	t_group("ultrawidelock_ml: tree vs sklearn");

	/* The generated feature contract must not drift from the vectors: if the
	 * model is regenerated with a different feature set and only one of the
	 * two files lands, every comparison below would still "pass" against the
	 * wrong columns. */
	T_EQ("feature count matches the vectors",
	     ULTRAWIDELOCK_ML_LOS_VEC_FEATURES, ULTRAWIDELOCK_ML_LOS_N_FEATURES);

	int mismatch = 0;
	int leaves_clear = 0, leaves_obstructed = 0;

	for (int i = 0; i < ULTRAWIDELOCK_ML_LOS_VEC_COUNT; i++) {
		const enum ultrawidelock_ml_los_class got =
			ultrawidelock_ml_los_classify(ultrawidelock_ml_los_vec_in[i]);

		if ((int)got != (int)ultrawidelock_ml_los_vec_expect[i]) {
			mismatch++;
		}
		if (got == ULTRAWIDELOCK_ML_LOS_CLEAR) {
			leaves_clear++;
		} else {
			leaves_obstructed++;
		}
	}

	T_EQ("all vectors match sklearn", mismatch, 0);

	/* A tree that answered one class for everything would satisfy the loop
	 * above if the vectors happened to be single-class. They are not, and
	 * this is what says so. */
	T_OK("vectors reach both classes", leaves_clear > 0 && leaves_obstructed > 0);

	t_group("ultrawidelock_ml: vendor rule and disagreement");

	/* Decawave APS006's threshold is 6 dB, strictly greater. Pin the boundary
	 * rather than a comfortable value on either side of it. */
	T_EQ("5.9 dB is clear", ultrawidelock_ml_los_vendor(5.9f), ULTRAWIDELOCK_ML_LOS_CLEAR);
	T_EQ("exactly 6 dB is clear", ultrawidelock_ml_los_vendor(6.0f), ULTRAWIDELOCK_ML_LOS_CLEAR);
	T_EQ("6.1 dB is obstructed", ultrawidelock_ml_los_vendor(6.1f), ULTRAWIDELOCK_ML_LOS_OBSTRUCTED);

	/* The disagreement rate is the drift monitor the classifier ships with,
	 * so it has to be non-trivial: if the tree and the vendor rule always
	 * agreed, counting disagreements would monitor nothing. On the public set
	 * they differ on roughly a fifth of samples. */
	int disagree = 0;

	for (int i = 0; i < ULTRAWIDELOCK_ML_LOS_VEC_COUNT; i++) {
		/* pwr_diff is no longer in the feature vector, so sweep the vendor
		 * rule across its own threshold instead of feeding it a column that
		 * does not exist: alternating sides guarantees both verdicts appear. */
		if (ultrawidelock_ml_los_disagrees(ultrawidelock_ml_los_vec_in[i], (i % 2) ? 9.0f : 3.0f)) {
			disagree++;
		}
	}

	T_OK("tree and vendor rule disagree sometimes", disagree > 0);
	T_OK("but not always", disagree < ULTRAWIDELOCK_ML_LOS_VEC_COUNT);

	/* The range correction is the one piece of the feature extractor that ships,
	 * and it replaces log10f with a table, so pin it against values computed
	 * off-target. 20*log10(d/100) is 0 dB at 1 m, +6 at 2 m, -20 at 10 cm.
	 * gen_model.py gate 3 holds the whole curve to 0.183 dB; these are the
	 * anchors a reader of this file can check by hand. */
	T_OK("range correction is 0 dB at 1 m",
	     fabsf(ultrawidelock_ml_los_range_correction(100) - 0.0f) < 0.2f);
	T_OK("range correction is +6.02 dB at 2 m",
	     fabsf(ultrawidelock_ml_los_range_correction(200) - 6.0206f) < 0.2f);
	T_OK("range correction is -20 dB at 10 cm",
	     fabsf(ultrawidelock_ml_los_range_correction(10) + 20.0f) < 0.2f);
	T_OK("range correction clamps below 10 cm rather than diverging",
	     ultrawidelock_ml_los_range_correction(0) == ultrawidelock_ml_los_range_correction(10));
	T_OK("range correction is monotonic across the reader's whole span", ({
		     bool up = true;
		     for (uint16_t d = 11; d < 2000; d++) {
			     if (ultrawidelock_ml_los_range_correction(d) <
				 ultrawidelock_ml_los_range_correction((uint16_t)(d - 1))) {
				     up = false;
				     break;
			     }
		     }
		     up;
	     }));
	T_OK("fp_resid is fp_pwr plus the correction",
	     fabsf(ultrawidelock_ml_los_fp_resid(-80.0f, 200) - (-80.0f + 6.0206f)) < 0.2f);

	/* Clamping: a feature far outside the training range must saturate rather
	 * than wrap into the opposite end of the int16 space, which would turn an
	 * out-of-range measurement into a confident wrong class instead of a
	 * saturated one. Both extremes must simply return a valid class. */
	float extreme[ULTRAWIDELOCK_ML_LOS_N_FEATURES];

	for (int i = 0; i < ULTRAWIDELOCK_ML_LOS_N_FEATURES; i++) {
		extreme[i] = 1e30f;
	}
	T_OK("huge features saturate to a valid class",
	     ultrawidelock_ml_los_classify(extreme) == ULTRAWIDELOCK_ML_LOS_CLEAR ||
	     ultrawidelock_ml_los_classify(extreme) == ULTRAWIDELOCK_ML_LOS_OBSTRUCTED);

	for (int i = 0; i < ULTRAWIDELOCK_ML_LOS_N_FEATURES; i++) {
		extreme[i] = -1e30f;
	}
	T_OK("hugely negative features saturate to a valid class",
	     ultrawidelock_ml_los_classify(extreme) == ULTRAWIDELOCK_ML_LOS_CLEAR ||
	     ultrawidelock_ml_los_classify(extreme) == ULTRAWIDELOCK_ML_LOS_OBSTRUCTED);

	for (int i = 0; i < ULTRAWIDELOCK_ML_LOS_N_FEATURES; i++) {
		extreme[i] = 0.0f;
	}
	T_OK("all-zero features return a valid class",
	     ultrawidelock_ml_los_classify(extreme) == ULTRAWIDELOCK_ML_LOS_CLEAR ||
	     ultrawidelock_ml_los_classify(extreme) == ULTRAWIDELOCK_ML_LOS_OBSTRUCTED);

	t_group("ultrawidelock_ml: integer log2");

	/* Exact powers of two are the mantissa-is-zero path, where the table is
	 * consulted at index 0 and must contribute nothing. An error here would be a
	 * constant offset on every feature rather than a rounding wobble. */
	T_OK("log2(1) is 0", fabsf(ultrawidelock_ml_log2_u64(1) - 0.0f) < 1e-6f);
	T_OK("log2(2) is 1", fabsf(ultrawidelock_ml_log2_u64(2) - 1.0f) < 1e-6f);
	T_OK("log2(1024) is 10", fabsf(ultrawidelock_ml_log2_u64(1024) - 10.0f) < 1e-6f);

	/* Past 32 bits, which is the whole reason this is not the u32 version it
	 * replaced: rx_pwr's numerator is a 17-bit area shifted left by 17. */
	T_OK("log2(2^40) is 40", fabsf(ultrawidelock_ml_log2_u64(1ull << 40) - 40.0f) < 1e-6f);
	T_OK("log2(2^63) is 63", fabsf(ultrawidelock_ml_log2_u64(1ull << 63) - 63.0f) < 1e-6f);

	/* Zero is defined rather than undefined. Callers reject it first, but a
	 * shift by 64 is undefined behaviour and this is what says the guard is
	 * still there. */
	T_OK("log2(0) is 0 rather than undefined", ultrawidelock_ml_log2_u64(0) == 0.0f);

	T_OK("log2 is monotonic over four decades", ({
		     bool up = true;
		     for (uint64_t v = 2; v < 100000; v = v + 1 + v / 64) {
			     if (ultrawidelock_ml_log2_u64(v) <= ultrawidelock_ml_log2_u64(v - 1)) {
				     up = false;
				     break;
			     }
		     }
		     up;
	     }));

	t_group("ultrawidelock_ml: feature extraction from CIA registers");

	/* One synthetic reception at realistic magnitudes. The expected values are
	 * what parse_alab.py's float64 formulas give for these registers:
	 *
	 *   num    = 11000^2 + 9000^2 + 7000^2 = 251,000,000
	 *   fp_pwr = 10*log10(251e6 / 48^2)      - 121.74 = -71.3681 dB
	 *   rx_pwr = 10*log10(13 * 2^17 / 48^2)  - 121.74 = -93.0503 dB
	 *
	 * gen_model.py's gate 4 holds the same arithmetic against all 544 captured
	 * receptions; these are the anchors a reader can check with a calculator.
	 * The tolerance is 0.01 dB against a measured worst case of 0.0004. */
	struct ultrawidelock_ml_cia cia = {
		.f1 = 11000, .f2 = 9000, .f3 = 7000, .accum_count = 48, .channel_area = 13,
	};
	float feat[ULTRAWIDELOCK_ML_LOS_N_FEATURES];
	float pwr_diff = 0.0f;

	T_OK("a good reception is accepted", ultrawidelock_ml_los_features(&cia, 100, feat, &pwr_diff));
	T_OK("rx_pwr matches the float64 formula",
	     fabsf(feat[ULTRAWIDELOCK_ML_LOS_F_RX_PWR] - (-93.0503f)) < 0.01f);
	T_OK("pwr_diff is rx_pwr minus fp_pwr",
	     fabsf(pwr_diff - (-93.0503f - -71.3681f)) < 0.01f);

	/* At exactly 1 m the correction is zero, so fp_resid must equal fp_pwr.
	 * That is what makes this a check on the extractor rather than on the
	 * correction, which has its own pins above. */
	T_OK("fp_resid equals fp_pwr at 1 m",
	     fabsf(feat[ULTRAWIDELOCK_ML_LOS_F_FP_RESID] - (-71.3681f)) < 0.01f);

	/* And moves with range by exactly the correction, in the direction that
	 * makes a distant reception look stronger once spreading is removed. */
	float feat_2m[ULTRAWIDELOCK_ML_LOS_N_FEATURES];

	T_OK("2 m is accepted too", ultrawidelock_ml_los_features(&cia, 200, feat_2m, NULL));
	T_OK("fp_resid at 2 m is 6.02 dB above 1 m",
	     fabsf((feat_2m[ULTRAWIDELOCK_ML_LOS_F_FP_RESID] -
		    feat[ULTRAWIDELOCK_ML_LOS_F_FP_RESID]) -
		   6.0206f) < 0.01f);
	T_OK("rx_pwr does not move with range",
	     feat_2m[ULTRAWIDELOCK_ML_LOS_F_RX_PWR] == feat[ULTRAWIDELOCK_ML_LOS_F_RX_PWR]);

	/* NULL is a supported argument, not merely one that happens not to crash:
	 * a caller that only wants the class should not have to invent a float. */
	T_OK("pwr_diff may be NULL", ultrawidelock_ml_los_features(&cia, 100, feat, NULL));

	/* A failed CIA read must be REJECTED, not classified. Left in, the logarithm
	 * turns a zeroed register into a -120 dB outlier that the tree will happily
	 * assign a class to. Every zero has to be caught, so test each one alone
	 * rather than all three at once. */
	struct ultrawidelock_ml_cia bad = cia;

	bad.accum_count = 0;
	T_OK("zero accumulator count is rejected", !ultrawidelock_ml_los_features(&bad, 100, feat, NULL));

	bad = cia;
	bad.channel_area = 0;
	T_OK("zero channel area is rejected", !ultrawidelock_ml_los_features(&bad, 100, feat, NULL));

	bad = cia;
	bad.f1 = bad.f2 = bad.f3 = 0;
	T_OK("all-zero F1..F3 is rejected", !ultrawidelock_ml_los_features(&bad, 100, feat, NULL));

	/* But one zero among F1..F3 is a real measurement, not a failed read: the
	 * sum of squares is what has to be non-zero. Rejecting these would silently
	 * drop weak first paths, which are exactly the obstructed ones. */
	bad = cia;
	bad.f3 = 0;
	T_OK("a single zero among F1..F3 is still a valid reception",
	     ultrawidelock_ml_los_features(&bad, 100, feat, NULL));

	/* The extractor's output must be usable by the classifier without any
	 * further handling. This is the whole point of the pair existing. */
	T_OK("extracted features classify to a valid class", ({
		     ultrawidelock_ml_los_features(&cia, 150, feat, &pwr_diff);
		     const enum ultrawidelock_ml_los_class c = ultrawidelock_ml_los_classify(feat);
		     c == ULTRAWIDELOCK_ML_LOS_CLEAR || c == ULTRAWIDELOCK_ML_LOS_OBSTRUCTED;
	     }));

	t_group("ultrawidelock_ml: training columns (ultrawidelock_ml_los_diag)");

	/* The same synthetic reception as above, with a peak amplitude added, so
	 * fp_pwr and rx_pwr can be checked against the SAME two anchors the feature
	 * extractor is held to -- if the two entry points ever compute the first
	 * path's energy differently, these numbers move apart.
	 *
	 *   num        = 251,000,000 (from f1/f2/f3 above)
	 *   fp_pwr     = -71.3681 dB
	 *   rx_pwr     = -93.0503 dB
	 *   delta_p    = rx_pwr - fp_pwr           = -21.6822 dB
	 *   fp_peak    = 10*log10(251e6 / 20000^2) =  -2.0239 dB
	 *
	 * Computed in float64 off-target, like the anchors above; the tolerance is
	 * the same 0.01 dB. */
	struct ultrawidelock_ml_cia dcia = cia;
	struct ultrawidelock_ml_diag dg;

	dcia.peak_amp = 20000;

	T_OK("a good reception yields training columns", ultrawidelock_ml_los_diag(&dcia, &dg));
	T_OK("fp_pwr matches the float64 formula", fabsf(dg.fp_pwr_db - (-71.3681f)) < 0.01f);
	T_OK("rx_pwr matches the float64 formula", fabsf(dg.rx_pwr_db - (-93.0503f)) < 0.01f);
	T_OK("delta_p is rx_pwr minus fp_pwr", fabsf(dg.delta_p_db - (-21.6822f)) < 0.01f);
	T_OK("fp_peak is the first-path energy over the squared peak amplitude",
	     fabsf(dg.fp_peak_db - (-2.0239f)) < 0.01f);

	/* The two entry points must agree EXACTLY, not merely to a tolerance: they
	 * are the same arithmetic on the same registers, so any difference is a
	 * second implementation that has appeared, and a capture fitted through one
	 * would not describe the classifier fed by the other. */
	float dfeat[ULTRAWIDELOCK_ML_LOS_N_FEATURES];
	float dpwr = 0.0f;

	T_OK("features accept the same reception",
	     ultrawidelock_ml_los_features(&dcia, 100, dfeat, &dpwr));
	T_OK("diag rx_pwr is bit-identical to the feature vector's",
	     dg.rx_pwr_db == dfeat[ULTRAWIDELOCK_ML_LOS_F_RX_PWR]);
	T_OK("diag delta_p is bit-identical to the extractor's pwr_diff",
	     dg.delta_p_db == dpwr);

	/* peak_amp is NOT a model input, and this is what says so: the shipped
	 * classifier must answer the same with it set, unset, or wrong. Without
	 * this, adding the field to struct ultrawidelock_ml_cia could quietly move an
	 * unlock decision. */
	struct ultrawidelock_ml_cia nopeak = dcia;
	float nofeat[ULTRAWIDELOCK_ML_LOS_N_FEATURES];

	nopeak.peak_amp = 0;
	T_OK("features ignore peak_amp entirely", ({
		     ultrawidelock_ml_los_features(&nopeak, 100, nofeat, NULL);
		     nofeat[ULTRAWIDELOCK_ML_LOS_F_FP_RESID] ==
			     dfeat[ULTRAWIDELOCK_ML_LOS_F_FP_RESID] &&
		     nofeat[ULTRAWIDELOCK_ML_LOS_F_RX_PWR] ==
			     dfeat[ULTRAWIDELOCK_ML_LOS_F_RX_PWR];
	     }));

	/* SIGN IS THE DISCRIMINANT, so pin both sides of zero rather than one
	 * comfortable value. With the peak equal to the largest of F1..F3 the first
	 * path IS the strongest arrival and the ratio sits just above 0 dB, bounded
	 * by 10*log10(3) = 4.7712 for three equal taps; a peak far above the first
	 * path drives it negative, which is what an obstruction looks like. */
	struct ultrawidelock_ml_cia atpeak = dcia;

	atpeak.peak_amp = 11000; /* == f1, the largest first-path tap */
	T_OK("first path at the peak gives a small positive ratio", ({
		     ultrawidelock_ml_los_diag(&atpeak, &dg);
		     fabsf(dg.fp_peak_db - 3.1689f) < 0.01f;
	     }));
	T_OK("and never exceeds the three-equal-taps ceiling", dg.fp_peak_db < 4.7712f);

	struct ultrawidelock_ml_cia farpeak = dcia;

	farpeak.peak_amp = 200000; /* an arrival 20x the first path's amplitude */
	T_OK("a first path far below the peak gives a negative ratio", ({
		     ultrawidelock_ml_los_diag(&farpeak, &dg);
		     dg.fp_peak_db < 0.0f;
	     }));

	/* Rejections. The first three are the extractor's, so a row can never
	 * appear in a capture that the classifier would have dropped; the fourth is
	 * this function's own, and it is what catches a caller passing ipatovPeak
	 * unmasked -- which would otherwise produce a ratio 2^21 too large that
	 * still reads as a plausible dB number. */
	struct ultrawidelock_ml_diag untouched = dg;
	struct ultrawidelock_ml_cia dbad = dcia;

	dbad.peak_amp = 0;
	T_OK("zero peak amplitude is rejected", !ultrawidelock_ml_los_diag(&dbad, &dg));
	T_OK("and nothing is written when it is",
	     dg.fp_peak_db == untouched.fp_peak_db && dg.rx_pwr_db == untouched.rx_pwr_db);

	dbad = dcia;
	dbad.accum_count = 0;
	T_OK("zero accumulator count is rejected here too", !ultrawidelock_ml_los_diag(&dbad, &dg));

	dbad = dcia;
	dbad.channel_area = 0;
	T_OK("zero channel area is rejected here too", !ultrawidelock_ml_los_diag(&dbad, &dg));

	dbad = dcia;
	dbad.f1 = dbad.f2 = dbad.f3 = 0;
	T_OK("all-zero F1..F3 is rejected here too", !ultrawidelock_ml_los_diag(&dbad, &dg));

	T_OK("a NULL destination is rejected rather than dereferenced",
	     !ultrawidelock_ml_los_diag(&dcia, NULL));

	t_group("ultrawidelock_ml: confidence");

	/* Compared with a tolerance rather than for equality: a host may contract
	 * the multiply-add into an FMA where the Cortex-M4F does not. 1e-3 is far
	 * below anything a confidence threshold could care about, and the checks
	 * that carry real weight are the structural ones below. */
	int conf_mismatch = 0;
	int negative = 0;

	for (size_t i = 0; i < sizeof(ultrawidelock_ml_los_lin_vectors) /
				 sizeof(ultrawidelock_ml_los_lin_vectors[0]); i++) {
		const float got = ultrawidelock_ml_los_confidence(ultrawidelock_ml_los_lin_vectors[i].feat);

		if (fabsf(got - ultrawidelock_ml_los_lin_vectors[i].confidence) > 1e-3f) {
			conf_mismatch++;
		}
		if (got < 0.0f) {
			negative++;
		}
	}

	T_EQ("all vectors match the fitted boundary", conf_mismatch, 0);

	/* The return is a magnitude by contract, and a caller thresholding it
	 * would silently accept everything if a sign ever leaked through. */
	T_EQ("confidence is never negative", negative, 0);

	/*
	 * The structure that makes it a confidence rather than a number: it is a
	 * distance from a boundary, so sweeping one feature across that boundary
	 * has to fall to a minimum and rise again. A monotone sweep would mean the
	 * sign leaked; a flat one would mean the coefficients did not land.
	 *
	 * rx_pwr is held at a value the captures actually contain; fp_resid is
	 * swept because it is the stronger of the two features.
	 */
	float prev = ultrawidelock_ml_los_confidence((const float[]){-40.0f, -93.0f});
	int falls = 0, rises = 0;

	for (float fp = -42.0f; fp >= -160.0f; fp -= 2.0f) {
		const float now = ultrawidelock_ml_los_confidence((const float[]){fp, -93.0f});

		if (now < prev) {
			falls++;
		} else if (now > prev) {
			rises++;
		}
		prev = now;
	}

	T_OK("confidence falls towards the boundary and rises past it",
	     falls > 0 && rises > 0);

	/* And the minimum has to be a real crossing rather than a shallow dip,
	 * otherwise "distance from the boundary" is not what is being returned. */
	float lowest = ultrawidelock_ml_los_confidence((const float[]){-40.0f, -93.0f});

	for (float fp = -42.0f; fp >= -160.0f; fp -= 2.0f) {
		const float now = ultrawidelock_ml_los_confidence((const float[]){fp, -93.0f});

		if (now < lowest) {
			lowest = now;
		}
	}

	T_OK("the sweep passes within 1 dB of the boundary", lowest < 1.0f);

	t_group("ultrawidelock_ml: range correction");

	/*
	 * The four runs of RESULTS.md Result 19, as the reader reported them. These
	 * are the measurement, so they are what the constants have to reproduce: a
	 * regression pin here would only say the arithmetic has not changed, which
	 * is not the claim being made.
	 */
	T_EQ("100 cm clear: 75 -> 101",
	     ultrawidelock_ml_los_range_true_cm(75, ULTRAWIDELOCK_ML_LOS_CLEAR), 101);
	T_EQ("200 cm clear: 174 -> 200",
	     ultrawidelock_ml_los_range_true_cm(174, ULTRAWIDELOCK_ML_LOS_CLEAR), 200);
	T_EQ("100 cm blocked: 155 -> 96",
	     ultrawidelock_ml_los_range_true_cm(155, ULTRAWIDELOCK_ML_LOS_OBSTRUCTED), 96);
	T_EQ("200 cm blocked: 263 -> 204",
	     ultrawidelock_ml_los_range_true_cm(263, ULTRAWIDELOCK_ML_LOS_OBSTRUCTED), 204);

	/* The correction is an offset, so it must not change with range: the same
	 * input gap has to survive. This is the property Result 19 tested against a
	 * scale factor and is the one a future refit could silently break. */
	T_EQ("clear correction is range-independent",
	     ultrawidelock_ml_los_range_true_cm(500, ULTRAWIDELOCK_ML_LOS_CLEAR) -
		     ultrawidelock_ml_los_range_true_cm(100, ULTRAWIDELOCK_ML_LOS_CLEAR),
	     400);

	/* Obstructed always reads further, so correcting it always moves the range
	 * DOWN. A sign error here would make the lock less eager exactly when the
	 * owner is having trouble, which is the opposite of the point. */
	T_OK("obstructed corrects downward relative to clear",
	     ultrawidelock_ml_los_range_true_cm(200, ULTRAWIDELOCK_ML_LOS_OBSTRUCTED) <
		     ultrawidelock_ml_los_range_true_cm(200, ULTRAWIDELOCK_ML_LOS_CLEAR));

	/* Below the bias, the correction has no business being applied and must not
	 * wrap: every caller compares against an unsigned threshold, so a wrap would
	 * read as very far away and silently refuse to unlock. */
	T_EQ("an obstructed reading below the bias clamps at 0",
	     ultrawidelock_ml_los_range_true_cm(10, ULTRAWIDELOCK_ML_LOS_OBSTRUCTED), 0);
	T_EQ("and exactly at the bias it is 0",
	     ultrawidelock_ml_los_range_true_cm(59, ULTRAWIDELOCK_ML_LOS_OBSTRUCTED), 0);
	T_OK("just above the bias it is positive",
	     ultrawidelock_ml_los_range_true_cm(61, ULTRAWIDELOCK_ML_LOS_OBSTRUCTED) > 0);

	/* The largest range a reader reports must not overflow the uint16 return. */
	T_OK("the top of the reader's span does not wrap",
	     ultrawidelock_ml_los_range_true_cm(65000, ULTRAWIDELOCK_ML_LOS_CLEAR) > 65000);
}
