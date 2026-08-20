/** @file test_approach.c — predictive approach controller ("negative
 * latency"): ETA-scheduled fire timing, abort invariants, and the preserved
 * median/dwell presence path, replayed over synthetic walk-up profiles —
 * brisk walk, slow shuffle, stop-and-turn, hallway stationary, spike storms.
 * Samples arrive on the real 192 ms ranging-block grid with deterministic
 * uniform jitter, so every run is bit-identical. */
#include <string.h>

#include "ultrawidelock_approach.h"
#include "test.h"

#define BLOCK_MS 192

/* xorshift32: deterministic jitter, reseeded per profile. */
static uint32_t s_rng;

static uint32_t rng32(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static int32_t jitter(int32_t amp)
{
	return (int32_t)(rng32() % (uint32_t)(2 * amp + 1)) - amp;
}

/* Event recorder: count + first-occurrence time of each transition. */
struct rec {
	int n_predict, n_thresh, n_depart, n_abort;
	int64_t t_predict, t_thresh, t_depart, t_abort;
};

static void note(struct rec *r, enum ultrawidelock_approach_action a, int64_t t)
{
	switch (a) {
	case ULTRAWIDELOCK_APPROACH_UNLOCK_PREDICT:
		if (r->n_predict++ == 0) {
			r->t_predict = t;
		}
		break;
	case ULTRAWIDELOCK_APPROACH_UNLOCK_THRESHOLD:
		if (r->n_thresh++ == 0) {
			r->t_thresh = t;
		}
		break;
	case ULTRAWIDELOCK_APPROACH_RELOCK_DEPART:
		if (r->n_depart++ == 0) {
			r->t_depart = t;
		}
		break;
	case ULTRAWIDELOCK_APPROACH_RELOCK_ABORT:
		if (r->n_abort++ == 0) {
			r->t_abort = t;
		}
		break;
	default:
		break;
	}
}

/* A simulated walk-up: true distance moves linearly, one sample per block. */
struct walk {
	struct ultrawidelock_approach ap;
	struct rec r;
	int64_t t; /* ms of the last fed sample */
	float d;   /* true distance, cm */
};

static void walk_init(struct walk *w, uint32_t seed, float d0)
{
	memset(w, 0, sizeof(*w));
	ultrawidelock_approach_init(&w->ap, NULL);
	s_rng = seed;
	w->d = d0;
}

/* Same, with an explicit config (used to run a walk with the prediction path
 * compiled in but disabled, as the RSSI-power-gate builds do). */
static void walk_init_cfg(struct walk *w, uint32_t seed, float d0,
			  const struct ultrawidelock_approach_cfg *cfg)
{
	memset(w, 0, sizeof(*w));
	ultrawidelock_approach_init(&w->ap, cfg);
	s_rng = seed;
	w->d = d0;
}

/* One ranging block: move at v (cm/s, >0 = toward the door), then feed a
 * jittered sample (or the given override, for spike injection). */
static void walk_block(struct walk *w, float v_cm_s, int32_t noise, int32_t override_cm)
{
	w->t += BLOCK_MS;
	w->d -= v_cm_s * (float)BLOCK_MS / 1000.0f;
	int32_t cm = override_cm >= 0 ? override_cm : (int32_t)w->d + jitter(noise);

	note(&w->r, ultrawidelock_approach_feed(&w->ap, w->t, cm), w->t);
}

/* Feed blocks until the true distance passes d_to (either direction). Every
 * spike_every-th block (0 = never) sends spike_cm instead of the range. */
static void walk_until(struct walk *w, float v_cm_s, int32_t noise, float d_to, int spike_every,
		       int32_t spike_cm)
{
	int i = 0;

	while (v_cm_s > 0.0f ? w->d > d_to : w->d < d_to) {
		i++;
		walk_block(w, v_cm_s, noise,
			   (spike_every > 0 && i % spike_every == 0) ? spike_cm : -1);
	}
}

/* n blocks at fixed velocity (v may be 0: standing still). */
static void walk_blocks(struct walk *w, float v_cm_s, int32_t noise, int n)
{
	for (int i = 0; i < n; i++) {
		walk_block(w, v_cm_s, noise, -1);
	}
}

void test_approach(void)
{
	struct walk w;
	struct ultrawidelock_approach_cfg cfg;

	t_group("defaults + idle accessors");
	ultrawidelock_approach_defaults(&cfg);
	T_EQ("def.unlock_cm", cfg.unlock_cm, 100);
	T_EQ("def.relock_cm", cfg.relock_cm, 250);
	T_EQ("def.near_dwell", cfg.near_dwell, 2);
	T_EQ("def.far_dwell", cfg.far_dwell, 3);
	T_EQ("def.motor_ms", cfg.motor_ms, 500);
	T_EQ("def.margin_ms", cfg.margin_ms, 250);
	T_EQ("def.vmin_cm_s", cfg.vmin_cm_s, 30);
	T_EQ("def.far_silence_ms", cfg.far_silence_ms, 750);
	/* Strictly between unlock_cm and relock_cm, and the upper bound is the
	 * one that matters: at or above relock_cm the gate can never arm,
	 * because the trust gate does not vouch for ranges that far out. */
	T_EQ("def.approach_cm", cfg.approach_cm, 180);
	T_OK("def.approach_cm.below.relock", cfg.approach_cm < cfg.relock_cm);
	T_OK("def.approach_cm.above.unlock", cfg.approach_cm > cfg.unlock_cm);
	walk_init(&w, 1, 600.0f);
	T_EQ("idle.cfg.null.defaults", w.ap.cfg.unlock_cm, 100);
	T_OK("idle.locked", ultrawidelock_approach_locked(&w.ap));
	T_EQ("idle.est", ultrawidelock_approach_est_cm(&w.ap), -1);
	T_EQ("idle.vel", ultrawidelock_approach_vel_cm_s(&w.ap), 0);
	T_EQ("idle.eta", ultrawidelock_approach_eta_ms(&w.ap), -1);

	t_group("brisk walk 1.3 m/s: bolt open before the hand arrives");
	/* True ring (100 cm) crossing at (600-100)/130 = 3846 ms; reach
	 * (60 cm) at 4153 ms. The prediction must fire once, no aborts, and
	 * fire + motor (500 ms) must land before reach — that is the negative
	 * latency: retraction DONE by arrival, not started at it. */
	walk_init(&w, 0xB01DFACE, 600.0f);
	walk_until(&w, 130.0f, 25, 300.0f, 0, 0);
	T_OK("brisk.mid.est",
	     ultrawidelock_approach_est_cm(&w.ap) > 230 && ultrawidelock_approach_est_cm(&w.ap) < 380);
	T_OK("brisk.mid.vel",
	     ultrawidelock_approach_vel_cm_s(&w.ap) > 80 && ultrawidelock_approach_vel_cm_s(&w.ap) < 180);
	walk_until(&w, 130.0f, 25, 55.0f, 0, 0);
	T_EQ("brisk.predict.once", w.r.n_predict, 1);
	T_EQ("brisk.thresh.none", w.r.n_thresh, 0);
	T_EQ("brisk.abort.none", w.r.n_abort, 0);
	T_OK("brisk.unlocked", !ultrawidelock_approach_locked(&w.ap));
	T_OK("brisk.done.by.reach", w.r.t_predict + 500 <= 4153);
	T_OK("brisk.fire.bounded", w.r.t_predict >= 3846 - 1500);

	t_group("spike storm: metre-scale outliers shift nothing");
	/* Same brisk walk, every 6th block replaced by a 16 m spike (the
	 * bench-observed failure mode). Guarantees must hold unchanged. */
	walk_init(&w, 0xB01DFACE, 600.0f);
	walk_until(&w, 130.0f, 25, 55.0f, 6, 1600);
	T_EQ("spike.predict.once", w.r.n_predict, 1);
	T_EQ("spike.abort.none", w.r.n_abort, 0);
	T_OK("spike.done.by.reach", w.r.t_predict + 500 <= 4153 + BLOCK_MS);
	T_OK("spike.fire.bounded", w.r.t_predict >= 3846 - 1500);

	t_group("near spoof while stationary: no fire from downward spikes");
	/* Standing at 3 m; every 5th sample claims 70 cm. The gate drops them
	 * and the median outvotes them: the bolt never moves. */
	walk_init(&w, 0x5EED0005, 300.0f);
	for (int i = 1; i <= 100; i++) {
		walk_block(&w, 0.0f, 20, i % 5 == 0 ? 70 : -1);
	}
	T_EQ("spoof.predict.none", w.r.n_predict, 0);
	T_EQ("spoof.thresh.none", w.r.n_thresh, 0);
	T_OK("spoof.locked", ultrawidelock_approach_locked(&w.ap));

	t_group("slow shuffle 0.15 m/s: threshold path still unlocks");
	/* Below vmin (30 cm/s) the predictor stays quiet; the shipped
	 * median/dwell presence unlock carries the case. Ring crossing at
	 * (220-100)/15 = 8000 ms. */
	walk_init(&w, 0xCAFE0003, 220.0f);
	walk_until(&w, 15.0f, 20, 70.0f, 0, 0);
	T_EQ("shuffle.predict.none", w.r.n_predict, 0);
	T_EQ("shuffle.thresh.once", w.r.n_thresh, 1);
	T_OK("shuffle.unlocked", !ultrawidelock_approach_locked(&w.ap));
	T_OK("shuffle.at.crossing", w.r.t_thresh >= 7000 && w.r.t_thresh <= 10500);

	t_group("hallway stationary at 1.6 m: never pre-actuates");
	walk_init(&w, 0xDEAD0004, 160.0f);
	walk_blocks(&w, 0.0f, 25, 156); /* ~30 s */
	T_EQ("hall.predict.none", w.r.n_predict, 0);
	T_EQ("hall.thresh.none", w.r.n_thresh, 0);
	T_OK("hall.locked", ultrawidelock_approach_locked(&w.ap));

	t_group("standing inside the ring: REFUSED, the trajectory gate's whole point");
	/*
	 * This test used to assert the opposite -- a credential appearing at
	 * 80 cm and standing still opened the door in under a second, named
	 * "presence unlock preserved". That is the correct answer for a phone
	 * walking up and the wrong one for a phone that never left the house,
	 * and not opening while we are inside is the problem this lock exists to
	 * solve. cfg.approach_cm now refuses it: nothing here is ever seen at
	 * 180 cm, so the gate never arms.
	 */
	walk_init(&w, 0xFEED0006, 80.0f);
	walk_blocks(&w, 0.0f, 15, 20);
	T_EQ("stand.thresh.none", w.r.n_thresh, 0);
	T_EQ("stand.predict.none", w.r.n_predict, 0);
	T_OK("stand.locked", ultrawidelock_approach_locked(&w.ap));

	t_group("approach then stand: the same standstill DOES unlock, once earned");
	/* The counterpart, and the reason the gate is a gate rather than a ban
	 * on slow arrivals: walk in from beyond approach_cm, then stand exactly
	 * as above. The shuffle-up and stand-at-door cases the presence path
	 * exists for are untouched -- only the credential that never approached
	 * is refused. */
	walk_init(&w, 0xFEED0106, 200.0f);
	walk_until(&w, 20.0f, 15, 80.0f, 0, 0);
	walk_blocks(&w, 0.0f, 15, 20);
	T_EQ("earned.thresh.once", w.r.n_thresh, 1);
	T_OK("earned.unlocked", !ultrawidelock_approach_locked(&w.ap));

	t_group("trajectory gate disabled: the old behaviour is still reachable");
	/* approach_cm = 0 restores exactly what shipped before the gate, so a
	 * deployment that wants stand-at-door unlocking can have it back without
	 * a source change. */
	ultrawidelock_approach_defaults(&cfg);
	cfg.approach_cm = 0;
	walk_init_cfg(&w, 0xFEED0206, 80.0f, &cfg);
	walk_blocks(&w, 0.0f, 15, 20);
	T_EQ("ungated.thresh.once", w.r.n_thresh, 1);
	T_OK("ungated.fast", w.r.t_thresh <= 1000);

	t_group("gate re-arms per session: a relock demands a fresh approach");
	/* After a departure the credential comes back WITHOUT going out past
	 * approach_cm again -- it reappears at the door. It must not open. This
	 * is the session-gap case: iOS drops ranging, the session re-establishes
	 * near the door, and a gate that stayed armed would hand out a free
	 * unlock on every gap. */
	walk_init(&w, 0xFEED0306, 200.0f);
	walk_until(&w, 20.0f, 15, 80.0f, 0, 0);
	T_EQ("rearm.first.once", w.r.n_thresh, 1);
	ultrawidelock_approach_gone(&w.ap);
	memset(&w.r, 0, sizeof(w.r));
	w.d = 80.0f;
	walk_blocks(&w, 0.0f, 15, 20);
	T_EQ("rearm.second.none", w.r.n_thresh, 0);
	T_OK("rearm.locked", ultrawidelock_approach_locked(&w.ap));

	t_group("stop short after fire: predictive open aborts");
	/* Fires on the approach, then the walker halts at ~1.7 m and stands.
	 * Closing-speed decay (or the overdue deadline) must relock well
	 * before the peer would even time out. */
	walk_init(&w, 0xAB007007, 600.0f);
	walk_until(&w, 130.0f, 25, 170.0f, 0, 0);
	T_EQ("stop.predict.once", w.r.n_predict, 1);
	int64_t t_stop = w.t;

	walk_blocks(&w, 0.0f, 25, 20); /* stand for ~3.8 s */
	T_EQ("stop.abort.once", w.r.n_abort, 1);
	T_EQ("stop.thresh.none", w.r.n_thresh, 0);
	T_OK("stop.relocked", ultrawidelock_approach_locked(&w.ap));
	T_OK("stop.abort.fast", w.r.t_abort - t_stop <= 2500);

	t_group("departure: walk away relocks via far dwell");
	walk_init(&w, 0xD00D0008, 400.0f);
	walk_until(&w, 130.0f, 20, 70.0f, 0, 0);
	T_OK("depart.unlocked", !ultrawidelock_approach_locked(&w.ap));
	walk_until(&w, -160.0f, 20, 420.0f, 0, 0);
	T_EQ("depart.once", w.r.n_depart, 1);
	T_OK("depart.relocked", ultrawidelock_approach_locked(&w.ap));
	T_EQ("depart.gone.idle", ultrawidelock_approach_gone(&w.ap), ULTRAWIDELOCK_APPROACH_HOLD);

	t_group("peer gone while open: relock + reset");
	walk_init(&w, 0xD00D0009, 400.0f);
	walk_until(&w, 130.0f, 20, 70.0f, 0, 0);
	T_OK("gone.unlocked", !ultrawidelock_approach_locked(&w.ap));
	T_EQ("gone.relock", ultrawidelock_approach_gone(&w.ap), ULTRAWIDELOCK_APPROACH_RELOCK_DEPART);
	T_OK("gone.locked", ultrawidelock_approach_locked(&w.ap));
	T_EQ("gone.est.reset", ultrawidelock_approach_est_cm(&w.ap), -1);

	t_group("ranging dies after fire: overdue tick aborts");
	walk_init(&w, 0xF00D000A, 600.0f);
	for (int i = 0; i < 40 && w.r.n_predict == 0; i++) {
		walk_block(&w, 130.0f, 25, -1);
	}
	T_EQ("overdue.fired", w.r.n_predict, 1);
	int32_t eta = ultrawidelock_approach_eta_ms(&w.ap);

	T_OK("overdue.eta.sane", eta >= 0 && eta <= 750);
	for (int k = 1; k <= 20 && w.r.n_abort == 0; k++) {
		int64_t tk = w.t + (int64_t)k * 200;

		note(&w.r, ultrawidelock_approach_tick(&w.ap, tk), tk);
	}
	T_EQ("overdue.abort.once", w.r.n_abort, 1);
	/* 1800 is PRED_GRACE_MS: silence tolerance during a predictive open. It
	 * was 900 until the 2026-08-07 walk measured 0.9-1.5 s holes in the
	 * accepted stream of a LIVE approach; the deadline must outlast those,
	 * and the extra ~0.9 s of bolt-open on a genuinely dead session is the
	 * price. The 250 covers the tick cadence. */
	T_OK("overdue.by.deadline", w.r.t_abort - w.r.t_predict <= (int64_t)eta + 1800 + 250);
	T_OK("overdue.relocked", ultrawidelock_approach_locked(&w.ap));

	t_group("teleport: gate rejects, then re-bases; predictor disarms");
	walk_init(&w, 0x7E1E000B, 500.0f);
	walk_blocks(&w, 0.0f, 20, 10);
	w.d = 150.0f; /* impossible jump; 3 agreeing samples re-base */
	walk_blocks(&w, 0.0f, 20, 20);
	T_EQ("tele.predict.none", w.r.n_predict, 0);
	T_EQ("tele.thresh.none", w.r.n_thresh, 0); /* dead band */
	T_OK("tele.est.follows",
	     ultrawidelock_approach_est_cm(&w.ap) > 120 && ultrawidelock_approach_est_cm(&w.ap) < 180);
	T_OK("tele.vel.settled",
	     ultrawidelock_approach_vel_cm_s(&w.ap) > -25 && ultrawidelock_approach_vel_cm_s(&w.ap) < 25);

	t_group("estimator edges: stale gap re-bases, zero-dt clamps");
	walk_init(&w, 0x0C0C000C, 0.0f);
	note(&w.r, ultrawidelock_approach_feed(&w.ap, 1000, 400), 1000);
	note(&w.r, ultrawidelock_approach_feed(&w.ap, 2600, 380), 2600); /* > stale */
	T_EQ("stale.rebased.est", ultrawidelock_approach_est_cm(&w.ap), 380);
	T_EQ("stale.rebased.vel", ultrawidelock_approach_vel_cm_s(&w.ap), 0);
	note(&w.r, ultrawidelock_approach_feed(&w.ap, 2600, 380), 2600); /* dt clamp */
	T_OK("clamp.est.stable",
	     ultrawidelock_approach_est_cm(&w.ap) > 370 && ultrawidelock_approach_est_cm(&w.ap) < 390);

	t_group("predict_en=0: prediction path off, presence path untouched");
	/* The RSSI-power-gate build. Same brisk walk that fires a prediction
	 * above; with the path disabled the bolt must still open, but only on
	 * the shipped threshold rule, and no ETA may ever be published. */
	ultrawidelock_approach_defaults(&cfg);
	T_OK("def.predict_en", cfg.predict_en);
	cfg.predict_en = false;
	walk_init_cfg(&w, 0xB01DFACE, 600.0f, &cfg);
	walk_until(&w, 130.0f, 25, 55.0f, 0, 0);
	/* The estimator itself keeps running: `vel` still feeds the lab trace,
	 * it just cannot arm the bolt. */
	T_OK("off.vel.tracked",
	     ultrawidelock_approach_vel_cm_s(&w.ap) > 80 && ultrawidelock_approach_vel_cm_s(&w.ap) < 180);
	T_EQ("off.eta.never", ultrawidelock_approach_eta_ms(&w.ap), -1);
	/* Arriving and stopping: without the predictor the median has to settle
	 * inside the ring first, which is precisely the latency the prediction
	 * path exists to remove. */
	walk_blocks(&w, 0.0f, 25, 5);
	T_EQ("off.predict.none", w.r.n_predict, 0);
	T_EQ("off.abort.none", w.r.n_abort, 0);
	T_EQ("off.thresh.once", w.r.n_thresh, 1);
	T_OK("off.unlocked", !ultrawidelock_approach_locked(&w.ap));
	/* Departure still relocks on the presence path. */
	walk_until(&w, -130.0f, 25, 400.0f, 0, 0);
	T_EQ("off.depart.once", w.r.n_depart, 1);
	T_OK("off.relocked", ultrawidelock_approach_locked(&w.ap));
	/*
	 * The walk-away that hardware actually produces: a couple of far
	 * samples and then nothing, because the phone leaves UWB range or iOS
	 * stops ranging. far_dwell needs three and never gets them.
	 */
	t_group("departure by silence: two far samples then quiet still relocks");
	{
		struct walk d;

		walk_init(&d, 7, 600.0f);
		walk_until(&d, 130.0f, 10, 40.0f, 0, 0); /* arrive and unlock */
		T_OK("silence.unlocked", !ultrawidelock_approach_locked(&d.ap));

		/* Two far readings -- one short of far_dwell -- then silence. */
		d.t += BLOCK_MS;
		note(&d.r, ultrawidelock_approach_feed(&d.ap, d.t, 380), d.t);
		d.t += BLOCK_MS;
		note(&d.r, ultrawidelock_approach_feed(&d.ap, d.t, 420), d.t);
		T_OK("silence.still.open.after.two", !ultrawidelock_approach_locked(&d.ap));

		T_EQ("silence.nothing.before.the.window",
		     (long)ultrawidelock_approach_tick(&d.ap, d.t + 500), (long)ULTRAWIDELOCK_APPROACH_HOLD);
		T_EQ("silence.relocks.after.it", (long)ultrawidelock_approach_tick(&d.ap, d.t + 750),
		     (long)ULTRAWIDELOCK_APPROACH_RELOCK_DEPART);
		T_OK("silence.locked", ultrawidelock_approach_locked(&d.ap));
		T_EQ("silence.only.once", (long)ultrawidelock_approach_tick(&d.ap, d.t + 9000),
		     (long)ULTRAWIDELOCK_APPROACH_HOLD);
	}

	/*
	 * The case the gate protects: a phone put down INSIDE the ring also
	 * stops ranging, and relocking under its owner is the worse failure.
	 */
	t_group("silence while near: the bolt stays put");
	{
		struct walk n;

		walk_init(&n, 11, 600.0f);
		walk_until(&n, 130.0f, 10, 40.0f, 0, 0);
		T_OK("near.unlocked", !ultrawidelock_approach_locked(&n.ap));

		T_EQ("near.silence.holds", (long)ultrawidelock_approach_tick(&n.ap, n.t + 60000),
		     (long)ULTRAWIDELOCK_APPROACH_HOLD);
		T_OK("near.still.open", !ultrawidelock_approach_locked(&n.ap));
	}

	/*
	 * The walk-away hardware actually produces: the far ranges arrive but
	 * the integrity consensus will not vouch for them, so nothing is FED at
	 * all and only observe_departure() sees them.
	 */
	t_group("departure by silence: an unvouched far range still relocks");
	{
		struct walk u;

		walk_init(&u, 17, 600.0f);
		walk_until(&u, 130.0f, 10, 40.0f, 0, 0);
		T_OK("unvouched.unlocked", !ultrawidelock_approach_locked(&u.ap));

		/* Nothing fed from here on -- only observed. */
		ultrawidelock_approach_observe_departure(&u.ap, u.t + 200, 309);
		T_EQ("unvouched.holds.inside.the.window",
		     (long)ultrawidelock_approach_tick(&u.ap, u.t + 600), (long)ULTRAWIDELOCK_APPROACH_HOLD);
		T_EQ("unvouched.relocks.after.it", (long)ultrawidelock_approach_tick(&u.ap, u.t + 1000),
		     (long)ULTRAWIDELOCK_APPROACH_RELOCK_DEPART);
		T_OK("unvouched.locked", ultrawidelock_approach_locked(&u.ap));
	}

	/*
	 * The asymmetry, stated as a test: an unvouched range may CLOSE a door
	 * and may never hold one open. A near observation is discarded, so it
	 * cannot refresh the departure clock and stall a relock.
	 */
	t_group("an unvouched NEAR range changes nothing");
	{
		struct walk k;

		walk_init(&k, 19, 600.0f);
		walk_until(&k, 130.0f, 10, 40.0f, 0, 0);
		ultrawidelock_approach_observe_departure(&k.ap, k.t + 100, 380);

		/* A near reading arrives mid-window and must be ignored. */
		ultrawidelock_approach_observe_departure(&k.ap, k.t + 500, 12);
		T_EQ("near.observation.cannot.delay.the.relock",
		     (long)ultrawidelock_approach_tick(&k.ap, k.t + 900),
		     (long)ULTRAWIDELOCK_APPROACH_RELOCK_DEPART);
	}

	/* far_silence_ms == 0 restores the sample-only behaviour exactly. */
	t_group("departure by silence: disabled by config");
	{
		struct walk z;
		struct ultrawidelock_approach_cfg zcfg;

		ultrawidelock_approach_defaults(&zcfg);
		zcfg.far_silence_ms = 0;
		walk_init_cfg(&z, 13, 600.0f, &zcfg);
		walk_until(&z, 130.0f, 10, 40.0f, 0, 0);
		z.t += BLOCK_MS;
		note(&z.r, ultrawidelock_approach_feed(&z.ap, z.t, 380), z.t);

		T_EQ("off.silence.never.fires", (long)ultrawidelock_approach_tick(&z.ap, z.t + 60000),
		     (long)ULTRAWIDELOCK_APPROACH_HOLD);
		T_OK("off.still.open", !ultrawidelock_approach_locked(&z.ap));
	}

	t_group("channel correction: calibration always, obstruction only on a majority");
	{
		struct ultrawidelock_approach ap;
		struct ultrawidelock_approach_cfg cfg;

		/*
		 * EVERY case here arms the trajectory gate first, at 260 cm, and that is
		 * not boilerplate. approach_cm is 180, so a credential that never
		 * appears beyond it cannot unlock at all -- a constant 100 cm stream
		 * stays locked whatever the correction does, and a "stays locked" check
		 * written without the arming phase passes for the gate's reason rather
		 * than the one it claims to test. 260 clears 180 under all three
		 * conditions: uncorrected 260, corrected clear 285, corrected obstructed
		 * 201.
		 */
#define ARM_CM 260
#define ARM_N  6
#define HOLD_N 12

		/* OFF by default: the offset is one board's, and modules/ builds for
		 * three targets. A default that shifted every range would be wrong on
		 * two of them, so the whole suite above pins the uncorrected path. */
		ultrawidelock_approach_defaults(&cfg);
		T_OK("correction.off.by.default", !cfg.range_correct_en);

		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, false, 0.0f);
		}
		for (int i = 0; i < HOLD_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 100, false, 0.0f);
		}
		T_OK("default.100cm.unlocks.as.before", !ultrawidelock_approach_locked(&ap));

		ultrawidelock_approach_defaults(&cfg);
		cfg.range_correct_en = true;

		/*
		 * Same stream, correction on. 100 cm reported is a true ~126, outside
		 * the radius, so it must NOT unlock. The gate is armed in both cases, so
		 * the only difference between this and the check above is the
		 * calibration -- which is the "makes the lock stricter" half of Result
		 * 19 and the one a regression would silently undo.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, false, 0.0f);
		}
		for (int i = 0; i < HOLD_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 100, false, 0.0f);
		}
		T_OK("clear.100cm.stays.locked.after.calibration", ultrawidelock_approach_locked(&ap));

		/* 70 cm reported is a true ~96, inside it. */
		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, false, 0.0f);
		}
		for (int i = 0; i < HOLD_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 70, false, 0.0f);
		}
		T_OK("clear.70cm.unlocks", !ultrawidelock_approach_locked(&ap));

		/*
		 * The pocketed owner: a true 100 cm reads 155. The next three cases feed
		 * IDENTICAL ranges and timings and differ only in what the classifier
		 * said, so no pass among them can come from the range.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, false, 0.0f);
		}
		for (int i = 0; i < HOLD_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 155, false, 0.0f);
		}
		T_OK("blocked.155cm.unclassified.stays.locked", ultrawidelock_approach_locked(&ap));

		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, true, 3.0f);
		}
		for (int i = 0; i < HOLD_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 155, true, 3.0f);
		}
		T_OK("blocked.155cm.confident.unlocks", !ultrawidelock_approach_locked(&ap));

		/* Below the confidence floor a sample votes CLEAR rather than being
		 * dropped: a weak reading is evidence about the channel, not a gap. */
		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, true, 1.0f);
		}
		for (int i = 0; i < HOLD_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 155, true, 1.0f);
		}
		T_OK("blocked.low.confidence.stays.locked", ultrawidelock_approach_locked(&ap));

		/* Two in five is not a majority. The arming phase is deliberately CLEAR
		 * here: feeding it obstructed would leave five confident votes in the
		 * window and the correction would still be engaged while the minority
		 * pattern washed through, which is the hysteresis pinned below rather
		 * than the vote rule under test. */
		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, false, 0.0f);
		}
		for (int i = 0; i < 15; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 155, (i % 5) < 2,
							  3.0f);
		}
		T_OK("blocked.minority.vote.stays.locked", ultrawidelock_approach_locked(&ap));

		/*
		 * HYSTERESIS, pinned because it is a real cost of voting over a window
		 * and not an accident. When obstruction ends, the reported range drops
		 * by 84.5 cm immediately while the window keeps voting obstructed for up
		 * to ULTRAWIDELOCK_APPROACH_MEDIAN_N more samples, so for that long the
		 * controller subtracts a bias that is no longer there and believes the
		 * credential is ~84 cm nearer than it is. At the measured 0.762 s
		 * reception interval that is a window of a few seconds.
		 *
		 * It is bounded and it is the same failure the vote rule is there to
		 * make rare, arriving by a different route. This check exists so that
		 * anyone lengthening the window sees the cost.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < ARM_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, ARM_CM, true, 3.0f);
		}
		/* True 200 cm, obstructed: reported 259, corrected back to 200. */
		for (int i = 0; i < HOLD_N; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 3000 + i * 200, 259, true, 3.0f);
		}
		T_OK("blocked.at.true.200.stays.locked", ultrawidelock_approach_locked(&ap));
		/* The subject turns: still a true 200, now reported 175. Two samples is
		 * not enough to turn the window over, so the stale obstructed vote
		 * subtracts 84.5 that is no longer there. */
		(void)ultrawidelock_approach_feed_channel(&ap, 6000, 175, false, 0.0f);
		(void)ultrawidelock_approach_feed_channel(&ap, 6200, 175, false, 0.0f);
		T_OK("stale.obstructed.vote.moves.the.estimate.inward",
		     ultrawidelock_approach_est_cm(&ap) < 175);

