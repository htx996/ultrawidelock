#!/usr/bin/env bash
# regress-hil.sh — the hardware tier of the regression suite: build every
# DWM3001CDK configuration, then prove on the bench that the ones that matter
# still work on air.
#
# `make check` proves the logic and `make fw-regress` proves it links. Neither
# says anything about the radio, and the things this firmware gets wrong at the
# radio are the expensive ones: an STS the reader cannot decrypt, a Pre-POLL that
# never arms, a board that boots without finding its own DW3110. Those need
# boards, so they live here rather than in a suite anything can run.
#
# Usage:
#   scripts/regress-hil.sh                  # build, then every hardware stage
#   scripts/regress-hil.sh --skip-build     # judge what is already built/flashed
#   scripts/regress-hil.sh --selftest       # add the UWB boot self-test stage,
#                                           # which REFLASHES the reader
#   HITL_ARGS="--skip-enrol" scripts/regress-hil.sh   # passed through to hitl-run.sh
#
# Exit: 0 PASS · 1 FAIL (a stage ran and did not pass) · 2 REFUSED (a stage could
# not run: no probe, no signing key -- the reason on stderr). The split matters
# on a bench, where "the rig is unplugged" and "the firmware broke" arrive
# through the same red and must not be confused.
#
# Artifacts land in build/regress-hil/<timestamp>/: one log per stage, plus
# verdict.txt mapping each stage to its row in docs/hardware-validation.md.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT" || exit 2

SKIP_BUILD=0
WANT_SELFTEST=0
for arg in "$@"; do
	case "$arg" in
	--skip-build) SKIP_BUILD=1 ;;
	--selftest) WANT_SELFTEST=1 ;;
	-h | --help)
		sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "unknown option: $arg (see --help)" >&2
		exit 2
		;;
	esac
done

TS="$(date +%Y%m%d-%H%M%S)"
ART="$REPO_ROOT/build/regress-hil/$TS"
mkdir -p "$ART"

die() { # refused, not failed
	printf '  refused: %s\n' "$*" >&2
	printf 'REFUSED  %s\n' "$*" >>"$ART/verdict.txt"
	exit 2
}

# stage <name> <hardware-validation row(s)> <command...>
fails=0
stage() {
	local name="$1" rows="$2"
	shift 2
	printf '\n── regress-hil · %s  (%s)\n' "$name" "$rows"
	if "$@" >"$ART/$name.log" 2>&1; then
		printf '  ok    %s\n' "$name"
		printf 'PASS     %-12s %s\n' "$name" "$rows" >>"$ART/verdict.txt"
	else
		local rc=$?
		printf '  FAIL  %s  (rc=%d, %s)\n' "$name" "$rc" "$ART/$name.log"
		printf 'FAIL     %-12s %s  rc=%d\n' "$name" "$rows" "$rc" >>"$ART/verdict.txt"
		fails=$((fails + 1))
	fi
}

printf '\n  ultrawidelock · hardware-in-the-loop regression\n'
printf '  artifacts: %s\n' "$ART"

# ---- preflight ---------------------------------------------------------------
# Everything below flashes or reads a board. A missing probe or key is the rig,
# not the firmware, so it refuses here rather than failing a stage later.
command -v probe-rs >/dev/null 2>&1 ||
	die "probe-rs not installed -- the reader cannot be flashed or watched"
probe-rs list >"$ART/probes.log" 2>&1 ||
	die "probe-rs found no debug probe (see $ART/probes.log)"
bash "$REPO_ROOT/scripts/check-signing-key.sh" >"$ART/key.log" 2>&1 ||
	die "no signing key -- run 'make dfu-key' (see $ART/key.log)"

# ---- build -------------------------------------------------------------------
if [ "$SKIP_BUILD" = 0 ]; then
	stage build "compile+fit" make --no-print-directory fw-regress
	# A hardware verdict from a tree that did not build is not a verdict.
	[ "$fails" = 0 ] || {
		printf '\n  x regress-hil: the tree does not build -- no board was touched\n\n'
		exit 1
	}
fi

# ---- hardware ----------------------------------------------------------------
# The walk-up loop: enrol, flash the DK initiator, and judge both boards off
# ESTABLISHED / Pre-POLL / PREPOLL OK. This is the automatable core of the
# DK-and-reader rows of docs/hardware-validation.md.
# shellcheck disable=SC2086 # HITL_ARGS is a deliberate word list
stage walkup "CDK-5..CDK-8" bash "$REPO_ROOT/scripts/hitl-run.sh" ${HITL_ARGS:-}

# The UWB boot self-test proves the radio answers over SPI at all -- worth having
# when walkup fails, because it separates "no DW3110" from "no agreement". It is
# opt-in because it replaces the image on the reader, which the walkup stage
# above assumes is the deployed one.
if [ "$WANT_SELFTEST" = 1 ]; then
	# The RTT stream never ends on its own, so the monitor is given a window and
	# then killed. `timeout` is GNU coreutils and this bench is a Mac, so the
	# window is a backgrounded sleep-and-kill instead.
	stage uwb-selftest "CDK-4" bash -c '
		set -e
		b="${ULTRAWIDELOCK_BUILD_ROOT:-build}/cdk-selftest"
		make --no-print-directory selftest
		make --no-print-directory flash CDK_BUILD="$b"
		rtt="$(mktemp)"
		make --no-print-directory monitor CDK_RTT_BUILD="$b" >"$rtt" 2>&1 &
		mon=$!
		( sleep 60; kill "$mon" 2>/dev/null ) &
		win=$!
		wait "$mon" 2>/dev/null || true
		kill "$win" 2>/dev/null || true
		cat "$rtt"
		grep -q "DEV_ID = 0xdeca0302" "$rtt"
	'
fi

# ---- verdict -----------------------------------------------------------------
printf '\n  verdict: %s\n' "$ART/verdict.txt"
if [ "$fails" -ne 0 ]; then
	printf '  x regress-hil: %d stage(s) failed -- logs in %s\n\n' "$fails" "$ART"
	exit 1
fi
printf '  + regress-hil: every hardware stage passed\n'
printf '  Rows CDK-9, CDK-10 and CDK-14..CDK-18 stay manual -- see docs/hardware-validation.md\n\n'
