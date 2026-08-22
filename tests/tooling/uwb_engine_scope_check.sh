#!/usr/bin/env bash
#
# uwb_engine_scope_check.sh — keep the DW3000 engine's file set closed.
#
# WHAT IS BEING PREVENTED. The public UWB contract is the six functions in
# modules/ultrawidelock_uwb/include/ultrawidelock/uwb.h; everything that names the Qorvo
# radio API (dwt_* calls and types, deca_*.h headers) is the DW3000 engine
# backend behind that contract, and it is a closed set of files. A dwt_ call or
# deca include added anywhere else quietly re-couples the chip-agnostic zone —
# the FiRa session logic, CCC key schedule, credential adapter, apps — to one
# vendor's silicon, and the next chipset port inherits it as a rewrite. This
# gate keeps the boundary mechanical: a new chipset implements the contract,
# it never edits the zone.
#
#   tests/tooling/uwb_engine_scope_check.sh              # scan the tracked sources
#   tests/tooling/uwb_engine_scope_check.sh --self-test  # prove the gate can fail
#   make check / make scope                              # runs it as `uwb engine scope`
#
# Exit 0 clean, 1 on a finding, 2 if the gate could not do its job.
#
# WHAT IS EXEMPT, and why each one is not a hole:
#
#   modules/ultrawidelock_dw3000/**            the vendored driver and its integration
#   uwb_seam.h                       the engine's own call discipline (dwt-typed
#                                    by design; see uwb_seam_check.sh)
#   modules/ultrawidelock_uwb/src/driver/**    radio init, ISR, RX/CIR diagnostics
#   ccc_shim_rx.c ccc_shim_wrap.c    the CCC STS shims that program the radio
#   ccc_sts.c                        the register-level key/IV packer
#   facade/flight_recorder.c         reads radio timestamps into the flight log
#   ports/zephyr/dw3000/**           the wiring seams' Zephyr backend
#   ports/esp32/components/ultrawidelock_uwb/port/**  the same two seams plus stubs, ESP32
#   ports/freertos-nrf52833/uwb/**   the same again, standalone FreeRTOS: the
#                                    DW3110 SPI and reset/IRQ/wake backends,
#                                    which call dwt_isr and dwt_checkidlerc as
#                                    the ESP-IDF ones do, and the seam stubs
#   examples/zephyr/anchor/**        bench tools that drive the radio directly
#   examples/zephyr/nrf5340dk-initiator/**  on purpose; they are DW3000 rigs
#   tests/**                         host doubles and fixtures
#
# Adding a file here is a decision that it belongs to the DW3000 engine.
# If the code is session, key, message, or app logic: keep it chip-agnostic
# and call the contract instead.
#
# Trailing comments are stripped before matching, so a struct field documented
# as "/**< dwt_rxdiag_t::ipatovF1 */" is prose, not coupling. Uppercase DWT_
# macros are deliberately NOT matched: ARM CoreSight (DWT->CTRL, DWT_CTRL_*)
# owns that prefix on Cortex-M, and Qorvo's DWT_ macros cannot be used without
# a deca include, which is matched.

set -euo pipefail

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
	R=$'\033[31m' G=$'\033[32m' Z=$'\033[0m'
else
	R='' G='' Z=''
fi

cd "$(dirname "$0")/../.."

# One definition, used by the scan AND by the self-test.
TOKEN_RE='(^|[^_[:alnum:]])(dwt_[a-z0-9_]+|deca_[a-z0-9_]*\.h)'

# Lines that open inside a comment body ("<n>: * prose" after grep -n).
COMMENT_LINE_RE='^[0-9]+:[[:space:]]*(\*|//|/\*)'

# Drop everything from the first comment marker to end of line, so a trailing
# doc comment cannot fire while code before it still can. Code resuming after
# an inline close ("/*x*/ dwt_foo()") is invisible to this; nothing in the
# tree writes that, and the self-test pins the shapes that matter.
strip_trailing_comments() { sed -E 's|//.*$||; s|/\*.*$||'; }

