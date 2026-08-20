/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_approach.c
 * Kalman-filtered approach controller for predictive unlock. Tracks distance (cm), velocity (cm/s),
 * and estimated time-to-arrival (ms) at the unlock radius. Supervises presence via median filtering
 * of trusted ranges and fires predictive unlock when closing speed and ETA meet thresholds. Factory
 * defaults: unlock 100 cm, relock 250 cm, dwell times 2 s and 3 s, motor delay 500 ms, margin 250
 * ms, velocity floor 30 cm/s, prediction enabled.
 */
/*
 * ultrawidelock_approach — predictive approach controller. See ultrawidelock_approach.h for
 * the model; this file holds the estimator tuning.
 */
#include "ultrawidelock_approach.h"

#include <string.h>

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
#include "ultrawidelock_ml.h"

/*
 * ultrawidelock_approach.h mirrors enum ultrawidelock_ml_carry_class rather than including it,
 * because that header is pure logic with no module dependencies. This is where
 * the mirror is checked, and it has to be checked value for value: the widening
 * table is indexed by the class, so a reordered enum would not fail to compile,
 * it would quietly hand a pocket the bag's number.
 */
_Static_assert((int)ULTRAWIDELOCK_APPROACH_CARRY_CLEAR == (int)ULTRAWIDELOCK_ML_CARRY_CLEAR,
	       "carry enums must agree: CLEAR");
_Static_assert((int)ULTRAWIDELOCK_APPROACH_CARRY_HAND == (int)ULTRAWIDELOCK_ML_CARRY_HAND,
	       "carry enums must agree: HAND");
_Static_assert((int)ULTRAWIDELOCK_APPROACH_CARRY_POCKET == (int)ULTRAWIDELOCK_ML_CARRY_POCKET,
	       "carry enums must agree: POCKET");
_Static_assert((int)ULTRAWIDELOCK_APPROACH_CARRY_BAG == (int)ULTRAWIDELOCK_ML_CARRY_BAG,
	       "carry enums must agree: BAG");
_Static_assert(ULTRAWIDELOCK_APPROACH_CARRY_N == ULTRAWIDELOCK_ML_CARRY_N_CLASSES,
	       "carry enums must agree: class count");
#endif

/* Kalman tuning. Measurement noise from bench range scatter (~30 cm sigma on
 * normal blocks; the metre-scale outliers are gated, not modelled). Process
 * noise from human gait acceleration (~1.5 m/s^2). */
#define KF_R_CM2       900.0f   /* (30 cm)^2 */
#define KF_QA          22500.0f /* (150 cm/s^2)^2 */
#define KF_GATE_CM     100.0f   /* innovation gate; > max per-block walk step */
#define KF_REJECT_MAX  3        /* consecutive rejects -> re-base on the data */
#define KF_STALE_MS    1000     /* sample gap that re-bases instead of coasting */
#define KF_VINIT_CM2S2 40000.0f /* (200 cm/s)^2 initial velocity variance */
#define KF_MIN_SAMPLES 6        /* accepted samples before predictions arm */

/* Prediction guards: two consecutive in-window samples to fire; a predictive
 * open must convert to presence before its deadline or it aborts. The deadline
 * is RENEWED by every accepted sample that still shows the approach closing,
 * so the grace bounds silence and stalls rather than the whole conversion.
 * It therefore has to outlast the holes the 2026-08-07 walk measured in the
 * accepted-sample stream -- 0.9-1.5 s of dropped rounds mid-walk -- which
 * 900 ms did not: the deadline expired between two samples of a live approach
 * and the bolt toggled shut with the owner at 65 cm. The cost of 1800 is that
 * a prediction whose ranging dies outright stays open ~0.9 s longer before the
 * tick reclaims it; the cost of 900 was relocking someone mid-arrival. */
#define PRED_DWELL_N  2
#define PRED_GRACE_MS 1800

/*
 * Pocket-walk guards, all three from the 2026-08-08 00:01 walk (phone
 * pocketed, 34% RX errors, trusted samples arriving in clumps with 1-12 s
 * holes between them).
 *
 * MEDIAN_STALE_MS: window entries older than this stop voting. At 00:01:39
 * two fresh samples at 193 and 159 cm were outvoted by 9-second-old 38-88 cm
 * entries and the bolt opened with the owner at 1.6 m; the same staleness ran
 * the other way on the 23:22 walk, where old far entries delayed a real
 * arrival by two rounds. The median exists to reject metre-scale spikes
 * within a burst, not to let one burst impersonate another.
 *
 * BAND_SILENCE_MS: the silence relock for a credential last seen in the
 * (unlock, relock) DEAD BAND. The 750 ms far_silence tier is right when the
 * last evidence was >= relock_cm -- that is a departure -- but in the band it
 * relocked mid-approach at 00:01:40 during an ordinary 1.7 s pocket trust
 * hole, un-opening a door it had opened 800 ms earlier. Must exceed the
 * measured in-approach holes (0.9-2.0 s).
 *
 * Deliberately NOT here: a minimum fresh-voter count for the f >= approach_cm
 * arm. Staleness can leave a lone inflated sample as the only voter that
 * arms, but that is exactly the wlen == 1 session-start arm the ESP walk-up
 * has always shipped (its suite pins one 200 cm sample arming the gate), and
 * session_up() plus the departure-observation arm already bound what arming
 * is worth. Requiring three far samples broke the shipped contract to close
 * a hole that was never closed.
 */
#define MEDIAN_STALE_MS 2500
#define BAND_SILENCE_MS 2500

