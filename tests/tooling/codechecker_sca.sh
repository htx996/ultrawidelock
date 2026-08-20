#!/usr/bin/env bash
#
# codechecker_sca.sh — Clang Static Analyzer over the portable tree, via CodeChecker.
#
# WHAT IS BEING PREVENTED, and why this is not the cppcheck gate again. cppcheck
# reads a translation unit the way a careful reader does: pattern by pattern,
# one function at a time. The Clang Static Analyzer walks execution paths
# symbolically -- it carries what it knows about a value across function
# boundaries and down each branch separately, so it reaches the bugs that only
# exist as a sequence: a NULL checked on one arm and dereferenced on the other,
# a buffer freed on an error path a later line still reads, an allocation whose
# only exit forgets it. Those are the shapes that survive both a review and a
# test suite, and neither cppcheck nor CBMC is looking for them here.
#
#   tests/tooling/cppcheck_gate.sh  patterns, portable tree, ~15s  (make lint)
#   tests/host/cbmc.sh              proofs, wire parsers only      (make cbmc)
#   this script                     paths, portable tree, ~10s     (make sca)
#
#   tests/tooling/codechecker_sca.sh              # analyse, print findings
#   tests/tooling/codechecker_sca.sh --self-test  # prove the gate can fail
#   make sca                                      # the same thing
#
# Exit 0 clean, 1 on a finding, 2 if the gate could not do its job.
#
# NOT PART OF `make check` OR CI, deliberately. CodeChecker is a Python package
# rather than a one-line brew install, so requiring it would make a clean
# checkout fail for a reason that has nothing to do with the change under test.
# It is the deeper pass you run before a release or when touching parsing and
# session code, and the natural next step is CI running `CodeChecker diff`
# against a stored baseline so only NEW findings fail a pull request.
#
# WHERE THE COMPILE FLAGS COME FROM. The analyser needs the exact flags each
# file is really compiled with. tests/host/sources.sh already holds them -- it
# is the single definition the host suites, the coverage run and the sanitiser
# run all read -- so this script sources it and emits a compilation database
# from UNIT_SRCS, INCS and DEFS rather than keeping a second copy that could
# drift. That database is left at build/host/compile_commands.json, where an
# editor's clangd will also find it.
#
# Only UNIT_SRCS is analysed: our production sources. The test bodies and the
# host fakes (TEST_SRCS, SHIM_SRCS) are excluded -- a fake's whole job is to be
# a simplification, and findings about one say nothing about the firmware.
#
# ports/ AND apps/ ARE NOT COVERED HERE EITHER. Their flags live inside Zephyr's
# and ESP-IDF's own build systems, so a database for them has to come from a
# real `west build` / `idf.py build`, which needs the toolchains this script
# deliberately does not require. See cppcheck_gate.sh for the same boundary.

set -euo pipefail

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
	R=$'\033[31m' G=$'\033[32m' Y=$'\033[33m' Z=$'\033[0m'
else
	R='' G='' Y='' Z=''
fi

cd "$(dirname "$0")/../.."
ROOT="$PWD"

OUT="${ULTRAWIDELOCK_BUILD_ROOT:-$ROOT/build}/host"
CDB="$OUT/compile_commands.json"
REPORTS="$OUT/codechecker"

# CodeChecker is usually a venv or pipx install rather than something on PATH.
# CODECHECKER lets a caller point at one without changing this file.
CC_BIN="${CODECHECKER:-CodeChecker}"

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
	printf '\n  %s!! CodeChecker not installed -- PATH ANALYSIS SKIPPED%s\n' "$Y" "$Z"
	printf '     it is not part of `make check` or CI; this is the deeper pass.\n'
	printf '     install:  python3 -m venv .venv-sca\n'
	printf '               .venv-sca/bin/pip install codechecker\n'
	printf '               CODECHECKER=.venv-sca/bin/CodeChecker make sca\n\n'
	exit 0
fi

# clangsa is the analyser this gate is about. clang-tidy and the others are left
# to whatever the install happens to have: `--analyzers clangsa` below means a
# machine without clang-tidy gets the same result as one with it, so the gate
# says the same thing everywhere.
if ! command -v clang >/dev/null 2>&1; then
	printf '%s  sca: clang not found -- clangsa cannot run%s\n' "$R" "$Z" >&2
	exit 2
fi

