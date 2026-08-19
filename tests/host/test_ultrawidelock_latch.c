/**
 * @file test_ultrawidelock_latch.c — the inside veto: every way of losing
 *       evidence must leave the door shut.
 *
 * The suite is organised around the safety table in docs/inside-latch.md.
 * Each evidence-loss row that can be expressed without hardware has a test
 * here, named for the row it defends.
 */

#include "test.h"

#include "ultrawidelock_latch.h"

#include <string.h>

#define CRED_A 0x0A0A0A0Au
#define CRED_B 0x0B0B0B0Bu

#define T0 100000

/* Drive a clean walk-up: `n` confident OUTSIDE windows starting far out. */
static int64_t walk_up(struct ultrawidelock_latch *l, uint32_t cred, int64_t start_ms, int n)
{
	int64_t t = start_ms;

	for (int i = 0; i < n; i++) {
		int32_t mm = 4000 - i * 800;

		if (mm < 200) {
			mm = 200;
		}
		ultrawidelock_latch_note_window(l, cred, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE, mm, t);
		t += 2000;
	}
	return t;
}

/* A credential that entered long ago and has since had a chance to leave. */
static void seed_departed(struct ultrawidelock_latch *l, uint32_t cred)
{
	ultrawidelock_latch_note_grant(l, cred, 0);
	ultrawidelock_latch_note_opportunity(l, cred, 1000);
}

static void test_resting_state_is_inside(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;

	t_group("latch: the resting state is INSIDE");
	ultrawidelock_latch_init(&l, NULL);

	/* Row 5: first boot / unknown credential. Nothing is known, so nothing
	 * opens. */
	T_OK("rest.unknown_cred", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, T0, &why));
	T_OK("rest.reason_session", (why & ULTRAWIDELOCK_LATCH_R_NO_SESSION) != 0u);
	T_OK("rest.reason_record", (why & ULTRAWIDELOCK_LATCH_R_NO_RECORD) != 0u);

	/* A session alone changes nothing. */
	ultrawidelock_latch_session_open(&l, CRED_A);
	T_OK("rest.session_only", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, T0, &why));
	T_OK("rest.no_windows", (why & ULTRAWIDELOCK_LATCH_R_WINDOWS) != 0u);

	T_OK("rest.null", !ultrawidelock_latch_may_passive_unlock(NULL, CRED_A, T0, &why));
}

static void test_clean_walkup_clears(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0xFFu;
	int64_t t;

	t_group("latch: a proven approach clears, once");
	ultrawidelock_latch_init(&l, NULL);
	seed_departed(&l, CRED_A);

	ultrawidelock_latch_session_open(&l, CRED_A);
	t = walk_up(&l, CRED_A, T0, 3);

	T_OK("clear.granted", ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_EQ("clear.reason_clean", why, 0);

	/* The grant re-latches: the phone just walked in. */
	ultrawidelock_latch_note_grant(&l, CRED_A, t);
	T_OK("clear.relatched", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t + 1, &why));
	T_OK("clear.dwell", (why & ULTRAWIDELOCK_LATCH_R_DWELL) != 0u);
	T_OK("clear.opportunity_spent",
	     (why & ULTRAWIDELOCK_LATCH_R_NO_OPPORTUNITY) != 0u);
	/* And progress is discarded, so the next approach starts from zero. */
	T_OK("clear.windows_reset", (why & ULTRAWIDELOCK_LATCH_R_WINDOWS) != 0u);
}

