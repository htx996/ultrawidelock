/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_approach.h
 * Configuration and state for approach detection and predictive unlock: unlock/relock thresholds in
 * centimeters, sample-count dwell times, motor retraction time, scheduling margin, minimum closing
 * speed, and a flag to enable or disable predictive ToA unlock.
 */
/*
 * ultrawidelock_approach — predictive approach controller ("negative latency").
 *
 * Turns the per-block trusted UWB range stream into bolt decisions. Presence
 * path: median filter + near/far dwell counters across a hysteresis band (slow
 * shuffles still unlock). Prediction path: a 1-D constant-velocity Kalman
 * filter yields distance + closing speed; when the ETA at the unlock radius
 * drops inside the retraction window (motor_ms + margin_ms), retraction starts
 * early so it COMPLETES at arrival. Invariant: open only while an authenticated
 * credential is closing -- prediction needs a converged filter, speed above
 * vmin_cm_s and two qualifying samples, and an opened bolt relocks the moment
 * the approach stops or the arrival is overdue.
 *
 * Pure logic, caller-allocated state, no platform dependencies. Call from task
 * context only (float math; never from the UWB RX/ISR path).
 */
#ifndef ULTRAWIDELOCK_APPROACH_H
#define ULTRAWIDELOCK_APPROACH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spike-rejecting median window over trusted ranges (odd; samples). */
#define ULTRAWIDELOCK_APPROACH_MEDIAN_N 5

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
/*
 * How the credential is being carried, mirroring enum ultrawidelock_ml_carry_class
 * value for value. Mirrored rather than included because this file is pure logic
 * with no module dependencies -- ultrawidelock_approach.c includes ultrawidelock_ml.h under the
 * same guard and static-asserts the two agree, so a divergence is a build error
 * and not a silently mis-indexed widening table.
 */
enum ultrawidelock_approach_carry {
	ULTRAWIDELOCK_APPROACH_CARRY_CLEAR = 0,
	ULTRAWIDELOCK_APPROACH_CARRY_HAND = 1,
	ULTRAWIDELOCK_APPROACH_CARRY_POCKET = 2,
	ULTRAWIDELOCK_APPROACH_CARRY_BAG = 3,
};
#define ULTRAWIDELOCK_APPROACH_CARRY_N 4
#endif

/* One decision per fed sample / tick; the caller mirrors these onto the bolt.
 * UNLOCK/RELOCK values fire once per transition (the controller tracks the
 * bolt state), so acting on every non-HOLD return is idempotent-safe. */
enum ultrawidelock_approach_action {
	ULTRAWIDELOCK_APPROACH_HOLD = 0,
	/* ETA inside the retraction window: start the bolt now so it finishes
	 * at arrival. */
	ULTRAWIDELOCK_APPROACH_UNLOCK_PREDICT,
	/* Classic presence unlock: median inside unlock_cm for near_dwell
	 * consecutive samples. */
	ULTRAWIDELOCK_APPROACH_UNLOCK_THRESHOLD,
	/* Departure: median beyond relock_cm for far_dwell consecutive
	 * samples (or the peer vanished, via ultrawidelock_approach_gone()). */
	ULTRAWIDELOCK_APPROACH_RELOCK_DEPART,
	/* Predictive open aborted: the approach stopped/turned or arrival is
	 * overdue while still outside the unlock radius. */
	ULTRAWIDELOCK_APPROACH_RELOCK_ABORT,
};

/**
 * Configuration for approach detection: unlock_cm (presence radius and ETA target), relock_cm
 * (departure threshold), near_dwell/far_dwell (sample counts to unlock/relock), motor_ms (bolt
 * retraction time), margin_ms (scheduling slack, >= 192 ms to avoid missing discrete samples),
 * vmin_cm_s (min closing speed to arm prediction), predict_en (false disables prediction and leaves
 * presence path unchanged; also false whenever RSSI power gate is active).
 */
