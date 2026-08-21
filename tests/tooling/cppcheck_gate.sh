#!/usr/bin/env bash
#
# cppcheck_gate.sh — static analysis over our C, with the vendored trees cut out.
#
# WHAT IS BEING PREVENTED. The host suites prove behaviour on the paths a test
# thinks to exercise, and `make test-san` proves memory behaviour on those same
# paths. Neither says anything about a branch no suite reaches -- and on this
# firmware most branches are exactly that: a malformed APDU, a truncated UWB
# frame, an error return nobody fakes. cppcheck reads every path whether a test
# reaches it or not, so a use-after-free or an out-of-bounds index down an error
# arm fails here instead of on a lock somebody owns.
#
# It is a complement to the two proofs already in the tree, not a replacement:
#   tests/host/cbmc.sh   proves the wire parsers exhaustively, within bounds
#   make test-san        proves the tested paths under ASan + UBSan
#   this gate            reads every path, shallowly, across the whole tree
#
#   tests/tooling/cppcheck_gate.sh              # scan the tracked sources
#   tests/tooling/cppcheck_gate.sh --self-test  # prove the gate can actually fail
#   make check / make lint                      # runs it as the `lint` suite
#
# Exit 0 clean, 1 on a finding, 2 if the gate could not do its job.
#
# WHAT IS SCANNED. modules/ and include/ -- the portable tree, which is the code
# that compiles the same way everywhere and so is the code cppcheck can read
# without being handed an SDK. Two vendored trees inside it are cut, for the
# reason port_purity_check.sh exempts the same two: a finding in them is a bug
# report for someone else's repository.
#
#   modules/ultrawidelock_dw3000/dwt_uwb_driver/   vendored Qorvo decadriver
#   modules/ultrawidelock_dfu/src/detools/         vendored delta-patch engine
#
# WHY ports/ AND apps/ ARE NOT SCANNED, and what covers them instead. Their
# sources are inseparable from Zephyr and ESP-IDF: IS_ENABLED(), BT_GATT_CCC(),
# CFG_TUD_MEM_ALIGN and CONFIG_* are macros cppcheck cannot expand from source
# alone. Pointed at them it reports its own missing headers -- unparsed #if
# conditions, deliberate `#error` config guards it tripped itself, linker-symbol
# arithmetic in startup code that every C runtime on earth performs. That is
# noise about our build environment, not findings about our code, and a gate
# whose output is mostly noise is a gate people learn to skip.
#
# Analysing that code needs the real compile flags from a real build -- a
# compilation database emitted by `west build` or `idf.py build`, fed to a tool
# that reads one. `make sca` is that tool, but it is pointed at the same
# portable tree for the same reason, so nothing covers ports/ and apps/
# statically today. Whoever wires a target build's compile_commands.json into
# `make sca` closes that gap. This gate covers the portable half properly
# rather than covering everything badly.
#
# WHICH CHECKS. warning + portability only. `style` and `performance` are off on
# purpose -- they are opinions about how C should read, and a gate that fails a
# pull request over an opinion gets weakened until it fails nothing. warning and
# portability are claims about defects: uninitialised reads, out-of-bounds
# indices, integer-width assumptions that break between the 32-bit targets and
# the 64-bit host the suites run on. Those are worth a red build.
#
# --error-exitcode=1 makes every enabled finding fail. There is no baseline file
# and no ratchet, because the baseline is empty and the way it stays empty is
# that adding to it is not an option.
#
# ABOUT SUPPRESSIONS. cppcheck cannot see that a bounded loop fills an array, so
# `for (i = 0; i < N; i++) buf[i] = ...` followed by a read of buf is reported as
# an uninitialised read every time ("Assuming condition is false" in its own
# note). Those sites carry an inline `cppcheck-suppress <id>` comment on the line
# above the report, with the reason in an ordinary comment above that. Inline, so
# the justification sits where the next reader is confused and moves with the
# code if it moves.
#
# The reason is a separate comment rather than a `# ...` tail on the suppression
# itself, and the marker is the single-line form rather than the begin/end pair,
# because both of those conveniences are newer than the cppcheck some
# distributions ship and this gate has to say the same thing on every machine.
# The two exemptions that belong to the tool rather than to a line of code are
# --suppress arguments in run_cppcheck below, documented there.
#
# SOME OF THOSE MARKERS ARE CURRENTLY NO-OPS, and they stay anyway. Which
# fill-loop false positives cppcheck actually emits moves with its version and
# with the flags it is handed: on 2.21 with the flags below only two of the
# seven still fire (ultrawidelock_approach.c's `t[n / 2]` and the `t, tlen`
# read in hkdf_expand, both cases needing an argument about a caller rather
# than about the line). The other five fired on the very first scan of this
# tree and no longer do. A marker that suppresses nothing costs a comment; a
# missing one costs a red pull request on whichever cppcheck a runner
# installed, so the whole set is kept. The cost being accepted, stated plainly:
# a dead marker would also hide a REAL uninitialised read if one ever appeared
# on exactly that line.