# The DW3000 engine file set. See the header for why each entry is not a hole.
ENGINE_RE='^(modules/ultrawidelock_dw3000/
|modules/ultrawidelock_uwb/include/uwb_seam\.h
|modules/ultrawidelock_uwb/src/driver/
|modules/ultrawidelock_uwb/src/ccc/ccc_shim_rx\.c
|modules/ultrawidelock_uwb/src/ccc/ccc_shim_wrap\.c
|modules/ultrawidelock_uwb/src/ccc/ccc_sts\.c
|modules/ultrawidelock_uwb/src/facade/flight_recorder\.c
|ports/zephyr/dw3000/
|ports/esp32/components/ultrawidelock_uwb/port/
|ports/freertos-nrf52833/uwb/
|examples/zephyr/anchor/
|examples/zephyr/nrf5340dk-initiator/
|tests/)'
ENGINE_RE=${ENGINE_RE//$'\n'/}

scan_paths() {
	git ls-files 'modules/*.c' 'modules/*.h' 'modules/*.cpp' \
		'ports/*.c' 'ports/*.h' 'ports/*.cpp' 'apps/*.c' 'apps/*.h' 'apps/*.cpp' \
		'examples/*.c' 'examples/*.h' 'examples/*.cpp'
}

# ---- scan -------------------------------------------------------------------

scan() {
	local findings=0 f hits
	while IFS= read -r f; do
		[[ $f =~ $ENGINE_RE ]] && continue
		hits=$(grep -nE "$TOKEN_RE" "$f" | grep -vE "$COMMENT_LINE_RE" |
			strip_trailing_comments | grep -E "$TOKEN_RE" || true)
		[ -n "$hits" ] || continue
		while IFS= read -r hit; do
			printf '%s  %s:%s%s\n' "$R" "$f" "$hit" "$Z"
			findings=$((findings + 1))
		done <<<"$hits"
	done < <(scan_paths)

	if [ "$findings" -gt 0 ]; then
		printf '%scheck-uwb-engine-scope: %d reference(s) to the Qorvo radio API outside the DW3000 engine%s\n' \
			"$R" "$findings" "$Z" >&2
		printf '  Chip-agnostic code calls the contract in <ultrawidelock/uwb.h> instead.\n' >&2
		printf '  If the file genuinely IS engine code, add it to ENGINE_RE with a reason in the header.\n' >&2
		return 1
	fi
	printf '%s  ok   no vendor radio reference outside the DW3000 engine%s\n' "$G" "$Z"
	return 0
}

# The gate only means something while the engine it names exists where it says.
# A moved or renamed engine would otherwise leave the gate scanning nothing.
check_zone() {
	local missing=0 f
	for f in modules/ultrawidelock_uwb/src/driver/uwb_min.c \
		modules/ultrawidelock_uwb/src/ccc/ccc_shim_rx.c \
		modules/ultrawidelock_uwb/include/ultrawidelock/uwb.h; do
		if [ ! -f "$f" ]; then
			printf '%s  engine landmark missing: %s (update ENGINE_RE and this list)%s\n' \
				"$R" "$f" "$Z" >&2
			missing=$((missing + 1))
		fi
	done
	[ "$missing" -eq 0 ] || return 2
	printf '%s  ok   the engine file set is where the gate says it is%s\n' "$G" "$Z"
}

# ---- self-test --------------------------------------------------------------

self_test() {
	local fails=0

	local should_fire=(
		'	dwt_configure(&cfg);'
		'	dwt_sts_cp_iv_t iv;'
		'#include <deca_device_api.h>'
		'#include "deca_interface.h"'
		'	ts = dwt_readsystimestamphi32();'
	)
	local should_not=(
		'	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;'
		'	my_dwt_helper(mode);'
		'	ultrawidelock_uwb_configure_phy(&cfg);'
		'	uint32_t f1;'
	)

	local line n=0
	for line in "${should_fire[@]}"; do
		if printf '%s\n' "$line" | grep -qE "$TOKEN_RE"; then
			n=$((n + 1))
		else
			printf '%s  self-test FAILED: missed a coupling: %s%s\n' "$R" "$line" "$Z" >&2
			fails=$((fails + 1))
		fi
	done
	[ "$fails" -ne 0 ] || printf '%s  self-test: detector fires on all %d coupling shapes%s\n' \
		"$G" "$n" "$Z"

	local quiet=0
	for line in "${should_not[@]}"; do
		if printf '%s\n' "$line" | grep -qE "$TOKEN_RE"; then
			printf '%s  self-test FAILED: fired on a legitimate line: %s%s\n' \
				"$R" "$line" "$Z" >&2
			fails=$((fails + 1))
		else
			quiet=$((quiet + 1))
		fi
	done
	[ "$fails" -ne 0 ] || printf '%s  self-test: quiet on all %d legitimate shapes%s\n' \
		"$G" "$quiet" "$Z"

	# The trailing-comment strip must silence a doc-comment mention and keep a
	# real call that happens to carry a trailing comment.
	local drop='7:	uint32_t f1; /**< dwt_rxdiag_t::ipatovF1 */'
	local keep='9:	dwt_configure(&cfg); /* traced */'
	if printf '%s\n' "$drop" | strip_trailing_comments | grep -qE "$TOKEN_RE"; then
		printf '%s  self-test FAILED: comment strip kept a doc mention%s\n' "$R" "$Z" >&2
		fails=$((fails + 1))
	fi
	if ! printf '%s\n' "$keep" | strip_trailing_comments | grep -qE "$TOKEN_RE"; then
		printf '%s  self-test FAILED: comment strip dropped a real call%s\n' "$R" "$Z" >&2
		fails=$((fails + 1))
	fi
	[ "$fails" -ne 0 ] || printf '%s  self-test: comment strip drops prose, keeps code%s\n' \
		"$G" "$Z"

	# The engine set must not quietly grow to cover the chip-agnostic zone.
	local f
	for f in modules/ultrawidelock_uwb/src/fira/fira_session.c \
		modules/ultrawidelock_uwb/src/facade/ultrawidelock_uwb_facade.c \
		modules/ultrawidelock_uwb/src/ccc/ccc_shim.c \
		modules/ultrawidelock_uwb/src/ccc/ccc_kdf.c \
		scripts/nrf5340dk-build.sh; do
		if [[ $f =~ $ENGINE_RE ]]; then
			printf '%s  self-test FAILED: %s is exempt, but it is chip-agnostic%s\n' \
				"$R" "$f" "$Z" >&2
			fails=$((fails + 1))
		fi
	done
	[ "$fails" -ne 0 ] || printf '%s  self-test: the engine set still excludes the chip-agnostic zone%s\n' \
		"$G" "$Z"

	if [ "$fails" -ne 0 ]; then
		printf '%scheck-uwb-engine-scope: the gate itself is broken%s\n' "$R" "$Z" >&2
		return 2
	fi
	return 0
}

case "${1-}" in
--self-test)
	self_test
	;;
"")
	check_zone || exit $?
	scan
	;;
*)
	printf 'usage: %s [--self-test]\n' "$0" >&2
	exit 2
	;;
esac
