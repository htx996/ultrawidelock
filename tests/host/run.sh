#!/usr/bin/env bash
#
# Build + run the full host test suite (correctness gate). Plain C, no NCS
# toolchain or hardware. Compiles our logic modules against tests/host/shim and
# runs every module suite. `make coverage` builds the same sources instrumented.
#
# Nine binaries get built here before the last one runs, and the compiles are
# the slow half, so each is a step of scripts/lib/ui.sh's progress display: on a
# terminal that is a bar and a percentage, everywhere else one line per step.
# The stage_* functions below are only that division -- every compile and every
# binary is the one that was here before, in the same order.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tests/host/sources.sh"
. "$ROOT/scripts/lib/ui.sh"

# One build root for the whole repo; the host suites own build/host.
OUT="${ULTRAWIDELOCK_BUILD_ROOT:-$ROOT/build}/host"
mkdir -p "$OUT"
# SAN=1: same suite rebuilt under ASan + UBSan (`make test-san`).
san_flags=
if [ -n "${SAN:-}" ]; then
  san_flags='-g -fsanitize=address,undefined -fno-sanitize-recover=all'
fi

# Shapes clang merely warns about and GCC rejects outright, promoted so a local
# run fails where CI would. `static ULTRAWIDELOCK_THREAD_STACK_DEFINE(...)` on a
# macro that already says static is the one that got through: every macOS run
# green, the Linux gate unable to compile the suite at all. Probed rather than
# assumed, because GCC errors on an unknown -Werror= name and does not need
# these anyway -- it already treats them as errors.
# A string, not an array, for the same reason san_flags is one: bash 3.2 is still
# what /usr/bin/env bash finds on a stock mac, and it treats an empty array under
# `set -u` as an unbound variable.
gcc_parity=
for f in -Werror=duplicate-decl-specifier; do
  if "${CC:-cc}" "$f" -x c -fsyntax-only /dev/null 2>/dev/null; then
    gcc_parity="$gcc_parity $f"
  fi
done

# --- target-only sources, separate small binaries --------------------------
# These compile production sources whose exported symbols the main binary
# already fakes (dw_rx_stub.c) or that need incompatible fakes, so they cannot
# join host_test. All of them run against recording doubles: branch logic and
# argument plumbing only, no hardware or crypto truth.
SRC="$ROOT/modules/ultrawidelock_uwb/src"
HOSTD="$ROOT/tests/host"

# -w: the shim intentionally leaves some args unused, and the in-tree modules are
# lint-gated by the real Zephyr build, not here. Errors still fail the build.
# -lm: test_ultrawidelock_door.c derives its expected chord lengths with cos/sqrt rather
# than restating the module's arithmetic. glibc keeps those in libm, so the link
# fails on Linux without this; on macOS libSystem already has them and the flag
# is a no-op, which is why this only ever breaks in CI.
stage_core_build() {
	# shellcheck disable=SC2086  # san_flags is a deliberate word-split flag list
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags "${DEFS[@]}" "${INCS[@]}" \
	   "${TEST_SRCS[@]}" "${SHIM_SRCS[@]}" "${UNIT_SRCS[@]}" \
	   -lm -o "$OUT/host_test"
}

stage_core_run() {
	# Quiet: suites assert, they don't need the UWB diag firehose on stdout (run
	# the binary directly, without ULTRAWIDELOCK_TEST_QUIET, to get it back).
	ULTRAWIDELOCK_TEST_QUIET=1 "$OUT/host_test"
}