set -euo pipefail

# Same shape as tests/tooling/uwb_seam_check.sh, the sibling gate this mirrors.
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
	R=$'\033[31m' G=$'\033[32m' Y=$'\033[33m' Z=$'\033[0m'
else
	R='' G='' Y='' Z=''
fi

cd "$(dirname "$0")/../.."

# The scan roots and the cuts. Written once and shared by the scan and the
# self-test, so the self-test cannot pass against a narrower tree than the scan.
ROOTS=(modules include)
IGNORES=(
	modules/ultrawidelock_dw3000/dwt_uwb_driver
	modules/ultrawidelock_dfu/src/detools
)

# cppcheck resolves `#include "ultrawidelock_x.h"` only if it is told where the
# module include directories are. Without them it silently analyses a
# configuration in which the header never arrived -- which is how a deliberate
# `#error` guard gets reported as a defect. Built by glob rather than listed, so
# a new module is covered the day it appears.
inc_args=()
for d in include modules/*/include; do
	[ -d "$d" ] && inc_args+=("-I$d")
done

# ---- how long the scan takes -----------------------------------------------
# Two flags, neither of which changes a single finding, and together they are
# the difference between this gate being the reason `make check` takes minutes
# and it being noise. It is the suite people wait on, so this is worth the ink.
#
# -j: cppcheck analyses one file at a time by default, which on this tree is
# about twenty seconds of a single core while scripts/test-runner.sh has the
# other seven idle -- the fast suites have all landed by then. The job count is
# the machine's, not a constant, so a laptop and a CI runner each use what they
# have. Only `unusedFunction` is incompatible with -j, and --enable above asks
# for warning and portability, so nothing is lost by turning it on.
#
# --cppcheck-build-dir: cppcheck's own incremental cache. It keys the stored
# analysis on the file AFTER preprocessing, so editing a header re-checks every
# source that includes it and a stale pass is not a way this can fail -- the
# same reason it is safe to leave on in CI. An unchanged tree re-scans in about
# a second instead of twenty.
#
# It lives under the repo's one build root with everything else generated, so
# `make clean` takes it and no scan output is left anywhere else in the tree.
# Created here rather than by the caller: cppcheck does not create the directory
# and treats a missing one as a hard error, which would turn the cache into a
# way for the gate to stop working.
CPPCHECK_JOBS="${CPPCHECK_JOBS:-$( (getconf _NPROCESSORS_ONLN || sysctl -n hw.ncpu) 2>/dev/null || echo 4)}"
CPPCHECK_CACHE="${ULTRAWIDELOCK_BUILD_ROOT:-$PWD/build}/host/cppcheck"
mkdir -p "$CPPCHECK_CACHE" 2>/dev/null || CPPCHECK_CACHE=

# The exemptions, all of them, in one place. Two rounds of red CI decided both
# the form and the location, so both are worth stating.
#
# NOT A --suppressions-list FILE. cppcheck strips `#` comments out of one and
# older builds then reject the empty line that is left ("Failed to add
# suppression. No id."), so a file whose prose explained itself could not be read
# by the version the runner installs. `id:path` on a command line is the oldest
# and most portable form cppcheck has.
#
# NOT INLINE `cppcheck-suppress` MARKERS EITHER, which is the less obvious half.
# An inline marker suppresses the NEXT LINE, so it only works if the analyser
# reports where you expected. It does not: the uninitialised-`t` false positive
# in the hashing code lands on hkdf_expand's `update(&s, t, tlen)` under 2.21 and
# on sha256_update's `memcpy(s->buf, p, len)` -- a different function -- under
# the 2.13 a stock Ubuntu runner installs. Markers placed for one version are
# inert on the other, which is exactly how this gate failed CI a second time.
# Suppressing by id and path holds still across both.
#
# The cost, stated rather than buried: `uninitvar` is off for the whole of
# ultrawidelock_hash.c and ultrawidelock_approach.c, not just the lines that
# trip it. Both are covered from other directions -- the SHA-256/HMAC/HKDF KATs
# in tests/host, the CBMC harnesses, and `make test-san` -- so a real
# uninitialised read there has three other ways to be caught. Narrow it again
# the day cppcheck can see that a bounded loop fills the array it just filled.
#
#   preprocessorErrorDirective in ultrawidelock_ml_feat.c
#     The file opens with `#if ULTRAWIDELOCK_ML_LOS_N_FEATURES != 2 / #error`.
#     N_FEATURES comes from the generated ultrawidelock_ml_los_features.h via
#     ultrawidelock_ml.h; cppcheck brute-forces preprocessor configurations and
#     analyses ones where that include did not resolve, where N_FEATURES is 0,
#     the guard is true and the #error is "reached". It is reporting that the
#     guard works. NOT A HOLE: every real compile of this file -- host suites and
#     all three ports -- resolves the header and evaluates the condition for
#     real, so a regenerated model with a different feature count still fails
#     the build.
#
#   objectIndex in ultrawidelock_hash.c
#     sha256_block() takes `const uint8_t p[64]` and indexes all 64. cppcheck
#     reaches it down a path where the caller passed a one-byte object
#     (&counter, in hkdf_expand) and reports indexing past it. That path does
#     not exist: ultrawidelock_sha256_update() only passes the caller's pointer
#     once len >= BLOCK, and the short buffered call goes through s->buf.
#     Scoped to this file rather than an inline marker at the site because the
#     report lands on two lines of one statement, and the block form that would
#     cover both (cppcheck-suppress-begin) is newer than the cppcheck some
#     distributions ship. NOT A HOLE, but it is the widest exemption here: it
#     costs objectIndex on ~300 lines of self-contained hashing whose bounds
#     tests/host and CBMC both exercise.
run_cppcheck() { # <extra args...> -- prints findings, returns cppcheck's status
	local ig_args=()
	local i
	for i in "${IGNORES[@]}"; do
		ig_args+=("-i$i")
	done
	# See the CPPCHECK_JOBS / CPPCHECK_CACHE block above for why these are on.
	# Both are appended as arrays so an unusable cache directory drops the flag
	# rather than passing an empty path, and the self-test runs through exactly
	# the flags the real scan does.
	local fast_args=(-j "$CPPCHECK_JOBS")
	[ -n "$CPPCHECK_CACHE" ] && fast_args+=("--cppcheck-build-dir=$CPPCHECK_CACHE")
	cppcheck \
		"${fast_args[@]}" \
		--enable=warning,portability \
		--suppress=preprocessorErrorDirective:modules/ultrawidelock_ml/src/ultrawidelock_ml_feat.c \
		--suppress=objectIndex:modules/ultrawidelock_cred/src/ultrawidelock_hash.c \
		--suppress=uninitvar:modules/ultrawidelock_cred/src/ultrawidelock_hash.c \
		--suppress=uninitvar:modules/ultrawidelock_cred/src/ultrawidelock_approach.c \
		--suppress=toomanyconfigs \
		--suppress=missingInclude \
		--suppress=missingIncludeSystem \
		--error-exitcode=1 \
		--quiet \
		"${inc_args[@]}" \
		"${ig_args[@]}" \
		"$@"
}

# ---- the gate cannot do its job -------------------------------------------
# A missing cppcheck is reported and skipped rather than failed: it is a
# developer-machine gap, not a defect in the tree, and CI installs it so the
# scan always really runs somewhere. Loud on purpose -- a skipped scan that
# reads like a pass is worse than no scan. Same call gitleaks gets in mk/host.mk.
if ! command -v cppcheck >/dev/null 2>&1; then
	printf '\n  %s!! cppcheck not installed -- STATIC ANALYSIS SKIPPED%s\n' "$Y" "$Z"
	printf '     CI still runs it. install: brew install cppcheck\n\n'
	exit 0
fi

# The cuts must still be there. A vendored tree that is renamed or removed
# leaves a `-i` pointing at nothing: the scan silently widens to include code we
# do not own, and the first sign is a pull request failing on the decadriver.
# The same ratchet port_purity_check.sh applies to its exemptions -- an
# exemption nobody can retire is an exemption nobody is checking.
for i in "${IGNORES[@]}"; do
	if [ ! -d "$i" ]; then
		printf '%s  cppcheck gate: cut path no longer exists: %s%s\n' "$R" "$i" "$Z" >&2
		printf '  it moved or went away. Update IGNORES in %s.\n' "$0" >&2
		exit 2
	fi
done

# ---- self-test -------------------------------------------------------------
# Proves the gate can fail, on a defect of the class it exists to catch, run
# through the same flags the real scan uses. A gate nobody has watched fail is
# a gate nobody knows is wired up.
if [ "${1:-}" = "--self-test" ]; then
	tmp="$(mktemp -d -t oa-cppcheck.XXXXXX)"
	trap 'rm -rf "$tmp"' EXIT
	cat >"$tmp/bad.c" <<-'EOF'
		#include <stdint.h>

		/* An out-of-bounds read on a fixed array: the shape this gate exists
		 * to catch on a path no host suite reaches. */
		int32_t oob(void)
		{
			int32_t buf[4] = {0, 0, 0, 0};

			return buf[7];
		}
	EOF
	printf '\n  cppcheck gate self-test\n\n'
	if run_cppcheck "$tmp/bad.c" >/dev/null 2>&1; then
		printf '  %sFAIL%s  the gate passed a planted out-of-bounds read\n\n' "$R" "$Z"
		exit 2
	fi
	printf '  %sok%s    the gate fails on a planted out-of-bounds read\n\n' "$G" "$Z"
	exit 0