/*
 * Confident-obstructed samples needed in the median window before the NLOS
 * correction engages: a strict majority of ULTRAWIDELOCK_APPROACH_MEDIAN_N.
 *
 * CHOSEN FROM THE MEASURED PER-RECEPTION RATES, not for tidiness. The classifier
 * calls 4.7% of clear receptions obstructed and 81% of blocked ones (RESULTS.md
 * Result 19's tripod captures). Over five samples that gives:
 *
 *     >= 3 of 5   false-obstructed 1 in 1,035    engages on 94.9% of blocked windows
 *     >= 4 of 5   false-obstructed 1 in 42,588   engages on 75.8%
 *     >= 5 of 5   false-obstructed 1 in 4.4M     engages on 34.9%
 *
 * A false obstructed subtracts 84.5 cm and can open the door for a phone at a
 * true 185 cm, so the first column is a security number. The third column is the
 * pocketed owner the correction exists for, and at 5-of-5 it abandons them two
 * times in three. 3-of-5 is where both are acceptable.
 */
#define NLOS_VOTES_N ((ULTRAWIDELOCK_APPROACH_MEDIAN_N / 2) + 1)

/**
 * Reset the Kalman filter to a new state given a fresh measurement: clears rejection history,
 * zeroes velocity, reinitializes covariance, and resets prediction state.
 */
static void kf_rebase(struct ultrawidelock_approach *ap, int64_t now_ms, int32_t cm)
{
	ap->kf_init = true;
	ap->accepted = 1;
	ap->rejects = 0;
	ap->last_ms = now_ms;
	ap->d = (float)cm;
	ap->v = 0.0f;
	ap->p00 = KF_R_CM2;
	ap->p01 = 0.0f;
	ap->p11 = KF_VINIT_CM2S2;
	ap->pred_dwell = 0;
	ap->eta_ms = -1;
}

/* Time update always (time really passed), measurement update only inside the
 * innovation gate. Returns true when the sample updated the estimate. */
static bool kf_update(struct ultrawidelock_approach *ap, int64_t now_ms, int32_t cm)
{
	if (!ap->kf_init || (now_ms - ap->last_ms) > KF_STALE_MS) {
		kf_rebase(ap, now_ms, cm);
		return true;
	}

	float dt = (float)(now_ms - ap->last_ms) / 1000.0f;

	if (dt < 0.001f) {
		dt = 0.001f;
	}
	ap->last_ms = now_ms;

	/* predict: x = F x, P = F P F' + Q (constant-velocity F, accel Q) */
	ap->d += ap->v * dt;
	float dt2 = dt * dt;

	ap->p00 += dt * (ap->p01 + ap->p01) + dt2 * ap->p11 + KF_QA * dt2 * dt2 / 4.0f;
	ap->p01 += dt * ap->p11 + KF_QA * dt2 * dt / 2.0f;
	ap->p11 += KF_QA * dt2;

	float y = (float)cm - ap->d;
	float ay = y < 0.0f ? -y : y;

	if (ay > KF_GATE_CM) {
		/* Metre-scale spike: skip the measurement update. A run of
		 * agreeing "spikes" is real data (a gap, a re-appearance) —
		 * re-base on it rather than wedging the gate shut. */
		if (++ap->rejects >= KF_REJECT_MAX) {
			kf_rebase(ap, now_ms, cm);
			return true;
		}
		return false;
	}
	ap->rejects = 0;

	float s = ap->p00 + KF_R_CM2;
	float k0 = ap->p00 / s;
	float k1 = ap->p01 / s;

	ap->d += k0 * y;
	ap->v += k1 * y;
	ap->p11 -= k1 * ap->p01;
	ap->p01 -= k0 * ap->p01;
	ap->p00 -= k0 * ap->p00;
	if (ap->accepted < KF_MIN_SAMPLES) {
		ap->accepted++;
	}
	return true;
}

/* Median of the first n samples of win (n in [1, ULTRAWIDELOCK_APPROACH_MEDIAN_N]).
 * Rejects the metre-scale spikes in the per-block UWB distance without the
 * lag of a running average. */
static int32_t range_median(const int32_t *win, int n)
{
	int32_t t[ULTRAWIDELOCK_APPROACH_MEDIAN_N];

	for (int i = 0; i < n; i++) {
		t[i] = win[i];
	}
	for (int i = 1; i < n; i++) { /* insertion sort; n <= 5 */
		int32_t v = t[i];
		int j = i - 1;

		while (j >= 0 && t[j] > v) {
			t[j + 1] = t[j];
			j--;
		}
		t[j + 1] = v;
	}
	return t[n / 2];
}

/* The median over the entries still young enough to vote (MEDIAN_STALE_MS).
 * The sample just filed is age zero and always qualifies, so the guard is
 * unreachable in practice; it exists so an empty vote is structurally
 * impossible rather than argued from a caller invariant. */
static int32_t range_median_fresh(const struct ultrawidelock_approach *ap, int64_t now_ms)
{
	int32_t t[ULTRAWIDELOCK_APPROACH_MEDIAN_N];
	int n = 0;

	for (int i = 0; i < ap->wlen; i++) {
		if ((now_ms - ap->win_ms[i]) <= MEDIAN_STALE_MS) {
			t[n++] = ap->win[i];
		}
	}
	if (n == 0) {
		int newest = (ap->wpos + ULTRAWIDELOCK_APPROACH_MEDIAN_N - 1) % ULTRAWIDELOCK_APPROACH_MEDIAN_N;

		return ap->win[newest];
	}
	return range_median(t, n);
}

/**
 * Initialize an approach configuration with factory defaults: unlock at 100 cm, relock at 250 cm,
 * dwell times 2 s and 3 s, motor delay 500 ms, margin 250 ms, velocity floor 30 cm/s, predictive
 * unlock enabled.
 */
