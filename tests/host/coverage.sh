#!/usr/bin/env bash
#
# Line coverage for our own (asxeem) host-testable code, via clang source-based
# coverage. Instruments every host suite in the repo — the ultrawidelock_uwb KAT suite
# (same sources as run.sh, see sources.sh) and the shared-core suites mirrored
# from tests/shared/run.sh and merges their profiles into one report.
#
# Sources that never enter a host build are not hidden: this script discovers
# them (find over modules/ + ports/, no hand-list to go stale) and the report
# prints each as a 0% row — tagged "untested" when nothing platform-bound
# blocks a host build, "target-only" when it needs Zephyr/ESP-IDF/PSA/NimBLE
# or silicon. Headers with static-inline bodies join the accounting: llvm
# attributes their lines when an instrumented TU instantiates them, and the
# rest fall through to the 0% table. Non-C surfaces (python, web pages, the
# nRF add-on patches, shell tooling) are listed in a block of their own.
# Excluded entirely: the vendored trees (modules/ultrawidelock_dw3000 = the Qorvo
# decadriver, modules/ultrawidelock_dfu/src/detools) plus workspace/ and */test/
# harnesses. Coverage is a measure of OUR code, so third-party sources are not
# in the denominator.
#
# CI (ci.yml, via the coverage gate) enforces the line floor on summary.json, which spans the
# instrumented files only; the terminal table's closing "all our code" total
# additionally folds in the 0% rows.
#
# Artifacts under build/host/coverage/ (build/ is gitignored). The instrumented
# suites may report test failures; coverage is still generated (execution is
# what counts). A crash (signal) would abort, and should.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tests/host/sources.sh"
. "$ROOT/scripts/lib/ui.sh"