fi

# ---- the scan --------------------------------------------------------------
scan_roots=()
for d in "${ROOTS[@]}"; do
	[ -d "$d" ] && scan_roots+=("$d")
done
if [ ${#scan_roots[@]} -eq 0 ]; then
	printf '%s  cppcheck gate: none of the scan roots exist%s\n' "$R" "$Z" >&2
	exit 2
fi

printf '\n  cppcheck %s over %s\n\n' "$(cppcheck --version | awk '{print $2}')" "${scan_roots[*]}"

out="$(mktemp -t oa-cppcheck-out.XXXXXX)"
trap 'rm -f "$out"' EXIT

if run_cppcheck "${scan_roots[@]}" >"$out" 2>&1; then
	printf '  %sok%s    1 passed, 0 failed  ·  no findings\n\n' "$G" "$Z"
	exit 0
fi

# One `  FAIL <finding>` row per finding, on stdout, BEFORE the raw output.
#
# The row shape is not decoration. scripts/test-runner.sh runs the suites in
# parallel and replays a failing one through
#
#     grep -E '^[[:space:]]+FAIL[[:space:]]|RESULT: FAIL|error|Error'
#
# so anything that does not match that is dropped from the log. cppcheck writes
# most findings as `warning:`, which matches none of it: the first CI failure of
# this gate reported "2 finding(s)" and not one word about what or where, which
# is a gate that cannot be acted on from the log it produces. The same row also
# feeds suite_counts, so the failed column counts findings instead of always
# reading 1.
grep -E '\[[a-zA-Z]+\]$' "$out" | while IFS= read -r line; do
	printf '  %sFAIL%s  %s\n' "$R" "$Z" "$line"
done

# The raw output after the rows: notes, the "Assuming condition is false" lines
# and everything else needed to judge a finding, for whoever has the full log.
printf '\n'
cat "$out" >&2

# cppcheck exits nonzero for two different reasons: it found something (that is
# --error-exitcode), or it could not run -- a bad flag, an unreadable path, a
# version that does not know an option used above. Both look identical from the
# status alone, and calling the second one "findings" would report a broken gate
# as a red diff and send someone hunting a defect that was never there.
#
# The `[checkid]` suffix is what a finding line ends with, so its absence is the
# discriminator: nonzero with nothing to show means the tool failed, which is
# exit 2, the same code every other gate here uses for "could not do its job".
n="$(grep -cE '\[[a-zA-Z]+\]$' "$out" || true)"
if [ "${n:-0}" -eq 0 ]; then
	printf '\n%s  cppcheck gate: cppcheck exited nonzero but reported no findings%s\n' "$R" "$Z" >&2
	printf '  that is a broken run, not a defect. Output above; check the flags\n' >&2
	printf '  in %s against `cppcheck --version` (%s).\n\n' "$0" "$(cppcheck --version 2>/dev/null || echo unknown)" >&2
	exit 2
fi

printf '\n  %sFAIL%s  0 passed, 1 failed  ·  %s finding(s)\n' "$R" "$Z" "$n" >&2
printf '        a false positive gets an inline `cppcheck-suppress <id>` comment\n' >&2
printf '        on the line above it, and the reason on the line above that\n\n' >&2
exit 1
