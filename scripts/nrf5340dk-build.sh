#!/usr/bin/env bash
#
# build.sh {build|rebuild|flash|flash-erase|build-flash}: build the
# credential NFC+UWB image for the nRF5340 DK from the self-contained ./workspace.
# Run scripts/bootstrap.sh first.
#
# Named for its board because BOARD below is hardcoded: this script builds
# nrf5340dk/nrf5340/cpuapp and nothing else. The DWM3001CDK is built straight
# from apps/dwm3001cdk-lock/ by mk/cdk.mk, and the ESP32 apps by mk/esp32.mk.
#
# Layers our modules + ISC dw3000 onto the fetched add-on via out-of-tree
# overlays. Output → build/nrf5340dk (git-ignored), or build/nrf5340dk-blob
# when ULTRAWIDELOCK_SOURCE=0, so flipping that flag no longer forces a pristine rebuild.
#
# Incremental by default — a full from-scratch (pristine) build runs only when it
# has to: first build, changed build flags (UWB chip / self-test / config), or
# when you ask for one. A preflight first checks the workspace is bootstrapped.
#
#   scripts/nrf5340dk-build.sh build                  # incremental where safe (fast)
#   scripts/nrf5340dk-build.sh rebuild                # force a clean pristine build
#   PRISTINE=1 scripts/nrf5340dk-build.sh build       # same as rebuild
#   UWB_SELFTEST=1 scripts/nrf5340dk-build.sh build   # one-shot boot self-test, no iPhone (diagnostic)
#   PRETTY=1 scripts/nrf5340dk-build.sh build         # curated/clean console (reversible; default verbose)
#   ULTRAWIDELOCK_SOURCE=0 scripts/nrf5340dk-build.sh build   # legacy Nordic credential binary fallback
#   UWB_CHIP=dw3720 scripts/nrf5340dk-build.sh build  # select the plugged-in UWB chip (default: dw3000)
#   LTO=1 scripts/nrf5340dk-build.sh build            # link-time optimisation (overlays/lto.conf)
#   DFU=1 scripts/nrf5340dk-build.sh build            # MCUboot + Matter OTA (overlays/sysbuild-dfu.conf)
#
# NOTE both default to OFF *here* and ON via `make nrf-build`, which is the same
# split the DWM3001CDK uses: mk/ is the policy layer and decides what a plain
# build means, this script only does what it is told. Call it directly and you
# get neither unless you ask.
#
# DFU=1 needs this checkout's image-signing key (`make dfu-key`) and refuses to
# build without one, because a bootloader that trusts MCUboot's published demo
# key trusts everybody. SIGN_KEY=<absolute path> overrides where it looks.
set -euo pipefail

# Lives in scripts/ like every other build program; the product it builds is
# apps/nrf5340dk-lock, which holds the overlays and the size baseline but no
# sources of its own -- the application itself is Nordic's, fetched into
# ./workspace and patched from integrations/nrfconnect-door-lock.
TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="$TREE/apps/nrf5340dk-lock"
WS="${ULTRAWIDELOCK_WS:-$TREE/workspace}"

