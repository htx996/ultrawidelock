/** @file test_main.c — runs every module suite, prints a summary table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"

/* The modules' per-frame UWB diag gate (ultrawidelock_diag.h); on for host builds. */
extern volatile int ultrawidelock_uwb_diag_on;

struct suite {
	const char *name;
	void (*fn)(void);
};

static void rule(const char *dim, const char *rst, int n)
{
	printf("    %s", dim);
	for (int i = 0; i < n; i++) {
		printf("─");
	}
	printf("%s\n", rst);
}

int main(void)
{
	/* run.sh sets ULTRAWIDELOCK_TEST_QUIET so `make test` shows the summary table, not
	 * the UWB diag firehose. Unset (coverage, a manual run, debugging a
	 * suite), the diag default is untouched. */
	if (getenv("ULTRAWIDELOCK_TEST_QUIET") != NULL) {
		ultrawidelock_uwb_diag_on = 0;
	}

	static const struct suite suites[] = {
		{ "ultrawidelock_advertising", test_ultrawidelock_advertising },
		{ "ultrawidelock_ble", test_ultrawidelock_ble },
		{ "ultrawidelock_nfc", test_ultrawidelock_nfc },
		{ "pn532", test_pn532 },
		{ "ccc_kdf", test_ccc_kdf },
		{ "ccc_mac", test_ccc_mac },
		{ "ccc_sts", test_ccc_sts },
		{ "ccc_shim", test_ccc_shim },
		{ "ccc_session", test_ccc_session },
		{ "prepoll_schedule", test_prepoll_schedule },
		{ "ultrawidelock_uwb_msg_builder", test_ultrawidelock_uwb_msg_builder },
		{ "ultrawidelock_uwb_msg_parser", test_ultrawidelock_uwb_msg_parser },
		{ "ultrawidelock_uwb_adapter", test_ultrawidelock_uwb_adapter },
		{ "ultrawidelock_uwb_msg", test_ultrawidelock_uwb_msg },
		{ "ultrawidelock_uwb_session", test_ultrawidelock_uwb_session },
		{ "ultrawidelock_prov", test_ultrawidelock_prov },
		{ "ultrawidelock_hash", test_ultrawidelock_hash },
		{ "ultrawidelock_assert", test_ultrawidelock_assert },
		{ "ultrawidelock_device_uwb", test_ultrawidelock_device_uwb },
		{ "cherry", test_cherry },
		{ "fira", test_fira },
		{ "facade", test_facade },
		{ "prepoll_gate", test_prepoll_gate },
		{ "prepoll_round", test_prepoll_round },
		{ "rssi_gate", test_rssi_gate },
		{ "ultrawidelock_door", test_ultrawidelock_door },
		{ "ultrawidelock_fusion", test_ultrawidelock_fusion },
		{ "ultrawidelock_report", test_ultrawidelock_report },
		{ "ultrawidelock_satellite", test_ultrawidelock_satellite },
		{ "ultrawidelock_side", test_ultrawidelock_side },
		{ "ultrawidelock_side_replay", test_ultrawidelock_side_replay },
		{ "ultrawidelock_slam", test_ultrawidelock_slam },
		{ "approach", test_approach },
		{ "grant", test_grant },
		{ "logfmt", test_ultrawidelock_logfmt },
		{ "trace", test_trace },
		{ "ccc_shim_wrap", test_ccc_shim_wrap },
		{ "flight_recorder", test_flight_recorder },
		{ "matter_tlv", test_matter_tlv },
		{ "matter_msg", test_matter_msg },
		{ "matter_mrp", test_matter_mrp },
		{ "matter_crypto", test_matter_crypto },
		{ "matter_btp", test_matter_btp },
		{ "matter_pase", test_matter_pase },
		{ "matter_spake2p", test_matter_spake2p },
		{ "matter_pase_sm", test_matter_pase_sm },
		{ "matter_exchange", test_matter_exchange },
		{ "matter_im", test_matter_im },
		{ "matter_im_invoke", test_matter_im_invoke },
		{ "matter_im_write", test_matter_im_write },
		{ "matter_im_events", test_matter_im_events },
		{ "matter_attest", test_matter_attest },
		{ "matter_fabric", test_matter_fabric },
		{ "matter_addnoc", test_matter_addnoc },
		{ "matter_network", test_matter_network },
		{ "matter_case", test_matter_case },
		{ "matter_case_client", test_matter_case_client },
		{ "matter_client_sm", test_matter_client_sm },
		{ "matter_im_client", test_matter_im_client },
		{ "matter_binding", test_matter_binding },
		{ "matter_client", test_matter_client },
		{ "matter_clusters", test_matter_clusters },
		{ "ultrawidelock_ml", test_ultrawidelock_ml },
		{ "ultrawidelock_port", test_ultrawidelock_port },
	};
	const int n = (int)(sizeof(suites) / sizeof(suites[0]));
	/* Sized from the table, not a literal 32: these are indexed by suite, so a
	 * fixed bound silently overflows the moment the table outgrows it. */
	int npass[sizeof(suites) / sizeof(suites[0])];
	int nfail[sizeof(suites) / sizeof(suites[0])];
	int npend[sizeof(suites) / sizeof(suites[0])];

	int color = isatty(1) && getenv("NO_COLOR") == NULL;
	const char *B = color ? "\033[1m" : "";
	const char *D = color ? "\033[2m" : "";
	const char *G = color ? "\033[32m" : "";
	const char *R = color ? "\033[31m" : "";
	const char *Y = color ? "\033[33m" : "";
	const char *C = color ? "\033[36m" : "";
	const char *X = color ? "\033[0m" : "";

	int namew = (int)strlen("module");
	for (int i = 0; i < n; i++) {
		int l = (int)strlen(suites[i].name);

		if (l > namew) {
			namew = l;
		}
	}

	/* Run every suite; a failing assertion prints its own FAIL line here, above
	 * the summary table (VERBOSE=1 also shows group headers). */
	for (int i = 0; i < n; i++) {
		int p0 = t_pass, f0 = t_fail, r0 = t_pending;

		suites[i].fn();
		npass[i] = t_pass - p0;
		nfail[i] = t_fail - f0;
		npend[i] = t_pending - r0;
	}

	printf("\n  %sTests%s  %s·  our code — host logic%s\n\n", B, X, D, X);
	printf("    %s%-*s   %5s   result%s\n", D, namew, "module", "tests", X);
	for (int i = 0; i < n; i++) {
		int tot = npass[i] + nfail[i] + npend[i];
		const char *tint = nfail[i] ? R : npend[i] ? Y : G;
		const char *glyph = nfail[i] ? "✗" : npend[i] ? "○" : "✓";

		printf("    %s%-*s%s   %s%5d%s   %s%s%s", C, namew, suites[i].name, X, D,
		       tot, X, tint, glyph, X);
		if (nfail[i]) {
			printf("  %s%d failed%s", R, nfail[i], X);
		} else if (npend[i]) {
			printf("  %s%d pending%s", Y, npend[i], X);
		}
		printf("\n");
	}

	rule(D, X, namew + 17);

	int tp = t_pass, tf = t_fail, tr = t_pending;
	const char *tint = tf ? R : tr ? Y : G;
	const char *glyph = tf ? "✗" : tr ? "○" : "✓";

	printf("    %s%-*s%s   %s%5d%s   %s%s%s%s %d passed", B, namew, "TOTAL", X, B,
	       tp + tf + tr, X, tint, B, glyph, X, tp);
	if (tf) {
		printf(", %s%d failed%s", R, tf, X);
	}
	if (tr) {
		printf(", %s%d pending%s", Y, tr, X);
	}
	printf("\n\n");

	if (tf > 0) {
		printf("  %s%sRESULT: FAIL%s (%d failing)\n", B, R, X, tf);
		return 1;
	}
	if (tr > 0) {
		printf("  %s%sRESULT: RECORD%s (%d pending — bake the values in)\n", B,
		       Y, X, tr);
		return 2;
	}
	printf("  %s%sRESULT: PASS%s\n", B, G, X);
	return 0;
}
