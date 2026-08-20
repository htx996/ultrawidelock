#!/usr/bin/env bash
#
# test-runner.sh — run every host-side suite, print failures and a summary.
#
#   firmware   tests/host/run.sh                the KAT suite
#   shared     tests/shared/run.sh              portable core + ESP port stages
#   sdk        tests/sdk/run.sh                 installed C package consumer
#   drift      drift_check.py + patch ID self-test
#   seam       tests/tooling/uwb_seam_check.sh  no call bypasses the STS seam
#   scope      tests/tooling/uwb_engine_scope_check.sh  no vendor radio API outside the DW3000 engine
#   purity     tests/tooling/port_purity_check.sh  one source, one OS per port
#   lint       tests/tooling/cppcheck_gate.sh   cppcheck over the portable tree
#   sizegate   tests/tooling/cdk_size_test.sh   the CDK size gate's own logic
#   zopt       tests/tooling/zephyr_opt_suite.sh  the instrument + dashboard tools
#   twin       tests/tooling/twin_suite.sh      the WASM firmware twin, when the
#                                               emscripten SDK is present
#   ui         scripts/lib/ui.sh --self-test    the progress display keeps the
#                                               output it wraps byte for byte
#
# Opt-in, not in the default set:
#
#   freertos   tests/ports/freertos-nrf52833/run.sh  standalone RTOS contract
#   patchdrift tests/tooling/patch_drift_check.sh    integration patches still apply
#
# patchdrift fetches from public GitHub, so it cannot be in a set that has to
# pass offline; it runs from `make regress` instead, which a bench runs before
# every push, rather than being left for someone to remember.
#
# The FreeRTOS port has no hardware verdict yet -- no bring-up, no coexistence
# proof, none of the four release gates -- so it does not get a vote on whether
# this repository is green. Run it with `make check-freertos`, or fold it in
# with SUITES="... freertos".
#
# That has a cost worth stating, because this port has already paid it once:
# make freertos-ncs-source-check silently stopped compiling for weeks precisely
# because it was not in a default set, and nothing noticed until someone ran it
# by hand. An opt-in suite rots. Whoever moves the port to hardware should move
# this line into the default set at the same time.
#
# Default: suites run in parallel, failures replayed when done. SERIAL=1 streams
# full output one suite at a time. SUITES="firmware shared" scopes. Exit is
# nonzero if any suite fails.
#
#   scripts/test-runner.sh --self-test   # prove the counter counts each check once
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/scripts/lib/ui.sh"

suite_cmd() {
	case "$1" in
	firmware) echo "bash tests/host/run.sh" ;;
	shared) echo "bash tests/shared/run.sh" ;;
	sdk) echo "bash tests/sdk/run.sh" ;;
	drift) echo "bash tests/tooling/drift_suite.sh" ;;
	seam) echo "bash tests/tooling/uwb_seam_check.sh" ;;
	scope) echo "bash tests/tooling/uwb_engine_scope_check.sh" ;;
	purity) echo "bash tests/tooling/port_purity_check.sh" ;;
	lint) echo "bash tests/tooling/cppcheck_gate.sh" ;;
	sizegate) echo "bash tests/tooling/cdk_size_test.sh" ;;
	zopt) echo "bash tests/tooling/zephyr_opt_suite.sh" ;;
	twin) echo "bash tests/tooling/twin_suite.sh" ;;
	ui) echo "bash scripts/lib/ui.sh --self-test" ;;
	freertos) echo "bash tests/ports/freertos-nrf52833/run.sh" ;;
	patchdrift) echo "bash tests/tooling/patch_drift_check.sh" ;;
	esac
}

suite_label() {
	case "$1" in
	firmware) echo "firmware (C host)" ;;
	shared) echo "shared core (C host)" ;;
	sdk) echo "SDK package (CMake)" ;;
	drift) echo "constant drift" ;;
	seam) echo "uwb seam" ;;
	scope) echo "uwb engine scope" ;;
	purity) echo "port purity" ;;
	lint) echo "cppcheck" ;;
	sizegate) echo "cdk size gate" ;;
	zopt) echo "zephyr-opt tooling" ;;
	twin) echo "wasm twin" ;;
	ui) echo "progress display" ;;
	freertos) echo "FreeRTOS port" ;;
	patchdrift) echo "integration patches" ;;
	esac
}

