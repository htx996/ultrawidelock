#!/usr/bin/env bash
#
# Hold the port's UWB source set to what it claims.
#
# ports/freertos-nrf52833/uwb/uwb.cmake declares that set, and the host suite
# has no cross-compiler to link it with. Until a target build runs, a path that
# does not exist or a configuration that selects the wrong backend is invisible
# here and surfaces as an unrelated missing symbol at someone else's first link.
# So it is asserted now.
#
# The fragment is read by CMake itself, in script mode, with the handful of
# commands the surrounding build would have supplied stubbed out so the library
# it declares can be inspected instead of built. Re-parsing the file by hand
# would only check this script's copy of the expansion rules.
#
# What is checked, and how each one goes wrong quietly:
#
#   every manifest exists   a renamed role manifest expands to nothing rather
#                           than to an error, so its sources simply vanish
#   every path resolves     a manifest entry naming a moved file still compiles
#                           on the ports that do not read it
#   the crypto adapter      CCC uses the one portable adapter and links the
#                           standalone primitive provider beneath it
#   the Final snapshot      not a diagnostic on this part: without it the ranges
#                           are kilometres wide, on a single core where BLE
#                           shares the CPU with the ranging callbacks
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
CMAKE_FRAGMENT="${ULTRAWIDELOCK_UWB_CMAKE:-$ROOT/ports/freertos-nrf52833/uwb/uwb.cmake}"

checks=0
failures=0

check() { # <label> <status>
	checks=$((checks + 1))
	if [ "$2" -eq 0 ]; then
		printf '  ok   %s\n' "$1"
	else
		failures=$((failures + 1))
		printf '  FAIL %s\n' "$1"
	fi
}

# ---- self-test --------------------------------------------------------------
# The checks below pass against a fragment that is right, and they would also
# pass against a fragment they had quietly stopped reading. So each one is shown
# a fragment carrying the defect it exists to catch, and has to fail.
tmp=
self_test() {
	local mutants=0 survived=0 i=0

	# Not local: the EXIT trap runs after this function's scope is gone.
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT

	mutate() { # <description> <sed expression>
		local desc=$1 expr=$2 frag="$tmp/mutant_$i.cmake"

		i=$((i + 1))
		mutants=$((mutants + 1))
		sed -e "$expr" "$CMAKE_FRAGMENT" >"$frag"
		if cmp -s "$frag" "$CMAKE_FRAGMENT"; then
			printf '  FAIL mutation changed nothing: %s\n' "$desc"
			survived=$((survived + 1))
			return
		fi
		if ULTRAWIDELOCK_UWB_CMAKE="$frag" "$0" >"$tmp/out" 2>&1; then
			printf '  FAIL survives the check: %s\n' "$desc"
			survived=$((survived + 1))
		else
			printf '  ok   caught: %s\n' "$desc"
		fi
	}

	mutate "a role manifest that no longer exists" \
		's|responder_engine.list|responder_engine_gone.list|'
	mutate "a platform backend named by a path that moved" \
		's|uwb/dw3000_spi_freertos.c|uwb/dw3000_spi.c|'
	mutate "the hardware backend dropped from the set" \
		'/dw3000_hw_freertos.c/d'
	mutate "an include directory that does not exist" \
		's|modules/ultrawidelock_uwb/src/facade|modules/ultrawidelock_uwb/src/facade_gone|'
	mutate "the primitive adapter role dropped from the source set" \
		'/crypto_prim.list/d'
	mutate "the Final-capture snapshot dropped as if it were a diagnostic" \
		'/CONFIG_ULTRAWIDELOCK_UWB_FINAL_SNAPSHOT=1/d'
	mutate "the engine built as something other than a responder" \
		's|CONFIG_ULTRAWIDELOCK_UWB_RESPONDER=1|CONFIG_ULTRAWIDELOCK_UWB_INITIATOR=1|'
	mutate "the DW3720 device layer selected instead of the DW3000" \
		's|CONFIG_DW3000_CHIP_DW3000=1|CONFIG_DW3000_CHIP_DW3720=1|'
	mutate "the Zephyr backend dragged in beside this port's own" \
		's|uwb/dw3000_hw_freertos.c|../zephyr/dw3000/dw3000_hw.c|'
	mutate "the engine linked without the primitive provider its STS seam needs" \
		'/PRIVATE ultrawidelock_prim/d'
	mutate "the STS seam left to the Zephyr file this port does not compile" \
		'/ultrawidelock_seam_stubs.c/d'

	printf 'uwb-sources-self-test: %s (%d mutations)\n' \
		"$([ "$survived" -eq 0 ] && echo PASS || echo FAIL)" "$mutants"
	[ "$survived" -eq 0 ]
}