# 1) uwb driver + shell (uwb_min/isr/rxdiag/cirdiag/selftest + ultrawidelock_shell) on
#    the drvfake radio + logfake zephyr surface.
stage_uwb_driver() {
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags \
		-DULTRAWIDELOCK_PORT_HOST -D_DEFAULT_SOURCE -DCONFIG_ULTRAWIDELOCK_CRED=1 -DCONFIG_ULTRAWIDELOCK_UWB_CIRDIAG=1 \
		-DCONFIG_ULTRAWIDELOCK_UWB_SELFTEST_DELAY_MS=250 \
		-I"$HOSTD/shim" -I"$HOSTD" -I"$HOSTD/logfake" \
		-I"$ROOT/modules/ultrawidelock_uwb/include" \
		-I"$SRC/driver" -I"$SRC/ccc" -I"$SRC/fira" -I"$SRC/facade" -I"$ROOT/ports/zephyr/shell" \
		-I"$ROOT/modules/ultrawidelock_port/include" -I"$ROOT/modules/ultrawidelock_dw3000/include" \
		"$HOSTD/test.c" "$HOSTD/drv_main.c" \
		"$HOSTD/test_uwb_min.c" "$HOSTD/test_uwb_isr.c" "$HOSTD/test_uwb_rxdiag.c" \
		"$HOSTD/test_uwb_cirdiag.c" \
		"$HOSTD/test_uwb_selftest.c" "$HOSTD/test_ultrawidelock_shell.c" \
		"$HOSTD/shim/drvfake.c" \
		"$ROOT/tests/host/port/osal_host.c" \
		"$SRC/driver/uwb_min.c" "$SRC/driver/uwb_isr.c" "$SRC/driver/uwb_rxdiag.c" \
		"$SRC/driver/uwb_cirdiag.c" \
		"$SRC/driver/uwb_selftest.c" "$ROOT/ports/zephyr/shell/ultrawidelock_shell.c" \
		-o "$OUT/host_test_drv"
	ULTRAWIDELOCK_TEST_QUIET=1 "$OUT/host_test_drv"
}

# 2) PSA/mbedTLS crypto seams over recording fakes (psafake/). The two backend
#    files define the same crypto_aes_ecb_encrypt symbol as aes_ref.c, so each
#    is compiled alone with a -D rename (a compile flag, not a source edit).
stage_crypto_backends() {
	psa_flags=(-std=c11 -O1 -w -I"$HOSTD/psafake" -I"$ROOT/modules/ultrawidelock_uwb/include"
		-I"$SRC/ccc")
	# shellcheck disable=SC2086
	"${CC:-cc}" "${psa_flags[@]}" $san_flags -c \
		-Dcrypto_aes_ecb_encrypt=ultrawidelock_test_psa_ecb \
		"$SRC/ccc/ccc_crypto_psa.c" -o "$OUT/ccc_crypto_psa_host.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" "${psa_flags[@]}" $san_flags -c \
		-Dcrypto_aes_ecb_encrypt=ultrawidelock_test_mbedtls_ecb \
		"$SRC/ccc/ccc_crypto_mbedtls.c" -o "$OUT/ccc_crypto_mbedtls_host.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" "${psa_flags[@]}" $san_flags \
		-I"$HOSTD" -I"$ROOT/modules/ultrawidelock_cred/include" \
		"$HOSTD/test.c" "$HOSTD/test_psa_backends.c" "$HOSTD/psafake/psafake.c" \
		"$ROOT/modules/ultrawidelock_cred/src/ultrawidelock_prim_psa.c" \
		"$OUT/ccc_crypto_psa_host.o" "$OUT/ccc_crypto_mbedtls_host.o" \
		-o "$OUT/host_test_psa"
	"$OUT/host_test_psa"
}

# 3) NFC ECP emitter (C++) over fake RFAL/reader-storage headers (ecpfake/).
stage_nfc_ecp() {
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -c "$HOSTD/test.c" -o "$OUT/test_harness_c.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags \
		-DCONFIG_DOOR_LOCK_RFAL_LOG_LEVEL=3 \
		-I"$HOSTD" -I"$HOSTD/ecpfake" \
		"$HOSTD/test_nfc_ecp.cpp" "$ROOT/modules/ultrawidelock_nfc/src/nfc_prop_ecp.cpp" \
		"$OUT/test_harness_c.o" \
		-o "$OUT/host_test_ecp"
	"$OUT/host_test_ecp"
}