struct ultrawidelock_approach_cfg {
	int32_t unlock_cm; /* presence radius; also the ETA target */
	int32_t relock_cm; /* departure radius (hysteresis band top) */
	int near_dwell;    /* consecutive medians <= unlock_cm to unlock */
	int far_dwell;     /* consecutive medians >= relock_cm to relock */
	int32_t motor_ms;  /* bolt retraction time for this lock model */
	int32_t margin_ms; /* scheduling slack on top of motor_ms; keep it
			    * >= one ranging block (192 ms) so the discrete
			    * sample grid cannot miss the window */
	int32_t vmin_cm_s; /* min closing speed for a prediction to fire */
	/*
	 * How long a silence that STARTED beyond relock_cm counts as departure.
	 *
	 * far_dwell needs consecutive far SAMPLES, and a phone being carried
	 * away stops producing them: it leaves UWB range, or iOS stops ranging,
	 * usually after one or two far readings. Measured on hardware -- 13 cm,
	 * 139 cm, 378 cm, then silence, then the link dropped 3.2 s later with
	 * far_dwell at 1 of 3. The relock then had no session left to go out
	 * on and was replayed into the NEXT approach, which the user sees as a
	 * door that stays unlocked all the way down the street and relocks as
	 * they come back.
	 *
	 * Silence alone must NOT mean departure -- a phone held still nearby
	 * also stops ranging, and relocking under the owner's hand is the worse
	 * failure. The last measurement is what separates them: last seen far
	 * and now quiet is leaving; last seen near and now quiet is standing
	 * there. 0 disables and restores the sample-only behaviour.
	 */
	int32_t far_silence_ms;
	/*
	 * Trajectory gate: no auto-unlock until this credential has been SEEN at
	 * or beyond approach_cm in the current session. 0 disables it and
	 * restores the behaviour that shipped before it.
	 *
	 * WHAT IT REMOVES, deliberately. A credential that first appears already
	 * inside the unlock radius used to open the door in about a second, on
	 * the presence path, with no approach at all. That is the correct answer
	 * for a phone walking up and the wrong one for a phone that was in the
	 * house the whole time, and "do not open when we are inside" is the
	 * problem this lock exists to solve. Both unlock paths are gated, not
	 * just the predictive one: a walk-up satisfies this trivially, and only a
	 * credential that materialised at the door is refused.
	 *
	 * WHAT IT DOES NOT DO. It is not an inside/outside test. Someone indoors
	 * walking to the door produces exactly the same deep monotonic approach
	 * as someone outdoors. This filters a credential that never approached --
	 * loiter, a session re-established at the door, a phone sitting on a hall
	 * table -- and nothing else.
	 *
	 * IT MUST STAY BELOW WHAT THE TRUST GATE VOUCHES FOR, and this is the
	 * trap. Ranges beyond relock_cm are the ones range-integrity consensus
	 * declines to vouch for, so they never reach ultrawidelock_approach_feed(); set
	 * approach_cm at or above relock_cm and the gate can never arm, and the
	 * lock silently stops auto-unlocking altogether. The default leaves a
	 * 70 cm margin under relock_cm for that reason.
	 *
	 * Arming reads the MEDIAN, not the raw sample, so one spike cannot arm
	 * it. And arming is deliberately not available through
	 * ultrawidelock_approach_observe_departure(): that channel carries unvouched
	 * ranges, which may relock a door and must never help open one.
	 */
	int32_t approach_cm;
	/*
	 * Channel-corrected ranging. 0 disables both corrections and restores the
	 * behaviour that shipped before them; the defaults enable both.
	 *
	 * TWO CORRECTIONS, AND ONLY ONE OF THEM IS CONDITIONAL, which is the whole
	 * design. RESULTS.md Result 19 measured this reader as reporting 25.5 cm
	 * short of true, always, because nothing programs the DW3000 antenna delay.
	 * That is calibration and it applies unconditionally. It also makes the lock
	 * STRICTER: unlock_cm = 100 has really been firing at a true 125 cm.
	 *
	 * The second is that a body in the path adds a constant 84.5 cm, so an owner
	 * with the phone on the far side of themselves reads 155 cm at a true 100
	 * and never crosses the threshold. Undoing that ADDS permission, so it is
	 * gated three ways: the reception must be classified obstructed, the
	 * classifier's confidence must clear nlos_conf_min, and it must hold across
	 * a majority of the median window (see ultrawidelock_approach_feed_channel).
	 *
	 * WHAT A WRONG ANSWER COSTS, stated because it is the reason for the gates.
	 * A false obstructed subtracts 84.5 cm, so a phone genuinely at 185 cm reads
	 * as 100 and the door opens for a walk-by. At the measured per-reception
	 * false rate of 4.7%, a majority of five brings that to about 1 in 1,035
	 * while still engaging on 94.9% of genuinely blocked windows.
	 *
	 * LEAVE THIS FALSE. Result 21 repeated Result 19's capture with a second body
	 * and measured the obstruction offset at +127.0 cm [+109, +136] against the
	 * original +82.0 [+62, +93] in the same frame-length slice, intervals not
	 * overlapping. The gates above bound how often the correction engages
	 * wrongly; they cannot bound how wrong it is when it engages rightly, and a
	 * 47 cm spread is larger than the margin the paragraph above is defending.
	 * The obstruction itself replicated cleanly as -10 dB of first-path power in
	 * both sessions, so the classifier is sound and only this subtraction is not:
	 * the supportable use is to widen unlock_cm while obstructed, which needs the
	 * sign and not the magnitude. The antenna half would be safe on its own but
	 * is not separately switched, because it has never had a caller that wanted
	 * one without the other.
	 */
	bool range_correct_en;
	/*
	 * Confidence floor for the obstruction correction, in the units
	 * ultrawidelock_ml_los_confidence() returns. 2.61 is the top quartile boundary
	 * measured over the 399 tripod receptions of Results 18 and 19, where the
	 * linear sign is right 96% of the time against 39% in the bottom quartile.
	 * Install-dependent: it is one room, one phone and one body.
	 */
	float nlos_conf_min;
	/*
	 * Centimetres to ADD to unlock_cm while the window says obstructed, and the
	 * shippable half of what Results 18-21 measured. 0 disables it, which is the
	 * default, because the right number is an install's policy and not a constant
	 * this file can know.
	 *
	 * WHY THIS AND NOT range_correct_en's SUBTRACTION. Result 21 killed the
	 * subtraction because its magnitude did not replicate across two bodies
	 * (+82.0 against +127.0, non-overlapping intervals) while its SIGN did, along
	 * with the -10 dB of first-path attenuation the classifier actually reads. A
	 * widening consumes only the sign: it says "be more permissive while a body
	 * is in the way" and leaves how much to the installer, where a subtraction
	 * claims to know a true distance it does not know.
	 *
	 * IT IS NOT SAFER, AND SAYING SO WOULD BE WRONG. Widening to 220 opens for a
	 * misclassified walk-by at 220 exactly as subtracting 120 would. Two things
	 * are genuinely better and neither is permissiveness. First, the subtraction
	 * fed a fabricated distance into the Kalman filter, so a wrong constant
	 * corrupted the velocity estimate and every ETA built on it; this touches one
	 * comparison and the estimator never sees it. Second, a policy number invites
	 * the tuning it needs, where a measured-looking constant invites trust it has
	 * not earned.
	 *
	 * The false-positive arithmetic from range_correct_en carries over unchanged,
	 * because the gate is the same one: a 4.7% per-reception false rate through a
	 * majority of five is about 1 in 1,035 windows, engaging on 94.9% of genuinely
	 * blocked ones.
	 *
	 * Clamped at init to keep unlock_cm + nlos_widen_cm strictly below
	 * approach_cm, or the trajectory gate would arm and fire on the same sample
	 * and stop gating anything. See ultrawidelock_approach_cfg_default().
	 */
	int32_t nlos_widen_cm;
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	/*
	 * PER-CARRY-MODE OVERRIDE of nlos_widen_cm, indexed by enum
	 * ultrawidelock_approach_carry. Every entry defaults to 0 and 0 means "no policy
	 * for this class, use nlos_widen_cm", so a controller that never fills this
	 * in behaves exactly as it did before the table existed -- which is how the
	 * binary ultrawidelock_approach_feed_channel() callers keep working unchanged.
	 *
	 * WHY A TABLE AND NOT ONE NUMBER. The complaint BodyCal exists for is that
	 * the trigger distance depends on where the phone is: a hand at the hip, a
	 * trouser pocket and a rucksack are different amounts of water between the
	 * antenna and the reader, and one widening tuned for a pocket is wrong for
	 * the other two in both directions. Splitting the number is the mechanism
	 * for making them agree.
	 *
	 * NONE OF THESE NUMBERS IS MEASURED, AND THIS FILE WILL NEVER SUPPLY ONE.
	 * Result 21 refuted the obstruction MAGNITUDE across two bodies while its
	 * sign held; splitting obstructed into three carry modes does not repair
	 * that, it multiplies it by three. These stay install policy for the same
	 * reason nlos_widen_cm does, and docs/bodycal-falsification.md is the
	 * protocol that says whether per-class numbers are even stable enough to
	 * tune -- per-class residual std <= ~15 cm, or widening-only is all that is
	 * supportable and this table stays at 0.
	 *
	 * IT CANNOT BE MORE PERMISSIVE THAN THE SINGLE NUMBER WAS DANGEROUS. Each
	 * entry gets exactly the clamps nlos_widen_cm gets -- negatives to zero,
	 * and unlock_cm + entry held strictly under approach_cm -- applied at init,
	 * per entry. See ultrawidelock_approach_init().
	 *
	 * ENTRIES THE MODEL CANNOT REACH ARE DEAD, and they are not inert. The
	 * shipped tree is two-class, so ultrawidelock_ml_los_carry_classify() only ever
	 * returns CLEAR or HAND; POCKET and BAG entries are then unreachable and an
	 * installer tuning them is tuning nothing. Ask
	 * ultrawidelock_ml_los_carry_trained() before offering them.
	 *
	 * The CLEAR entry exists so the table is indexable by the class directly and
	 * has no other purpose: the vote below never selects a widening while the
	 * window says clear, so index 0 is never read.
	 */
	int32_t nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_N];