void ultrawidelock_approach_defaults(struct ultrawidelock_approach_cfg *cfg)
{
	cfg->unlock_cm = 100;
	cfg->relock_cm = 250;
	cfg->near_dwell = 2;
	cfg->far_dwell = 3;
	cfg->motor_ms = 500;
	cfg->margin_ms = 250;
	cfg->vmin_cm_s = 30;
	/*
	 * OFF by default, and the reason is not caution about the measurement.
	 *
	 * The 25.5 cm antenna-delay offset is THIS BOARD's, measured on one
	 * DWM3001CDK in one room (RESULTS.md Result 19). `modules/` is compiled by
	 * the nRF5340 DK and both ESP32 targets as well, and none of them has ever
	 * had that number measured. Defaulting it on would inject a 25 cm error into
	 * every install that is not this one, in the direction that refuses to
	 * unlock.
	 *
	 * It also shifts EVERY range the controller sees, so switching it on flips
	 * behaviour the existing suite pins: fed 100 cm, a corrected controller sees
	 * a true ~126 and correctly declines to unlock. That is the intended effect
	 * and it is exactly why it cannot arrive by default.
	 *
	 * Turn it on per install, once that install's offset is known.
	 */
	cfg->range_correct_en = false;
	/* Top-quartile boundary over the 399 tripod receptions; install-dependent. */
	cfg->nlos_conf_min = 2.61f;
	/*
	 * Off. Result 21 established the SIGN of the obstruction effect across two
	 * bodies and refuted its magnitude, so this file can supply the mechanism and
	 * not the number. An install that wants a pocketed owner to unlock at the same
	 * physical distance as a hand-held one sets it, having measured its own door.
	 *
	 * If you want a starting point rather than a measurement: the two sessions
	 * bracket 80 to 127 cm at 1 m, and the smaller end is the one to try first,
	 * because under-widening costs a step forward and over-widening costs a
	 * stranger an open door.
	 */
	cfg->nlos_widen_cm = 0;
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	/*
	 * All four off, for the reason the single number is off, three times over.
	 * Result 21 refuted the obstruction magnitude across two BODIES; splitting
	 * obstructed by carry mode does not recover a magnitude, it asks the same
	 * unanswered question once per class. docs/bodycal-falsification.md is the
	 * capture that would answer it, and until it has run there is no honest
	 * number for this file to put here. Zero also means "use nlos_widen_cm",
	 * which is what keeps every pre-table caller behaving identically.
	 */
	for (int c = 0; c < ULTRAWIDELOCK_APPROACH_CARRY_N; c++) {
		cfg->nlos_widen_class_cm[c] = 0;
	}
#endif
	/*
	 * 180 cm, and the ceiling is what picks it rather than the floor. It has
	 * to be high enough that a credential sitting at the door cannot reach
	 * it -- 80 cm above unlock_cm -- and strictly below relock_cm, because
	 * ranges past relock_cm are the ones the trust gate declines to vouch
	 * for and therefore never arrive here at all. At 250 the gate would
	 * never arm and the lock would quietly stop opening; 180 leaves 70 cm of
	 * margin under that cliff.
	 */
	cfg->approach_cm = 180;
	/*
	 * 750 ms, and the number is measured rather than chosen. A ranging block
	 * is 192 ms, so this is four missed blocks -- a phone that is still there
	 * and still ranging cannot be quiet that long by accident.
	 *
	 * It was 1,500 ms, which was too conservative and cost a relock on
	 * 2026-08-02 17:40 by 500 ms: the last far sample was fed 1,000 ms before
	 * the session ended, so only ~870 ms of silence had accumulated when the
	 * link dropped. The walk-away that DID work the same afternoon had 1.2 s.
	 * Real departures land either side of 1,500 ms and reliably above 750.
	 *
	 * Erring short is the safe direction here, and only here: this rule fires
	 * only when the last measurement was already beyond relock_cm, so being
	 * early can relock a door the credential has left and can never relock one
	 * it is standing at.
	 */
	cfg->far_silence_ms = 750;
	cfg->predict_en = true;
}

/**
 * Initialize an approach controller to locked state with zero velocity and no prediction in flight.
 * If cfg is NULL, load factory defaults; otherwise copy the provided configuration.
 */
void ultrawidelock_approach_init(struct ultrawidelock_approach *ap,
				 const struct ultrawidelock_approach_cfg *cfg)
{
	memset(ap, 0, sizeof(*ap));
	if (cfg != NULL) {
		ap->cfg = *cfg;
	} else {
		ultrawidelock_approach_defaults(&ap->cfg);
	}
	/*
	 * Keep the trajectory gate reachable, whatever band the caller chose.
	 *
	 * approach_cm has to sit strictly inside (unlock_cm, relock_cm): at or
	 * below unlock_cm it arms on arrival and gates nothing, and at or above
	 * relock_cm it can never arm at all, because ranges that far out are the
	 * ones the trust gate declines to vouch for and they never reach
	 * ultrawidelock_approach_feed(). The second failure is silent and total -- the
	 * lock simply stops auto-unlocking -- so it is worth a clamp rather than
	 * a comment. The default band makes this a no-op; it exists for callers
	 * that narrow unlock_cm/relock_cm and keep the default approach_cm, which
	 * ports/esp32 is one config change away from doing.
	 */
	if (ap->cfg.approach_cm > 0 && (ap->cfg.approach_cm <= ap->cfg.unlock_cm ||
					ap->cfg.approach_cm >= ap->cfg.relock_cm)) {
		ap->cfg.approach_cm =
			ap->cfg.unlock_cm + (ap->cfg.relock_cm - ap->cfg.unlock_cm) / 2;
	}
	/*
	 * Keep the trajectory gate meaningful while the window says obstructed.
	 *
	 * The gate arms at approach_cm and fires at unlock_cm, and it only gates
	 * anything because there is distance between them. A widening eats that
	 * distance from the wrong end: at unlock_cm 100, approach_cm 180 and a
	 * widening of 120, the effective radius becomes 220 and every sample that
	 * arms the gate also fires it, so a credential that never approached opens
	 * the door -- which is the exact hole 574dbb91 closed.
	 *
	 * Clamped rather than rejected, because the alternative is a config error
	 * that disables a security gate silently. Negative values are clamped away
	 * for the same reason: a negative widening would make the lock stricter only
	 * while obstructed, which no caller can want and which this name does not
	 * say.
	 */
	if (ap->cfg.nlos_widen_cm < 0) {
		ap->cfg.nlos_widen_cm = 0;
	}
	if (ap->cfg.approach_cm > 0 &&
	    ap->cfg.unlock_cm + ap->cfg.nlos_widen_cm >= ap->cfg.approach_cm) {
		ap->cfg.nlos_widen_cm = ap->cfg.approach_cm - ap->cfg.unlock_cm - 1;
	}
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	/*
	 * Every per-class entry gets the identical pair of clamps, per entry. The
	 * table is an override for the number above, so an unclamped entry would be
	 * a way to route around a clamp that exists to stop the trajectory gate
	 * arming and firing on the same sample -- the hole 574dbb91 closed, reopened
	 * through a different field name. A 0 entry stays 0 and keeps meaning "use
	 * nlos_widen_cm", which is already clamped.
	 */
	for (int c = 0; c < ULTRAWIDELOCK_APPROACH_CARRY_N; c++) {
		if (ap->cfg.nlos_widen_class_cm[c] < 0) {
			ap->cfg.nlos_widen_class_cm[c] = 0;
		}
		if (ap->cfg.approach_cm > 0 &&
		    ap->cfg.unlock_cm + ap->cfg.nlos_widen_class_cm[c] >= ap->cfg.approach_cm) {
			ap->cfg.nlos_widen_class_cm[c] =
				ap->cfg.approach_cm - ap->cfg.unlock_cm - 1;
		}
	}
#endif

	ap->locked = true;
	ap->eta_ms = -1;
}