# 4) DWM3001CDK port glue over a fake settings backend (settingsfake/).
#    Its own binary because the fake <zephyr/settings/settings.h> would collide
#    with the other suites' Zephyr surface, the same reason (1) and (3) are
#    split out. Compiled with -Wall -Wextra rather than the -w the suites above
#    use: this port has never been warning-checked by anything, since the
#    clang-format and clang-tidy gates both cover modules/ only.
#    Real port source, not a host copy -- the point is to test what ships.
stage_cdk_port() {
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -Wall -Wextra $gcc_parity $san_flags \
		-DCONFIG_LOG_DEFAULT_LEVEL=3 -DCONFIG_ULTRAWIDELOCK_PROV_CLEAR_ON_BOOT=0 \
		-I"$HOSTD" -I"$HOSTD/settingsfake" -I"$HOSTD/logfake" \
		-I"$ROOT/modules/ultrawidelock_matter/include" \
		-I"$ROOT/modules/ultrawidelock_cred/include" -I"$ROOT/ports/zephyr/store" \
		"$HOSTD/test.c" "$HOSTD/test_matter_fab_settings.c" \
		"$HOSTD/settingsfake/settingsfake.c" \
		"$ROOT/ports/zephyr/store/matter_fab_settings.c" \
		"$ROOT/ports/zephyr/store/ultrawidelock_prov_settings.c" \
		"$ROOT/modules/ultrawidelock_cred/src/ultrawidelock_prov.c" \
		-o "$ROOT/build/host_test_cdk"
	"$ROOT/build/host_test_cdk"
	"$ROOT/tests/ports/zephyr/matter_srp_lifecycle_check.sh"
	"$ROOT/tests/ports/zephyr/ble_link_liveness_check.sh"
}

# 5) Delta update, both halves, over the ultrawidelock_flash host backend (RAM
#    partitions that enforce the nRF driver's word and page alignment rules),
#    the host OSAL's virtual clock, the recording PSA (psafake/) and a scripted
#    detools (dfufake/). Its own binary because the SMP half needs the
#    zcbor/mcumgr doubles in smpfake/. CONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR is
#    on here so the explicit "rc" key a legacy client expects is compiled and
#    checked.
#    _DEFAULT_SOURCE because dfu_receiver.c includes ultrawidelock_port.h, whose
#    ultrawidelock_uptime_us() names CLOCK_MONOTONIC. Under -std=c11 glibc hides
#    that behind a feature-test macro, so the stage does not compile on Linux at
#    all -- while Darwin exposes it unconditionally and every macOS run stays
#    green. A feature macro cannot be set from the header, because it has to
#    precede the first libc include; it belongs on the compile line, which is
#    what stage_uwb_driver above already does for the same reason.
stage_delta_update() {
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags \
		-DULTRAWIDELOCK_PORT_HOST -D_DEFAULT_SOURCE -DCONFIG_ULTRAWIDELOCK_DFU_SMP_IMG=1 -DCONFIG_ULTRAWIDELOCK_DFU_APPLIER_CHUNK=256 \
		-DCONFIG_MCUMGR_GRP_OS_RESET_HOOK=1 -DCONFIG_MCUMGR_GRP_ENUM_DETAILS_NAME=1 \
		-DCONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR=1 \
		-I"$HOSTD" -I"$HOSTD/dfufake" -I"$HOSTD/smpfake" -I"$HOSTD/logfake" \
		-I"$HOSTD/psafake" -I"$ROOT/modules/ultrawidelock_port/include" \
		-I"$ROOT/modules/ultrawidelock_dfu/include" -I"$ROOT/modules/ultrawidelock_dfu/src" \
		"$HOSTD/test.c" "$HOSTD/test_dfu.c" "$HOSTD/test_dfu_smp.c" \
		"$HOSTD/dfufake/dfufake.c" "$HOSTD/smpfake/smpfake.c" "$HOSTD/psafake/psafake.c" \
		"$ROOT/tests/host/port/osal_host.c" "$ROOT/tests/host/port/flash_host.c" \
		"$ROOT/modules/ultrawidelock_dfu/src/dfu_crc.c" \
		"$ROOT/modules/ultrawidelock_dfu/src/dfu_receiver.c" \
		"$ROOT/modules/ultrawidelock_dfu/src/dfu_applier.c" \
		"$ROOT/ports/zephyr/dfu/dfu_smp_img.c" \
		-o "$OUT/host_test_dfu"
	"$OUT/host_test_dfu"
}