# A linked git worktree usually has no NCS workspace of its own (the ~6.5 GB
# tree lives in the primary checkout); fall back to the primary's workspace so
# builds still work. An explicit ULTRAWIDELOCK_WS wins.
if [ -z "${ULTRAWIDELOCK_WS:-}" ] && [ ! -d "$WS/.west" ]; then
  _common="$(git -C "$TREE" rev-parse --git-common-dir 2>/dev/null || true)"
  if [ -n "$_common" ]; then
    case "$_common" in /*) ;; *) _common="$TREE/$_common" ;; esac
    _main="$(cd "$(dirname "$_common")" 2>/dev/null && pwd || true)"
    if [ -n "$_main" ] && [ -d "$_main/workspace/.west" ]; then WS="$_main/workspace"; fi
  fi
fi

NCS_VER="${NCS_VER:-v3.3.0}"
OV="$APP_DIR/overlays"
ADDON="$WS/ncs-door-lock-and-access-control"
APP="$ADDON/applications/matter-aliro-door-lock-app"
PATCH_DIR="$TREE/integrations/nrfconnect-door-lock/patches"
PATCH_STATE="$WS/.ultrawidelock-patches.sha256"
# One build root for the whole repo (Makefile exports ULTRAWIDELOCK_BUILD_ROOT); every
# producer derives its own subdirectory under it, so `make clean` is one rm.
# ULTRAWIDELOCK_SOURCE picks the subdirectory rather than reconfiguring one shared dir:
# the two link different credential implementations, and sharing a directory made
# every flip a from-scratch rebuild. Validated in do_build, not here, so an
# unknown value still dies with the message that names the legal ones.
BUILD_ROOT="${ULTRAWIDELOCK_BUILD_ROOT:-$TREE/build}"
case "${ULTRAWIDELOCK_SOURCE:-1}" in
0) BUILD_NAME="nrf5340dk-blob" ;;
*) BUILD_NAME="nrf5340dk" ;;
esac
BUILD="${ULTRAWIDELOCK_BUILD:-$BUILD_ROOT/$BUILD_NAME}"
BOARD="nrf5340dk/nrf5340/cpuapp"

# The MCUboot image-signing key, used only when DFU=1 puts a bootloader in the
# build. One key per checkout, shared with the DWM3001CDK: the top-level
# Makefile exports SIGN_KEY and this is the same default, spelled again because
# CI calls this script directly and never goes through make.
#
# Absolute on purpose. A relative path resolves inside the MCUboot repository
# (boot/zephyr/CMakeLists.txt:428) and silently becomes MCUboot's published demo
# key; scripts/check-signing-key.sh refuses one for that reason.
default_sign_key="$TREE/apps/dwm3001cdk-lock/keys/mcuboot_ec_p256.pem"
if [ -f "$TREE/firmware/keys/mcuboot_ec_p256.pem" ]; then
  default_sign_key="$TREE/firmware/keys/mcuboot_ec_p256.pem"
fi
SIGN_KEY="${SIGN_KEY:-$default_sign_key}"

# Launch a west command through nrfutil's Nordic SDK toolchain manager for the configured NCS version. Ensures all builds use the pinned toolchain without calling bare west.
# ULTRAWIDELOCK_TOOLCHAIN=env skips that wrapper and runs the command directly — for
# environments with the toolchain already on PATH (the NCS toolchain container
# in CI, where nrfutil's toolchain index is not reachable).
if [ "${ULTRAWIDELOCK_TOOLCHAIN:-}" = env ]; then
  launch() { "$@"; }
else
  # Launch a command in the NCS toolchain environment for the configured version.
  launch() { nrfutil sdk-manager toolchain launch --ncs-version "$NCS_VER" -- "$@"; }
fi
# Compute SHA-1 hash; tries shasum first (BSD/macOS), falls back to sha1sum (Linux). Filters output to the hash hex string only.
sha()    { if command -v shasum >/dev/null 2>&1; then shasum; else sha1sum; fi; }

# --- pretty, quiet-by-default output -----------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  BOLD=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GRN=$'\033[32m'
  YLW=$'\033[33m'; BLU=$'\033[34m'; RST=$'\033[0m'
else
  BOLD=; DIM=; RED=; GRN=; YLW=; BLU=; RST=
fi
# Print a section header to stdout: blue "==>" followed by bold text. Used to mark the start of major build phases (preflight, build, done).
hdr() { printf '\n%s==>%s %s%s%s\n' "$BLU" "$RST" "$BOLD" "$*" "$RST"; }
# Print a checkmark to stdout in green followed by text. Used to mark successful completion of build steps.
ok()  { printf '  %s✓%s %s\n' "$GRN" "$RST" "$*"; }
# Print a key-value pair indented: dim key (9 chars wide) and value. Used to display build configuration during the build phase.
kv()  { printf '  %s%-9s%s %s\n' "$DIM" "$1" "$RST" "$2"; }
# Print an error message to stderr and exit with status 1. First line prints the error text in red; remaining arguments are printed as indented hints (dim text with arrow prefix). Used by preflight checks and build validation to fail fast on missing prerequisites or configuration errors.
die() {
  printf '\n%s✗ %s%s\n' "$RED" "$1" "$RST" >&2; shift || true
  local h; for h in "$@"; do printf '  %s→ %s%s\n' "$DIM" "$h" "$RST" >&2; done
  exit 1
}

# Resolve UWB_CHIP -> the dw3000 decadriver's chip Kconfig choice (modules/ultrawidelock_dw3000/Kconfig).
# Same DT node + wiring for both; only which *_device.c/dwt_driver builds changes.
resolve_chip() {
  case "$(printf '%s' "${UWB_CHIP:-dw3000}" | tr '[:upper:]' '[:lower:]')" in
    dw3000|dwm3000|dw3110) CHIP_FLAG="-DCONFIG_DW3000_CHIP_DW3000=y"; CHIP_NAME="DW3000/DWM3000" ;;
    dw3720|qm33|qm33xx)    CHIP_FLAG="-DCONFIG_DW3000_CHIP_DW3720=y"; CHIP_NAME="DW3720/QM33xx" ;;
    *) die "unknown UWB_CHIP='${UWB_CHIP:-}'" "use dw3000 or dw3720" ;;
  esac
}

# Verify bootstrap.sh left everything the build needs. All cheap fs/git checks.
preflight() {
  hdr "preflight"

  if [ "${ULTRAWIDELOCK_TOOLCHAIN:-}" = env ]; then
    command -v west >/dev/null 2>&1 \
      || die "west not found on PATH (ULTRAWIDELOCK_TOOLCHAIN=env)" \
             "unset ULTRAWIDELOCK_TOOLCHAIN to use the nrfutil-pinned toolchain"
    ok "toolchain from environment (west $(west --version 2>/dev/null | awk '{print $NF}'))"
  else
    command -v nrfutil >/dev/null 2>&1 \
      || die "nrfutil not found on PATH" \
             "install: https://www.nordicsemi.com/Products/Development-tools/nrf-util"
    ok "nrfutil $(nrfutil --version 2>/dev/null | head -1 | awk '{print $2}')"
  fi

  { [ -d "$WS/.west" ] && [ -d "$APP" ]; } \
    || die "workspace not bootstrapped ($WS)" "run: make bootstrap"
  ok "workspace initialized"

  # bootstrap patches these three fetched repos; a pristine repo means it did not run.
  local repo unpatched=""
  for repo in "$ADDON" "$WS/nrf" "$WS/modules/lib/matter"; do
    [ -d "$repo/.git" ] || continue
    [ -n "$(git -C "$repo" status --porcelain --untracked-files=no 2>/dev/null)" ] \
      || unpatched="$unpatched $(basename "$repo")"
  done
  [ -z "$unpatched" ] \
    || die "integration patches missing on:$unpatched" "run: make bootstrap"

  local expected_patch_state actual_patch_state=""
  expected_patch_state="$("$TREE/scripts/integration-patch-id.py" \
    "$PATCH_DIR" "${HA:-0}")"
  [ ! -f "$PATCH_STATE" ] || actual_patch_state="$(sed -n '1p' "$PATCH_STATE")"
  [ "$actual_patch_state" = "$expected_patch_state" ] \
    || die "integration patch set changed or HA mode differs" \
           "run: make bootstrap$( [ "${HA:-0}" = 1 ] && printf ' HA=1' )"
  ok "integration patches applied"

  local f missing=""
  for f in ultrawidelock-cred.conf dw3000-nfc.overlay pm_static.yml sysbuild-ultrawidelock.conf; do
    [ -f "$OV/$f" ] || missing="$missing $f"
  done
  [ -z "$missing" ] || die "overlay files missing:$missing"
  ok "overlays present"
}

# Build the credential UWB firmware image. Runs preflight checks, resolves chip config, applies optional overlays (pretty console, latency diagnostics, self-test), computes a signature from all -D flags, and runs west build (pristine if config changed, incremental otherwise). Writes build signature to a cache file to detect future flag changes. Outputs merged.hex to BUILD directory.
do_build() {
  preflight
  resolve_chip

  local selftest=""
  [ "${UWB_SELFTEST:-0}" = 1 ] && selftest="-DCONFIG_ULTRAWIDELOCK_UWB_SELFTEST=y"

  # Range-integrity gate (see modules/ultrawidelock_uwb/Kconfig). Default off = shadow mode
  # (verdict logged, every block still latches). STRICT=1 drops a block whose STS
  # correlated poorly instead of feeding it to the unlock seam.
  local strict=""
  [ "${STRICT:-0}" = 1 ] && strict="-DCONFIG_ULTRAWIDELOCK_RANGE_GATE_STRICT=y"

  # The independent public API is the default credential implementation. Keep the
  # Nordic archive available as an explicit diagnostic fallback so the two
  # implementations can still be compared while the source stack matures.
  local ultrawidelock_source="${ULTRAWIDELOCK_SOURCE:-1}"
  local ultrawidelock_source_flag=""
  case "$ultrawidelock_source" in
    1) ultrawidelock_source_flag="-DCONFIG_ULTRAWIDELOCK_CRED_SOURCE_STACK=y" ;;
    0) ultrawidelock_source_flag="-DCONFIG_ULTRAWIDELOCK_CRED_SOURCE_STACK=n" ;;
    *) die "unknown ULTRAWIDELOCK_SOURCE='$ultrawidelock_source'" "use 1 (source, default) or 0 (Nordic binary)" ;;
  esac

  # ULTRAWIDELOCK_TRACE=1: capture proprietary/source BLE session boundaries without
  # exposing URSK itself (only its truncated SHA-256 fingerprint is logged).
  local ultrawidelock_trace=""
  [ "${ULTRAWIDELOCK_TRACE:-0}" = 1 ] && ultrawidelock_trace="-DCONFIG_ULTRAWIDELOCK_CRED_TRACE=y"

  if [ -n "$ultrawidelock_trace" ]; then
    local trace_patch="$TREE/integration/patches/ultrawidelock-ble-trace.patch"
    if git -C "$ADDON" apply --check --reverse "$trace_patch" 2>/dev/null; then
      ok "credential BLE trace integration already applied"
    elif git -C "$ADDON" apply --check "$trace_patch" 2>/dev/null; then
      git -C "$ADDON" apply --whitespace=nowarn "$trace_patch"
      ok "credential BLE trace integration applied"
    else
      die "credential BLE trace patch does not match the workspace" "$trace_patch"
    fi
  fi
  # NFC=pn532|st25r|none selects the reader behind the ultrawidelock_nfc transport seam.
  # Default: st25r, the upstream X-NUCLEO-NFC12A1/ST25R300 path, matching the build
  # before the seam existed. NFC=none is for a DK with no NFC frontend: nothing
  # is compiled in and boot proceeds BLE/UWB-only with no NFC error.
  local -a nfc_flags=()
  local nfc_overlay="" nfc_conf="" nfc_name="none"
  case "${NFC:-st25r}" in
    none) ;;
    pn532)
      # INF: the reader is up, so keep the console quiet during polling. Raise to
      # _DBG to get the raw PN532 RX-frame hexdumps back for bus-level debugging.
      nfc_flags=(-DCONFIG_ULTRAWIDELOCK_NFC_TRANSPORT_PN532=y -DCONFIG_SPI=y
                 -DCONFIG_ULTRAWIDELOCK_NFC_LOG_LEVEL_INF=y)
      nfc_overlay=";$OV/pn532.overlay"
      nfc_name="PN532"
      ;;
    st25r)
      nfc_flags=(-DCONFIG_NFC_DRIVER_STM=y -DCONFIG_ULTRAWIDELOCK_NFC_TRANSPORT_RFAL=y
                 -DCONFIG_ULTRAWIDELOCK_NFC_LOG_LEVEL_INF=y)
      nfc_conf=";$OV/st25r.conf"
      nfc_name="ST25R"
      ;;
    *) die "unknown NFC transport (use pn532, st25r, or none)" "NFC=$NFC" ;;
  esac

  # PRETTY=1: layer ultrawidelock-pretty.conf after ultrawidelock-cred.conf (curated console +
  # log-level cuts). Reversible: drop PRETTY and the flag + levels revert to the
  # verbose default. It rides EXTRA_CONF_FILE (in the build signature), so
  # toggling it forces the reconfigure the changed log levels need.
  local pretty_conf=""
  [ "${PRETTY:-0}" = 1 ] && pretty_conf=";$OV/ultrawidelock-pretty.conf"

  # LAT=1: layer diag-latency.conf (Matter DBG logging) to timestamp the
  # LockState ReportData egress vs the attribute set. Off-by-default diagnostic;
  # rides EXTRA_CONF_FILE (in the signature), so toggling it forces a reconfigure.
  local lat_conf=""
  [ "${LAT:-0}" = 1 ] && lat_conf=";$OV/diag-latency.conf"

  # CIR=1: layer diag-cirdiag.conf (CIA/CIR diagnostics: `ultrawidelock cir`). Off by
  # default because the windowed tap read costs walk-up latency while armed.
  # Rides EXTRA_CONF_FILE (in the signature), so toggling it forces a reconfigure.
  local cir_conf=""
  [ "${CIR:-0}" = 1 ] && cir_conf=";$OV/diag-cirdiag.conf"

  # HA=1: layer ultrawidelock-ha.conf (Home Assistant / multi-admin). Off by default so the
  # Apple Home demo image is untouched; see that file for why. Needs the matching
  # `make bootstrap HA=1`, which applies the data-model patches this pairs with.
  # Rides EXTRA_CONF_FILE (in the signature), so toggling it forces a reconfigure.
  local ha_conf=""
  [ "${HA:-0}" = 1 ] && ha_conf=";$OV/ultrawidelock-ha.conf"

  # LTO=1: layer lto.conf (whole-program codegen on the app image). OFF by default
  # in this script and ON via `make nrf-build`, which is where the policy lives;
  # it earned that default on 2026-08-03 with a walk-up unlock, because a size
  # number cannot vouch for the ranging arm deadline. lto.conf has the full
  # reasoning and the Kconfig trap. Rides EXTRA_CONF_FILE (in the
  # signature), so toggling it forces the pristine rebuild that changing codegen
  # needs anyway. Last in the list so nothing layered before it can undo it.
  local lto_conf=""
  local -a lto_flags=()
  if [ "${LTO:-0}" = 1 ]; then
    lto_conf=";$OV/lto.conf"
    # Keep LTO OFF in the bootloader. EXTRA_CONF_FILE is NOT application-only:
    # sysbuild forwards an un-namespaced value to every image in the same domain,
    # so with DFU=1 the MCUboot image inherited this overlay too. MCUboot then
    # built with CONFIG_LTO + CONFIG_ISR_TABLES_LOCAL_DECLARATION and hard-faulted
    # at boot with the PC inside _sw_isr_table: an RTC interrupt dispatched
    # through a bad table entry and executed the table as code. The board came up
    # dead with both consoles silent, because this MCUboot has no logging.
    # Measured on hardware 2026-08-03 (serial-numbered DK, mass-erased and
    # reflashed), diagnosed over SWD from the halted PC and the exception frame.
    #
    # Undoing it per-image is deliberate: namespacing the whole EXTRA_CONF_FILE to
    # the application would also stop ultrawidelock-cred.conf reaching MCUboot, which is a
    # bigger behavioural change than this bug warrants. The bootloader has nothing
    # to gain from whole-program codegen anyway.
    lto_flags=(-Dmcuboot_CONFIG_LTO=n -Dmcuboot_CONFIG_ISR_TABLES_LOCAL_DECLARATION=n)
  fi

  # DFU=1: MCUboot + Matter OTA back on. Three coupled switches, which is why they
  # are decided together here rather than as three independent options: the
  # sysbuild overlay is SWAPPED (never layered; the two files disagree on every
  # symbol), the app gets dfu.conf to point Matter's bootloader choice back at
  # MCUboot, and the flash map becomes the add-on's own MCUboot layout instead of
  # our no-bootloader one. That last map is named rather than copied into
  # overlays/, so there is one definition of it and it is upstream's. Not the
  # _uwb_dfu variant: that one is for a QM35 coprocessor this port does not have
  # (see sysbuild-dfu.conf).
  # MCUBOOT_LOG=1: give the bootloader a console. It has none by default, which is
  # why a bootloader that faults presents as a completely silent board with both
  # VCOMs dead and nothing to go on but a halted PC over SWD. Mirrors DFU_LOG=1 on
  # the DWM3001CDK. Diagnostic only: MCUboot's slot is 32 KB and already ~91% full
  # without this, so it may not fit, and the linker is the thing that will say so.
  local -a mcuboot_log_flags=()
  [ "${MCUBOOT_LOG:-0}" = 1 ] && mcuboot_log_flags=(
    -Dmcuboot_CONFIG_SERIAL=y -Dmcuboot_CONFIG_CONSOLE=y
    -Dmcuboot_CONFIG_UART_CONSOLE=y -Dmcuboot_CONFIG_PRINTK=y
    -Dmcuboot_CONFIG_LOG=y -Dmcuboot_CONFIG_LOG_MODE_MINIMAL=y
  )

  local dfu_conf="" sb_conf="$OV/sysbuild-ultrawidelock.conf" pm_yml="$OV/pm_static.yml"
  local -a sign_flags=()
  if [ "${DFU:-0}" = 1 ]; then
    dfu_conf=";$OV/dfu.conf"
    sb_conf="$OV/sysbuild-dfu.conf"
    pm_yml="$APP/pm_static_nrf5340dk_nrf5340_cpuapp.yml"
    [ -f "$pm_yml" ] || die "add-on MCUboot flash map not found" \
                            "expected $pm_yml" "run: make bootstrap"

    # A bootloader is only worth having if the key it trusts is one only this
    # checkout holds. Configure nothing and MCUboot signs with the key published
    # in its own repository, and this build did exactly that until now: the
    # generated mcuboot .config named workspace/bootloader/mcuboot/root-ec-p256.pem.
    # The DWM3001CDK has refused that since it grew a bootloader
    # (apps/dwm3001cdk-lock/sysbuild.cmake); this is the same refusal, run from here because
    # the application is a fetched upstream tree that never gets a sysbuild.cmake
    # of ours. Checked BEFORE the ~90 s configure so the fix arrives immediately.
    "$TREE/scripts/check-signing-key.sh" "$SIGN_KEY" \
      || die "refusing to build a bootloader anybody can sign for" \
             "the reason is printed above" \
             "DFU=0 builds the no-bootloader bench layout, which needs no key"

    # The inner quotes are part of the value: zephyr/cmake/modules/kconfig.cmake:264
    # writes a command-line cache variable through verbatim, and a Kconfig string
    # without quotes is a syntax error rather than a fallback to anything.
    sign_flags=(-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="\"$SIGN_KEY\"")
  fi

  # Every -D flag that, if changed, requires a from-scratch configure. Overlay
  # *content* edits are handled incrementally by Zephyr (configure-deps), so only
  # flag changes are captured here.
  local -a dflags=(
    -DEXTRA_CONF_FILE="$OV/ultrawidelock-cred.conf${nfc_conf}${pretty_conf}${lat_conf}${cir_conf}${ha_conf}${dfu_conf}${lto_conf}"
    -Dipc_radio_EXTRA_CONF_FILE="$OV/ipc_radio.conf"
    -DEXTRA_DTC_OVERLAY_FILE="$OV/dw3000-nfc.overlay${nfc_overlay}"
    -DPM_STATIC_YML_FILE="$pm_yml"
    -DSB_EXTRA_CONF_FILE="$sb_conf"
    # ultrawidelock_cred is first for the same reason apps/dwm3001cdk-lock lists
    # it: it declares CONFIG_ULTRAWIDELOCK_PRIM and builds the library behind it,
    # and ultrawidelock_uwb links `ultrawidelock_prim` unconditionally. Without
    # it in this list the symbol is never declared, the library is never built,
    # and the image fails at the final link with `cannot find
    # -lultrawidelock_prim` -- several hundred objects after the mistake.
    -DZEPHYR_EXTRA_MODULES="$TREE/modules/ultrawidelock_cred;$TREE/modules/ultrawidelock_uwb;$TREE/modules/ultrawidelock_nfc;$TREE/modules/ultrawidelock_cred_stack;$TREE/modules/ultrawidelock_dw3000;$TREE/ports/zephyr"
    -DCONFIG_DOOR_LOCK_BLE_UWB=y -DCONFIG_ULTRAWIDELOCK_UWB=y -DCONFIG_ULTRAWIDELOCK_UWB_RESPONDER=y
    -DCONFIG_ULTRAWIDELOCK_CRED=y -DCONFIG_DW3000=y "$CHIP_FLAG" -DCONFIG_SPI_ASYNC=y
    -DCONFIG_SHELL=n -DCONFIG_CHIP_LIB_SHELL=n -DCONFIG_NCS_SAMPLE_MATTER_TEST_SHELL=n
  )
  [ -n "$selftest" ] && dflags+=("$selftest")
  [ -n "$strict" ] && dflags+=("$strict")
  dflags+=("$ultrawidelock_source_flag")
  if [ -n "$ultrawidelock_trace" ]; then
    dflags+=(
      "$ultrawidelock_trace"
      -DCONFIG_DOOR_LOCK_ALIRO_BLE_SERVICE_LOG_LEVEL_INF=y
      -DCONFIG_DOOR_LOCK_ALIRO_GATT_SERVER_LOG_LEVEL_DBG=y
      -DCONFIG_DOOR_LOCK_ALIRO_L2CAP_SERVER_LOG_LEVEL_INF=y
    )
  fi
  [ ${#nfc_flags[@]} -gt 0 ] && dflags+=("${nfc_flags[@]}")
  [ ${#lto_flags[@]} -gt 0 ] && dflags+=("${lto_flags[@]}")
  [ ${#sign_flags[@]} -gt 0 ] && dflags+=("${sign_flags[@]}")
  [ ${#mcuboot_log_flags[@]} -gt 0 ] && dflags+=("${mcuboot_log_flags[@]}")

  # The signature lives beside the build root, never inside the build directory:
  # `west build -p always` deletes that directory, so a signature stored there
  # would vanish with it and every build would read as "prior config unknown".
  local sig sig_file
  sig_file="$BUILD_ROOT/_sig/$(basename "$BUILD").sig"
  mkdir -p "$(dirname "$sig_file")"
  sig="$(printf '%s\0' "$BOARD" "$APP" "$NCS_VER" "${dflags[@]}" | sha | awk '{print $1}')"

  # Decide: pristine (full, slow) vs incremental (fast).
  local pristine=0 reason=""
  if [ "${PRISTINE:-0}" = 1 ]; then
    pristine=1; reason="requested"
  elif [ ! -f "$BUILD/build.ninja" ]; then
    pristine=1; reason="no configured build yet"
  elif [ ! -f "$sig_file" ]; then
    pristine=1; reason="prior build config unknown"
  elif [ "$(cat "$sig_file" 2>/dev/null)" != "$sig" ]; then
    pristine=1; reason="build flags changed"
  fi

  hdr "build"
  kv "app"   "$(basename "$APP")"
  [ "$WS" != "$TREE/workspace" ] && kv "workspace" "${DIM}shared${RST} $WS"
  kv "board" "$BOARD"
  kv "chip"  "$CHIP_NAME${selftest:+   (self-test ON)}${pretty_conf:+   (pretty ON)}${strict:+   (gate STRICT)}${ultrawidelock_trace:+   (credential trace ON)}${lto_conf:+   (LTO ON)}"
  if [ "$ultrawidelock_source" = 1 ]; then
    kv "ultrawidelock" "in-tree source (default)"
  else
    kv "ultrawidelock" "Nordic binary (fallback)"
  fi
  kv "nfc"   "$nfc_name"
  if [ -n "$dfu_conf" ]; then
    kv "dfu" "MCUboot + Matter OTA   ${DIM}(secondary slot on external QSPI)${RST}"
    kv "key" "$SIGN_KEY"
  else
    kv "dfu" "${DIM}none (no bootloader; app owns flash from 0x0)${RST}"
  fi
  if [ "$pristine" = 1 ]; then
    kv "mode" "${YLW}pristine${RST} ${DIM}($reason)${RST}"
  else
    kv "mode" "${GRN}incremental${RST}"
  fi

  local start=$SECONDS
  if [ "$pristine" = 1 ]; then
    ( cd "$WS" && launch west build -b "$BOARD" --sysbuild "$APP" -p always -d "$BUILD" -- "${dflags[@]}" )
  else
    ( cd "$WS" && launch west build -d "$BUILD" )
  fi

  # LTO asked for is not LTO applied: an unmet Kconfig dependency drops CONFIG_LTO
  # from the generated .config silently (see overlays/lto.conf), and the build then
  # succeeds while measuring nothing. Read it back off the artefact instead.
  if [ -n "$lto_conf" ]; then
    local app_config="$BUILD/matter-aliro-door-lock-app/zephyr/.config"
    [ -f "$app_config" ] || die "app .config not found" "$app_config"
    local sym
    for sym in CONFIG_LTO CONFIG_ISR_TABLES_LOCAL_DECLARATION; do
      grep -qx "$sym=y" "$app_config" \
        || die "LTO=1 requested but $sym is not set in the built image" \
               "Kconfig dropped it rather than warning; see $OV/lto.conf" \
               "$app_config"
    done
    ok "LTO active in the linked image (CONFIG_LTO + ISR_TABLES_LOCAL_DECLARATION)"

    # ...and NOT active anywhere else. The first version of this guard only
    # checked that LTO was on where it was wanted, which is why it passed on a
    # build whose bootloader had silently inherited it and would not boot.
    local other
    for other in mcuboot b0n ipc_radio; do
      local other_config="$BUILD/$other/zephyr/.config"
      [ -f "$other_config" ] || continue
      grep -qx "CONFIG_LTO=y" "$other_config" \
        && die "LTO leaked into the $other image" \
               "that image is not the application and must not be LTO-built" \
               "$other_config"
    done
    ok "LTO confined to the application image"
  fi

  # Which key the bootloader ACTUALLY trusts, read off the artefact rather than
  # inferred from the flag we passed. Same lesson the LTO guard above is made of:
  # a -D flag that sysbuild declined to honour leaves a build that succeeds while
  # meaning something else. Here the something else is a bootloader that boots
  # anybody's firmware, and nothing in the build output would say so -- MCUboot
  # downgrades it to a message(WARNING) that a ten-thousand-line log swallows.
  if [ -n "$dfu_conf" ]; then
    local boot_config="$BUILD/mcuboot/zephyr/.config"
    [ -f "$boot_config" ] || die "mcuboot .config not found" "$boot_config"
    local built_key
    built_key=$(sed -n 's/^CONFIG_BOOT_SIGNATURE_KEY_FILE="\(.*\)"$/\1/p' "$boot_config")
    "$TREE/scripts/check-signing-key.sh" "$built_key" \
      || die "the built bootloader trusts a key this checkout does not own" \
             "SB_CONFIG_BOOT_SIGNATURE_KEY_FILE did not reach the mcuboot image" \
             "$boot_config"
    [ "$built_key" = "$SIGN_KEY" ] \
      || die "the built bootloader signs with a different key than requested" \
             "asked for $SIGN_KEY" \
             "got      $built_key"
    ok "MCUboot signs with this checkout's key, not MCUboot's published one"
  fi

  if [ "$ultrawidelock_source" = 1 ]; then
    local ultrawidelock_map="$BUILD/matter-aliro-door-lock-app/zephyr/zephyr.map"
    [ -f "$ultrawidelock_map" ] || die "credential source link map not found" "$ultrawidelock_map"
    if grep -q 'libaliro_ble\.a' "$ultrawidelock_map"; then
      die "proprietary credential archive still contributed linked code" \
          "source stack must define the complete application-used public ABI"
    fi
    ok "credential source stack linked without libaliro_ble.a members"
  fi
  printf '%s' "$sig" > "$sig_file"
  local secs=$(( SECONDS - start ))

  hdr "done"
  ok "$BUILD/merged.hex"
  kv "time" "$(printf '%dm%02ds' $((secs/60)) $((secs%60)))"
}

# Verify that a west build has completed in BUILD directory (build.ninja exists). Called before flash operations to fail fast if build has not run.
require_built() {
  [ -f "$BUILD/build.ninja" ] || die "no build in $BUILD" "run: $0 build"
}

# Resolve which J-Link probe to flash, into SNR. Only nRF5340DKs (board version
# PCA10095 in nrfutil device list) qualify, so another attached probe (e.g. a
# DWM3001CDK) is never a candidate. One DK -> auto-select it; several -> prompt;
# none -> fail loud. The flash always names its target explicitly via --dev-id.
resolve_snr() {
  command -v nrfutil >/dev/null 2>&1 || die "nrfutil not found on PATH"
  local -a snrs=()
  local s
  while read -r s; do snrs+=("$s"); done < <(
    nrfutil device list 2>/dev/null | awk -v RS='' '/Board version[ \t]+PCA10095/ {print $1}'
  )
  if [ "${#snrs[@]}" = 0 ]; then
    die "no nRF5340DK attached" "check: nrfutil device list"
  elif [ "${#snrs[@]}" = 1 ]; then
    SNR="${snrs[0]}"
  else
    printf '  %d nRF5340DKs attached:\n' "${#snrs[@]}"
    local i; for i in "${!snrs[@]}"; do printf '    %d) %s\n' "$((i+1))" "${snrs[$i]}"; done
    local pick
    read -rp "  flash which? [1-${#snrs[@]}] " pick \
      || die "no board selected" "non-interactive? flash directly: west flash --dev-id <snr>"
    { [[ "$pick" =~ ^[0-9]+$ ]] && [ "$pick" -ge 1 ] && [ "$pick" -le "${#snrs[@]}" ]; } \
      || die "invalid selection '$pick'"
    SNR="${snrs[$((pick-1))]}"
  fi
  kv "target" "nRF5340DK $SNR"
}

# Confirm the board we just wrote is not sitting in an APPROTECT-engaged state.
#
# This runs AFTER the flash rather than before, because a mass erase is one of
# the ways a board gets into that state: `west flash --erase` blanks UICR, and a
# blank UICR reads as APPROTECT ENGAGED on the nRF5340 until firmware writes it
# open again. So the dangerous moment is the one immediately after this command
# succeeds, when everything looks like it worked.
#
# Failing here is a warning, not a build failure: the image IS on the board and
# saying so is more useful than pretending the flash did not happen. What must
# never happen is silence, because a locked board does not announce itself. It
# serves PARTIAL debug reads, so RAM above some address starts returning
# "memory protection issue" and the board reads as physically broken.
#
# scripts/check-approtect.sh owns the actual test, including its self-test. This
# is a call site, not a second implementation.
warn_if_locked() {
  local checker="$TREE/scripts/check-approtect.sh" rc=0
  [ -x "$checker" ] || return 0
  "$checker" --device "$SNR" || rc=$?
  if [ "$rc" = 1 ]; then
    printf '\n  %s^^ the board is locked. It will behave like failing hardware.%s\n' \
      "$YLW" "$RST" >&2
  fi
  return 0
}

case "${1:-build}" in
  build)        do_build ;;
  rebuild)      PRISTINE=1; do_build ;;
  flash)        require_built; hdr "flash";         resolve_snr; ( cd "$WS" && launch west flash -d "$BUILD" --dev-id "$SNR" ); warn_if_locked ;;
  flash-erase)  require_built; hdr "flash (erase)"; resolve_snr; ( cd "$WS" && launch west flash --erase -d "$BUILD" --dev-id "$SNR" ); warn_if_locked ;;
  # An nRF53 with an erased UICR engages APPROTECT on the next POWER CYCLE, not
  # the next reset -- so a board that flashed fine all day refuses the first
  # flash after a recable with "must be recovered". This recovers and flashes in
  # one motion. MASS-ERASES BOTH CORES: fine for the stateless initiator, but on
  # the door-lock app it costs external_nvs -- the provisioned reader storage --
  # so it is its own verb rather than a fallback inside flash-erase.
  flash-recover) require_built; hdr "flash (recover)"; resolve_snr; ( cd "$WS" && launch west flash --recover -d "$BUILD" --dev-id "$SNR" ); warn_if_locked ;;
  build-flash)  do_build; hdr "flash";              resolve_snr; ( cd "$WS" && launch west flash -d "$BUILD" --dev-id "$SNR" ); warn_if_locked ;;
  *) echo "usage: [UWB_CHIP=dw3000|dw3720] [PRISTINE=1] $0 {build|rebuild|flash|flash-erase|build-flash}"; exit 2 ;;
esac