# passed/failed counts from a suite's captured output. Harnesses differ, so
# count the universal per-check rows plus each harness's own totals line.
#
# A harness that prints both -- a row per check AND a "PASS (N checks)" line --
# must be counted once, not twice. The totals line is therefore only believed
# when the binary that printed it emitted no rows of its own, which is tracked
# by resetting the row count at each totals line.
suite_counts() { # <outfile> -> "passed failed"
	awk '
		/^[[:space:]]+ok[[:space:]]/    { p++; rows++ }
		/^[[:space:]]+FAIL[[:space:]]/  { f++; rows++ }
		/^Ran [0-9]+ tests?/            { p += $2 }
		/skipped=[0-9]+/ {
			if (match($0, /skipped=[0-9]+/)) {
				p -= substr($0, RSTART + 8, RLENGTH - 8)
			}
		}
		/TOTAL[[:space:]]+[0-9]+[[:space:]]+✓/ { p += $2 }
		/: (PASS|FAIL) \([0-9]+ checks/ {
			if (rows == 0 && match($0, /\([0-9]+ checks/)) {
				p += substr($0, RSTART + 1, RLENGTH - 8)
			}
			rows = 0
		}
		/constants? verified/           { p += $1 }
		/all [0-9]+ patches apply cleanly/ { p += $3 }
		END { printf "%d %d", p + 0, f + 0 }
	' "$1"
}

# ---- self-test --------------------------------------------------------------
# The counter reads several harness dialects, and the way it gets those wrong is
# by counting one check twice. That is invisible in a green run -- the totals
# just drift upward -- so the shapes are pinned here.
self_test() {
	local tmp fails=0 got want name
	tmp="$(mktemp)"
	trap 'rm -f "$tmp"' RETURN

	expect() { # <name> <want-passed> <want-failed>
		name="$1"; want="$2 $3"
		got="$(suite_counts "$tmp")"
		if [ "$got" != "$want" ]; then
			printf '  self-test FAILED: %s counted "%s", expected "%s"\n' \
				"$name" "$got" "$want" >&2
			fails=$((fails + 1))
		fi
	}

	# Rows plus a totals line: the harness most of this repo uses. The totals
	# line restates the rows and must not be added to them.
	printf '  ok   one\n  ok   two\nRESULT: PASS (2 checks)\n' >"$tmp"
	expect "rows with a totals line" 2 0

	# A totals line with no rows behind it is the only count available.
	printf 'RESULT: PASS (7 checks)\n' >"$tmp"
	expect "a totals line alone" 7 0

	# Both dialects in one stream, which is what the FreeRTOS suite emits.
	printf '  ok   a\nRESULT: PASS (1 checks)\nRESULT: PASS (5 checks)\n' >"$tmp"
	expect "rows then a bare totals line" 6 0

	# Failures count as failures and still stop the totals line double-counting.
	printf '  ok   a\n  FAIL b\nRESULT: FAIL (2 checks)\n' >"$tmp"
	expect "a failing harness" 1 1

	# The patch drift check counts patches, and prints one line for all of them.
	printf '    \xe2\x9c\x93 all 15 patches apply cleanly at the pinned revisions\n' >"$tmp"
	expect "patches applied" 15 0

	# Forked scenarios report parts and a scenario count, never a check total.
	printf '  ok   a\n  ok   b\nRESULT-PART: 2 checks\nRESULT: PASS (1 scenarios)\n' >"$tmp"
	expect "forked scenarios" 2 0

	if [ "$fails" -ne 0 ]; then
		return 1
	fi
	printf '  self-test: the counter counts each check exactly once\n'
}

if [ "${1:-}" = "--self-test" ]; then
	self_test
	exit $?
fi

run_suite() { # <suite> <outfile> <metafile>
	local s="$1" out="$2" meta="$3" cmd t0 t1 rc=0
	cmd="$(suite_cmd "$s")"
	t0=$(date +%s)
	if [[ "${SERIAL:-0}" == "1" ]]; then
		printf '\n== %s ==\n' "$(suite_label "$s")"
		# shellcheck disable=SC2086 # cmd is a fixed two-word recipe from suite_cmd
		$cmd 2>&1 | tee "$out" || rc=$?
	else
		# shellcheck disable=SC2086
		$cmd >"$out" 2>&1 || rc=$?
	fi
	t1=$(date +%s)
	read -r passed failed <<<"$(suite_counts "$out")"
	printf '%s|%d|%d|%d|%d\n' "$s" "$passed" "$failed" "$((t1 - t0))" "$rc" >"$meta"
}

SEL="${SUITES:-firmware shared sdk drift seam scope purity lint sizegate zopt twin ui}"
declare -a NAMES OUTS METAS PIDS
n=0
for s in $SEL; do
	NAMES[n]="$s"
	OUTS[n]="$(mktemp -t oa-suite-out.XXXXXX)"
	METAS[n]="$(mktemp -t oa-suite-meta.XXXXXX)"
	n=$((n + 1))
done
# ui.sh installs its own EXIT trap in ui_attach, so the temp files are swept
# here rather than in a second one that would replace it.
ui_attach

# The suites run as background jobs, which POSIX has ignore SIGINT in a
# non-interactive shell -- so ^C reaches this script and not the seven compilers
# under it. Each one's process tree is ended explicitly, or interrupting
# `make check` returns the prompt and leaves the machine busy for two minutes.
kill_suites() {
	local i s
	for ((i = 0; i < ${#PIDS[@]}; i++)); do
		ui_kill_tree "${PIDS[i]}" || true
	done
	# A second sweep by command line. The walk above reads the process table
	# one fork at a time, so a suite that was between forks when it was read
	# can leave a child behind, reparented and unreachable from its pid. What
	# that child cannot escape is being the exact command this script started.
	# Each match is killed as a tree of its own: a bare pkill would take the
	# suite script out from over its compiler and orphan the expensive half.
	local p
	for s in $SEL; do
		for p in $(pgrep -f "$(suite_cmd "$s")" 2>/dev/null || true); do
			ui_kill_tree "$p" || true
		done
	done
	return 0
}
trap 'kill_suites; _ui_cleanup; rm -f "${OUTS[@]}" "${METAS[@]}"' EXIT
# ${left:-} because ^C can land before the poll loop has set it, and this trap
# runs under `set -u`.
trap 'ui_clear; printf "\n  interrupted with %s of %s suites still running\n\n" "${left:-?}" "${n:-?}" >&2; kill_suites; _ui_cleanup; rm -f "${OUTS[@]}" "${METAS[@]}"; trap - INT; kill -INT $$' INT
trap 'kill_suites; _ui_cleanup; rm -f "${OUTS[@]}" "${METAS[@]}"; trap - TERM; kill -TERM $$' TERM

printf '\n  ultrawidelock · host-side test suites\n'
if [[ "${SERIAL:-0}" == "1" ]]; then
	for i in $(seq 0 $((n - 1))); do
		run_suite "${NAMES[i]}" "${OUTS[i]}" "${METAS[i]}"
	done
else
	printf '  %d suites in parallel · a row lands as each one finishes\n\n' "$n"
	for i in $(seq 0 $((n - 1))); do
		run_suite "${NAMES[i]}" "${OUTS[i]}" "${METAS[i]}" &
		PIDS[i]=$!
	done
	# A row per suite as it lands. Poll the meta files rather than wait in
	# index order: the suites finish out of order, so waiting on index 0 first
	# holds every later row behind the slowest suite. Without this the run is a
	# bare banner for minutes, which reads as a hang rather than as work.
	declare -a REAPED
	for i in $(seq 0 $((n - 1))); do REAPED[i]=0; done
	left=$n
	started=$(date +%s)
	while [[ "$left" -gt 0 ]]; do
		for i in $(seq 0 $((n - 1))); do
			[[ "${REAPED[i]}" == 1 ]] && continue
			# run_suite writes the meta line last, so a non-empty file means
			# the work is finished and this wait returns immediately.
			[[ -s "${METAS[i]}" ]] || continue
			wait "${PIDS[i]}" || true
			REAPED[i]=1
			left=$((left - 1))
			IFS='|' read -r _ passed failed secs rc <"${METAS[i]}"
			mark="+"
			if [[ "$rc" != 0 || "$failed" != 0 ]]; then mark="x"; fi
			ui_clear
			printf '  %s %-22s %8d %8d %5ss\n' \
				"$mark" "$(suite_label "${NAMES[i]}")" "$passed" "$failed" "$secs"
		done
		if [[ "$left" -gt 0 ]]; then
			# Even with the rows landing out of order, the first of them is
			# still however long the quickest suite takes. This line carries
			# the clock and the names of what is still out, so the gap before
			# it reads as work rather than as a hang.
			running=
			for i in $(seq 0 $((n - 1))); do
				[[ "${REAPED[i]}" == 1 ]] && continue
				running="${running:+$running, }${NAMES[i]}"
			done
			ui_status "$(((n - left) * 100 / n))" \
				"$((n - left))/$n done · $(($(date +%s) - started))s · $running"
			sleep 0.1 2>/dev/null || sleep 1
		fi
	done
	ui_clear
	# Replay only what needs eyes: the FAIL rows of any failing suite.
	for i in $(seq 0 $((n - 1))); do
		IFS='|' read -r _ _ failed _ rc <"${METAS[i]}"
		if [[ "$rc" != 0 || "$failed" != 0 ]]; then
			printf '\n== %s ==\n' "$(suite_label "${NAMES[i]}")"
			grep -E '^[[:space:]]+FAIL[[:space:]]|RESULT: FAIL|error|Error' "${OUTS[i]}" | head -40 || true
		fi
	done
fi

printf '\n  %-24s %8s %8s %6s\n' "Suite" "Passed" "Failed" "Time"
tp=0 tf=0 tt=0 bad=0
for i in $(seq 0 $((n - 1))); do
	IFS='|' read -r s passed failed secs rc <"${METAS[i]}"
	mark="+"
	if [[ "$rc" != 0 || "$failed" != 0 ]]; then mark="x" bad=1; fi
	tp=$((tp + passed)) tf=$((tf + failed))
	[[ "$secs" -gt "$tt" ]] && tt=$secs
	printf '  %s %-22s %8d %8d %5ss\n' "$mark" "$(suite_label "$s")" "$passed" "$failed" "$secs"
done
printf '  %s %-22s %8d %8d %5ss\n' "*" "Total" "$tp" "$tf" "$tt"

if [[ "$bad" == 0 ]]; then
	printf '\n  + All host-side suites passed.\n'
	printf '  Hardware-in-loop validation (DK/ESP32 + iPhone) runs separately.\n\n'
else
	printf '\n  x Suite failure — FAIL rows above.\n\n'
	exit 1
fi