OUT="${ULTRAWIDELOCK_BUILD_ROOT:-$ROOT/build}/host/coverage"
BIN="$OUT/host_test_cov"
mkdir -p "$OUT"
rm -f "$OUT"/*.profraw # stale profiles from removed suites break the merge

# Apple toolchains front the LLVM tools with xcrun; Linux has them on PATH bare.
llvm_tool() { if command -v xcrun >/dev/null 2>&1; then xcrun "$@"; else "$@"; fi; }

# Every instrumented compile is a step of scripts/lib/ui.sh, which is why the
# compilers are wrapped rather than called directly. There are forty-odd of them
# and no plan is passed to ui_begin: the step names are the -o targets, and the
# previous run's cache supplies both the count and the weights. The first run on
# a fresh checkout counts steps without a percentage, and every run after that
# has one.
_cov_label() { # the -o target, as a name a reader recognises
	local prev= a out=
	for a in "$@"; do
		[ "$prev" = "-o" ] && out="$a"
		prev="$a"
	done
	[ -n "$out" ] || {
		printf 'compile'
		return 0
	}
	out="$(basename "$out")"
	out="${out%.o}"
	out="${out#cov_}"
	out="${out%_cov}"
	printf '%s' "$out"
}

# The instrumentation flags are clang-only: macOS cc is clang, Linux CI sets CC=clang.
# -w: coverage is not a lint gate (run.sh / the Zephyr build are). Errors still fail.
_cov_cc() {
	"${CC:-cc}" -std=c11 -O0 -g -w \
		-fprofile-instr-generate -fcoverage-mapping "$@"
}
cov_cc() { ui_run "$(_cov_label "$@")" _cov_cc "$@"; }

ui_begin "coverage"

# --- suite 1: the ultrawidelock_uwb host KAT suite (same sources as run.sh) -----------
# -lm for the same reason run.sh needs it: test_ultrawidelock_door.c calls cos/sqrt, which
# glibc keeps out of libc.
cov_cc "${DEFS[@]}" "${INCS[@]}" \
	"${TEST_SRCS[@]}" "${SHIM_SRCS[@]}" "${UNIT_SRCS[@]}" -lm -o "$BIN"
LLVM_PROFILE_FILE="$OUT/host.profraw" "$BIN" >"$OUT/run.log" 2>&1 || true

# --- suite 2..n: portable core host KATs (mirror of tests/shared/run.sh) ----
ET="$ROOT/tests/ports/esp32"
SHARED="$ROOT/tests/shared"
CRED="$ROOT/modules/ultrawidelock_cred"
LOCK_MAIN="$ROOT/apps/esp32-matter-lock/main"

# Units those suites exercise; joins UNIT_SRCS in the coverage denominator.
CORE_UNIT_SRCS=(
	"$CRED/src/ultrawidelock_hash.c"
	"$CRED/src/ultrawidelock_crypto.c"
	"$CRED/src/ultrawidelock_advtag.c"
	"$CRED/src/ultrawidelock_apdu.c"
	"$CRED/src/ultrawidelock_stepup.c"
	"$CRED/src/ultrawidelock_stepup_parse.c"
	"$CRED/src/ultrawidelock_prov.c"
	"$CRED/src/ultrawidelock_lat.c"
	"$CRED/src/ultrawidelock_reader.c"
	"$CRED/src/ultrawidelock_ranging.c"
	"$CRED/src/ultrawidelock_device.c"
	"$CRED/src/ultrawidelock_device_apdu.c"
	"$CRED/src/ultrawidelock_ble_central.c"
	"$CRED/src/ultrawidelock_assert_ec.c"
	"$LOCK_MAIN/lock_led.c"
)

OBJS=()
run_suite() { # <name> <bin>: run one instrumented suite into its own profile
	LLVM_PROFILE_FILE="$OUT/$1.profraw" "$2" >>"$OUT/run.log" 2>&1 || true
	OBJS+=(-object "$2")
}

cov_cc -I"$CRED/include" -I"$CRED/src" \
	"$SHARED/test_ultrawidelock_crypto.c" \
	"$CRED/src/ultrawidelock_hash.c" "$CRED/src/ultrawidelock_crypto.c" "$CRED/src/ultrawidelock_advtag.c" \
	"$SHARED/ultrawidelock_prim_host.c" -o "$OUT/cov_crypto"
run_suite crypto "$OUT/cov_crypto"

cov_cc -I"$CRED/include" -I"$CRED/src" \
	"$SHARED/test_ultrawidelock_apdu.c" "$CRED/src/ultrawidelock_apdu.c" -o "$OUT/cov_apdu"
run_suite apdu "$OUT/cov_apdu"

cov_cc -I"$SHARED" -I"$CRED/include" -I"$CRED/src" \
	"$SHARED/test_ultrawidelock_stepup.c" \
	"$CRED/src/ultrawidelock_stepup.c" "$CRED/src/ultrawidelock_stepup_wire.c" \
	"$CRED/src/ultrawidelock_stepup_parse.c" "$CRED/src/ultrawidelock_tlv.c" \
	"$CRED/src/ultrawidelock_hash.c" "$CRED/src/ultrawidelock_crypto.c" \
	"$SHARED/ultrawidelock_prim_host.c" -o "$OUT/cov_stepup"
run_suite stepup "$OUT/cov_stepup"

cov_cc -I"$CRED/include" -I"$CRED/src" \
	"$SHARED/test_ultrawidelock_prov.c" "$CRED/src/ultrawidelock_prov.c" -o "$OUT/cov_prov"
run_suite prov "$OUT/cov_prov"

# Only the trace-on lat build is instrumented: the gate-off variant maps the
# same lines differently and the two profiles would not merge cleanly.
cov_cc -D_POSIX_C_SOURCE=200809L -DULTRAWIDELOCK_PORT_HOST -DCONFIG_ULTRAWIDELOCK_LAT_TRACE=1 \
	-I"$CRED/include" -I"$ROOT/modules/ultrawidelock_port/include" \
	"$SHARED/test_ultrawidelock_lat.c" "$CRED/src/ultrawidelock_lat.c" -o "$OUT/cov_lat"
run_suite lat "$OUT/cov_lat"

cov_cc -I"$LOCK_MAIN" \
	"$ET/test_lock_led.c" "$LOCK_MAIN/lock_led.c" -o "$OUT/cov_led"
run_suite led "$OUT/cov_led"

cov_cc -D_POSIX_C_SOURCE=200809L -DULTRAWIDELOCK_PORT_HOST \
	-I"$CRED/include" -I"$CRED/src" -I"$ROOT/modules/ultrawidelock_port/include" \
	"$SHARED/test_ultrawidelock_reader.c" \
	"$CRED/src/ultrawidelock_reader.c" "$CRED/src/ultrawidelock_apdu.c" \
	"$CRED/src/ultrawidelock_crypto.c" "$CRED/src/ultrawidelock_hash.c" \
	"$CRED/src/ultrawidelock_prov.c" \
	"$SHARED/ultrawidelock_prim_host.c" -o "$OUT/cov_reader"
run_suite reader "$OUT/cov_reader"

cov_cc -D_POSIX_C_SOURCE=200809L -DULTRAWIDELOCK_PORT_HOST \
	-I"$CRED/include" -I"$CRED/src" -I"$ROOT/modules/ultrawidelock_port/include" \
	-I"$ROOT/modules/ultrawidelock_uwb/include" \
	"$SHARED/test_ultrawidelock_ranging.c" \
	"$CRED/src/ultrawidelock_ranging.c" "$CRED/src/ultrawidelock_crypto.c" \
	"$CRED/src/ultrawidelock_hash.c" \
	"$SHARED/ultrawidelock_prim_host.c" -o "$OUT/cov_ranging"
run_suite ranging "$OUT/cov_ranging"

# The initiator half of the credential stack: ultrawidelock_device.c and its APDU
# codec, over the reference host primitives. tests/shared/run.sh has proved these
# since they were written; this mirror was simply never added, so they were
# reported as having no host tests at all.
cov_cc -DULTRAWIDELOCK_DEVICE_HAVE_EC -I"$CRED/include" -I"$CRED/src" \
	"$SHARED/test_ultrawidelock_device.c" \
	"$CRED/src/ultrawidelock_device.c" "$CRED/src/ultrawidelock_device_apdu.c" \
	"$CRED/src/ultrawidelock_apdu.c" "$CRED/src/ultrawidelock_crypto.c" \
	"$CRED/src/ultrawidelock_hash.c" \
	"$SHARED/ultrawidelock_prim_host.c" -o "$OUT/cov_device"
run_suite device "$OUT/cov_device"

cov_cc -I"$CRED/include" \
	"$SHARED/test_ultrawidelock_ble_central.c" "$CRED/src/ultrawidelock_ble_central.c" \
	-o "$OUT/cov_ble_central"
run_suite ble_central "$OUT/cov_ble_central"

cov_cc -I"$CRED/include" -I"$CRED/src" \
	"$SHARED/test_ultrawidelock_assert_ec.c" \
	"$CRED/src/ultrawidelock_assert.c" "$CRED/src/ultrawidelock_assert_ec.c" \
	"$CRED/src/ultrawidelock_hash.c" \
	"$SHARED/ultrawidelock_prim_host.c" -o "$OUT/cov_assert_ec"
run_suite assert_ec "$OUT/cov_assert_ec"

# Header-inline logic (ultrawidelock_port.h et al.) is exercised by the port-headers
# unit test; instrumenting it attributes those lines to the headers below.
cov_cc -I"$ROOT/modules/ultrawidelock_port/include" -I"$ROOT/modules/ultrawidelock_uwb/include" \
	-I"$ROOT/modules/ultrawidelock_cred/include" -I"$ROOT/modules/ultrawidelock_dw3000/include" \
	"$SHARED/test_port_headers.c" -o "$OUT/cov_hdrs"
run_suite hdrs "$OUT/cov_hdrs"

# --- target-only sources on recording doubles (mirrors of the run.sh side
# binaries and the tests/ports/esp32 sdkfake stages). These measure branch
# logic against fakes — never hardware, radio, or crypto truth.
SRC="$ROOT/modules/ultrawidelock_uwb/src"
HOSTD="$ROOT/tests/host"
ECOMP="$ROOT/ports/esp32/components"
EREADER="$ROOT/examples/esp32/reader/main"
SDKFAKE="$ET/sdkfake"

SIDE_UNIT_SRCS=(
	"$SRC/driver/uwb_min.c"
	"$SRC/driver/uwb_isr.c"
	"$SRC/driver/uwb_rxdiag.c"
	"$SRC/driver/uwb_cirdiag.c"
	"$SRC/driver/uwb_selftest.c"
	"$ROOT/ports/zephyr/shell/ultrawidelock_shell.c"
	"$SRC/ccc/ccc_crypto_psa.c"
	"$SRC/ccc/ccc_crypto_mbedtls.c"
	"$CRED/src/ultrawidelock_prim_psa.c"
	"$ROOT/modules/ultrawidelock_nfc/src/nfc_prop_ecp.cpp"
	"$ECOMP/ultrawidelock_ble/ultrawidelock_ble_esp32.c"
	"$CRED/src/ultrawidelock_ble_nimble.c"
	"$ECOMP/ultrawidelock_reader/ultrawidelock_prov_nvs.c"
	"$ECOMP/ultrawidelock_reader/ultrawidelock_stepup_worker.c"
	"$ECOMP/ultrawidelock_reader/presence_link.c"
	"$ECOMP/piv_ccid/piv_ccid.c"
	"$ECOMP/piv_ccid/piv_apdu.c"
	"$ROOT/ports/zephyr/store/matter_fab_settings.c"
	"$ROOT/modules/ultrawidelock_dfu/src/dfu_crc.c"
	"$ECOMP/ultrawidelock_uwb/port/dw3000_hw.c"
	"$ECOMP/ultrawidelock_uwb/port/dw3000_spi.c"
	"$ECOMP/ultrawidelock_uwb/port/ultrawidelock_seam_stubs.c"
	"$EREADER/app_shell.c"
	"$EREADER/main.c"
	"$LOCK_MAIN/app_driver.cpp"
	"$LOCK_MAIN/app_main.cpp"
	"$LOCK_MAIN/app_shell.cpp"
	"$LOCK_MAIN/lock/door_lock_manager.cpp"
	"$LOCK_MAIN/lock/door_lock_callbacks.cpp"
	"$LOCK_MAIN/lock/ultrawidelock_reader_delegate.cpp"
	"$ROOT/modules/ultrawidelock_dfu/src/dfu_receiver.c"
	"$ROOT/modules/ultrawidelock_dfu/src/dfu_applier.c"
	"$ROOT/ports/zephyr/dfu/dfu_smp_img.c"
	"$ROOT/ports/zephyr/nfc/pn532_bus_spi.c"
	"$ROOT/modules/ultrawidelock_nfc/src/transport_pn532.cpp"
	"$ROOT/modules/ultrawidelock_nfc/src/transport_none.cpp"
	"$ROOT/modules/ultrawidelock_cred_stack/src/cred_stack.cpp"
	"$ROOT/modules/ultrawidelock_cred_stack/src/session.cpp"
)

cov_cc -DULTRAWIDELOCK_PORT_HOST -D_DEFAULT_SOURCE -DCONFIG_ULTRAWIDELOCK_CRED=1 -DCONFIG_ULTRAWIDELOCK_UWB_CIRDIAG=1 \
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
	"$SRC/driver/uwb_selftest.c" "$ROOT/ports/zephyr/shell/ultrawidelock_shell.c" -o "$OUT/cov_drv"
ULTRAWIDELOCK_TEST_QUIET=1 LLVM_PROFILE_FILE="$OUT/drv.profraw" "$OUT/cov_drv" \
	>>"$OUT/run.log" 2>&1 || true
OBJS+=(-object "$OUT/cov_drv")

psa_flags=(-I"$HOSTD/psafake" -I"$ROOT/modules/ultrawidelock_uwb/include" -I"$SRC/ccc")
cov_cc "${psa_flags[@]}" -c -Dcrypto_aes_ecb_encrypt=ultrawidelock_test_psa_ecb \
	"$SRC/ccc/ccc_crypto_psa.c" -o "$OUT/ccc_crypto_psa_cov.o"
cov_cc "${psa_flags[@]}" -c -Dcrypto_aes_ecb_encrypt=ultrawidelock_test_mbedtls_ecb \
	"$SRC/ccc/ccc_crypto_mbedtls.c" -o "$OUT/ccc_crypto_mbedtls_cov.o"
# ultrawidelock_seal.c rides this stage for the same reason it does in run.sh:
# it is a PSA seam, and the shared-core binary has no psa/crypto.h. Read from
# the role manifest, like every other consumer of it.
seal_srcs=()
for _r in seal link anchor_msg; do
	while IFS= read -r _l; do
		_l="${_l%%#*}"
		_l="${_l#"${_l%%[![:space:]]*}"}"
		_l="${_l%"${_l##*[![:space:]]}"}"
		[ -n "$_l" ] && seal_srcs+=("$ROOT/$_l")
	done < "$ROOT/modules/ultrawidelock_anchor/roles/$_r.list"
done
cov_cc "${psa_flags[@]}" -I"$HOSTD" -I"$CRED/include" \
	-I"$ROOT/modules/ultrawidelock_anchor/include" \
	"$HOSTD/test.c" "$HOSTD/test_psa_backends.c" "$HOSTD/psafake/psafake.c" \
	"$HOSTD/test_ultrawidelock_seal.c" "$HOSTD/test_ultrawidelock_link.c" \
	"${seal_srcs[@]}" \
	"$CRED/src/ultrawidelock_prim_psa.c" \
	"$OUT/ccc_crypto_psa_cov.o" "$OUT/ccc_crypto_mbedtls_cov.o" -o "$OUT/cov_psa"
run_suite psa "$OUT/cov_psa"

# C++ suite: same instrumentation flags through the C++ driver.
cov_cc -c "$HOSTD/test.c" -o "$OUT/test_harness_c_cov.o"
"${CXX:-c++}" -std=c++17 -O0 -g -w -fprofile-instr-generate -fcoverage-mapping \
	-DCONFIG_DOOR_LOCK_RFAL_LOG_LEVEL=3 \
	-I"$HOSTD" -I"$HOSTD/ecpfake" \
	"$HOSTD/test_nfc_ecp.cpp" "$ROOT/modules/ultrawidelock_nfc/src/nfc_prop_ecp.cpp" \
	"$OUT/test_harness_c_cov.o" -o "$OUT/cov_ecp"
run_suite ecp "$OUT/cov_ecp"

# -DESP_PLATFORM and the ultrawidelock_port include are what tests/ports/esp32/run.sh
# compiles this pair with, and they are not optional: ultrawidelock_ble_nimble.c
# includes <ultrawidelock_log.h>, which lives there, and the ESP_PLATFORM branch of it is
# the one that resolves to sdkfake's esp_log.h -- the path the target takes.
cov_cc -DESP_PLATFORM \
	-I"$SDKFAKE" -I"$CRED/include" -I"$CRED/src" \
	-I"$ROOT/modules/ultrawidelock_port/include" \
	"$ET/test_esp_ultrawidelock_ble.c" "$ECOMP/ultrawidelock_ble/ultrawidelock_ble_esp32.c" \
	"$CRED/src/ultrawidelock_ble_nimble.c" \
	"$CRED/src/ultrawidelock_advtag.c" "$CRED/src/ultrawidelock_hash.c" \
	"$SHARED/ultrawidelock_prim_host.c" \
	"$SDKFAKE/fake_nimble.c" "$SDKFAKE/fake_nvs.c" -o "$OUT/cov_esp_ble"
run_suite esp_ble "$OUT/cov_esp_ble"

cov_cc -I"$SDKFAKE" -I"$CRED/include" \
	"$ET/test_esp_prov_nvs.c" "$ECOMP/ultrawidelock_reader/ultrawidelock_prov_nvs.c" \
	"$CRED/src/ultrawidelock_prov.c" "$SDKFAKE/fake_nvs.c" -o "$OUT/cov_esp_nvs"
run_suite esp_nvs "$OUT/cov_esp_nvs"

# The PIV CCID core and the presence link, mirroring tests/ports/esp32/run.sh.
# Both run in make check today, inside the shared suite; neither was measured.
cov_cc -I"$ECOMP/piv_ccid/include" \
	"$ET/test_piv_ccid.c" "$ECOMP/piv_ccid/piv_ccid.c" "$ECOMP/piv_ccid/piv_apdu.c" \
	-o "$OUT/cov_esp_piv"
run_suite esp_piv "$OUT/cov_esp_piv"

cov_cc -D_POSIX_C_SOURCE=200809L -DULTRAWIDELOCK_PORT_HOST \
	-DCONFIG_ULTRAWIDELOCK_PRESENCE_TIMEOUT_MS=1 -DCONFIG_ULTRAWIDELOCK_PRESENCE_MAX_CM=40 \
	-I"$SDKFAKE" -I"$ECOMP/ultrawidelock_reader" \
	-I"$CRED/include" -I"$CRED/src" -I"$ROOT/modules/ultrawidelock_port/include" \
	-I"$ROOT/modules/ultrawidelock_uwb/include" \
	"$ET/test_esp_presence_link.c" \
	"$ECOMP/ultrawidelock_reader/presence_link.c" \
	"$CRED/src/ultrawidelock_assert.c" "$CRED/src/ultrawidelock_hash.c" \
	"$SDKFAKE/fake_nvs.c" -o "$OUT/cov_esp_presence"
run_suite esp_presence "$OUT/cov_esp_presence"

# The DWM3001CDK settings glue, mirroring stage 4 of tests/host/run.sh. That
# stage had no mirror here at all, so make test proved this file's 51 checks
# while the report called it never built.
cov_cc -DCONFIG_LOG_DEFAULT_LEVEL=3 \
	-I"$HOSTD" -I"$HOSTD/settingsfake" -I"$HOSTD/logfake" \
	-I"$ROOT/modules/ultrawidelock_matter/include" -I"$ROOT/ports/zephyr/store" \
	"$HOSTD/test.c" "$HOSTD/test_matter_fab_settings.c" \
	"$HOSTD/settingsfake/settingsfake.c" \
	"$ROOT/ports/zephyr/store/matter_fab_settings.c" -o "$OUT/cov_cdk_fab"
run_suite cdk_fab "$OUT/cov_cdk_fab"

cov_cc -DCONFIG_ULTRAWIDELOCK_CRED_STEPUP=1 \
	-I"$SDKFAKE" -I"$ET" -I"$SHARED" -I"$CRED/include" -I"$CRED/src" \
	"$ET/test_esp_stepup_worker.c" "$ECOMP/ultrawidelock_reader/ultrawidelock_stepup_worker.c" \
	"$CRED/src/ultrawidelock_stepup.c" "$CRED/src/ultrawidelock_stepup_wire.c" \
	"$CRED/src/ultrawidelock_stepup_parse.c" "$CRED/src/ultrawidelock_tlv.c" \
	"$CRED/src/ultrawidelock_hash.c" "$CRED/src/ultrawidelock_crypto.c" \
	"$SHARED/ultrawidelock_prim_host.c" "$SDKFAKE/fake_freertos.c" -o "$OUT/cov_esp_worker"
run_suite esp_worker "$OUT/cov_esp_worker"

# _POSIX_C_SOURCE because main.c now includes ultrawidelock_port.h, whose host build calls
# clock_gettime(CLOCK_MONOTONIC): glibc declares neither without it, while macOS
# declares both unconditionally, so omitting it builds locally and fails on CI.
cov_cc -D_POSIX_C_SOURCE=200809L -DCONFIG_ULTRAWIDELOCK_CRED_STEPUP=1 -DULTRAWIDELOCK_PORT_HOST \
	-I"$SDKFAKE" -I"$EREADER" -I"$ROOT/modules/ultrawidelock_uwb/include" \
	-I"$CRED/include" -I"$ROOT/modules/ultrawidelock_port/include" \
	"$ET/test_esp_app_shell.c" "$EREADER/app_shell.c" \
	"$EREADER/main.c" \
	"$SDKFAKE/fake_freertos.c" "$SDKFAKE/fake_esp.c" -o "$OUT/cov_esp_shell"
run_suite esp_shell "$OUT/cov_esp_shell"

# Exercise the C6 variant here; the regular port suite builds both S3 and C6.
# board_pins.h deliberately has no implicit target fallback, and dw3000_hw.c
# derives its cycle rate and worker core from these target settings.
cov_cc -DCONFIG_ULTRAWIDELOCK_UWB_CIRDIAG=1 \
	-DCONFIG_IDF_TARGET_ESP32C6=1 \
	-DCONFIG_FREERTOS_NUMBER_OF_CORES=1 \
	-DCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160 \
	-I"$SDKFAKE" -I"$ECOMP/ultrawidelock_uwb/port" -I"$ROOT/modules/ultrawidelock_uwb/include" \
	-I"$ROOT/modules/ultrawidelock_dw3000/include" -I"$ROOT/modules/ultrawidelock_dw3000/dwt_uwb_driver" \
	"$ET/test_esp_dw3000_port.c" \
	"$ECOMP/ultrawidelock_uwb/port/dw3000_hw.c" "$ECOMP/ultrawidelock_uwb/port/dw3000_spi.c" \
	"$SDKFAKE/fake_driver.c" "$SDKFAKE/fake_freertos.c" -o "$OUT/cov_esp_dw"
run_suite esp_dw "$OUT/cov_esp_dw"

cov_cc -DCONFIG_ULTRAWIDELOCK_UWB_CIRDIAG=1 -DCONFIG_ULTRAWIDELOCK_CRED=1 \
	-I"$ROOT/modules/ultrawidelock_dw3000/dwt_uwb_driver" -I"$ROOT/modules/ultrawidelock_uwb/include" \
	"$ET/test_esp_seam_stubs.c" \
	"$ECOMP/ultrawidelock_uwb/port/ultrawidelock_seam_stubs.c" -o "$OUT/cov_esp_seam"
run_suite esp_seam "$OUT/cov_esp_seam"

# C++ suite: the six matter-lock app sources on the matterfake CHIP/esp-matter
# doubles (mirror of the run.sh matter-lock stage; lock_led.c is C, own object).
MLOCK="$LOCK_MAIN"
MFAKE="$ET/matterfake"
cov_cc -c "$MLOCK/lock_led.c" -o "$OUT/lock_led_matter_cov.o"
# app_main.cpp's reader task drives the shared ultrawidelock_approach controller (C);
# link it in like lock_led.c or the app_main.o has undefined references.
cov_cc -I"$CRED/include" -c "$CRED/src/ultrawidelock_approach.c" -o "$OUT/ultrawidelock_approach_matter_cov.o"
"${CXX:-c++}" -std=c++17 -O0 -g -w -fprofile-instr-generate -fcoverage-mapping \
	-DCONFIG_ENABLE_ULTRAWIDELOCK_BLE_UWB=1 -DCONFIG_ULTRAWIDELOCK_CRED_LAB=1 \
	-DCONFIG_ULTRAWIDELOCK_UWB_CIRDIAG=1 \
	-DCONFIG_ULTRAWIDELOCK_LAT_TRACE=1 -DCONFIG_IDF_TARGET_ESP32C6=1 -DULTRAWIDELOCK_PORT_HOST \
	-I"$MFAKE" -I"$SDKFAKE" -I"$MLOCK" -I"$MLOCK/lock" \
	-I"$CRED/include" -I"$ROOT/modules/ultrawidelock_port/include" \
	-I"$ROOT/modules/ultrawidelock_uwb/include" \
	"$ET/test_esp_matter_lock.cpp" \
	"$MLOCK/app_driver.cpp" "$MLOCK/app_main.cpp" "$MLOCK/app_shell.cpp" \
	"$MLOCK/lock/door_lock_manager.cpp" "$MLOCK/lock/door_lock_callbacks.cpp" \
	"$MLOCK/lock/ultrawidelock_reader_delegate.cpp" \
	"$MFAKE/matterfake.cc" "$OUT/lock_led_matter_cov.o" \
	"$OUT/ultrawidelock_approach_matter_cov.o" -o "$OUT/cov_esp_matter"
run_suite esp_matter "$OUT/cov_esp_matter"

# Delta update, both halves plus the SMP front door, on the RAM flash of
# dfufake/ (which enforces the nRF word/page alignment rules), psafake's
# recording PSA and a scripted detools. Mirrors run.sh stage 5.
cov_cc -DULTRAWIDELOCK_PORT_HOST -DCONFIG_ULTRAWIDELOCK_DFU_SMP_IMG=1 -DCONFIG_ULTRAWIDELOCK_DFU_APPLIER_CHUNK=256 \
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
	"$ROOT/ports/zephyr/dfu/dfu_smp_img.c" -o "$OUT/cov_dfu"
run_suite dfu "$OUT/cov_dfu"

# C++ suite: the ultrawidelock_nfc transport seam over nfcfake, with the REAL pn532.c and
# pn532_apdu.c linked in. transport_none.cpp is renamed on its own compile step
# because it defines the same five symbols as transport_pn532.cpp. Mirrors
# run.sh stage 6.
NFC_DEF=(-DCONFIG_ULTRAWIDELOCK_NFC_LOG_LEVEL=3 -DCONFIG_ULTRAWIDELOCK_NFC_PN532_THREAD_STACK_SIZE=2048
	-DCONFIG_ULTRAWIDELOCK_NFC_PN532_POLL_PERIOD_MS=200
	-DCONFIG_ULTRAWIDELOCK_NFC_PN532_EXCHANGE_TIMEOUT_MS=1000
	-DULTRAWIDELOCK_PORT_HOST)
NFC_INC=(-I"$HOSTD" -I"$HOSTD/nfcfake" -I"$ROOT/modules/ultrawidelock_nfc/include"
	-I"$ROOT/modules/ultrawidelock_nfc/src" -I"$ROOT/modules/ultrawidelock_port/include")
_cov_cxx() {
	"${CXX:-c++}" -std=c++17 -O0 -g -w \
		-fprofile-instr-generate -fcoverage-mapping "$@"
}
cov_cxx() { ui_run "$(_cov_label "$@")" _cov_cxx "$@"; }
cov_cc -c "$HOSTD/test.c" -o "$OUT/test_harness_nfc_cov.o"
cov_cc -c -I"$ROOT/modules/ultrawidelock_nfc/include" -I"$ROOT/modules/ultrawidelock_nfc/src" \
	"$ROOT/modules/ultrawidelock_nfc/src/pn532.c" \
	-o "$OUT/pn532_nfc_cov.o"
cov_cc -c -I"$ROOT/modules/ultrawidelock_nfc/include" -I"$ROOT/modules/ultrawidelock_nfc/src" \
	"$ROOT/modules/ultrawidelock_nfc/src/pn532_apdu.c" \
	-o "$OUT/pn532_apdu_nfc_cov.o"
cov_cc -c "${NFC_DEF[@]}" "${NFC_INC[@]}" \
	"$ROOT/ports/zephyr/nfc/pn532_bus_spi.c" -o "$OUT/pn532_bus_spi_cov.o"
cov_cxx -c "${NFC_DEF[@]}" "${NFC_INC[@]}" \
	"$ROOT/modules/ultrawidelock_nfc/src/transport_pn532.cpp" -o "$OUT/transport_pn532_cov.o"
cov_cxx -c -DUltraWideLockNfc=UltraWideLockNfcNone "${NFC_DEF[@]}" "${NFC_INC[@]}" \
	"$ROOT/modules/ultrawidelock_nfc/src/transport_none.cpp" -o "$OUT/transport_none_cov.o"
cov_cxx -c "${NFC_INC[@]}" "$HOSTD/nfcfake/nfcfake.cpp" -o "$OUT/nfcfake_cov.o"
cov_cxx -c "${NFC_DEF[@]}" "${NFC_INC[@]}" "$HOSTD/test_nfc_transport.cpp" \
	-o "$OUT/test_nfc_transport_cov.o"
cov_cxx "$OUT/test_nfc_transport_cov.o" "$OUT/nfcfake_cov.o" "$OUT/test_harness_nfc_cov.o" \
	"$OUT/transport_none_cov.o" "$OUT/transport_pn532_cov.o" "$OUT/pn532_bus_spi_cov.o" \
	"$OUT/pn532_nfc_cov.o" "$OUT/pn532_apdu_nfc_cov.o" -o "$OUT/cov_nfc"
run_suite nfc "$OUT/cov_nfc"

# uwb_seam.h's engine-less tier, alone and WITHOUT CONFIG_ULTRAWIDELOCK_CRED. Every
# other object here compiles that header as declarations only and so emits no
# mapping for it; this one carries the inline bodies, which is why it has to
# stay a translation unit of its own. Mirrors run.sh stage 7.
cov_cc -I"$HOSTD" -I"$HOSTD/shim" -I"$HOSTD/logfake" \
	-I"$ROOT/modules/ultrawidelock_uwb/include" -I"$SRC/driver" \
	-I"$ROOT/modules/ultrawidelock_dw3000/include" \
	"$HOSTD/test.c" "$HOSTD/test_uwb_seam.c" -o "$OUT/cov_seam"
run_suite seam "$OUT/cov_seam"

# C++ suite: the credential source stack over the Interface doubles in stackfake/.
# The protocol codecs beside it are already in UNIT_SRCS and are compiled again
# here; they carry no conditional compilation, so both objects map the same
# lines the same way and the profiles merge. Mirrors run.sh stage 8.
STK="$ROOT/modules/ultrawidelock_cred_stack/src"
STK_DEF=(-DCONFIG_NCS_ALIRO_LOG_LEVEL_VALUE=3 -DCONFIG_NCS_ALIRO_BLE_UWB=1
	-DCONFIG_DOOR_LOCK_EXPEDITED_FAST_PHASE=1 -DCONFIG_DOOR_LOCK_STEP_UP_PHASE=1
	-DCONFIG_DOOR_LOCK_BLE_UWB_MAX_SESSIONS=2 -DCONFIG_ULTRAWIDELOCK_CRED_APDU_BUFFER_SIZE=1024
	-DCONFIG_MAX_NUMBER_OF_KPERSISTENT=4
	-DCONFIG_DOOR_LOCK_STORAGE_MAX_STORED_ACCESS_DOCUMENTS=2)
STK_INC=(-I"$HOSTD" -I"$HOSTD/stackfake" -I"$STK" -I"$STK/protocol"
	-I"$ROOT/modules/ultrawidelock_cred/include" -I"$ROOT/modules/ultrawidelock_cred/src")
STK_OBJS=()
cov_cc -c "$HOSTD/test.c" -o "$OUT/test_harness_stack_cov.o"
for stk_src in advertising_core protocol/ble_message protocol/ble_timeout \
	protocol/nfc_select protocol/nfc_auth; do
	stk_obj="$OUT/stk_$(basename "$stk_src")_cov.o"
	cov_cc "${STK_DEF[@]}" -I"$STK" -I"$STK/protocol" \
		-I"$ROOT/modules/ultrawidelock_cred/include" -c "$STK/$stk_src.c" -o "$stk_obj"
	STK_OBJS+=("$stk_obj")
done
# Shared step-up wire codecs + parser (mirrors run.sh stage 8).
for ultrawidelock_src in ultrawidelock_tlv ultrawidelock_stepup_wire ultrawidelock_stepup_parse; do
	stk_obj="$OUT/stk_${ultrawidelock_src}_cov.o"
	cov_cc -I"$ROOT/modules/ultrawidelock_cred/include" -I"$ROOT/modules/ultrawidelock_cred/src" \
		-c "$ROOT/modules/ultrawidelock_cred/src/$ultrawidelock_src.c" -o "$stk_obj"
	STK_OBJS+=("$stk_obj")
done
# Real symmetric crypto; see run.sh stage 8 for what is real and what is not.
cov_cc -I"$ROOT/modules/ultrawidelock_cred/include" -I"$ROOT/modules/ultrawidelock_cred/src" \
	-c "$ROOT/modules/ultrawidelock_cred/src/ultrawidelock_hash.c" -o "$OUT/stk_ultrawidelock_hash_cov.o"
cov_cc -I"$ROOT/modules/ultrawidelock_cred/include" -I"$ROOT/modules/ultrawidelock_cred/src" \
	-c "$SHARED/ultrawidelock_prim_host.c" -o "$OUT/stk_ultrawidelock_prim_host_cov.o"
cov_cxx -c "${STK_DEF[@]}" "${STK_INC[@]}" "$STK/cred_stack.cpp" -o "$OUT/stk_cred_stack_cov.o"
cov_cxx -c "${STK_DEF[@]}" "${STK_INC[@]}" "$STK/session.cpp" -o "$OUT/stk_session_cov.o"
cov_cxx -c "${STK_DEF[@]}" "${STK_INC[@]}" "$HOSTD/stackfake/stackfake.cpp" \
	-o "$OUT/stackfake_cov.o"
cov_cxx -c "${STK_DEF[@]}" "${STK_INC[@]}" "$HOSTD/test_ultrawidelock_stack.cpp" \
	-o "$OUT/test_ultrawidelock_stack_cov.o"
cov_cxx "$OUT/test_ultrawidelock_stack_cov.o" "$OUT/stackfake_cov.o" "$OUT/test_harness_stack_cov.o" \
	"$OUT/stk_cred_stack_cov.o" "$OUT/stk_session_cov.o" "${STK_OBJS[@]}" \
	"$OUT/stk_ultrawidelock_hash_cov.o" "$OUT/stk_ultrawidelock_prim_host_cov.o" \
	-o "$OUT/cov_stack"
run_suite stack "$OUT/cov_stack"

_cov_merge() { llvm_tool llvm-profdata merge -sparse "$OUT"/*.profraw -o "$OUT/host.profdata"; }
ui_run "merge profiles" _cov_merge

# CORE_UNIT_SRCS was written assuming it does not overlap UNIT_SRCS, and that
# stopped being true once the shared-core units gained host unit tests of their
# own: ultrawidelock_hash.c and ultrawidelock_prov.c now appear in both. A repeated path makes
# llvm-cov print the file twice and count its lines twice in the denominator,
# which moves the number the CI floor is checked against. Dedupe rather than
# deleting the entries, so an overlap added later cannot reintroduce it.
ALL_UNIT_SRCS=()
for src in "${UNIT_SRCS[@]}" "${CORE_UNIT_SRCS[@]}" "${SIDE_UNIT_SRCS[@]}"; do
	seen=0
	for kept in ${ALL_UNIT_SRCS[@]+"${ALL_UNIT_SRCS[@]}"}; do
		[ "$kept" = "$src" ] && { seen=1; break; }
	done
	[ "$seen" -eq 0 ] && ALL_UNIT_SRCS+=("$src")
done

# Our headers, all of them: llvm-cov attributes inline-function coverage to
# the header wherever an instrumented TU instantiated it, and silently skips
# paths with no coverage mapping, so the whole set can be passed.
HDR_SRCS=()
while IFS= read -r h; do
	HDR_SRCS+=("$ROOT/$h")
done < <(cd "$ROOT" && find modules ports -name '*.h' ! -path '*/test/*' \
	! -path 'modules/ultrawidelock_dw3000/*' ! -path 'modules/ultrawidelock_dfu/src/detools/*' | LC_ALL=C sort)

# Browsable HTML, restricted to the units under test.
_cov_html() {
	llvm_tool llvm-cov show "$BIN" "${OBJS[@]}" -instr-profile="$OUT/host.profdata" \
		"${ALL_UNIT_SRCS[@]}" \
		-format=html -output-dir="$OUT/html" \
		-show-line-counts-or-regions -show-branches=count >/dev/null
}
ui_run "html report" _cov_html

# Machine-readable summary for the terminal table (and the CI floor).
_cov_export() {
	llvm_tool llvm-cov export "$BIN" "${OBJS[@]}" -instr-profile="$OUT/host.profdata" \
		-summary-only "${ALL_UNIT_SRCS[@]}" "${HDR_SRCS[@]}" >"$OUT/summary.json"
}
ui_run "summary export" _cov_export

# --- everything of ours that never enters a host build ----------------------
# Discovered, not hand-listed, so a new source file shows up here on its own.
# Headers join the scan when they hold static-inline bodies; the report drops
# any candidate llvm-cov already measured, so only truly unbuilt code remains.
UNBUILT_TSV="$OUT/unbuilt.tsv"
: >"$UNBUILT_TSV"
PLATFORM_RE='#include [<"](zephyr/|esp_|nimble/|host/|freertos/|nvs|mbedtls/|psa/|deca_|app/|driver/|crypto/|lib/)'
# Every suite in the repo, including the ones this script does not mirror. A file
# absent from the measured set is not thereby untested: the opt-in FreeRTOS
# suite alone compiles thirty-odd of these. Saying "no host tests yet" about a
# file with a test beside it is worse than saying nothing, because it is the
# sentence someone reads before deciding to go and write that test.
SUITE_SCRIPTS="$(cd "$ROOT" && ls tests/host/run.sh tests/shared/run.sh tests/sdk/run.sh \
	tests/ports/*/run.sh 2>/dev/null)"