/**
 * Record a predictive unlock abort and relock the door. Called when prediction is active but the
 * phone has stopped approaching or moved away.
 */
static enum ultrawidelock_approach_action pred_abort(struct ultrawidelock_approach *ap)
{
	ap->locked = true;
	ap->pred_open = false;
	ap->pred_dwell = 0;
	/* A relocked bolt must earn a fresh approach. Without this an aborted
	 * prediction leaves the gate armed, and the credential that just stopped
	 * short could open the door by standing still.
	 *
	 * ONE exception, from the 2026-08-07 walk: when the abort catches the
	 * estimate already inside approach_cm, the credential did not stop short
	 * -- it arrived faster than the median could follow -- and clearing the
	 * arm here locked the owner out at 65 cm until they retreated past
	 * approach_cm and walked back in. Keeping the arm cannot open the door on
	 * its own: crossing unlock_cm still takes the median plus near_dwell, and
	 * the stand-still credential the comment above fears produces neither. */
	if (!(ap->kf_init && ap->cfg.approach_cm > 0 && ap->d <= (float)ap->cfg.approach_cm)) {
		ap->approach_armed = false;
	}
	return ULTRAWIDELOCK_APPROACH_RELOCK_ABORT;
}

/**
 * Update the Kalman filter state with a new range measurement, compute estimated time-to-arrival
 * (ETA) at the unlock radius, track presence via a median-filter window, and supervise predictive
 * unlock (fire early when closing speed and ETA permit, abort if the phone stops or moves away).
 * Return the action code: UNLOCK_THRESHOLD (entered unlock zone), RELOCK_DEPART (exited relock
 * zone), UNLOCK_PREDICT (fired a predictive unlock), or HOLD (no action).
 */
void ultrawidelock_approach_session_up(struct ultrawidelock_approach *ap)
{
	/* The RSSI power gate cannot open without the phone approaching, so a NEW
	 * ranging session is the approach evidence approach_cm was asking for. See
	 * the header for the capture that showed the far range never arrives. */
	ap->approach_armed = true;
}

/**
 * Turn a median of reported ranges into a median of true ones.
 *
 * Unconditional part: the reader is a fixed 25.5 cm short because nothing
 * programs the DW3000 antenna delay. Conditional part: a body in the path adds a
 * constant 84.5 cm, and that is undone only when a strict majority of the window
 * were confident obstructed calls. Both constants and the arithmetic live in
 * ultrawidelock_ml so that the model they were measured beside owns them.
 *
 * Without CONFIG_ULTRAWIDELOCK_ML_LOS there is no classifier to ask and no module to call,
 * so this is the identity and the controller behaves exactly as it shipped.
 */
/**
 * Whether a strict majority of the median window were confident obstructed calls.
 *
 * A partly-filled window cannot carry a majority of five and reports clear, so
 * early samples never widen and never get the conditional subtraction. That is
 * the safe direction: both consumers of this are the ones that add permission.
 *
 * VOTES AGE OUT exactly as median entries do (MEDIAN_STALE_MS, the shared
 * win_ms[] timestamps). Before this, an obstructed majority earned before a
 * 1-12 s pocket trust hole kept unlock_cm widened by nlos_widen_cm all the way
 * through it -- a permission-adding decision fed by stale evidence, the same
 * shape as the 00:01:39 ghost grant at the top of this file, only latent
 * because nothing ships a nonzero widening yet. The cost of ageing them: a
 * still pocketed owner whose iOS ranging pauses past the horizon (silences of
 * 1.6-3.07 s are on record) loses the widening until samples resume, so the
 * resting-at-the-door case leans on the band silence tier and on ranging
 * coming back, not on a vote nobody can refresh.
 *
 * Without CONFIG_ULTRAWIDELOCK_ML_LOS nothing ever writes ch_win, so this is constant
 * false and both consumers fold away.
 */
static bool nlos_blocked(const struct ultrawidelock_approach *ap, int64_t now_ms)
{
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	int votes = 0;

	for (int i = 0; i < ap->wlen; i++) {
		if (ap->ch_win[i] && (now_ms - ap->win_ms[i]) <= MEDIAN_STALE_MS) {
			votes++;
		}
	}
	return (ap->wlen >= ULTRAWIDELOCK_APPROACH_MEDIAN_N) && (votes >= NLOS_VOTES_N);
#else
	(void)ap;
	(void)now_ms;
	return false;
#endif
}