# ---- the compilation database ---------------------------------------------
# Sourced, not parsed: sources.sh is bash and defines the arrays directly.
build_cdb() {
	mkdir -p "$OUT"
	# shellcheck source=/dev/null
	. "$ROOT/tests/host/sources.sh"
	# Empty is not a valid answer, and on the bash 3.2 a stock mac ships
	# "${EMPTY[@]}" under `set -u` aborts rather than expanding to nothing --
	# so an emptied array would end this script mid-function with a bare
	# "unbound variable" instead of saying which list went missing.
	if [ "${#UNIT_SRCS[@]}" -eq 0 ] || [ "${#INCS[@]}" -eq 0 ] || [ "${#DEFS[@]}" -eq 0 ]; then
		printf '%s  sca: tests/host/sources.sh defined no sources, includes or defines%s\n' \
			"$R" "$Z" >&2
		exit 2
	fi
	python3 - "$ROOT" "$CDB" "${DEFS[@]}" -- "${INCS[@]}" -- "${UNIT_SRCS[@]}" <<-'PY'
		import json, sys

		root, out = sys.argv[1], sys.argv[2]
		rest = sys.argv[3:]
		i = rest.index("--")
		defs, rest = rest[:i], rest[i + 1:]
		i = rest.index("--")
		incs, srcs = rest[:i], rest[i + 1:]

		# -o /dev/null: the analyser never links, and a real object path would
		# race the host suites writing the same names into the same directory.
		db = [
		    {
		        "directory": root,
		        "file": s,
		        "arguments": ["cc", "-std=c11", "-c"] + defs + incs
		        + [s, "-o", "/dev/null"],
		    }
		    for s in srcs
		]
		with open(out, "w") as f:
		    json.dump(db, f, indent=1)
		print(len(db))
	PY
}

run_analysis() { # <compile-db> <report-dir>
	"$CC_BIN" analyze "$1" \
		--analyzers clangsa \
		--output "$2" \
		--jobs "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
}

# ---- self-test -------------------------------------------------------------
# A planted use-after-free: a path bug, which is the class this gate exists for
# and the class cppcheck's single-function view is weakest on.
if [ "${1:-}" = "--self-test" ]; then
	tmp="$(mktemp -d -t oa-sca.XXXXXX)"
	trap 'rm -rf "$tmp"' EXIT
	cat >"$tmp/bad.c" <<-'EOF'
		#include <stdlib.h>

		int uaf(int n)
		{
			int *p = malloc(sizeof(int) * 4);

			if (p == NULL) {
				return -1;
			}
			p[0] = n;
			free(p);
			return p[0];
		}
	EOF
	cat >"$tmp/cdb.json" <<-EOF
		[{"directory": "$tmp", "file": "$tmp/bad.c",
		  "arguments": ["cc", "-std=c11", "-c", "$tmp/bad.c", "-o", "/dev/null"]}]
	EOF
	printf '\n  CodeChecker sca self-test\n\n'
	run_analysis "$tmp/cdb.json" "$tmp/reports" >/dev/null 2>&1 || true
	st_rc=0
	"$CC_BIN" parse "$tmp/reports" >/dev/null 2>&1 || st_rc=$?
	# Exactly 2 -- reports present. Any other status means the self-test never
	# demonstrated anything, and reporting "ok" off a bare non-zero would be the
	# same fail-open the real run is written to avoid.
	if [ "$st_rc" -ne 2 ]; then
		printf '  %sFAIL%s  planted use-after-free: parse exited %s, wanted 2\n\n' \
			"$R" "$Z" "$st_rc"
		exit 2
	fi
	printf '  %sok%s    the gate fails on a planted use-after-free\n\n' "$G" "$Z"
	exit 0
fi

# ---- the run ---------------------------------------------------------------
printf '\n  CodeChecker · clangsa over the portable tree\n\n'

n_files="$(build_cdb)"
printf '  compile database: %s files  ->  %s\n\n' "$n_files" "${CDB#"$ROOT"/}"

# analyze exits nonzero when it FINDS something as well as when it breaks, so
# its status cannot distinguish the two. `parse` is the one that reports: 0 when
# there is nothing to report, 2 when there is.
if ! run_analysis "$CDB" "$REPORTS" >"$OUT/codechecker-analyze.log" 2>&1; then
	if ! grep -q "Analysis finished" "$OUT/codechecker-analyze.log"; then
		printf '%s  sca: the analysis did not finish%s\n' "$R" "$Z" >&2
		tail -20 "$OUT/codechecker-analyze.log" >&2
		exit 2
	fi
fi

# The verdict comes from parse's EXIT STATUS, not from grepping its prose: 0 is
# nothing to report, 2 is reports present. Reading the wording instead would
# fail open the day CodeChecker rephrases a line -- the grep stops matching, the
# gate prints "no findings", and it keeps saying that forever. A gate is allowed
# to break; it is not allowed to break quietly into a pass.
parse_out="$OUT/codechecker-parse.log"
parse_rc=0
"$CC_BIN" parse "$REPORTS" >"$parse_out" 2>&1 || parse_rc=$?

case "$parse_rc" in
0)
	printf '  %sok%s    1 passed, 0 failed  ·  no findings\n\n' "$G" "$Z"
	exit 0
	;;
2)
	grep -vE '^Found no defects' "$parse_out" >&2 || true
	printf '\n  %sFAIL%s  0 passed, 1 failed\n' "$R" "$Z" >&2
	printf '        a false positive gets a `// codechecker_suppress [checker] why`\n' >&2
	printf '        comment on the line above the report\n\n' >&2
	exit 1
	;;
*)
	printf '\n%s  sca: CodeChecker parse exited %s -- neither clean (0) nor reports (2)%s\n' \
		"$R" "$parse_rc" "$Z" >&2
	tail -20 "$parse_out" >&2
	printf '\n  treating that as a broken gate, not a clean tree.\n\n' >&2
	exit 2
	;;
esac
