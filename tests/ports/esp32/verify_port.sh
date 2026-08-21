#!/usr/bin/env bash
#
# On-target build/link guard for the additive ESP32 port. The engine logic is
# covered by tests/host; this checks the things unique to THIS port that can
# silently regress as main is merged in:
#   1. the esp32s3 build still links,
#   2. the CCC STS seam is still wired (all four uwb_seam.h helpers defined, and
#      the engine reaching the radio through them rather than around them),
#   3. the excluded diagnostic files stay out of the build,
#   4. the app still fits its partition.
#
# Auto-sources the ESP-IDF env via $IDF_EXPORT (default $HOME/esp/esp-idf/export.sh,
# same convention as examples/esp32/reader/Makefile; override to relocate). Skips with a
# clear notice if ESP-IDF is absent, so the fast host test (run.sh) never needs it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
# The bench project still produces the ultrawidelock_uwb_esp32s3.* artifacts checked below.
PROJ="$REPO_ROOT/examples/esp32/reader"
# NOT the app's own default build directory. `idf.py build` there picks up the
# project's sdkconfig, which is untracked working state -- `make presence-on`
# writes to it, so a developer who has ever run that gets a local PASS on an
# image CI would never produce, and every check below silently describes a
# different configuration than the one that ships. Build from sdkconfig.defaults
# into directories of this script's own, so a local run and a CI run assert the
# same thing, and so running the guard never clobbers a developer's own build.
#
# Both live under the one repo build root, so `make clean` removes them. They
# are absolute because idf.py runs with its cwd in $PROJ, not here.
BUILD_ROOT="${ULTRAWIDELOCK_BUILD_ROOT:-$REPO_ROOT/build}"
BUILD="$BUILD_ROOT/esp32-example-reader-esp32s3-verify"
fail=0
note() { printf '  %-4s %s\n' "$1" "$2"; }
check() { if eval "$2"; then note ok "$1"; else note FAIL "$1"; fail=1; fi; }

# Building firmware is a separate job. This suite normally runs on a machine
# with no ESP-IDF, so it never reaches the build below;
# ULTRAWIDELOCK_NO_TARGET_BUILD=1 makes a developer shell that HAS ESP-IDF sourced behave
# the same way, so a pre-PR sweep stays host-only instead of turning into a
# multi-minute build the sweep was never meant to include.
if [ -n "${ULTRAWIDELOCK_NO_TARGET_BUILD:-}" ]; then
	echo "SKIP verify_port.sh: target build off by request (ULTRAWIDELOCK_NO_TARGET_BUILD=1)."
	exit 0
fi

# Bring the ESP-IDF env onto PATH automatically if it is not already, without
# baking any machine-specific path into the repo: source $IDF_EXPORT (default
# $HOME/esp/esp-idf/export.sh, override to relocate). In CI the file is absent,
# so we fall through to the SKIP below. Relax -e/-u across the source since
# export.sh is not written for a strict-mode caller.
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
if ! command -v idf.py >/dev/null 2>&1 && [ -f "$IDF_EXPORT" ]; then
	set +eu
	# shellcheck disable=SC1090
	. "$IDF_EXPORT" >/dev/null 2>&1 || true
	set -eu
fi

if ! command -v idf.py >/dev/null 2>&1; then
	echo "SKIP verify_port.sh: idf.py not on PATH; set IDF_EXPORT=/path/to/esp-idf/export.sh"
	exit 0
fi

echo "building esp32s3 target (defaults only)..."
( cd "$PROJ" && idf.py -B "$BUILD" \
	-DSDKCONFIG="$BUILD/sdkconfig" \
	-DSDKCONFIG_DEFAULTS="sdkconfig.defaults" \
	build >/dev/null )
# The guard is only as good as the config it built. Assert the defaults build is
# really presence-free, or section 4 below proves nothing by passing.
if grep -q '^CONFIG_ULTRAWIDELOCK_PRESENCE=y' "$BUILD/sdkconfig"; then
	echo "verify_port: FAIL — the defaults build has presence ON; sdkconfig.defaults changed?" >&2
	exit 1
fi

echo "1. build artifacts"
check "app binary"       "[ -f '$BUILD/ultrawidelock_uwb_esp32s3.bin' ]"
check "app elf"          "[ -f '$BUILD/ultrawidelock_uwb_esp32s3.elf' ]"
check "partition table"  "[ -f '$BUILD/partition_table/partition-table.bin' ]"