/**
 * The unlock radius in force for this sample.
 *
 * Widened by cfg.nlos_widen_cm while the window says obstructed, so an owner
 * whose body is between the phone and the reader crosses the threshold at the
 * same place a hand-held phone would. Result 21 is why this is a widening and
 * not a correction to the range: the sign of the obstruction effect replicated
 * across two bodies and its magnitude did not, and a widening spends only the
 * sign.
 *
 * Every unlock_cm comparison in this file goes through here, deliberately. The
 * regression 4c6083d8 fixed was two thresholds disagreeing about the same
 * boundary, and a widening applied to the fire decision but not to the ETA or
 * the silence rule would rebuild that dead band on purpose.
 */
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
/*
 * The widening cfg would apply to one carry class: the per-class entry when the
 * installer set one, and cfg.nlos_widen_cm when they did not.
 *
 * ZERO MEANS "NO POLICY HERE", not "no widening here", and that is what keeps
 * every caller that predates the table working: an untouched table is all zeros,
 * so every class falls through to the single number and the controller behaves
 * byte for byte as it did. An installer who genuinely wants one class NOT
 * widened while others are sets nlos_widen_cm to 0 and fills the classes they do
 * want, which is the same configuration expressed the way round that works.
 */
static int32_t widen_for_class(const struct ultrawidelock_approach *ap, int c)
{
	const int32_t per_class = ap->cfg.nlos_widen_class_cm[c];

	return (per_class > 0) ? per_class : ap->cfg.nlos_widen_cm;
}

/*
 * Plurality carry class over exactly the votes nlos_blocked() counts.
 *
 * Same window, same confidence floor (ch_win already encodes it), same staleness
 * horizon. This deliberately re-walks the window rather than sharing a pass with
 * nlos_blocked(): the two are asked at different points in feed_carry() -- once
 * before this sample is filed and once after -- and a cached verdict is how the
 * dead band 4c6083d8 fixed got built the first time. Five iterations.
 *
 * Ties go to the smallest widening, because a split window is the window failing
 * to identify the geometry and the answer to that is the stricter threshold. See
 * ultrawidelock_approach_nlos_carry() in the header.
 */
static int nlos_carry_class(const struct ultrawidelock_approach *ap, int64_t now_ms)
{
	int votes[ULTRAWIDELOCK_APPROACH_CARRY_N] = {0};
	int best = ULTRAWIDELOCK_APPROACH_CARRY_HAND;

	for (int i = 0; i < ap->wlen; i++) {
		if (ap->ch_win[i] && (now_ms - ap->win_ms[i]) <= MEDIAN_STALE_MS) {
			votes[ap->carry_win[i]]++; /* clamped in range at file time */
		}
	}

	/* Index 0 is CLEAR and can never be a vote here: ch_win[i] is true only
	 * for confident OBSTRUCTED samples, and feed_carry() files CLEAR into
	 * ch_win as false. Start at HAND so an empty window returns the least
	 * specific obstructed class rather than CLEAR, and the caller's blocked
	 * check is what decides whether any of this is read. */
	for (int c = ULTRAWIDELOCK_APPROACH_CARRY_HAND; c < ULTRAWIDELOCK_APPROACH_CARRY_N; c++) {
		if (votes[c] > votes[best] ||
		    (votes[c] == votes[best] && widen_for_class(ap, c) < widen_for_class(ap, best))) {
			best = c;
		}
	}
	return best;
}
#endif

static int32_t effective_unlock_cm(const struct ultrawidelock_approach *ap, int64_t now_ms)
{
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	if (!nlos_blocked(ap, now_ms)) {
		return ap->cfg.unlock_cm;
	}
	return ap->cfg.unlock_cm + widen_for_class(ap, nlos_carry_class(ap, now_ms));
#else
	return ap->cfg.unlock_cm + (nlos_blocked(ap, now_ms) ? ap->cfg.nlos_widen_cm : 0);
#endif
}

static int32_t channel_correct(const struct ultrawidelock_approach *ap, int64_t now_ms,
			       int32_t median_cm)
{
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	if (!ap->cfg.range_correct_en || median_cm < 0) {
		return median_cm;
	}

	return (int32_t)ultrawidelock_ml_los_range_true_cm((uint16_t)median_cm,
						 nlos_blocked(ap, now_ms) ? ULTRAWIDELOCK_ML_LOS_OBSTRUCTED
									  : ULTRAWIDELOCK_ML_LOS_CLEAR);
#else
	(void)ap;
	(void)now_ms;
	return median_cm;
#endif
}

/*
 * ONE FEED BODY, TWO ENTRY POINTS, AND THE DEFAULT BUILD MUST NOT NOTICE.
 *
 * Without CONFIG_ULTRAWIDELOCK_ML_LOS the body below IS
 * ultrawidelock_approach_feed_channel(): same name, same signature, no wrapper and no
 * extra call. That build's object code is byte-identical to the one that
 * shipped before carry modes existed, which is the whole point -- the classifier
 * is off by default and an image that does not ask for it must not move.
 *
 * With the classifier compiled in the body becomes a static helper that
 * feed_channel() and feed_carry() both tail-call, and its class parameter widens
 * from "was this obstructed" to "which obstruction was it". `carry != 0` is the
 * bool in the first case and CARRY_CLEAR-or-not in the second, which is the same
 * question, so the body reads the same either way.
 *
 * A macro rather than two copies of the function: the alternative is duplicating
 * the presence path, the trajectory gate, the dwell counters and the silence
 * rules -- the code whose dead-band regressions (4c6083d8, 574dbb91) were caused
 * by two places disagreeing about one boundary. There is exactly one of it.
 */
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
#define FEED_LINKAGE static
#define FEED_NAME    feed_one
#define FEED_CARRY_T uint8_t
static enum ultrawidelock_approach_action FEED_NAME(struct ultrawidelock_approach *ap,
						    int64_t now_ms, int32_t cm,
						    FEED_CARRY_T carry, float confidence);
#else
#define FEED_LINKAGE
#define FEED_NAME    ultrawidelock_approach_feed_channel
#define FEED_CARRY_T bool
#endif