static void test_opportunity_is_required(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: RF alone never clears the veto");
	ultrawidelock_latch_init(&l, NULL);

	/* Entered, never had a chance to leave. This is the failure the module
	 * exists for: a phone indoors that the differential misreads. */
	ultrawidelock_latch_note_grant(&l, CRED_A, 0);
	ultrawidelock_latch_session_open(&l, CRED_A);
	t = walk_up(&l, CRED_A, T0, 6);

	T_OK("opp.refused", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_EQ("opp.reason", why, ULTRAWIDELOCK_LATCH_R_NO_OPPORTUNITY);

	/* Give it one and the same evidence now suffices. */
	ultrawidelock_latch_note_opportunity(&l, CRED_A, t);
	T_OK("opp.granted", ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
}

static void test_inside_window_contradicts(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: one INSIDE window outweighs the agreements before it");
	ultrawidelock_latch_init(&l, NULL);
	seed_departed(&l, CRED_A);
	ultrawidelock_latch_session_open(&l, CRED_A);

	t = walk_up(&l, CRED_A, T0, 2);
	ultrawidelock_latch_note_window(&l, CRED_A, ULTRAWIDELOCK_SIDE_LABEL_INSIDE, 900, t);
	t += 2000;
	ultrawidelock_latch_note_window(&l, CRED_A, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE, 900, t);

	T_OK("contra.refused", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_OK("contra.reason", (why & ULTRAWIDELOCK_LATCH_R_WINDOWS) != 0u);
}

static void test_dead_band_does_not_destroy_a_proven_approach(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: the dead band neither helps nor harms");
	ultrawidelock_latch_init(&l, NULL);
	seed_departed(&l, CRED_A);
	ultrawidelock_latch_session_open(&l, CRED_A);

	t = walk_up(&l, CRED_A, T0, 3);
	/* At the plane the differential is genuinely ambiguous -- MEASURED
	 * 2026-08-11 at 49% of windows. It must not undo what was proven at
	 * 4 m, and it must not substitute for it either. */
	ultrawidelock_latch_note_window(&l, CRED_A, ULTRAWIDELOCK_SIDE_LABEL_UNKNOWN, 400, t);
	t += 500;
	ultrawidelock_latch_note_window(&l, CRED_A, ULTRAWIDELOCK_SIDE_LABEL_THRESHOLD, 300, t);

	T_OK("dead.survives", ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
}

static void test_proven_approach_expires(void)
{
	struct ultrawidelock_latch l;
	struct ultrawidelock_latch_cfg cfg;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: a proven approach is evidence, not a mode");
	ultrawidelock_latch_defaults(&cfg);
	ultrawidelock_latch_init(&l, &cfg);
	seed_departed(&l, CRED_A);
	ultrawidelock_latch_session_open(&l, CRED_A);

	t = walk_up(&l, CRED_A, T0, 3);
	T_OK("expire.fresh", ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));

	t += (int64_t)cfg.clear_valid_ms + 1;
	T_OK("expire.stale", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_OK("expire.reason", (why & ULTRAWIDELOCK_LATCH_R_STALE) != 0u);
}

static void test_run_must_start_far_out(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t = T0;

	t_group("latch: a clear must begin where the sign is trustworthy");
	ultrawidelock_latch_init(&l, NULL);
	seed_departed(&l, CRED_A);
	ultrawidelock_latch_session_open(&l, CRED_A);

	/* Six confident OUTSIDE windows, all taken at the door where the two
	 * witnesses are near equidistant. Never starts a run. */
	for (int i = 0; i < 6; i++) {
		ultrawidelock_latch_note_window(&l, CRED_A, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE,
						800, t);
		t += 2000;
	}
	T_OK("start.near_refused", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_OK("start.reason", (why & ULTRAWIDELOCK_LATCH_R_WINDOWS) != 0u);

	/* An unknown range cannot start one either. */
	ultrawidelock_latch_note_window(&l, CRED_A, ULTRAWIDELOCK_SIDE_LABEL_OUTSIDE, -1, t);
	T_OK("start.no_range", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
}

static void test_session_scoping(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: evidence never crosses sessions or credentials");
	ultrawidelock_latch_init(&l, NULL);
	seed_departed(&l, CRED_A);
	seed_departed(&l, CRED_B);

	ultrawidelock_latch_session_open(&l, CRED_A);
	t = walk_up(&l, CRED_A, T0, 3);
	T_OK("scope.a_ok", ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));

	/* B's phone is elsewhere; A's walk-up says nothing about it. */
	T_OK("scope.b_refused", !ultrawidelock_latch_may_passive_unlock(&l, CRED_B, t, &why));
	T_OK("scope.b_reason", (why & ULTRAWIDELOCK_LATCH_R_NO_SESSION) != 0u);

	/* Row 6: the session drops mid-approach. Progress goes with it. */
	ultrawidelock_latch_session_close(&l);
	T_OK("scope.closed", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	ultrawidelock_latch_session_open(&l, CRED_A);
	T_OK("scope.reopened", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_OK("scope.reopen_reason", (why & ULTRAWIDELOCK_LATCH_R_WINDOWS) != 0u);

	/* Windows fed for a credential with no open session are ignored. */
	ultrawidelock_latch_session_close(&l);
	(void)walk_up(&l, CRED_A, t, 5);
	ultrawidelock_latch_session_open(&l, CRED_A);
	T_OK("scope.offline_windows",
	     !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
}

static void test_multi_credential_household(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: one phone leaving does not free the one that stayed");
	ultrawidelock_latch_init(&l, NULL);

	/* Both came home. */
	ultrawidelock_latch_note_grant(&l, CRED_A, 0);
	ultrawidelock_latch_note_grant(&l, CRED_B, 0);

	/* A goes out and comes back: opening the door for A is a crossing
	 * opportunity for B, but B still has to prove it took one. */
	ultrawidelock_latch_note_opportunity(&l, CRED_A, 10000);
	ultrawidelock_latch_session_open(&l, CRED_A);
	t = walk_up(&l, CRED_A, T0, 3);
	T_OK("multi.a_ok", ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	ultrawidelock_latch_note_grant(&l, CRED_A, t);

	ultrawidelock_latch_session_close(&l);
	ultrawidelock_latch_session_open(&l, CRED_B);
	t = walk_up(&l, CRED_B, t + 100000, 3);
	/* B is on the sofa. It has an opportunity now (A opened the door), so
	 * the only thing standing between B and a wrong unlock is the RF
	 * evidence -- which in this synthetic run says OUTSIDE. This test pins
	 * that the opportunity alone did NOT decide it: flip the windows to
	 * INSIDE and B must be refused. */
	ultrawidelock_latch_note_window(&l, CRED_B, ULTRAWIDELOCK_SIDE_LABEL_INSIDE, 900, t);
	T_OK("multi.b_inside_refused",
	     !ultrawidelock_latch_may_passive_unlock(&l, CRED_B, t, &why));
}

static void test_persistence(void)
{
	struct ultrawidelock_latch l, r;
	uint8_t buf[ULTRAWIDELOCK_LATCH_BLOB_LEN];
	uint8_t why = 0u;
	size_t n;
	int64_t t;

	t_group("latch: row 4, the lock reboots");
	ultrawidelock_latch_init(&l, NULL);
	ultrawidelock_latch_note_grant(&l, CRED_A, 5000);
	ultrawidelock_latch_note_opportunity(&l, CRED_A, 9000);
	ultrawidelock_latch_note_grant(&l, CRED_B, 6000);

	n = ultrawidelock_latch_serialize(&l, buf, sizeof(buf));
	T_OK("persist.wrote", n > 0);
	T_OK("persist.read", ultrawidelock_latch_deserialize(&r, NULL, buf, n));

	/* Beliefs survive; per-approach progress does not. */
	ultrawidelock_latch_session_open(&r, CRED_A);
	t = walk_up(&r, CRED_A, T0, 3);
	T_OK("persist.a_opportunity_kept",
	     ultrawidelock_latch_may_passive_unlock(&r, CRED_A, t, &why));

	ultrawidelock_latch_session_close(&r);
	ultrawidelock_latch_session_open(&r, CRED_B);
	t = walk_up(&r, CRED_B, T0, 3);
	T_OK("persist.b_still_inside",
	     !ultrawidelock_latch_may_passive_unlock(&r, CRED_B, t, &why));
	T_OK("persist.b_reason", (why & ULTRAWIDELOCK_LATCH_R_NO_OPPORTUNITY) != 0u);
}

static void test_corrupt_storage_fails_closed(void)
{
	struct ultrawidelock_latch l, r;
	uint8_t buf[ULTRAWIDELOCK_LATCH_BLOB_LEN];
	uint8_t why = 0u;
	size_t n;
	int64_t t;

	t_group("latch: row 5, storage is corrupt");
	ultrawidelock_latch_init(&l, NULL);
	ultrawidelock_latch_note_grant(&l, CRED_A, 5000);
	ultrawidelock_latch_note_opportunity(&l, CRED_A, 9000);
	n = ultrawidelock_latch_serialize(&l, buf, sizeof(buf));

	buf[4] ^= 0xFFu; /* flip a byte inside the record */
	T_OK("corrupt.rejected", !ultrawidelock_latch_deserialize(&r, NULL, buf, n));
	/* Rejected means reset, not half-restored: everyone reads INSIDE. */
	ultrawidelock_latch_session_open(&r, CRED_A);
	t = walk_up(&r, CRED_A, T0, 3);
	T_OK("corrupt.fails_closed", !ultrawidelock_latch_may_passive_unlock(&r, CRED_A, t, &why));
	T_OK("corrupt.reason", (why & ULTRAWIDELOCK_LATCH_R_NO_RECORD) != 0u);

	n = ultrawidelock_latch_serialize(&l, buf, sizeof(buf));
	buf[0] = 9u;
	T_OK("corrupt.bad_ver", !ultrawidelock_latch_deserialize(&r, NULL, buf, n));
	n = ultrawidelock_latch_serialize(&l, buf, sizeof(buf));
	buf[1] = ULTRAWIDELOCK_LATCH_MAX_CREDS + 1u;
	T_OK("corrupt.bad_count", !ultrawidelock_latch_deserialize(&r, NULL, buf, n));
	T_OK("corrupt.short", !ultrawidelock_latch_deserialize(&r, NULL, buf, 3u));
	T_OK("corrupt.null", !ultrawidelock_latch_deserialize(&r, NULL, NULL, 0u));
	T_EQ("corrupt.no_cap", ultrawidelock_latch_serialize(&l, buf, 4u), 0);
}

static void test_record_eviction_fails_closed(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: an evicted credential fails closed");
	ultrawidelock_latch_init(&l, NULL);

	/* Fill the table, then push one more in. The stalest goes. */
	for (uint32_t i = 0; i < ULTRAWIDELOCK_LATCH_MAX_CREDS; i++) {
		ultrawidelock_latch_note_grant(&l, 0x1000u + i, 1000 + (int64_t)i);
	}
	ultrawidelock_latch_note_grant(&l, CRED_A, 99000);
	ultrawidelock_latch_note_opportunity(&l, CRED_A, 99500);

	ultrawidelock_latch_session_open(&l, 0x1000u); /* the evicted one */
	t = walk_up(&l, 0x1000u, T0, 3);
	T_OK("evict.refused", !ultrawidelock_latch_may_passive_unlock(&l, 0x1000u, t, &why));
	T_OK("evict.reason", (why & ULTRAWIDELOCK_LATCH_R_NO_RECORD) != 0u);
}

static void test_broad_opportunity_is_safe(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: an unattributed door event clears nothing by itself");
	ultrawidelock_latch_init(&l, NULL);
	ultrawidelock_latch_note_grant(&l, CRED_A, 0);
	ultrawidelock_latch_note_grant(&l, CRED_B, 0);

	/* A sensed door swing has no identity, so it sets the opportunity for
	 * everybody. That breadth must not be worth anything on its own. */
	ultrawidelock_latch_note_opportunity(&l, ULTRAWIDELOCK_LATCH_CRED_ANY, 50000);

	ultrawidelock_latch_session_open(&l, CRED_A);
	T_OK("broad.no_rf", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, 60000, &why));
	T_OK("broad.reason", (why & ULTRAWIDELOCK_LATCH_R_WINDOWS) != 0u);
	T_OK("broad.opportunity_set",
	     (why & ULTRAWIDELOCK_LATCH_R_NO_OPPORTUNITY) == 0u);

	/* CRED_ANY is never a credential in its own right. */
	ultrawidelock_latch_note_grant(&l, ULTRAWIDELOCK_LATCH_CRED_ANY, 60000);
	ultrawidelock_latch_session_open(&l, ULTRAWIDELOCK_LATCH_CRED_ANY);
	t = walk_up(&l, ULTRAWIDELOCK_LATCH_CRED_ANY, T0, 3);
	T_OK("broad.any_refused",
	     !ultrawidelock_latch_may_passive_unlock(&l, ULTRAWIDELOCK_LATCH_CRED_ANY, t, &why));
}

static void test_unattributed_evidence_is_not_evidence(void)
{
	struct ultrawidelock_latch l;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: evidence with no credential attached to it");
	ultrawidelock_latch_init(&l, NULL);
	seed_departed(&l, CRED_A);

	/* The lock passes 0 while a BLE link is up but the reader cannot yet
	 * say whose it is. Nothing about that state may accumulate, and it may
	 * certainly never grant: an approach the lock cannot attribute is an
	 * approach it cannot vouch for. */
	ultrawidelock_latch_session_open(&l, 0u);
	t = walk_up(&l, 0u, T0, 6);
	T_OK("unattributed.refused", !ultrawidelock_latch_may_passive_unlock(&l, 0u, t, &why));
	T_OK("unattributed.no_record", (why & ULTRAWIDELOCK_LATCH_R_NO_RECORD) != 0u);

	/* And it must not have leaked into a real credential's record. */
	ultrawidelock_latch_session_open(&l, CRED_A);
	T_OK("unattributed.no_leak", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_OK("unattributed.leak_reason", (why & ULTRAWIDELOCK_LATCH_R_WINDOWS) != 0u);
}

static void test_opportunity_expiry_knob(void)
{
	struct ultrawidelock_latch l;
	struct ultrawidelock_latch_cfg cfg;
	uint8_t why = 0u;
	int64_t t;

	t_group("latch: optional opportunity expiry");
	ultrawidelock_latch_defaults(&cfg);
	cfg.opportunity_valid_ms = 30000u;
	ultrawidelock_latch_init(&l, &cfg);

	/* The opportunity is recorded after the entry dwell has run out, so
	 * this test isolates expiry from ULTRAWIDELOCK_LATCH_R_DWELL. */
	ultrawidelock_latch_note_grant(&l, CRED_A, 0);
	ultrawidelock_latch_note_opportunity(&l, CRED_A, 65000);
	ultrawidelock_latch_session_open(&l, CRED_A);
	t = walk_up(&l, CRED_A, 66000, 3);
	T_OK("expiry.inside_window", ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));

	ultrawidelock_latch_session_close(&l);
	ultrawidelock_latch_session_open(&l, CRED_A);
	t = walk_up(&l, CRED_A, 200000, 3);
	T_OK("expiry.expired", !ultrawidelock_latch_may_passive_unlock(&l, CRED_A, t, &why));
	T_OK("expiry.reason", (why & ULTRAWIDELOCK_LATCH_R_NO_OPPORTUNITY) != 0u);
}

void test_ultrawidelock_latch(void)
{
	test_resting_state_is_inside();
	test_clean_walkup_clears();
	test_opportunity_is_required();
	test_inside_window_contradicts();
	test_dead_band_does_not_destroy_a_proven_approach();
	test_proven_approach_expires();
	test_run_must_start_far_out();
	test_session_scoping();
	test_multi_credential_household();
	test_persistence();
	test_corrupt_storage_fails_closed();
	test_record_eviction_fails_closed();
	test_broad_opportunity_is_safe();
	test_unattributed_evidence_is_not_evidence();
	test_opportunity_expiry_knob();
}
