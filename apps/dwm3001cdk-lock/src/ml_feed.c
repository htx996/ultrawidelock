/* SPDX-License-Identifier: ISC */

/*
 * ml_feed.c — the channel-classifier glue between the CIA latch and the
 * approach controller. Split out of main.c so the grant loop reads as policy
 * and this file carries the measurement mechanics.
 */
#include "ml_feed.h"

#include "uwb_cirdiag.h" /* latched Ipatov scalars, for the channel classifier */

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
#include "ultrawidelock_ml.h"
#include "ultrawidelock_log.h"  /* ultrawidelock_printf -- the [ALAB] ev=ml classifier trace */
#include "ultrawidelock_port.h" /* ultrawidelock_uptime_us -- the [ALAB] timebase */
#endif

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS) && defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG)

/**
 * Centi-units: every dB column on the [FEAT] line is the value times 100, as an
 * integer.
 *
 * NOT A STYLE CHOICE. Printing a float here would need
 * CONFIG_CBPRINTF_FP_SUPPORT, which is a global switch that grows every printf
 * in the image, on a board whose only console is an 8 KB RTT buffer in
 * NO_BLOCK_SKIP mode -- the same buffer the CIR drain already has to be paced
 * against. The `ev=ml` line above has printed conf in centi-units since it was
 * written, for the same reason, and the host divides by 100.
 *
 * Truncates toward zero rather than rounding, so -71.368 dB prints as -7136 and
 * not -7137. 0.01 dB of quantisation against a log table whose own worst error
 * is 0.0086 dB, on registers the offline fit re-derives from the raw columns on
 * the same line anyway.
 */
#define FEAT_CENTI(x) ((int)((x) * 100.0f))

/**
 * One CSV row per ranging block, prefixed so a capture can be sieved out of the
 * console with a plain grep.
 *
 * THE COLUMN LIST IS THE CONTRACT, and the header row is emitted once, from
 * here, rather than written down in the host script -- a schema kept in two
 * places is a schema that silently gains a column on one side. The header is
 * distinguishable from data by being non-numeric, so a reader takes the last
 * one it saw and does not have to care that a board reset repeats it.
 *
 * RAW REGISTERS RIDE ALONG WITH THE DERIVED COLUMNS on purpose. Everything from
 * f1 rightwards is what the chip reported, so a fit that wants a column this
 * firmware does not compute -- a noise estimate above all, which the DW3000's
 * diagnostic bank does not expose (see struct ultrawidelock_ml_diag) -- can derive
 * it offline without a new image and a second walk-up.
 *
 * NO SLOPE, NO JITTER, AND NO RING BUFFER. Both are functions of a window of
 * rows, t_ms and cm are on every row, and computing them here would mean
 * choosing a window length now, in firmware, for a model that has not been
 * fitted yet. Offline the window is a parameter; on-device it is a flash cycle.
 */
static void ml_feed_csv(const struct uwb_cirdiag_ipatov *ip, int32_t cm,
			const float feat[ULTRAWIDELOCK_ML_LOS_N_FEATURES],
			const struct ultrawidelock_ml_diag *dg,
			enum ultrawidelock_ml_los_class cls, float conf, bool dis)
{
	static bool header_done;

	if (!header_done) {
		header_done = true;
		ultrawidelock_printf(
			"[FEAT] t_ms,n,cm,cls,conf_c,dis,fp_resid_c,rx_pwr_c,fp_pwr_c,"
			"delta_p_c,fp_peak_c,fp_idx_q6,peak_idx,peak_amp,f1,f2,f3,area,acc\n");
	}

	ultrawidelock_printf("[FEAT] %lld,%u,%d,%u,%d,%u,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u\n",
		   (long long)(ultrawidelock_uptime_us() / 1000), (unsigned)ip->n, (int)cm,
		   (unsigned)cls, FEAT_CENTI(conf), (unsigned)dis,
		   FEAT_CENTI(feat[ULTRAWIDELOCK_ML_LOS_F_FP_RESID]),
		   FEAT_CENTI(feat[ULTRAWIDELOCK_ML_LOS_F_RX_PWR]), FEAT_CENTI(dg->fp_pwr_db),
		   FEAT_CENTI(dg->delta_p_db), FEAT_CENTI(dg->fp_peak_db),
		   (unsigned)ip->fp_index, (unsigned)UWB_CIRDIAG_PEAK_IDX(ip->peak),
		   (unsigned)UWB_CIRDIAG_PEAK_AMP(ip->peak), (unsigned)ip->f1, (unsigned)ip->f2,
		   (unsigned)ip->f3, (unsigned)ip->power, (unsigned)ip->accum_count);
}

#endif /* CONFIG_ULTRAWIDELOCK_ML_LOS && CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG */

/**
 * Feed one trusted range, carrying this reception's channel class if there is one.
 *
 * WHERE THIS RUNS, because it is the only reason it is affordable. The classifier
 * needs dwt_readdiagnostics(), measured at 972 us on this board -- 53% of the
 * ~1836 us ranging arm deadline, which would be reckless on the RX path. It is
 * not on the RX path. uwb_cirdiag_capture() already takes that read AFTER the
 * shim re-arms, and this function only copies the result out in the main loop,
 * one ranging block (~192 ms) later. The work added here is five register copies,
 * three logarithms and two comparisons.
 *
 * The channel is read only when the capture counter has ADVANCED. The latch is
 * latest-wins with no queue, so a stale snapshot re-read across several ranging
 * rounds would let one obstructed reception carry a whole median window and
 * defeat the majority-of-five that gates the widening.
 *
 * Falls back to the plain feed whenever anything is missing -- stream disarmed,
 * nothing captured, a failed CIA read, or the classifier compiled out. A missing
 * class must read as CLEAR rather than as obstructed: clear is the unwidened
 * threshold, which is the behaviour that shipped.
 */
enum ultrawidelock_approach_action ml_feed_range(struct ultrawidelock_approach *ap, int64_t now,
					 int32_t cm)
{
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS) && defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG)
	static uint32_t last_diag_n;
	struct uwb_cirdiag_ipatov ip;

	if (cm >= 0 && uwb_cirdiag_last_ipatov(&ip) && ip.n != last_diag_n) {
		const struct ultrawidelock_ml_cia cia = {
			.f1 = ip.f1,
			.f2 = ip.f2,
			.f3 = ip.f3,
			.accum_count = ip.accum_count,
			.channel_area = ip.power,
			/* Read only by ultrawidelock_ml_los_diag(); ultrawidelock_ml_los_features()
			 * does not look at it, so the class this function feeds the
			 * approach controller is the same with it as without. */
			.peak_amp = UWB_CIRDIAG_PEAK_AMP(ip.peak),
		};
		float feat[ULTRAWIDELOCK_ML_LOS_N_FEATURES];
		float pwr_diff;

		last_diag_n = ip.n;
		if (ultrawidelock_ml_los_features(&cia, (uint16_t)cm, feat, &pwr_diff)) {
			/* Carry first, binary folded out of it, so the two can never
			 * disagree about the same reception. The fold is exact --
			 * CLEAR is clear, everything else obstructed -- so cls is the
			 * value ultrawidelock_ml_los_classify() would have returned and the
			 * ev=ml trace line is unchanged. */
			const enum ultrawidelock_ml_carry_class carry =
				ultrawidelock_ml_los_carry_classify(feat);
			const enum ultrawidelock_ml_los_class cls =
				ultrawidelock_ml_carry_to_los(carry);
			const float conf = ultrawidelock_ml_los_confidence(feat);
			const bool dis = ultrawidelock_ml_los_disagrees(feat, pwr_diff);
			struct ultrawidelock_ml_diag dg;

			/*
			 * One line per fresh latch, joinable to its ev=uwb.diag line by
			 * n=. conf_c is the dB-scaled confidence in centi-units, so the
			 * 2.61 vote gate reads as 261; dis is the tree-vs-vendor
			 * disagreement whose RATE is the label-free drift monitor
			 * ultrawidelock_ml_los_vendor() documents. Main-loop context, one ranging
			 * block after the reception, so this competes with no deadline.
			 *
			 * carry is the same call's carry mode and carry_t says
			 * whether a four-class model produced it -- with the shipped
			 * two-class tree carry only ever reads 0 or 1 and carry_t is
			 * 0, so a walk log makes clear which resolution it holds.
			 */
			ultrawidelock_printf(
				"[ALAB] t=%lld ev=ml n=%u cm=%d cls=%u conf_c=%d dis=%u carry=%u carry_t=%u\n",
				ultrawidelock_uptime_us(), ip.n, cm, (unsigned)cls,
				(int)(conf * 100.0f), (unsigned)dis, (unsigned)carry,
				(unsigned)ultrawidelock_ml_los_carry_trained());
			/* The training row, AFTER the classifier trace and before the feed,
			 * so a [FEAT] line exists for exactly the receptions the ev=ml line
			 * reports and the two join on n. Skipped silently when the extra
			 * columns are unavailable -- a zero peak is a failed read, and a row
			 * of zeroes in a fitted set is worse than a missing row. The feed
			 * below is reached either way: this returns nothing to the decision. */
			if (ultrawidelock_ml_los_diag(&cia, &dg)) {
				ml_feed_csv(&ip, cm, feat, &dg, cls, conf, dis);
			}
			return ultrawidelock_approach_feed_carry(
				ap, now, cm, (enum ultrawidelock_approach_carry)carry, conf);
		}
	}
#endif
	return ultrawidelock_approach_feed(ap, now, cm);
}

/**
 * The debounced verdict, printed on the edge only. This is the state
 * the widening consumes, so a walk with nlos_widen_cm still 0 shows
 * exactly where a widened build would have moved its threshold --
 * which is the reading that chooses the number.
 */
void ml_feed_vote_trace(struct ultrawidelock_approach *ap, int64_t now)
{
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS) && defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG)
	static bool was_blocked;
	const bool blocked = ultrawidelock_approach_nlos_blocked(ap, now);

	if (blocked != was_blocked) {
		was_blocked = blocked;
		ultrawidelock_printf("[ALAB] t=%lld ev=ml.vote blocked=%u\n",
			   ultrawidelock_uptime_us(), (unsigned)blocked);
	}
#else
	(void)ap;
	(void)now;
#endif
}