enum ultrawidelock_approach_action ultrawidelock_approach_feed(struct ultrawidelock_approach *ap,
							       int64_t now_ms, int32_t cm)
{
	return ultrawidelock_approach_feed_channel(ap, now_ms, cm, false, 0.0f);
}

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
enum ultrawidelock_approach_action
ultrawidelock_approach_feed_channel(struct ultrawidelock_approach *ap, int64_t now_ms, int32_t cm,
				    bool obstructed, float confidence)
{
	/* HAND is the least specific obstructed class; see the header for why this
	 * identity is the compatibility contract and not a shortcut. */
	return feed_one(ap, now_ms, cm,
			(uint8_t)(obstructed ? ULTRAWIDELOCK_APPROACH_CARRY_HAND
					     : ULTRAWIDELOCK_APPROACH_CARRY_CLEAR),
			confidence);
}

enum ultrawidelock_approach_action
ultrawidelock_approach_feed_carry(struct ultrawidelock_approach *ap, int64_t now_ms, int32_t cm,
				  enum ultrawidelock_approach_carry carry, float confidence)
{
	return feed_one(ap, now_ms, cm, (uint8_t)carry, confidence);
}
#endif

/* The one feed body; see the FEED_NAME comment above for what it is called in
 * each build. `carry` is 0 for CLEAR and an obstructed class otherwise. */
FEED_LINKAGE enum ultrawidelock_approach_action
FEED_NAME(struct ultrawidelock_approach *ap, int64_t now_ms, int32_t cm, FEED_CARRY_T carry,
	  float confidence)
{
	const bool obstructed = (carry != 0);

	/*
	 * Correct the estimator's input too, or the two paths disagree about where
	 * the credential is. The presence path works off the corrected median below;
	 * feeding the Kalman filter raw centimetres would leave the PREDICTIVE path
	 * firing its ETA against an uncorrected distance, which is the same bug as
	 * having no correction at all and harder to see.
	 *
	 * The vote used here is the window as it stood BEFORE this sample, since
	 * this sample has not been filed yet. One sample of lag on a class decision
	 * that already needs a majority of five is not worth a restructure to avoid.
	 */
	bool est_fresh = kf_update(ap, now_ms, channel_correct(ap, now_ms, cm));

	/* ETA to the unlock radius, kept current for the trace accessors and
	 * the fire decision below. Valid only with a converged filter, a real
	 * closing speed and the credential still outside the radius. */
	float closing = -ap->v;

	const int32_t radius_pre = effective_unlock_cm(ap, now_ms);

	if (ap->cfg.predict_en && est_fresh && ap->accepted >= KF_MIN_SAMPLES &&
	    closing >= (float)ap->cfg.vmin_cm_s && ap->d > (float)radius_pre) {
		ap->eta_ms = (int32_t)(((ap->d - (float)radius_pre) * 1000.0f) / closing);
	} else {
		ap->eta_ms = -1;
		ap->pred_dwell = 0;
	}

	/* Presence path — the shipped median/dwell controller, verbatim: every
	 * trusted sample enters the window (the median itself rejects spikes),
	 * dwell counters reset across the dead band. */
	ap->last_cm = cm;
	ap->last_feed_ms = now_ms;

	ap->win[ap->wpos] = cm;
	ap->win_ms[ap->wpos] = now_ms;
	ap->ch_win[ap->wpos] = obstructed && (confidence >= ap->cfg.nlos_conf_min);
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	/* Filed unconditionally beside the vote it labels, including for samples
	 * that voted clear: nlos_carry_class() reads it only where ch_win is true,
	 * so a stale label under a false vote is unreachable and blanking it would
	 * be a store nothing can observe. Clamped to range HERE rather than at the
	 * read, so nlos_carry_class() cannot be walked out of its votes[] array by
	 * a caller's bad enum, and so out-of-range lands on HAND -- the least
	 * specific obstructed class -- exactly as the header promises. */
	ap->carry_win[ap->wpos] =
		(carry < ULTRAWIDELOCK_APPROACH_CARRY_N) ? carry : ULTRAWIDELOCK_APPROACH_CARRY_HAND;
#endif
	ap->wpos = (ap->wpos + 1) % ULTRAWIDELOCK_APPROACH_MEDIAN_N;
	if (ap->wlen < ULTRAWIDELOCK_APPROACH_MEDIAN_N) {
		ap->wlen++;
	}
	int32_t f = channel_correct(ap, now_ms, range_median_fresh(ap, now_ms));

	/*
	 * Trajectory gate. Arm on the MEDIAN so a single spike cannot do it, and
	 * arm before the decisions below so an approach that crosses approach_cm
	 * and the unlock radius inside one median window still counts -- the
	 * gate is there to reject credentials that never approached, not to add
	 * latency to ones that did.
	 */
	if (ap->cfg.approach_cm > 0 && f >= ap->cfg.approach_cm) {
		ap->approach_armed = true;
	}
	const bool approached = (ap->cfg.approach_cm <= 0) || ap->approach_armed;

	if (f <= effective_unlock_cm(ap, now_ms)) {
		ap->far_dwell = 0;
		if (ap->pred_open) {
			/* Arrived: the predictive open converts to a normal
			 * presence unlock; departure rules take over. */
			ap->pred_open = false;
		}
		if (ap->locked && approached && ++ap->near_dwell >= ap->cfg.near_dwell) {
			ap->locked = false;
			return ULTRAWIDELOCK_APPROACH_UNLOCK_THRESHOLD;
		}
	} else if (f >= ap->cfg.relock_cm) {
		ap->near_dwell = 0;
		if (!ap->locked && ++ap->far_dwell >= ap->cfg.far_dwell) {
			ap->locked = true;
			ap->pred_open = false;
			ap->pred_dwell = 0;
			ap->approach_armed = false;
			return ULTRAWIDELOCK_APPROACH_RELOCK_DEPART;
		}
	} else {
		ap->near_dwell = 0;
		ap->far_dwell = 0;
	}

	/* Prediction path. Supervise an open first: it must keep closing and
	 * arrive on time, or the bolt goes back.
	 *
	 * Both velocity checks require a filter that KNOWS a velocity. A sample
	 * gap past KF_STALE_MS re-bases it with v = 0 and accepted = 1, and on
	 * the 2026-08-07 walk that made the first sample AFTER a ranging hole
	 * read as a stall -- the owner still mid-stride, the bolt shutting
	 * against them. Under KF_MIN_SAMPLES the velocity is not evidence of
	 * anything; the deadline alone supervises until the filter converges. */
	if (ap->pred_open) {
		if (now_ms >= ap->pred_deadline_ms) {
			return pred_abort(ap);
		}
		if (est_fresh && ap->accepted >= KF_MIN_SAMPLES) {
			if (closing < (float)ap->cfg.vmin_cm_s / 2.0f) {
				return pred_abort(ap);
			}
			/* Still closing: renew the grace. The deadline exists to
			 * catch silence and stalls, not to race the median filter
			 * into the unlock band. */
			ap->pred_deadline_ms = now_ms + PRED_GRACE_MS;
		}
		return ULTRAWIDELOCK_APPROACH_HOLD;
	}

	/* The predictive path is gated too, not just the presence one. It is the
	 * harder case to reach without an approach -- it wants a closing speed
	 * above vmin_cm_s -- but "hard to reach" is not "cannot", and the
	 * invariant this gate states is about every auto-unlock. */
	if (ap->locked && approached && ap->eta_ms >= 0 &&
	    ap->eta_ms <= ap->cfg.motor_ms + ap->cfg.margin_ms) {
		if (++ap->pred_dwell >= PRED_DWELL_N) {
			ap->locked = false;
			ap->pred_open = true;
			ap->pred_dwell = 0;
			ap->pred_deadline_ms = now_ms + ap->eta_ms + PRED_GRACE_MS;
			return ULTRAWIDELOCK_APPROACH_UNLOCK_PREDICT;
		}
	} else {
		ap->pred_dwell = 0;
	}
	return ULTRAWIDELOCK_APPROACH_HOLD;
}