while IFS= read -r rel; do
	f="$ROOT/$rel"
	for u in "${ALL_UNIT_SRCS[@]}"; do
		[ "$f" = "$u" ] && continue 2
	done
	tag=untested
	if grep -q -E "$PLATFORM_RE" "$f"; then
		tag=target-only
	fi
	# Matched on the last two path components, because two ports carry files of
	# the same name (dw3000_hw.c, ultrawidelock_seam_stubs.c) and a bare
	# basename would credit one port's file with the other's tests.
	frag="$(basename "$(dirname "$rel")")/$(basename "$rel")"
	for s in $SUITE_SCRIPTS; do
		if grep -q -F "$frag" "$ROOT/$s"; then
			tag=elsewhere
			break
		fi
	done
	printf '%s\t%s\n' "$rel" "$tag" >>"$UNBUILT_TSV"
# ports/zephyr/dw3000/ is excluded for the same reason modules/ultrawidelock_dw3000
# is: THIRD_PARTY_NOTICES puts both in the DW3000 scope, derived from
# br101/zephyr-dw3000-decadriver and carrying the component license. It was
# inside the module tree when this list was written and moved out later; the
# exclusion did not follow it, so 508 lines of upstream have been sitting in the
# denominator of a report whose header says third-party sources are not in it.
# The ESP-IDF backends of the same names stay: the notices file calls those
# project-original implementations of the same seam headers.
done < <(cd "$ROOT" && {
	find modules ports \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' \) \
		! -path '*/test/*' ! -path '*/managed_components/*' \
		! -path 'modules/ultrawidelock_dw3000/*' ! -path 'modules/ultrawidelock_dfu/src/detools/*' \
		! -path 'ports/zephyr/dw3000/*'
	find modules ports -name '*.h' ! -path '*/test/*' \
		! -path 'modules/ultrawidelock_dw3000/*' ! -path 'modules/ultrawidelock_dfu/src/detools/*' \
		! -path 'ports/zephyr/dw3000/*' \
		-exec grep -l 'static inline' {} +
} | LC_ALL=C sort)

# --- surfaces beyond C: listed for visibility, not instrumented --------------
SURFACES_TSV="$OUT/surfaces.tsv"
: >"$SURFACES_TSV"
surf() { printf '%s\t%s\t%s\n' "$1" "$2" "$3" >>"$SURFACES_TSV"; }
loc() { wc -l <"$ROOT/$1" | tr -d ' '; }

npatch="$(find "$ROOT/integrations/nrfconnect-door-lock/patches" -name '*.patch' | wc -l | tr -d ' ')"
nadded="$(find "$ROOT/integrations/nrfconnect-door-lock/patches" -name '*.patch' -exec cat {} + |
	grep -c '^+[^+]')"
surf "integrations/nrfconnect-door-lock/patches/ ($npatch patches)" "$nadded" \
	"our code inside the Nordic add-on — target-only"
nsh="$(cd "$ROOT" && find scripts release tests/tooling apps/nrf5340dk-lock -name '*.sh' | wc -l | tr -d ' ')"
nshl="$(cd "$ROOT" && find scripts release tests/tooling apps/nrf5340dk-lock -name '*.sh' -exec cat {} + |
	wc -l | tr -d ' ')"
surf "build + tooling shell ($nsh scripts)" "$nshl" \
	"tests/tooling covers patch drift"

# The table itself is the last step, and ui_end's footer goes under it.
_cov_report() {
	"$PY" "$ROOT/tests/host/coverage_report.py" \
		"$OUT/summary.json" "$OUT/html/index.html" "$UNBUILT_TSV" "$SURFACES_TSV"
}
ui_run "coverage table" _cov_report

# Surface a failing suite without aborting the coverage report.
if ! grep -q "RESULT: PASS" "$OUT/run.log"; then
	echo "    note: suite did not report PASS — see $OUT/run.log"
fi
ui_end