if [ "${1:-}" = "--self-test" ]; then
	self_test
	exit
fi

# ---- read the fragment ------------------------------------------------------

reader=$(mktemp -d)
trap 'rm -rf "$reader"' EXIT

cat >"$reader/read.cmake" <<CMAKE
cmake_minimum_required(VERSION 3.20)
set(ULTRAWIDELOCK_PORT_DIR "$ROOT/ports/freertos-nrf52833")

# Script mode has no targets, so the commands the fragment uses to declare its
# library are replaced by ones that record what it passed them. Everything is
# keyed on the target name: the fragment also declares a link-proof executable,
# and folding its sources or its --whole-archive link flags into the library's
# would make every check below answer a question about the wrong target.
set(ULTRAWIDELOCK_LIB ultrawidelock_uwb)
macro(add_library _name)
  if("\${_name}" STREQUAL "\${ULTRAWIDELOCK_LIB}")
    set(ULTRAWIDELOCK_SOURCES \${ARGN})
    list(REMOVE_ITEM ULTRAWIDELOCK_SOURCES STATIC)
  endif()
endmacro()
macro(add_executable _name)
endmacro()
macro(target_include_directories _name)
  if("\${_name}" STREQUAL "\${ULTRAWIDELOCK_LIB}")
    set(_args \${ARGN})
    list(REMOVE_ITEM _args PUBLIC PRIVATE INTERFACE)
    list(APPEND ULTRAWIDELOCK_INCLUDES \${_args})
  endif()
endmacro()
macro(target_compile_definitions _name)
  if("\${_name}" STREQUAL "\${ULTRAWIDELOCK_LIB}")
    set(_args \${ARGN})
    list(REMOVE_ITEM _args PUBLIC PRIVATE INTERFACE)
    list(APPEND ULTRAWIDELOCK_DEFINES \${_args})
  endif()
endmacro()
macro(target_link_libraries _name)
  if("\${_name}" STREQUAL "\${ULTRAWIDELOCK_LIB}")
    set(_args \${ARGN})
    list(REMOVE_ITEM _args PUBLIC PRIVATE INTERFACE)
    list(APPEND ULTRAWIDELOCK_LIBRARIES \${_args})
  endif()
endmacro()
macro(target_link_options _name)
endmacro()
macro(set_target_properties _name)
endmacro()
# The reachable-set measurement hangs its report off a custom target, which
# script mode cannot declare because there is no build to attach it to. It has
# nothing to say about the library's source set either way.
macro(add_custom_target _name)
endmacro()
# ultrawidelock_roles.cmake records a configure dependency, which script mode has no
# directory to hang off. Harmless to swallow: nothing here is being built.
macro(set_property)
endmacro()

include("$CMAKE_FRAGMENT")

foreach(_entry IN LISTS \${ULTRAWIDELOCK_ASK})
  message("\${_entry}")
endforeach()
CMAKE

ask() { # <variable> -> its entries, one per line
	cmake -DULTRAWIDELOCK_ASK="$1" -P "$reader/read.cmake" 2>&1
}

if ! srcs=$(ask ULTRAWIDELOCK_SOURCES); then
	printf '  FAIL the UWB fragment could not be read:\n%s\n' "$srcs"
	printf 'uwb-sources: FAIL (1 checks)\n'
	exit 1
fi
incs=$(ask ULTRAWIDELOCK_INCLUDES)
defs=$(ask ULTRAWIDELOCK_DEFINES)
libs=$(ask ULTRAWIDELOCK_LIBRARIES)
lists=$(ask ULTRAWIDELOCK_UWB_ROLE_LISTS)

# ---- checks -----------------------------------------------------------------

[ -n "$srcs" ]
check "the source set is not empty" $?

# A manifest that has been renamed or deleted expands to nothing rather than to
# an error, so the role's sources vanish and a check over the surviving paths
# still passes. The manifests themselves are the thing to assert on.
missing=0
while IFS= read -r path; do
	[ -n "$path" ] || continue
	if [ ! -f "$path" ]; then
		printf '       missing role manifest: %s\n' "$path"
		missing=1
	fi
