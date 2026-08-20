/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_ml_feat.c — five CIA registers and a range, turned into the model's input.
 *
 * parse_alab.py (tinyml repo) is the definition, reproduced register for
 * register because the model was fitted through exactly this arithmetic:
 *
 *     num    = F1^2 + F2^2 + F3^2
 *     fp_pwr = 10*log10(num / C^2)          - A
 *     rx_pwr = 10*log10(area * 2^17 / C^2)  - A
 *
 * C = ipatovAccumCount, area = ipatovPower (17-bit; the 2^17 undoes DW3000
 * scaling absent on the DW1000). A is eWINE's PRF-64 constant, not a
 * calibration: it cancels out of pwr_diff and the training absorbed it --
 * changing it invalidates the model. Zero C or area is a failed CIA read, not a
 * weak signal (real CPER-set receptions report ipatovPower = 0; through the log
 * they become -120 dB outliers), so those receptions return false.
 *
 * ultrawidelock_ml_los_diag() at the bottom shares that arithmetic and adds
 * fp_peak_db, but writes only to its own struct: it is a capture-run column, not
 * a model input, and calling it cannot move a classification. See
 * struct ultrawidelock_ml_diag for why there is no noise floor beside it.
 */

#include <stddef.h>

#include "ultrawidelock_ml.h"
#include "ultrawidelock_ml_log2.h"

#if ULTRAWIDELOCK_ML_LOS_N_FEATURES != 2
#error "ultrawidelock_ml_feat.c fills fp_resid and rx_pwr by name, and the generated \
feature set no longer has exactly those two. Regenerating the model with a \
different SUBSET changes what a caller must supply; update this file to match \
rather than letting it fill a wrong-length vector."
#endif

/* 10*log10(x) = (10 / log2(10)) * log2(x). */
#define K_DB_PER_LOG2_10 3.01029996f

/* eWINE's PRF-64 constant. See the file header for why its value is not a knob. */
#define A_CONST_PRF64 121.74f

/* ipatovPower is a channel area scaled by 2^17, not a power. */
#define CHANNEL_AREA_SHIFT 17

/*
 * 10*log10(numerator / denom^2), with the division done as a subtraction of
 * logarithms so no float division appears and the numerator never has to be
 * representable as a float in the first place. denom is squared in the log
 * domain for the same reason: denom^2 is fine in 32 bits, but keeping it here
 * means one code path and one rounding behaviour for every caller.
 *
 * A does NOT appear here, and this is the split that makes fp_peak_db honest:
 * eWINE's constant is an offset that turns a register ratio into a power, so it
 * belongs to pwr_db()'s two callers and cancels out of a ratio of two
 * quantities in the same units. Adding it to fp_peak_db would shift a
 * dimensionless quotient by 121.74 dB and still look like a dB number.
 */
static float ratio_db(uint64_t numerator, uint32_t denom)
{
	return K_DB_PER_LOG2_10 * (ultrawidelock_ml_log2_u64(numerator) -
				   2.0f * ultrawidelock_ml_log2_u64(denom));
}

/* 10*log10(numerator / count^2) - A. See the file header for why A is not a knob. */
static float pwr_db(uint64_t numerator, uint32_t count)
{
	return ratio_db(numerator, count) - A_CONST_PRF64;
}

/* F1^2 + F2^2 + F3^2.
 *
 * 64-bit because the fields are 22-bit in the DW3000's CIA registers even though
 * dwt_rxdiag_t declares them uint32_t, so three squares reach 2^44 and overflow
 * 32 bits. Shared by both entry points so the two cannot compute the first
 * path's energy differently.
 */
static uint64_t fp_energy(const struct ultrawidelock_ml_cia *cia)
{
	return (uint64_t)cia->f1 * cia->f1 + (uint64_t)cia->f2 * cia->f2 +
	       (uint64_t)cia->f3 * cia->f3;
}

bool ultrawidelock_ml_los_features(const struct ultrawidelock_ml_cia *cia, uint16_t dist_cm,
			 float feat[ULTRAWIDELOCK_ML_LOS_N_FEATURES], float *pwr_diff_db)
{
	uint64_t num;
	float fp_pwr, rx_pwr;

	/* 64-bit throughout: see fp_energy(), and the shifted channel area reaches
	 * 2^34, which overflows 32 bits on its own. */
	num = fp_energy(cia);

	if (num == 0u || cia->accum_count == 0u || cia->channel_area == 0u) {
		return false;
	}

	fp_pwr = pwr_db(num, cia->accum_count);
	rx_pwr = pwr_db((uint64_t)cia->channel_area << CHANNEL_AREA_SHIFT, cia->accum_count);

	/* By name and not by position: the enum is generated, and a regenerated
	 * model that reorders it must not silently reorder the meaning of these. */
	feat[ULTRAWIDELOCK_ML_LOS_F_FP_RESID] = ultrawidelock_ml_los_fp_resid(fp_pwr, dist_cm);
	feat[ULTRAWIDELOCK_ML_LOS_F_RX_PWR] = rx_pwr;

	if (pwr_diff_db != NULL) {
		*pwr_diff_db = rx_pwr - fp_pwr;
	}

	return true;
}

bool ultrawidelock_ml_los_diag(const struct ultrawidelock_ml_cia *cia,
			       struct ultrawidelock_ml_diag *out)
{
	uint64_t num;
	float fp_pwr, rx_pwr;

	if (out == NULL) {
		return false;
	}

	num = fp_energy(cia);

	/* The first three rejections are ultrawidelock_ml_los_features()'s, verbatim, so
	 * a capture and a classification agree on which receptions exist at all --
	 * a row that appears in the CSV and never in the classifier's input would
	 * be fitted on a population the model can never see. peak_amp is the
	 * fourth: the peak is the largest CIR sample by construction, so a
	 * reception with any first-path energy at all cannot report zero for it,
	 * and a caller that forgot to mask ipatovPeak lands here rather than in a
	 * ratio 2^21 too large. */
	if (num == 0u || cia->accum_count == 0u || cia->channel_area == 0u ||
	    cia->peak_amp == 0u) {
		return false;
	}

	fp_pwr = pwr_db(num, cia->accum_count);
	rx_pwr = pwr_db((uint64_t)cia->channel_area << CHANNEL_AREA_SHIFT, cia->accum_count);

	out->fp_pwr_db = fp_pwr;
	out->rx_pwr_db = rx_pwr;
	out->delta_p_db = rx_pwr - fp_pwr;
	out->fp_peak_db = ratio_db(num, cia->peak_amp);

	return true;
}