# 6) The ultrawidelock_nfc transport seam (C++), over fake Zephyr SPI/GPIO/kernel and a
#    recording credential stack (nfcfake/). pn532.c and pn532_apdu.c link in for
#    real, so the frames really are encoded and parsed by the shipping codec.
#    transport_none.cpp defines the same five symbols as transport_pn532.cpp,
#    so it is renamed on its own compile step with -DUltraWideLockNfc=UltraWideLockNfcNone -- a
#    compile flag, not a source edit, exactly as (2) renames the two crypto
#    backends.
stage_nfc_transport() {
	NFC_DEF=(-DCONFIG_ULTRAWIDELOCK_NFC_LOG_LEVEL=3 -DCONFIG_ULTRAWIDELOCK_NFC_PN532_THREAD_STACK_SIZE=2048
		-DCONFIG_ULTRAWIDELOCK_NFC_PN532_POLL_PERIOD_MS=200
		-DCONFIG_ULTRAWIDELOCK_NFC_PN532_EXCHANGE_TIMEOUT_MS=1000
		-DULTRAWIDELOCK_PORT_HOST) # transport_none logs through ultrawidelock_log.h
	NFC_INC=(-I"$HOSTD" -I"$HOSTD/nfcfake" -I"$ROOT/modules/ultrawidelock_nfc/include"
		-I"$ROOT/modules/ultrawidelock_nfc/src" -I"$ROOT/modules/ultrawidelock_port/include")
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -c "$HOSTD/test.c" -o "$OUT/test_harness_nfc.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -I"$ROOT/modules/ultrawidelock_nfc/include" \
		-I"$ROOT/modules/ultrawidelock_nfc/src" \
		-c "$ROOT/modules/ultrawidelock_nfc/src/pn532.c" -o "$OUT/pn532_nfc.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -I"$ROOT/modules/ultrawidelock_nfc/include" \
		-I"$ROOT/modules/ultrawidelock_nfc/src" \
		-c "$ROOT/modules/ultrawidelock_nfc/src/pn532_apdu.c" -o "$OUT/pn532_apdu_nfc.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags "${NFC_DEF[@]}" "${NFC_INC[@]}" \
		-c "$ROOT/ports/zephyr/nfc/pn532_bus_spi.c" -o "$OUT/pn532_bus_spi.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags "${NFC_DEF[@]}" "${NFC_INC[@]}" \
		-c "$ROOT/modules/ultrawidelock_nfc/src/transport_pn532.cpp" -o "$OUT/transport_pn532.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags -DUltraWideLockNfc=UltraWideLockNfcNone \
		"${NFC_DEF[@]}" "${NFC_INC[@]}" \
		-c "$ROOT/modules/ultrawidelock_nfc/src/transport_none.cpp" -o "$OUT/transport_none.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags "${NFC_INC[@]}" \
		-c "$HOSTD/nfcfake/nfcfake.cpp" -o "$OUT/nfcfake.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags "${NFC_DEF[@]}" "${NFC_INC[@]}" \
		-c "$HOSTD/test_nfc_transport.cpp" -o "$OUT/test_nfc_transport.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags \
		"$OUT/test_nfc_transport.o" "$OUT/nfcfake.o" "$OUT/test_harness_nfc.o" \
		"$OUT/transport_none.o" "$OUT/transport_pn532.o" "$OUT/pn532_bus_spi.o" \
		"$OUT/pn532_nfc.o" "$OUT/pn532_apdu_nfc.o" \
		-o "$OUT/host_test_nfc"
	"$OUT/host_test_nfc"
}