/**
 * Supervise an active predictive unlock when no new measurement arrives this window. If the
 * prediction deadline has passed, abort and relock the door. Return HOLD otherwise.
 */
void ultrawidelock_approach_observe_departure(struct ultrawidelock_approach *ap, int64_t now_ms,
					      int32_t cm)
{
	if (ap == NULL || cm < ap->cfg.relock_cm) {
		return;
	}
	/*
	 * No median window, no dwell counter, no filter update: an unvouched
	 * range must be able to start the departure clock and must never move
	 * the bolt.
	 */
	ap->last_cm = cm;
	ap->last_feed_ms = now_ms;
	/*
	 * It MAY re-arm the trajectory gate, and the 23:51 walk on 2026-08-07 is
	 * why. The trust gate declined the entire 131-83 cm retreat, so after the
	 * relock the median held [15,30,79,102,194] -- 79 -- and the f >= approach_cm
	 * re-arm was unreachable; the owner then stood at 0 cm for fifteen seconds
	 * against a door that could not open. Their 473-504 cm readings WERE seen,
	 * by this path alone.
	 *
	 * Why this does not hand the unlock decision unvouched data: arming is
	 * necessary and never sufficient (the unlock still takes a vouched median
	 * inside unlock_cm plus near_dwell), a range this far requires the session's
	 * own STS keys to produce at all, and the credential this gate exists to
	 * stop -- one resting NEAR the door, waiting for noise to wander the median
	 * across the radius -- produces no >= relock_cm reading to arm with. The
	 * residual case is a multipath-inflated reading from a resting phone, and
	 * its exposure is bounded by what already ships: session_up() arms
	 * unconditionally on every new BLE session, and iOS cycles the session every
	 * minute or two anyway.
	 */
	ap->approach_armed = true;
}

/**
 * Advance the approach state machine by one tick: handle predictive unlock abort on deadline,
 * departure by silence when measurements stop after the phone leaves the relock threshold, and
 * return the triggered action or HOLD if no action occurred.
 */
enum ultrawidelock_approach_action ultrawidelock_approach_tick(struct ultrawidelock_approach *ap,
							       int64_t now_ms)
{
	/* No sample this window: the estimate is frozen (no coasting — a
	 * measurement-free filter would happily "arrive" on its own). Only an
	 * overdue predictive open acts here. */
	if (ap->pred_open && now_ms >= ap->pred_deadline_ms) {
		return pred_abort(ap);
	}

	/*
	 * Departure by silence, which is how a real walk-away ends: the far
	 * samples stop before far_dwell can count three of them.
	 *
	 * GATED ON unlock_cm, NOT relock_cm, and the difference is a hole this
	 * cost 11 s of on 2026-08-07. The rule used to require the last
	 * measurement to be beyond relock_cm, so a credential that went silent
	 * anywhere in the 100-250 cm dead band left the bolt open indefinitely.
	 * The capture has it exactly: last range 207 cm, session died at
	 * 20:38:10.9, and nothing relocked until ranging RESUMED at 427 cm eleven
	 * seconds later. 207 was under 250, so the rule declined -- correctly by
	 * its own contract, and wrong for the owner walking away.
	 *
	 * unlock_cm keeps the protection the old threshold was reaching for. A
	 * phone resting near the door is inside unlock_cm, so it still goes quiet
	 * without the bolt moving under its owner; what changes is that a
	 * credential which had already left the unlock radius no longer needs to
	 * be seen crossing a second, larger one before silence counts.
	 */
	/*
	 * NOT while a predictive open is outstanding. That path fired the bolt in
	 * anticipation of an arrival and gives itself ETA + PRED_GRACE_MS to see it,
	 * the grace covering the median filter's lag into the unlock band. Widening
	 * the silence gate to unlock_cm let 750 ms of quiet relock a prediction that
	 * was still inside its own deadline, so a phone arriving through a gap in
	 * ranging would get the bolt shut and reopened under its hand. While a
	 * prediction is pending its deadline governs; once it converts to presence
	 * or aborts, silence does.
	 */
	/*
	 * The WIDENED radius, for the same reason the fire decision uses it: a
	 * pocketed phone resting at the door reports itself past unlock_cm and would
	 * otherwise be read as a departure the moment ranging paused. Whatever
	 * distance counts as "at the door" for opening has to count as "at the door"
	 * for staying open, or the widening buys an unlock and then takes it back.
	 */
	/*
	 * Two silence tiers. Last seen >= relock_cm is an unambiguous departure
	 * and keeps the fast far_silence_ms relock; last seen in the DEAD BAND is
	 * ambiguous -- on the 2026-08-08 pocketed walk it was an owner mid-arrival
	 * whose trust stream had a routine 1.7 s hole, and the 750 ms tier
	 * un-opened the door in their face -- so the band waits BAND_SILENCE_MS
	 * (or far_silence_ms if an install configured that even larger).
	 */
	int64_t need_ms = (int64_t)ap->cfg.far_silence_ms;

	if (ap->last_cm < ap->cfg.relock_cm && need_ms < BAND_SILENCE_MS) {
		need_ms = BAND_SILENCE_MS;
	}
	if (!ap->locked && !ap->pred_open && ap->cfg.far_silence_ms > 0 && ap->last_feed_ms != 0 &&
	    ap->last_cm > effective_unlock_cm(ap, now_ms) &&
	    (now_ms - ap->last_feed_ms) >= need_ms) {
		ap->locked = true;
		ap->near_dwell = 0;
		ap->far_dwell = 0;
		ap->pred_open = false;
		ap->pred_dwell = 0;
		/*
		 * Clear the arm only when the credential was last seen genuinely FAR.
		 * A phone that went quiet in the dead band (23:51 walk: trusted 102 cm,
		 * then silence) is still standing at its own door, and clearing the arm
		 * there demanded a fresh >= approach_cm median from someone one step
		 * away -- a lockout, not a defence. The resting-phone scenario is
		 * unaffected: it keeps producing samples, so this rule never fires on
		 * it in the first place.
		 */
		if (ap->last_cm >= ap->cfg.relock_cm) {
			ap->approach_armed = false;
		}
		return ULTRAWIDELOCK_APPROACH_RELOCK_DEPART;
	}
	return ULTRAWIDELOCK_APPROACH_HOLD;
}