#undef ARM_CM
#undef ARM_N
#undef HOLD_N
	}

	t_group("2026-08-07 walk-up regressions");
	{
		struct ultrawidelock_approach ap;
		struct ultrawidelock_approach_cfg cfg;

		/*
		 * THE 21.5 SECOND UNLOCK. Ranging came up at 55 cm and the bolt ignored
		 * a phone sitting between 0 and 57 cm for fourteen seconds, because
		 * approach_cm wanted a 180 cm sighting the RSSI gate guarantees will
		 * never arrive -- it holds ranging off until the phone is already at the
		 * door. Replayed here: a session that starts inside the radius and never
		 * leaves it.
		 */
		ultrawidelock_approach_defaults(&cfg);
		ultrawidelock_approach_init(&ap, &cfg);
		for (int i = 0; i < 10; i++) {
			(void)ultrawidelock_approach_feed(&ap, 1000 + i * 200, 55);
		}
		T_OK("session.starting.inside.the.radius.still.will.not.unlock.unarmed",
		     ultrawidelock_approach_locked(&ap));

		/* With the session edge reported, the same stream unlocks promptly. */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 10; i++) {
			(void)ultrawidelock_approach_feed(&ap, 1000 + i * 200, 55);
		}
		T_OK("session.up.arms.the.gate.and.the.walk.up.unlocks",
		     !ultrawidelock_approach_locked(&ap));

		/*
		 * THE 11 SECOND RELOCK. The owner left, the last vouched range was
		 * 207 cm, the session died, and nothing relocked until ranging resumed
		 * at 427 cm. 207 sits in the 100-250 dead band, and the silence rule
		 * used to demand relock_cm.
		 *
		 * The band now waits BAND_SILENCE_MS rather than far_silence_ms: the
		 * 2026-08-08 pocketed walk showed 750 ms of band silence is a routine
		 * trust hole mid-arrival, not a departure. 2.5 s here still beats the
		 * 11 s this test was written against by a factor of four.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 10; i++) {
			(void)ultrawidelock_approach_feed(&ap, 1000 + i * 200, 55);
		}
		T_OK("open.before.the.walk.away", !ultrawidelock_approach_locked(&ap));
		(void)ultrawidelock_approach_feed(&ap, 3200, 207);
		(void)ultrawidelock_approach_feed(&ap, 3400, 207);
		(void)ultrawidelock_approach_feed(&ap, 3600, 207);
		T_EQ("silence.in.the.dead.band.holds.through.a.pocket.gap",
		     (long)ultrawidelock_approach_tick(&ap, 3600 + cfg.far_silence_ms + 1),
		     (long)ULTRAWIDELOCK_APPROACH_HOLD);
		T_EQ("silence.in.the.dead.band.relocks",
		     (long)ultrawidelock_approach_tick(&ap, 3600 + 2500 + 1),
		     (long)ULTRAWIDELOCK_APPROACH_RELOCK_DEPART);

		/* And the protection the old threshold was reaching for survives: a
		 * phone resting INSIDE the unlock radius still goes quiet without the
		 * bolt moving under its owner. */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 10; i++) {
			(void)ultrawidelock_approach_feed(&ap, 1000 + i * 200, 55);
		}
		T_EQ("silence.at.the.door.does.not.relock",
		     (long)ultrawidelock_approach_tick(&ap, 3000 + cfg.far_silence_ms + 1),
		     (long)ULTRAWIDELOCK_APPROACH_HOLD);
	}

	t_group("nlos widening (Result 21)");
	{
		struct ultrawidelock_approach ap;
		struct ultrawidelock_approach_cfg cfg;
		const float conf = 9.0f; /* comfortably over nlos_conf_min */

		/*
		 * 150 cm is the whole point: a pocketed owner standing at a true ~70 cm
		 * reports themselves out here, and unlock_cm is 100. Without a widening
		 * the bolt never moves, which is the complaint that started this.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_cm = 80;
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, true, conf);
		}
		T_OK("obstructed.window.unlocks.at.150", !ultrawidelock_approach_locked(&ap));

		/* The same stream classified CLEAR must not. This is the control that
		 * makes the test above mean anything: without it the assertion would
		 * pass just as well with the widening deleted and unlock_cm raised. */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, false, conf);
		}
		T_OK("clear.window.stays.locked.at.150", ultrawidelock_approach_locked(&ap));

		/* Obstructed but UNCONFIDENT is the same as clear: the confidence floor
		 * is one of the three gates, and a widening that ignored it would engage
		 * on the classifier's worst quartile. */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, true,
							  cfg.nlos_conf_min - 0.01f);
		}
		T_OK("unconfident.obstructed.stays.locked", ultrawidelock_approach_locked(&ap));

		/* A minority of the window does not widen. Two of five obstructed, and
		 * NLOS_VOTES_N is three. */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, (i % 5) < 2,
							  conf);
		}
		T_OK("minority.vote.stays.locked", ultrawidelock_approach_locked(&ap));

		/*
		 * Silence at the widened radius must NOT relock. A pocketed phone
		 * resting at the door reports 150; if the silence rule kept using the
		 * unwidened 100 it would read that as a departure and shut the bolt
		 * under the owner it just opened for.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, true, conf);
		}
		T_OK("open.before.the.silence", !ultrawidelock_approach_locked(&ap));
		T_EQ("silence.inside.the.widened.radius.does.not.relock",
		     (long)ultrawidelock_approach_tick(&ap, 2600 + cfg.far_silence_ms + 1),
		     (long)ULTRAWIDELOCK_APPROACH_HOLD);

		/* Default config widens nothing, so every existing expectation holds. */
		ultrawidelock_approach_defaults(&cfg);
		T_EQ("widening.is.off.by.default", (long)cfg.nlos_widen_cm, 0L);

		/*
		 * The clamp. unlock_cm 100 and approach_cm 180 leave 79 cm of room, so a
		 * request for 500 comes back as 79 and the trajectory gate keeps a
		 * centimetre of daylight above the effective radius. Without this a
		 * widened sample would arm the gate and fire it at once, reopening the
		 * hole 574dbb91 closed.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_cm = 500;
		ultrawidelock_approach_init(&ap, &cfg);
		T_EQ("widening.clamped.under.approach_cm", (long)ap.cfg.nlos_widen_cm, 79L);

		/* Negative is nonsense in this direction (stricter only while
		 * obstructed) and is clamped away rather than honoured. */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_cm = -50;
		ultrawidelock_approach_init(&ap, &cfg);
		T_EQ("negative.widening.clamped.to.zero", (long)ap.cfg.nlos_widen_cm, 0L);

		/*
		 * The trace accessor is the walk's telemetry and must report exactly
		 * the state the widening consumes: false while the window is short,
		 * true once a confident majority lands, false again once clear
		 * samples wash the window. A divergence here would make the mlgate
		 * log lie about what the controller would have done.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_cm = 80;
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		T_OK("vote.accessor.starts.clear", !ultrawidelock_approach_nlos_blocked(&ap, 1000));
		for (int i = 0; i < 5; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, true, conf);
		}
		T_OK("vote.accessor.reports.blocked", ultrawidelock_approach_nlos_blocked(&ap, 1800));
		for (int i = 0; i < 5; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 2000 + i * 200, 150, false, conf);
		}
		T_OK("vote.accessor.clears.after.wash", !ultrawidelock_approach_nlos_blocked(&ap, 2800));

		/*
		 * Votes age with the median entries they share timestamps with. An
		 * obstructed majority earned before a trust hole must not keep the
		 * radius widened through it: every vote below is still TRUE in
		 * ch_win -- nothing overwrote them -- and past MEDIAN_STALE_MS they
		 * simply stop counting, so the effective threshold is unlock_cm
		 * again the moment the hole outlives the horizon.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_cm = 80;
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 5; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, true, conf);
		}
		T_OK("vote.fresh.majority.widens", ultrawidelock_approach_nlos_blocked(&ap, 1800));
		T_OK("vote.stale.majority.does.not", !ultrawidelock_approach_nlos_blocked(&ap, 4400));

		/* A fresh 3-of-5 majority still widens: the eligible-voter set
		 * changed, the NLOS_VOTES_N arithmetic did not. */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 5; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 1000 + i * 200, 150, i < 3, conf);
		}
		T_OK("vote.three.of.five.fresh.widens", ultrawidelock_approach_nlos_blocked(&ap, 1800));
		for (int i = 0; i < 3; i++) {
			(void)ultrawidelock_approach_feed_channel(&ap, 2000 + i * 200, 150, true, conf);
		}
		T_OK("vote.widened.unlock.still.fires", !ultrawidelock_approach_locked(&ap));
	}

	t_group("per-carry-mode widening (BodyCal)");
	{
		struct ultrawidelock_approach ap;
		struct ultrawidelock_approach_cfg cfg;
		const float conf = 9.0f; /* comfortably over nlos_conf_min */

		/*
		 * THE BACKWARD-COMPATIBILITY CONTRACT, first, because everything below
		 * is only safe if this holds: an untouched table is all zeros, every
		 * zero entry means "use nlos_widen_cm", and a binary feed_channel()
		 * caller therefore behaves exactly as it did before the table existed.
		 * The single-number group above is the rest of that assertion; this
		 * pins the two feeds to the same answer on the same stream.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_cm = 80;
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_HAND,
								conf);
		}
		T_OK("carry.hand.matches.binary.obstructed", !ultrawidelock_approach_locked(&ap));
		/* Read the widening back off ap.cfg, not cfg: 100 + 80 lands exactly on
		 * approach_cm, so init clamps it to 79 and an assertion written against
		 * the requested 80 would be testing the clamp by accident. */
		T_EQ("carry.empty.table.falls.through.to.single.number",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 2400),
		     (long)(ap.cfg.unlock_cm + ap.cfg.nlos_widen_cm));

		/* CLEAR through the carry feed is the clear stream: no widening, and
		 * the control that stops the assertion above passing for the wrong
		 * reason. */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_CLEAR,
								conf);
		}
		T_OK("carry.clear.stays.locked.at.150", ultrawidelock_approach_locked(&ap));
		T_EQ("carry.clear.selects.no.widening",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 2400),
		     (long)cfg.unlock_cm);

		/*
		 * THE POINT OF THE WHOLE TABLE. Same range, same confidence, same
		 * majority -- only the carry class differs, and the threshold that
		 * results differs with it. A bag is widened to 100 + 70 = 170 and
		 * opens at 150; a pocket is widened to 100 + 20 = 120 and does not.
		 * Neither number is measured; they are chosen here to be far apart
		 * enough that a table that was silently ignored could not pass both.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_POCKET] = 20;
		cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_BAG] = 70;
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_BAG,
								conf);
		}
		T_EQ("carry.bag.selects.its.own.entry",
		     (long)ultrawidelock_approach_nlos_carry(&ap, 2400),
		     (long)ULTRAWIDELOCK_APPROACH_CARRY_BAG);
		T_EQ("carry.bag.widens.by.70",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 2400),
		     (long)(cfg.unlock_cm + 70));
		T_OK("carry.bag.unlocks.at.150", !ultrawidelock_approach_locked(&ap));

		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_POCKET,
								conf);
		}
		T_EQ("carry.pocket.selects.its.own.entry",
		     (long)ultrawidelock_approach_nlos_carry(&ap, 2400),
		     (long)ULTRAWIDELOCK_APPROACH_CARRY_POCKET);
		T_EQ("carry.pocket.widens.by.20",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 2400),
		     (long)(cfg.unlock_cm + 20));
		T_OK("carry.pocket.stays.locked.at.150", ultrawidelock_approach_locked(&ap));

		/*
		 * A class with no entry of its own and no nlos_widen_cm to fall back on
		 * widens nothing. HAND is untouched in the config above, so an
		 * in-hand-obstructed window gets 100 + 0 even while pocket and bag are
		 * tuned -- the table is per class, not a blanket.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_HAND,
								conf);
		}
		T_EQ("carry.untuned.class.widens.nothing",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 2400),
		     (long)cfg.unlock_cm);
		T_OK("carry.untuned.class.stays.locked", ultrawidelock_approach_locked(&ap));

		/*
		 * THE SAME MAJORITY RULE, not a second one. Two bag samples out of five
		 * is not NLOS_VOTES_N, so nothing widens even though the bag entry is
		 * generous -- a per-class widening must not be reachable through a
		 * minority the binary widening would have refused.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(
				&ap, 1000 + i * 200, 150,
				(i % 5) < 2 ? ULTRAWIDELOCK_APPROACH_CARRY_BAG
					    : ULTRAWIDELOCK_APPROACH_CARRY_CLEAR,
				conf);
		}
		T_OK("carry.minority.does.not.widen", ultrawidelock_approach_locked(&ap));
		T_EQ("carry.minority.reports.clear", (long)ultrawidelock_approach_nlos_carry(&ap, 2400),
		     (long)ULTRAWIDELOCK_APPROACH_CARRY_CLEAR);

		/*
		 * THE SAME CONFIDENCE FLOOR, not a second one. A bag below
		 * nlos_conf_min votes clear and its class goes with it: an unconfident
		 * guess at WHICH geometry is worth less than the unconfident guess that
		 * there was one, and that one is already refused.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_BAG,
								cfg.nlos_conf_min - 0.01f);
		}
		T_EQ("carry.unconfident.bag.widens.nothing",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 2400),
		     (long)cfg.unlock_cm);
		T_OK("carry.unconfident.bag.stays.locked", ultrawidelock_approach_locked(&ap));

		/*
		 * THE SAME STALENESS HORIZON, not a second one. Votes age with the
		 * median entries they share timestamps with, so a bag majority earned
		 * before a pocket trust hole must stop widening once it outlives
		 * MEDIAN_STALE_MS -- the permission-adding-on-stale-evidence shape the
		 * single widening was already fixed for.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 5; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_BAG,
								conf);
		}
		T_EQ("carry.fresh.bag.majority.widens",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 1800),
		     (long)(cfg.unlock_cm + 70));
		T_EQ("carry.stale.bag.majority.does.not",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 4400),
		     (long)cfg.unlock_cm);
		T_EQ("carry.stale.bag.reports.clear",
		     (long)ultrawidelock_approach_nlos_carry(&ap, 4400),
		     (long)ULTRAWIDELOCK_APPROACH_CARRY_CLEAR);

		/*
		 * DISENGAGE BY WASH, the other direction: five bag samples then five
		 * pocket ones, all fresh. The window turns over and the threshold
		 * follows the class rather than latching on the first one that won.
		 */
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 5; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_BAG,
								conf);
		}
		T_EQ("carry.wash.starts.at.bag", (long)ultrawidelock_approach_nlos_carry(&ap, 1800),
		     (long)ULTRAWIDELOCK_APPROACH_CARRY_BAG);
		for (int i = 0; i < 5; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 2000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_POCKET,
								conf);
		}
		T_EQ("carry.wash.ends.at.pocket", (long)ultrawidelock_approach_nlos_carry(&ap, 2800),
		     (long)ULTRAWIDELOCK_APPROACH_CARRY_POCKET);
		T_EQ("carry.wash.moves.the.threshold.inward",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 2800),
		     (long)(cfg.unlock_cm + 20));

		/*
		 * TIES BREAK TOWARD THE SMALLEST WIDENING. Two pocket, two bag and one
		 * hand is five obstructed votes -- comfortably past NLOS_VOTES_N, so
		 * the window IS blocked -- with pocket and bag level at two each and
		 * hand a non-contender at one.
		 *
		 * THE ENTRIES ARE DELIBERATELY THE OTHER WAY ROUND HERE, generous
		 * pocket and strict bag, and that is what makes this test discriminate.
		 * The plurality scan keeps the first class it saw on a tie, so with the
		 * strict class at the LOWER enum index a plain enum-order rule and the
		 * smallest-widening rule agree and the test proves nothing. Putting the
		 * strict entry on bag -- the HIGHER index -- separates them: enum order
		 * gives pocket's 130 and opens at 150, the shipped rule gives bag's 120
		 * and does not. Verified by mutation: replacing the tie-break with a
		 * constant false fails the two assertions below.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_POCKET] = 70;
		cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_BAG] = 20;
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		{
			static const enum ultrawidelock_approach_carry split[5] = {
				ULTRAWIDELOCK_APPROACH_CARRY_POCKET,
				ULTRAWIDELOCK_APPROACH_CARRY_BAG,
				ULTRAWIDELOCK_APPROACH_CARRY_POCKET,
				ULTRAWIDELOCK_APPROACH_CARRY_BAG,
				ULTRAWIDELOCK_APPROACH_CARRY_HAND,
			};
			for (int i = 0; i < 5; i++) {
				(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
									split[i], conf);
			}
		}
		T_OK("carry.split.window.is.still.blocked",
		     ultrawidelock_approach_nlos_blocked(&ap, 1800));
		T_EQ("carry.tie.picks.the.stricter.class",
		     (long)ultrawidelock_approach_nlos_carry(&ap, 1800),
		     (long)ULTRAWIDELOCK_APPROACH_CARRY_BAG);
		T_EQ("carry.tie.breaks.to.the.smallest.widening",
		     (long)ultrawidelock_approach_effective_unlock_cm(&ap, 1800),
		     (long)(cfg.unlock_cm + 20));
		T_OK("carry.tie.stays.locked.at.150", ultrawidelock_approach_locked(&ap));

		/*
		 * PER-ENTRY CLAMPS, identical to nlos_widen_cm's. unlock_cm 100 and
		 * approach_cm 180 leave 79 cm, so a 500 cm bag entry comes back as 79
		 * -- otherwise the table would be a way to route around the clamp that
		 * stops the trajectory gate arming and firing on the same sample.
		 * Negatives clamp to 0 for the same reason they do on the single
		 * number: stricter-only-while-obstructed is not a thing a caller wants.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_BAG] = 500;
		cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_POCKET] = -50;
		ultrawidelock_approach_init(&ap, &cfg);
		T_EQ("carry.entry.clamped.under.approach_cm",
		     (long)ap.cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_BAG], 79L);
		T_EQ("carry.negative.entry.clamped.to.zero",
		     (long)ap.cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_POCKET], 0L);

		/* Every entry off by default, exactly as the single number is. */
		ultrawidelock_approach_defaults(&cfg);
		for (int c = 0; c < ULTRAWIDELOCK_APPROACH_CARRY_N; c++) {
			T_EQ("carry.table.is.off.by.default", (long)cfg.nlos_widen_class_cm[c], 0L);
		}

		/*
		 * The silence rule reads the SAME widened radius the fire decision did.
		 * A bag-carried phone resting at the door reports 150; against the
		 * unwidened 100 that is a departure, and relocking under the owner it
		 * just opened for is the failure this shares with the single-number
		 * case above.
		 */
		ultrawidelock_approach_defaults(&cfg);
		cfg.nlos_widen_class_cm[ULTRAWIDELOCK_APPROACH_CARRY_BAG] = 70;
		ultrawidelock_approach_init(&ap, &cfg);
		ultrawidelock_approach_session_up(&ap);
		for (int i = 0; i < 8; i++) {
			(void)ultrawidelock_approach_feed_carry(&ap, 1000 + i * 200, 150,
								ULTRAWIDELOCK_APPROACH_CARRY_BAG,
								conf);
		}
		T_OK("carry.open.before.the.silence", !ultrawidelock_approach_locked(&ap));
		T_EQ("carry.silence.inside.the.widened.radius.does.not.relock",
		     (long)ultrawidelock_approach_tick(&ap, 2600 + cfg.far_silence_ms + 1),
		     (long)ULTRAWIDELOCK_APPROACH_HOLD);
	}

	t_group("prediction vs ranging holes (2026-08-07 walk)");
	{
		struct walk w;

		/*
		 * The walk that found it: PREDICT fired ~120 cm out, dropped rounds
		 * then opened a >1 s hole in the accepted stream while the owner kept
		 * walking. The 900 ms deadline expired between two samples of a live
		 * approach, and the first sample after the hole re-based the filter
		 * (v = 0) and read as a stall: bolt toggled shut at 65 cm, and the
		 * cleared arm kept it shut until a retreat past approach_cm.
		 */
		walk_init(&w, 0xB01DFACE, 600.0f);
		while (w.r.n_predict == 0 && w.d > 110.0f) {
			walk_block(&w, 130.0f, 25, -1);
		}
		T_EQ("hole.predict.fired", w.r.n_predict, 1);
		/* A 1.2 s ranging hole, past KF_STALE_MS, the walker still moving
		 * (slower: they are almost at the door). */
		w.t += 1200 - BLOCK_MS;
		w.d -= 60.0f * (1200.0f - (float)BLOCK_MS) / 1000.0f;
		walk_until(&w, 60.0f, 25, 40.0f, 0, 0);
		T_EQ("hole.no.abort", w.r.n_abort, 0);
		T_OK("hole.door.open", !ultrawidelock_approach_locked(&w.ap));

		/*
		 * The lockout, deterministic (no jitter): at 100 cm/s the prediction
		 * fires around 150 cm -- inside approach_cm -- so an abort there
		 * must keep the arm. Standing still must not open the door, and
		 * resuming the walk must, WITHOUT first retreating past 180.
		 */
		walk_init(&w, 0, 400.0f);
		while (w.r.n_predict == 0 && w.d > 110.0f) {
			walk_block(&w, 100.0f, 0, -1);
		}
		T_EQ("lockout.predict.fired", w.r.n_predict, 1);
		walk_blocks(&w, 0.0f, 0, 30); /* stop dead: stall abort */
		T_EQ("lockout.abort.fired", w.r.n_abort, 1);
		T_OK("lockout.relocked", ultrawidelock_approach_locked(&w.ap));
		T_OK("lockout.arm.kept", w.ap.approach_armed);
		T_EQ("lockout.standing.no.open", w.r.n_thresh, 0);
		walk_until(&w, 100.0f, 0, 30.0f, 0, 0);
		T_OK("lockout.resume.opens", !ultrawidelock_approach_locked(&w.ap));

		/*
		 * The invariant the exception must not touch: an abort with the
		 * estimate OUTSIDE approach_cm still clears the arm. At 200 cm/s the
		 * prediction fires around 205 cm; ranging then dies outright, so the
		 * tick's deadline abort runs with the estimate frozen there -- the
		 * genuine stopped-short credential. (A stall abort with samples
		 * flowing would not do here: the constant-velocity filter overshoots
		 * INWARD during the deceleration, and the estimate at abort would sit
		 * inside approach_cm for exactly the wrong reason.)
		 */
		walk_init(&w, 0, 820.0f);
		while (w.r.n_predict == 0 && w.d > 195.0f) {
			walk_block(&w, 200.0f, 0, -1);
		}
		T_EQ("far.predict.fired", w.r.n_predict, 1);
		for (int k = 1; k <= 30 && w.r.n_abort == 0; k++) {
			int64_t tk = w.t + (int64_t)k * 200;

			note(&w.r, ultrawidelock_approach_tick(&w.ap, tk), tk);
		}
		T_EQ("far.abort.fired", w.r.n_abort, 1);
		T_OK("far.abort.clears.arm", !w.ap.approach_armed);
	}

	t_group("re-arming after a mid-session relock (2026-08-07 23:51 walk)");
	{
		struct walk w;

		/*
		 * The silence relock that last saw the credential NEAR (trusted
		 * 102 cm, then quiet) must keep the arm: the owner is one step from
		 * the door, and demanding a fresh >= approach_cm median from there
		 * was a lockout. Stepping back in must reopen with no far excursion.
		 */
		walk_init(&w, 0, 400.0f);
		walk_until(&w, 100.0f, 0, 60.0f, 0, 0);
		T_OK("neargap.unlocked", !ultrawidelock_approach_locked(&w.ap));
		walk_until(&w, -60.0f, 0, 130.0f, 0, 0); /* drift just outside */
		/* 130 cm is the dead band: the slower BAND_SILENCE_MS tier governs. */
		note(&w.r, ultrawidelock_approach_tick(&w.ap, w.t + 2600), w.t + 2600);
		T_EQ("neargap.silence.relock", w.r.n_depart, 1);
		T_OK("neargap.arm.kept", w.ap.approach_armed);
		w.t += 2600;
		walk_until(&w, 100.0f, 0, 30.0f, 0, 0);
		T_OK("neargap.reopens", !ultrawidelock_approach_locked(&w.ap));

		/*
		 * The 23:51 lockout replay: the trust gate declines the whole
		 * retreat, so the walk-away exists only as unvouched observations.
		 * The far observation before the relock arms; the relock (last seen
		 * 474, genuinely far) clears; the observation on the way BACK re-arms,
		 * and the vouched near samples then open the door. The old code left
		 * it shut for fifteen seconds of standing at 0 cm.
		 */
		walk_init(&w, 0, 400.0f);
		walk_until(&w, 100.0f, 0, 60.0f, 0, 0);
		T_OK("observe.unlocked", !ultrawidelock_approach_locked(&w.ap));
		ultrawidelock_approach_observe_departure(&w.ap, w.t + 500, 474);
		note(&w.r, ultrawidelock_approach_tick(&w.ap, w.t + 1400), w.t + 1400);
		T_EQ("observe.silence.relock", w.r.n_depart, 1);
		T_OK("observe.far.relock.clears.arm", !w.ap.approach_armed);
		ultrawidelock_approach_observe_departure(&w.ap, w.t + 2000, 473);
		T_OK("observe.rearms", w.ap.approach_armed);
		w.t += 2400; /* the KF gap re-bases the filter; that is fine here */
		w.d = 200.0f;
		walk_until(&w, 100.0f, 0, 30.0f, 0, 0);
		T_OK("observe.reopens", !ultrawidelock_approach_locked(&w.ap));

		/*
		 * The credential the gate exists for: a phone that never leaves
		 * cannot re-arm itself. After a far relock it sits at 110 cm --
		 * inside relock_cm, outside the radius -- producing neither a
		 * >= approach_cm median nor a >= relock_cm observation.
		 */
		walk_init(&w, 0, 400.0f);
		walk_until(&w, 100.0f, 0, 60.0f, 0, 0);
		ultrawidelock_approach_observe_departure(&w.ap, w.t + 500, 474);
		note(&w.r, ultrawidelock_approach_tick(&w.ap, w.t + 1400), w.t + 1400);
		T_OK("resting.arm.cleared", !w.ap.approach_armed);
		w.t += 1400;
		w.d = 110.0f;
		walk_blocks(&w, 0.0f, 0, 60); /* sit at 110 for ~11 s */
		T_OK("resting.still.locked", ultrawidelock_approach_locked(&w.ap));
		T_OK("resting.never.armed", !w.ap.approach_armed);
	}

	t_group("pocketed walk (2026-08-08 00:01)");
	{
		struct walk w;

		/*
		 * The 00:01:39 ghost grant: unlock, walk away seen only as an
		 * unvouched observation, an 8.5 s trust hole, and a return whose
		 * first samples read 193 and 159. The old window still held
		 * 40-90 cm entries from before the hole; their median said
		 * "present" and the bolt opened with the owner at 1.6 m. Stale
		 * entries no longer vote: the door opens when FRESH samples say so.
		 */
		walk_init(&w, 0, 400.0f);
		walk_until(&w, 100.0f, 0, 60.0f, 0, 0);
		T_OK("stale.opened", !ultrawidelock_approach_locked(&w.ap));
		ultrawidelock_approach_observe_departure(&w.ap, w.t + 400, 300);
		note(&w.r, ultrawidelock_approach_tick(&w.ap, w.t + 1300), w.t + 1300);
		T_OK("stale.relocked", ultrawidelock_approach_locked(&w.ap));
		ultrawidelock_approach_observe_departure(&w.ap, w.t + 2000, 300); /* re-arm */
		w.t += 8500;
		walk_block(&w, 0.0f, 0, 193);
		walk_block(&w, 0.0f, 0, 159);
		T_OK("stale.no.ghost.grant", ultrawidelock_approach_locked(&w.ap));
		walk_block(&w, 0.0f, 0, 60);
		walk_block(&w, 0.0f, 0, 45);
		walk_block(&w, 0.0f, 0, 40);
		walk_block(&w, 0.0f, 0, 35);
		T_OK("stale.real.grant", !ultrawidelock_approach_locked(&w.ap));

		/*
		 * The 00:01:40 mid-approach relock: last sample 159 (dead band),
		 * then an ordinary 1.7 s pocket trust hole. The 750 ms tier
		 * un-opened the door; the band now waits BAND_SILENCE_MS, and a
		 * genuinely silent band credential still relocks after it.
		 */
		walk_init(&w, 0, 400.0f);
		walk_until(&w, 100.0f, 0, 60.0f, 0, 0);
		T_OK("band.opened", !ultrawidelock_approach_locked(&w.ap));
		walk_block(&w, 0.0f, 0, 159); /* wandered out; still in the band */
		note(&w.r, ultrawidelock_approach_tick(&w.ap, w.t + 1700), w.t + 1700);
		T_OK("band.pocket.gap.no.relock", !ultrawidelock_approach_locked(&w.ap));
		note(&w.r, ultrawidelock_approach_tick(&w.ap, w.t + 2600), w.t + 2600);
		T_OK("band.long.silence.relocks", ultrawidelock_approach_locked(&w.ap));

		/* The fast tier is untouched: last seen >= relock_cm relocks on
		 * far_silence_ms as before. */
		walk_init(&w, 0, 400.0f);
		walk_until(&w, 100.0f, 0, 60.0f, 0, 0);
		ultrawidelock_approach_observe_departure(&w.ap, w.t + 400, 320);
		note(&w.r, ultrawidelock_approach_tick(&w.ap, w.t + 1200), w.t + 1200);
		T_OK("far.tier.still.fast", ultrawidelock_approach_locked(&w.ap));
		T_OK("far.tier.arm.cleared", !w.ap.approach_armed);

		/* A fresh far sample after a gap re-arms via the median, exactly as
		 * the wlen == 1 session-start arm always has (the ESP walk-up suite
		 * pins that contract): staleness must not have changed it. */
		w.t += 3000;
		walk_block(&w, 0.0f, 0, 200);
		T_OK("fresh.far.sample.arms", w.ap.approach_armed);
	}
}