#endif
	bool predict_en; /* arm the prediction path at all; false leaves the
			  * presence path exactly as it shipped. Off whenever
			  * the RSSI power gate is on: the gate withholds
			  * ranging until the credential is already inside
			  * unlock_cm, so no ETA can ever arm and the two
			  * features would only pretend to cooperate. */
};

/**
 * State machine and Kalman filter for approach detection and predictive unlock.
 * locked: bolt mirror state. win/wlen/wpos: median filter for distance. near_dwell/far_dwell:
 * hysteresis counters. kf_init/accepted/rejects/last_ms/d/v/p00/p01/p11: constant-velocity Kalman
 * filter (distance cm, velocity cm/s; negative velocity = closing).
 * pred_dwell/pred_open/pred_deadline_ms/eta_ms: predictive unlock path (fires when closing speed >=
 * vmin_cm_s and ETA to unlock_cm is within motor_ms + margin_ms).
 */
struct ultrawidelock_approach {
	struct ultrawidelock_approach_cfg cfg;

	/* bolt mirror + presence path */
	bool locked;
	int32_t win[ULTRAWIDELOCK_APPROACH_MEDIAN_N];
	/* Parallel to win[]: when each sample arrived. The median votes only
	 * entries younger than MEDIAN_STALE_MS -- a pocketed walk's trust holes
	 * run 1-12 s, and on 2026-08-08 a 9-second-old near entry manufactured
	 * presence at 1.6 m (the 00:01:39 ghost grant). */
	int64_t win_ms[ULTRAWIDELOCK_APPROACH_MEDIAN_N];
	/* Parallel to win[], same wpos/wlen: was THIS sample a confident obstructed
	 * call? The vote is taken over the same window the median is, so the
	 * correction costs no latency the median was not already spending. */
	bool ch_win[ULTRAWIDELOCK_APPROACH_MEDIAN_N];
#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
	/* Parallel to ch_win[]: WHICH obstructed class this sample was, so the
	 * widening can be looked up per carry mode. Meaningful only where
	 * ch_win[] is true; entries under a false ch_win are not read, because a
	 * sample that failed the confidence floor votes clear and a clear vote
	 * selects no widening. uint8_t rather than the enum so the array stays
	 * five bytes: this sits in per-credential state. */
	uint8_t carry_win[ULTRAWIDELOCK_APPROACH_MEDIAN_N];
#endif
	int wlen, wpos;
	int near_dwell, far_dwell;