done <<EOF
$lists
EOF
check "every role manifest the port reads exists" "$missing"

missing=0
while IFS= read -r path; do
	[ -n "$path" ] || continue
	if [ ! -f "$path" ]; then
		printf '       missing source: %s\n' "$path"
		missing=1
	fi
done <<EOF
$srcs
EOF
check "every source the fragment names exists" "$missing"

missing=0
while IFS= read -r path; do
	[ -n "$path" ] || continue
	if [ ! -d "$path" ]; then
		printf '       missing include directory: %s\n' "$path"
		missing=1
	fi
done <<EOF
$incs
EOF
check "every include directory the fragment names exists" "$missing"

# The two platform backends this port owns, and nothing standing in for them.
case "$srcs" in
*/freertos-nrf52833/uwb/dw3000_spi_freertos.c*) r=0 ;;
*) r=1 ;;
esac
check "the port's own SPI backend is in the source set" "$r"

case "$srcs" in
*/freertos-nrf52833/uwb/dw3000_hw_freertos.c*) r=0 ;;
*) r=1 ;;
esac
check "the port's own hardware backend is in the source set" "$r"

# The seam the Zephyr build gets from uwb_rxdiag.c, which is a Zephyr-module
# literal this port does not compile. Without it the callbacks reach the radio
# unmodified, the Pre-POLL is never decoded to warm the next block's STS, and
# the responder hears every Pre-POLL and answers none of them.
case "$srcs" in
*/freertos-nrf52833/uwb/ultrawidelock_seam_stubs.c*) r=0 ;;
*) r=1 ;;
esac
check "the port supplies its half of the STS seam" "$r"

# The bring-up. Without it nothing in the image calls the layer at all, so
# --gc-sections drops the whole archive and the port ships a UWB stack that has
# never executed a single instruction -- which is the state this file was
# written during, and the one worth not returning to by accident.
case "$srcs" in
*/freertos-nrf52833/uwb/ultrawidelock_freertos_uwb.c*) r=0 ;;
*) r=1 ;;
esac
check "the port's bring-up is in the source set" "$r"

# And not another port's, which implement the same symbols and would either
# clash at link time or, worse, win.
case "$srcs" in
*ports/zephyr/*|*ports/esp32/*) r=1 ;;
*) r=0 ;;
esac
check "no other port's backend is dragged in beside them" "$r"

case "$defs" in
*CONFIG_ULTRAWIDELOCK_CRYPTO_*) r=1 ;;
*) r=0 ;;
esac
check "UWB does not select a platform crypto backend" "$r"

case "$srcs" in
*modules/ultrawidelock_uwb/src/ccc/ccc_crypto_prim.c*) r=0 ;;
*) r=1 ;;
esac
check "the primitive crypto adapter is in the source set" "$r"

case "$libs" in
*ultrawidelock_prim*) r=0 ;;
*) r=1 ;;
esac
check "the engine links the primitive provider its STS seam reaches through" "$r"

case "$defs" in
*CONFIG_ULTRAWIDELOCK_UWB_FINAL_SNAPSHOT=1*) r=0 ;;
*) r=1 ;;
esac
check "the Final-capture snapshot is on, as this single-core part requires" "$r"

case "$defs" in
*CONFIG_ULTRAWIDELOCK_UWB_RESPONDER=1*) r=0 ;;
*) r=1 ;;
esac
check "the engine is built as a responder" "$r"

case "$defs" in
*CONFIG_DW3000_CHIP_DW3000=1*) r=0 ;;
*) r=1 ;;
esac
check "the DW3000 device layer is selected, not the DW3720 twin" "$r"

# The snapshot is compiled in, so the code it guards has to still be there.
if grep -q 'CONFIG_ULTRAWIDELOCK_UWB_FINAL_SNAPSHOT' "$ROOT/modules/ultrawidelock_uwb/src/ccc/ccc_shim_rx.c"; then
	r=0
else
	r=1
fi
check "the shim still gates its Final-capture path on that symbol" "$r"

printf 'uwb-sources: %s (%d checks)\n' \
	"$([ "$failures" -eq 0 ] && echo PASS || echo FAIL)" "$checks"
[ "$failures" -eq 0 ]