# 7) uwb_seam.h's engine-less tier. Compiled WITHOUT CONFIG_ULTRAWIDELOCK_CRED, alone:
#    that half of the header is inline bodies, and a header compiled two ways
#    inside one binary maps the same lines twice. See the file for the rest.
stage_uwb_seam() {
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags \
		-I"$HOSTD" -I"$HOSTD/shim" -I"$HOSTD/logfake" \
		-I"$ROOT/modules/ultrawidelock_uwb/include" -I"$SRC/driver" \
		-I"$ROOT/modules/ultrawidelock_dw3000/include" \
		"$HOSTD/test.c" "$HOSTD/test_uwb_seam.c" \
		-o "$OUT/host_test_seam"
	"$OUT/host_test_seam"
}

# 8) The credential source stack (C++): ultrawidelock_stack.cpp and session.cpp over the
#    Nordic Interface API as recording doubles (stackfake/). The protocol
#    codecs beside them are the shipping sources, linked in whole, so every
#    APDU and BLE frame here is built and parsed for real -- only the crypto
#    and the application callbacks are stand-ins. Its own binary because
#    stackfake's <ultrawidelock/*.h> are a different credential surface from the one
#    ecpfake and nfcfake carry, and all three would collide.
stage_cred_stack() {
	STK="$ROOT/modules/ultrawidelock_cred_stack/src"
	STK_DEF=(-DCONFIG_NCS_ALIRO_LOG_LEVEL_VALUE=3 -DCONFIG_NCS_ALIRO_BLE_UWB=1
		-DCONFIG_DOOR_LOCK_EXPEDITED_FAST_PHASE=1 -DCONFIG_DOOR_LOCK_STEP_UP_PHASE=1
		-DCONFIG_DOOR_LOCK_BLE_UWB_MAX_SESSIONS=2 -DCONFIG_ULTRAWIDELOCK_CRED_APDU_BUFFER_SIZE=1024
		-DCONFIG_MAX_NUMBER_OF_KPERSISTENT=4
		-DCONFIG_DOOR_LOCK_STORAGE_MAX_STORED_ACCESS_DOCUMENTS=2)
	STK_INC=(-I"$HOSTD" -I"$HOSTD/stackfake" -I"$STK" -I"$STK/protocol"
		-I"$ROOT/modules/ultrawidelock_cred/include" -I"$ROOT/modules/ultrawidelock_cred/src")
	STK_OBJS=()
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -c "$HOSTD/test.c" -o "$OUT/test_harness_stack.o"
	for stk_src in advertising_core protocol/ble_message protocol/ble_timeout \
		protocol/nfc_select protocol/nfc_auth; do
		stk_obj="$OUT/stk_$(basename "$stk_src").o"
		# shellcheck disable=SC2086
		"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags "${STK_DEF[@]}" -I"$STK" -I"$STK/protocol" \
			-I"$ROOT/modules/ultrawidelock_cred/include" -c "$STK/$stk_src.c" -o "$stk_obj"
		STK_OBJS+=("$stk_obj")
	done
	# The step-up wire codecs + DeviceResponse parser are the shared ultrawidelock_cred
	# sources (one source of protocol truth; session.cpp calls them directly).
	for ultrawidelock_src in ultrawidelock_tlv ultrawidelock_stepup_wire ultrawidelock_stepup_parse; do
		stk_obj="$OUT/stk_$ultrawidelock_src.o"
		# shellcheck disable=SC2086
		"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -I"$ROOT/modules/ultrawidelock_cred/include" \
			-I"$ROOT/modules/ultrawidelock_cred/src" \
			-c "$ROOT/modules/ultrawidelock_cred/src/$ultrawidelock_src.c" -o "$stk_obj"
		STK_OBJS+=("$stk_obj")
	done
	# The symmetric crypto is REAL: ultrawidelock_hash.c (SHA-256/HMAC/HKDF, pinned by
	# test_ultrawidelock_hash.c) and the reference AES-GCM in ultrawidelock_prim_host.c (pinned by
	# test_ultrawidelock_crypto.c). Only P-256 stays a stand-in -- this repo has none on host.
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -I"$ROOT/modules/ultrawidelock_cred/include" \
		-I"$ROOT/modules/ultrawidelock_cred/src" -c "$ROOT/modules/ultrawidelock_cred/src/ultrawidelock_hash.c" \
		-o "$OUT/stk_ultrawidelock_hash.o"
	# shellcheck disable=SC2086
	"${CC:-cc}" -std=c11 -O1 -w $gcc_parity $san_flags -I"$ROOT/modules/ultrawidelock_cred/include" \
		-I"$ROOT/modules/ultrawidelock_cred/src" -c "$ROOT/tests/shared/ultrawidelock_prim_host.c" \
		-o "$OUT/stk_ultrawidelock_prim_host.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags "${STK_DEF[@]}" "${STK_INC[@]}" \
		-c "$STK/cred_stack.cpp" -o "$OUT/stk_cred_stack.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags "${STK_DEF[@]}" "${STK_INC[@]}" \
		-c "$STK/session.cpp" -o "$OUT/stk_session.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags "${STK_DEF[@]}" "${STK_INC[@]}" \
		-c "$HOSTD/stackfake/stackfake.cpp" -o "$OUT/stackfake.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags "${STK_DEF[@]}" "${STK_INC[@]}" \
		-c "$HOSTD/test_ultrawidelock_stack.cpp" -o "$OUT/test_ultrawidelock_stack.o"
	# shellcheck disable=SC2086
	"${CXX:-c++}" -std=c++17 -O1 -w $san_flags \
		"$OUT/test_ultrawidelock_stack.o" "$OUT/stackfake.o" "$OUT/test_harness_stack.o" \
		"$OUT/stk_cred_stack.o" "$OUT/stk_session.o" "${STK_OBJS[@]}" \
		"$OUT/stk_ultrawidelock_hash.o" "$OUT/stk_ultrawidelock_prim_host.o" \
		-o "$OUT/host_test_stack"
	"$OUT/host_test_stack"
}

# The ":seconds" are first-run hints only, measured on the machine this was
# written on. From the second run the real durations come out of the cache under
# build/_ui, so the percentage is this checkout's own timings, not these. The
# sanitized build is several times slower and gets its own cache.
ULTRAWIDELOCK_UI_KEY="host-run${SAN:+-san}"
ui_begin "host test suite${SAN:+ (ASan + UBSan)}" \
	"core suite (build):5" "core suite (run):1" "uwb driver + shell:1" \
	"crypto backends:1" "nfc ecp emitter:1" "cdk port glue:1" \
	"delta update + smp:1" "nfc transport:1" "uwb seam tier:1" \
	"credential stack:2"

ui_run "core suite (build)" stage_core_build
ui_run "core suite (run)" stage_core_run
ui_run "uwb driver + shell" stage_uwb_driver
ui_run "crypto backends" stage_crypto_backends
ui_run "nfc ecp emitter" stage_nfc_ecp
ui_run "cdk port glue" stage_cdk_port
ui_run "delta update + smp" stage_delta_update
ui_run "nfc transport" stage_nfc_transport
ui_run "uwb seam tier" stage_uwb_seam
ui_run "credential stack" stage_cred_stack
ui_end