	/* constant-velocity Kalman filter (cm, cm/s; v < 0 = closing) */
	bool kf_init;
	int accepted; /* samples accepted since (re)init */
	int rejects;  /* consecutive innovation-gate rejects */
	int64_t last_ms;
	float d, v;
	float p00, p01, p11;

	/* prediction */
	int pred_dwell;           /* consecutive in-window samples */
	bool pred_open;           /* opened predictively, not yet arrived */
	int64_t pred_deadline_ms; /* arrive by this or RELOCK_ABORT */
	int32_t eta_ms;           /* last ETA to unlock_cm; -1 = none */

	/* Trajectory gate: has a median at or beyond cfg.approach_cm been seen
	 * since the bolt was last locked? Cleared on every return to locked and
	 * by ultrawidelock_approach_gone(), so each session must earn its own approach.
	 * See ultrawidelock_approach_cfg::approach_cm. */
	bool approach_armed;

	/* departure-by-silence; see ultrawidelock_approach_cfg::far_silence_ms */
	int32_t last_cm;      /* last RAW sample, not the median: the median is
			       * there to reject spikes while tracking, and the
			       * question here is what was actually measured last */
	int64_t last_feed_ms; /* when it arrived; 0 = nothing yet */
};

/* Fill cfg with the tuned defaults (100/250 cm band, 2/3 dwells, 500 ms
 * motor + 250 ms margin, 30 cm/s floor). */