echo "2. CCC STS seam (modules/ultrawidelock_uwb/include/uwb_seam.h)"
# Each helper must be defined (T) in some object, else the seam has no engine
# behind it and the CCC STS is never programmed.
# The symbol table is read ONCE and the four checks below query the string.
#
# It was a full walk of the build tree per check, spawning one nm per object
# file -- four scans, thousands of processes, and the slowest thing in
# `make check` by a distance: this suite is the last row to land and this was
# most of it. `-exec nm {} +` hands nm as many files as a command line holds
# instead of one, and hoisting the result out of the function stops the other
# three checks from repeating the walk at all.
#
# nm labels each file with a `path:` header when given several at once. That
# only adds lines between the symbols; every symbol line keeps the ` T name`
# shape these greps match, so what the checks see is unchanged.
#
# The lookup is a here-string, NOT `printf ... | grep -q`. This file runs under
# `set -o pipefail`, and `grep -q` exits the moment it matches: that closes the
# pipe under the producer, which dies of SIGPIPE (141), which pipefail hands
# back as the pipeline's status. The check would then report FAIL on a symbol it
# had just found -- all four of these did, and only because the match succeeded
# early. A here-string gives grep the text with no pipeline to break.
_SEAM_SYMS=
seam_syms_load() {
	[ -n "$_SEAM_SYMS" ] ||
		_SEAM_SYMS="$(find "$BUILD" -name '*.obj' -exec nm {} + 2>/dev/null || true)"
}
seamdef() { seam_syms_load; grep -qE " T $1\$" <<<"$_SEAM_SYMS"; }
check "def ultrawidelock_uwb_arm_rx"        "seamdef ultrawidelock_uwb_arm_rx"
check "def ultrawidelock_uwb_set_sts_iv"    "seamdef ultrawidelock_uwb_set_sts_iv"
check "def ultrawidelock_uwb_set_callbacks" "seamdef ultrawidelock_uwb_set_callbacks"
check "def ultrawidelock_uwb_configure_phy" "seamdef ultrawidelock_uwb_configure_phy"
# The caller must reach the radio THROUGH the seam. A regression here is silent
# on the bench: ranging still runs, the STS is just never substituted.
UWBMIN="$(find "$BUILD" -name 'uwb_min.c.obj' | head -1)"
check "uwb_min goes through the seam" \
	"[ -n '$UWBMIN' ] && nm '$UWBMIN' | grep -qE ' U ultrawidelock_uwb_arm_rx$'"
check "uwb_min does not call dwt_rxenable directly" \
	"[ -n '$UWBMIN' ] && ! nm '$UWBMIN' | grep -qE ' U dwt_rxenable$'"

echo "3. excluded diagnostic files stay out"
for d in uwb_rxdiag uwb_selftest ccc_crypto_psa ultrawidelock_shell ultrawidelock_logquiet dw3000_spi_trace; do
	check "no $d.obj" "! find '$BUILD' -name '$d*.obj' | grep -q ."
done

echo "4. presence build (CONFIG_ULTRAWIDELOCK_PRESENCE=y)"
# Presence is default n and no sdkconfig.defaults sets it, so everything above
# this line passes with every line of presence code uncompiled. Build it in its
# own directory, with its own sdkconfig, so the default build and a developer's
# working config are both left alone.
PBUILD="$BUILD_ROOT/esp32-example-reader-esp32s3-presence"
if ( cd "$PROJ" && idf.py -B "$PBUILD" \
	-DSDKCONFIG="$PBUILD/sdkconfig" \
	-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.presence" \
	build >/dev/null 2>&1 ); then
	note ok "presence build links"
	# Linking is not enough: --gc-sections has eliminated these before, which is
	# why the symbols are named individually rather than trusting the config.
	PNM="${ULTRAWIDELOCK_NM:-xtensa-esp32s3-elf-nm}"
	psym() { "$PNM" "$PBUILD/ultrawidelock_uwb_esp32s3.elf" 2>/dev/null | grep -qE " T $1$"; }
	dsym() { "$PNM" "$BUILD/ultrawidelock_uwb_esp32s3.elf" 2>/dev/null | grep -qE " T $1$"; }
	if command -v "$PNM" >/dev/null 2>&1; then
		check "presence_link_init survives gc"      "psym presence_link_init"
		check "presence_link_cmd survives gc"       "psym presence_link_cmd"
		check "ultrawidelock_assert_build_p256 survives gc" "psym ultrawidelock_assert_build_p256"
		check "ultrawidelock_assert_ec_sign survives gc"    "psym ultrawidelock_assert_ec_sign"
		# Negative control, asserted rather than assumed: a name that is linked
		# either way makes the four checks above pass whatever happens to the
		# presence config. ultrawidelock_ec_p256_keygen and ultrawidelock_ecdsa_p256_sign were
		# the obvious picks and are exactly that -- the credential path
		# pulls both in with presence off -- so the discrimination is checked
		# here instead of being taken on trust.
		check "presence absent from defaults build" \
			"! dsym presence_link_init && ! dsym ultrawidelock_assert_build_p256"
	else
		note skip "presence symbols ($PNM not on PATH; set ULTRAWIDELOCK_NM=)"
	fi
else
	note FAIL "presence build links"
	fail=1
fi

echo "5. app fits partition"
# check_sizes.py runs at build time and fails the build on overflow; re-assert
# the app binary is smaller than the 4 MB factory partition as a direct signal.
SZ=$(stat -f%z "$BUILD/ultrawidelock_uwb_esp32s3.bin" 2>/dev/null || stat -c%s "$BUILD/ultrawidelock_uwb_esp32s3.bin")
check "app < 4 MB factory ($SZ B)" "[ '$SZ' -lt 4194304 ]"

echo
if [ "$fail" -eq 0 ]; then echo "verify_port: PASS"; else echo "verify_port: FAIL"; fi
exit "$fail"