/**
 * Reset the approach controller to locked state while preserving its configuration. Return
 * RELOCK_DEPART if the door was unlocked before the reset, otherwise HOLD.
 */
enum ultrawidelock_approach_action ultrawidelock_approach_gone(struct ultrawidelock_approach *ap)
{
	bool was_open = !ap->locked;
	struct ultrawidelock_approach_cfg cfg = ap->cfg;

	ultrawidelock_approach_init(ap, &cfg);
	return was_open ? ULTRAWIDELOCK_APPROACH_RELOCK_DEPART : ULTRAWIDELOCK_APPROACH_HOLD;
}

/**
 * Undo the unlock transition the caller refused. See the header for why the
 * absence of this silently disabled auto-unlock after the first refusal.
 */
void ultrawidelock_approach_veto(struct ultrawidelock_approach *ap)
{
	if (ap == NULL || ap->locked) {
		return;
	}
	ap->locked = true;
	ap->pred_open = false;
	ap->pred_dwell = 0;
	ap->eta_ms = -1;
	/*
	 * near_dwell is deliberately NOT reset. The approach already satisfied
	 * it -- that is why an unlock was offered -- and the credential is still
	 * inside the radius. Clearing it would make the refusing policy pay the
	 * dwell again on every retry, which on a 3-sample dwell means the phone
	 * has usually walked through the door before the retry lands.
	 *
	 * approach_armed is likewise left alone: the trajectory gate's question
	 * ("did this credential ever approach from outside?") was answered, and
	 * a refusal by a different policy is not evidence against it.
	 */
}

/**
 * Return true if the door is locked, false if unlocked.
 */
bool ultrawidelock_approach_locked(const struct ultrawidelock_approach *ap)
{
	return ap->locked;
}

/**
 * The debounced channel verdict the widening consumes, for telemetry. See the
 * header for why it is exposed; the decision path calls nlos_blocked() directly.
 */
bool ultrawidelock_approach_nlos_blocked(const struct ultrawidelock_approach *ap, int64_t now_ms)
{
	return nlos_blocked(ap, now_ms);
}

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
enum ultrawidelock_approach_carry
ultrawidelock_approach_nlos_carry(const struct ultrawidelock_approach *ap, int64_t now_ms)
{
	if (!nlos_blocked(ap, now_ms)) {
		return ULTRAWIDELOCK_APPROACH_CARRY_CLEAR;
	}
	return (enum ultrawidelock_approach_carry)nlos_carry_class(ap, now_ms);
}

int32_t ultrawidelock_approach_effective_unlock_cm(const struct ultrawidelock_approach *ap,
						   int64_t now_ms)
{
	return effective_unlock_cm(ap, now_ms);
}
#endif

/**
 * Return the current estimated distance in centimeters. Returns -1 if the Kalman filter has not
 * been initialized (no valid measurement yet); otherwise returns the rounded estimate.
 */
int32_t ultrawidelock_approach_est_cm(const struct ultrawidelock_approach *ap)
{
	if (!ap->kf_init) {
		return -1;
	}
	return (int32_t)(ap->d + (ap->d < 0.0f ? -0.5f : 0.5f));
}

/**
 * Return the current velocity in centimeters per second (positive = approaching, negative =
 * receding). Returns 0 if the Kalman filter has not been initialized.
 */
int32_t ultrawidelock_approach_vel_cm_s(const struct ultrawidelock_approach *ap)
{
	if (!ap->kf_init) {
		return 0;
	}
	float c = -ap->v;

	return (int32_t)(c + (c < 0.0f ? -0.5f : 0.5f));
}

/**
 * Return the estimated time in milliseconds until approach completes (unlock reaches the door).
 * Value is -1 if not yet computed, or the reader has already locked the door.
 */
int32_t ultrawidelock_approach_eta_ms(const struct ultrawidelock_approach *ap)
{
	return ap->eta_ms;
}