void ultrawidelock_approach_defaults(struct ultrawidelock_approach_cfg *cfg);

/* cfg == NULL uses the defaults. Starts locked, idle. */
void ultrawidelock_approach_init(struct ultrawidelock_approach *ap,
				 const struct ultrawidelock_approach_cfg *cfg);

/**
 * Tell the controller a ranging session just came up, which counts as an approach.
 *
 * WHY THE SESSION IS THE EVIDENCE, and why waiting for a far UWB range was not.
 * approach_cm asks to have seen the credential at 180 cm before any auto-unlock,
 * to refuse a phone that was indoors all along. On this hardware that range
 * never arrives: the BLE RSSI power gate holds ranging off until the connection
 * crosses its open threshold, so UWB starts when the phone is ALREADY at the
 * door. A capture on 2026-08-07 has ranging going active at 55 cm and the bolt
 * then ignoring a phone sitting between 0 and 57 cm for fourteen seconds; it
 * opened 21.5 s in, and only because the owner had walked out to 342 cm and come
 * back, which is the one thing that could satisfy the gate.
 *
 * A phone cannot open a session without approaching -- that is what the RSSI
 * gate measures -- so session establishment carries the same evidence the far
 * range was standing in for, and it carries it at the moment it actually
 * happens. The gate still refuses a credential that was already connected and
 * idle indoors, because that session is not new.
 *
 * Call it when the reader arms ranging for a session. Harmless if approach_cm is
 * 0, and it never unlocks anything by itself.
 */
void ultrawidelock_approach_session_up(struct ultrawidelock_approach *ap);

/* One trusted range sample (task context, timestamps in ms, any monotonic
 * base). Runs both paths, returns at most one transition. */
enum ultrawidelock_approach_action ultrawidelock_approach_feed(struct ultrawidelock_approach *ap,
							       int64_t now_ms, int32_t cm);

/**
 * The same, plus what the channel classifier said about THIS reception.
 *
 * ultrawidelock_approach_feed() is this with obstructed = false and confidence = 0, so a
 * caller with no classifier keeps the antenna-delay correction and never gets the
 * obstruction one. Both are off entirely when cfg.range_correct_en is false.
 *
 * THE CORRECTION IS APPLIED TO THE MEDIAN, NOT TO THE SAMPLE, and that is not an
 * implementation detail. Blocked receptions scattered with an interquartile
 * spread of 38 and 47.5 cm against 7 and 11.5 for clear (RESULTS.md Result 19),
 * so one obstructed reception is a poor estimate of anything and correcting it
 * would move noise rather than remove bias. For a constant offset and a stable
 * class the two are identical anyway -- median(x + c) == median(x) + c -- so the
 * only case where it matters is a class that flips mid-window, which is exactly
 * the case the majority vote exists to handle.
 *
 * @param cm          the range as the reader produced it, uncorrected.
 * @param obstructed  ultrawidelock_ml_los_classify() == ULTRAWIDELOCK_ML_LOS_OBSTRUCTED for the
 *                    reception this range came from.
 * @param confidence  ultrawidelock_ml_los_confidence() for the same reception. Compared
 *                    against cfg.nlos_conf_min; a sample below it votes clear
 *                    rather than being discarded, because a low-confidence
 *                    reading is evidence about the channel and not a missing
 *                    measurement.
 */
