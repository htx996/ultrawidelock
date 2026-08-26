#!/usr/bin/env bash
#
# ci_scope_check.sh — the predicate in .github/workflows/ci.yml that decides
# whether a pull request compiles firmware at all.
#
# It is an ignore list, so its two failure directions are not symmetric. A path
# wrongly left OUT costs one build nobody needed, and says so in the log. A path
# wrongly put IN makes the job report green over code it never compiled, which
# is the exact hole ci.yml exists to close and which nothing else would notice.
# So the "must BUILD" half below is the half that matters.
#
# THE PATTERN IS READ OUT OF THE WORKFLOW, never copied. A copy would keep
# passing while the job it describes drifted away from it, which is the same
# silent-green failure in a different place.
#
# Usage: tests/tooling/ci_scope_check.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CI="$ROOT/.github/workflows/ci.yml"
PASS=0; FAIL=0

ok()  { PASS=$((PASS + 1)); printf '  ok    %-6s %s\n' "$1" "$2"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL  want %s got %s: %s\n' "$1" "$2" "$3"; }

[ -f "$CI" ] || { echo "ERROR: no $CI" >&2; exit 1; }

# Lift the three IGNORE= lines out of the job and run them here. Anchored on the
# variable name, so a rename fails loudly rather than silently testing nothing.
ignore_src="$(sed -n 's/^ *\(IGNORE=.*\)$/\1/p' "$CI")"
[ -n "$ignore_src" ] || {
  echo "ERROR: no IGNORE= assignment in $CI -- the scope predicate was renamed" >&2
  echo "       or rewritten, and this suite is checking nothing. Update it." >&2
  exit 1
}
eval "$ignore_src"
[ -n "${IGNORE:-}" ] || { echo "ERROR: IGNORE came out empty" >&2; exit 1; }

printf '\n== the scope predicate ==\n'
printf '  pattern: %s\n\n' "$IGNORE"

decide() { printf '%s\n' "$1" | grep -qvE "$IGNORE" && echo build || echo skip; }
check()  { got="$(decide "$2")"; [ "$got" = "$1" ] && ok "$1" "$2" || bad "$1" "$got" "$2"; }

# Cannot reach a compiler. Each of these is a claim someone made in ci.yml.
check skip docs/configuring.md
check skip docs/hardware-validation.md
check skip web/index.html
check skip README.md
check skip apps/dwm3001cdk-lock/README.md
check skip .github/workflows/pages.yml
check skip .github/workflows/release.yml
check skip release/dwm3001cdk/FLASH.md
check skip release/dwm3001cdk/flash.sh
check skip tests/host/sources.sh
check skip tests/shared/run.sh
check skip tests/sdk/run.sh
check skip tests/ports/freertos-nrf52833/run.sh
check skip scripts/release-notes.sh
check skip mk/host.mk

# Reaches a compiled image. A false "skip" here is the silent-green failure.
check build apps/dwm3001cdk-lock/src/main.c
check build apps/dwm3001cdk-lock/Kconfig
check build apps/dwm3001cdk-lock/boards/decawave_dwm3001cdk.overlay
check build apps/satellite/src/main.c
check build modules/ultrawidelock_cred/CMakeLists.txt
check build ports/zephyr/glue.c
check build west.yml
check build mk/cdk.mk
check build mk/satellite.mk
# The workspace is most of what gets compiled, and these two decide what it holds.
check build scripts/bootstrap.sh
check build scripts/lib/ws.sh
check build integrations/nrfconnect-door-lock/patches/custom_impl-uwb.patch
# Eleven patches became this directory; it is application source now.
check build integrations/nrfconnect-door-lock/matter-aliro-door-lock-app/src/main.cpp
# Looks as inert as its siblings and is not: INSTRUMENT_CONF in mk/cdk.mk reads
# it into a real build.
check build tests/tooling/zephyr-opt-overlays/latency-uwb-spi.conf
check build tests/tooling/patch_drift_check.sh
# The ignore list must not ignore the job that owns it.
check build .github/workflows/ci.yml
# The point of an ignore list: something nobody has thought of yet still builds.
check build somethingnew/thing.c

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
printf 'ci scope: PASS\n'