enum ultrawidelock_approach_action
ultrawidelock_approach_feed_channel(struct ultrawidelock_approach *ap, int64_t now_ms, int32_t cm,
				    bool obstructed, float confidence);

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
/**
 * The same again, carrying WHICH obstruction rather than merely that there was
 * one, so cfg.nlos_widen_class_cm[] can be indexed by it.
 *
 * ultrawidelock_approach_feed_channel(obstructed, conf) is exactly this with
 * carry = obstructed ? HAND : CLEAR, and that identity is the compatibility
 * contract: HAND is the least specific obstructed class, so a binary caller
 * lands in the table entry that says "some body is in the way" and, with the
 * table left at its 0 default, gets cfg.nlos_widen_cm exactly as before. Nothing
 * about the vote, the confidence floor, the majority-of-five or the staleness
 * horizon changes -- this only labels the vote it was already casting.
 *
 * The class is recorded, not trusted per sample. It is read only through the
 * same majority the widening has always needed, so one misclassified reception
 * cannot select a carry mode any more than it can select obstructed.
 *
 * @param cm          the range as the reader produced it, uncorrected.
 * @param carry       ultrawidelock_ml_los_carry_classify() for this reception, folded
 *                    into enum ultrawidelock_approach_carry (the two enums are
 *                    static-asserted equal). Out-of-range values are read as
 *                    HAND, for the reason ultrawidelock_ml_los_carry_classify() gives.
 * @param confidence  ultrawidelock_ml_los_confidence() for the same reception, compared
 *                    against cfg.nlos_conf_min exactly as in feed_channel(). A
 *                    sample below the floor votes CLEAR and its carry class is
 *                    discarded with it -- an unconfident guess at WHICH body
 *                    geometry is worth less than the unconfident guess that
 *                    there was one, and that one is already refused.
 *
 * REMEMBER WHAT THE SHIPPED MODEL CAN ACTUALLY RETURN. It is a two-class tree,
 * so POCKET and BAG never arrive and their table entries are dead. See
 * ultrawidelock_ml_los_carry_trained() and docs/bodycal-falsification.md.
 */
enum ultrawidelock_approach_action
ultrawidelock_approach_feed_carry(struct ultrawidelock_approach *ap, int64_t now_ms, int32_t cm,
				  enum ultrawidelock_approach_carry carry, float confidence);
#endif

/**
 * Record a range for the DEPARTURE decision alone, trust gate or no trust gate.
 *
 * Ranges beyond relock_cm are precisely the ones the range-integrity consensus
 * declines to vouch for, so a walk-away can never satisfy a "seen beyond
 * relock_cm" condition through ultrawidelock_approach_feed(). Measured 2026-08-02: the
 * trace showed 252 cm then 309 cm as the credential left, while the controller
 * had last been FED 210 cm, and both the silence rule and far_dwell refused --
 * correctly, on the data they had.
 *
 * Using an unvouched range here is safe in the one direction that matters. The
 * trust gate exists to stop a forged NEAR range opening a door; a forged FAR
 * range can only CLOSE one, and an attacker gains nothing by locking a lock.
 * So departure may read what the radio saw, while the unlock decision keeps
 * requiring what the radio can vouch for.
 *
 * ONE addition to that contract (2026-08-07, 23:51 walk): a >= relock_cm
 * observation also re-arms the trajectory gate. The trust gate declines most of
 * a retreat, so after a mid-session relock the median-based re-arm was
 * unreachable and an owner stood fifteen seconds at 0 cm against a shut door.
 * Arming is necessary and never sufficient -- the unlock still takes a vouched
 * near median plus dwell -- and the resting-at-the-door credential the gate
 * exists for produces no far reading to arm with. See the implementation
 * comment for the full argument, including why session_up() already bounds it.
 *
 * Ignores anything nearer than relock_cm, which is what keeps the asymmetry
 * honest: an unvouched range can cause a relock, may restore permission an
 * earlier approach had already earned, and can never by itself move the bolt.
 * Feed it only FRESH ranges -- the caller's generation epoch says which --
 * or the silence in ultrawidelock_approach_tick() never accumulates.
 */
void ultrawidelock_approach_observe_departure(struct ultrawidelock_approach *ap, int64_t now_ms,
					      int32_t cm);

/* Periodic call while no sample arrived (the controller's idle tick).
 * Supervises an overdue predictive open, and relocks a departure whose far
 * samples stopped before far_dwell could count them (far_silence_ms). */
enum ultrawidelock_approach_action ultrawidelock_approach_tick(struct ultrawidelock_approach *ap,
							       int64_t now_ms);

/* Peer gone (ranging silent past the caller's timeout): reset for the next
 * approach; returns RELOCK_DEPART if the bolt was open, else HOLD. */
enum ultrawidelock_approach_action ultrawidelock_approach_gone(struct ultrawidelock_approach *ap);

/*
 * The caller REFUSED the UNLOCK this controller just returned -- an outside
 * policy (the side gate) would not grant it. Hand the decision back.
 *
 * Both unlock paths clear ap->locked BEFORE returning their action, so without
 * this the controller believes the bolt is open while it is not. Every later
 * unlock test is guarded on ap->locked, so a single refusal disables auto-unlock
 * for the rest of the approach -- the door stays shut even after the refusing
 * policy changes its mind, until a full departure past relock_cm relocks it.
 * Observed: the side gate committed OUTSIDE at confidence 100 for ten straight
 * windows and no unlock was ever re-offered.
 *
 * Safe to call unconditionally after any action; it does nothing if the
 * controller already considers the bolt locked.
 */
void ultrawidelock_approach_veto(struct ultrawidelock_approach *ap);

/* Trace accessors (for the ALAB walk-up report / twin overlays). */
bool ultrawidelock_approach_locked(const struct ultrawidelock_approach *ap);
int32_t ultrawidelock_approach_est_cm(const struct ultrawidelock_approach *ap);   /* -1 = none */
int32_t ultrawidelock_approach_vel_cm_s(const struct ultrawidelock_approach *ap); /* >0 = closing */
int32_t ultrawidelock_approach_eta_ms(const struct ultrawidelock_approach *ap);   /* -1 = none */

/**
 * The debounced channel verdict: true while a strict majority of the median
 * window were confident obstructed calls YOUNGER than the staleness horizon --
 * votes age out with the median entries they rode in on, so a majority earned
 * before a trust hole cannot keep the radius widened through it. This is the
 * exact state the nlos_widen_cm widening consumes, exposed so a walk with the
 * widening still 0 can log where a widened build WOULD have moved its
 * threshold -- the reading an owner needs before choosing that number.
 * Telemetry only: the decision path reads the same state internally and never
 * through here. Constant false without CONFIG_ULTRAWIDELOCK_ML_LOS, like the widening.
 *
 * @param now_ms  the caller's clock, same monotonic base as the feeds; the
 *                verdict is a function of the window AND of when it is asked.
 */
bool ultrawidelock_approach_nlos_blocked(const struct ultrawidelock_approach *ap, int64_t now_ms);

#if defined(CONFIG_ULTRAWIDELOCK_ML_LOS)
/**
 * WHICH carry mode the window voted for, given that it voted obstructed at all.
 *
 * Plurality over exactly the votes ultrawidelock_approach_nlos_blocked() counts: fresh,
 * confident, obstructed. Meaningless -- and returns CLEAR -- whenever that
 * function returns false, which is the same "a partly-filled or stale window
 * widens nothing" rule stated once.
 *
 * TIES BREAK TOWARD THE SMALLEST CONFIGURED WIDENING, not toward the lowest
 * class index, and that is a security choice rather than a tidiness one. Two
 * classes at two votes each is the window not knowing which body geometry it is
 * looking at, and the answer to not knowing is the less permissive threshold.
 * With an unfilled table every entry is equal and the tie is decided by index,
 * which costs nothing because every candidate then yields the same widening.
 *
 * Telemetry, exactly like ultrawidelock_approach_nlos_blocked(): the decision path reads
 * the same state internally.
 */
enum ultrawidelock_approach_carry
ultrawidelock_approach_nlos_carry(const struct ultrawidelock_approach *ap, int64_t now_ms);

/**
 * The unlock radius in force right now, in centimetres: cfg.unlock_cm plus
 * whatever widening the window's carry vote currently selects, or exactly
 * cfg.unlock_cm when it selects none.
 *
 * Every unlock_cm comparison inside the controller goes through the same
 * computation -- the fire decision, the ETA target and the silence rule alike --
 * so this is the number to log beside a walk, and the number a test should assert
 * on rather than re-deriving the table lookup for itself.
 */
int32_t ultrawidelock_approach_effective_unlock_cm(const struct ultrawidelock_approach *ap,
						   int64_t now_ms);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_APPROACH_H */
