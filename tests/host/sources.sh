# shellcheck shell=bash
# shellcheck disable=SC2034  # every list here is consumed by the sourcing scripts
# Shared source lists for the host test + coverage builds. Sourced by run.sh and
# coverage.sh; the caller must set $ROOT to the repo root.
#
# UNIT_SRCS  — our code under test (the coverage denominator).
# TEST_SRCS  — the harness + per-module suites + the host AES double.
# SHIM_SRCS  — non-inline shim definitions (STS register no-ops, RX stubs).
# See coverage.sh for what is deliberately excluded and why.

SRC="$ROOT/modules/ultrawidelock_uwb/src"
CRED="$ROOT/modules/ultrawidelock_cred"
ANCHOR="$ROOT/modules/ultrawidelock_anchor"
SHIM="$ROOT/tests/host/shim"
HOST="$ROOT/tests/host"

# unit_srcs_from_role — append one role manifest (modules/<mod>/roles/<n>.list)
# to UNIT_SRCS. Same lists cmake/ultrawidelock_roles.cmake reads, so the host suite and
# the apps/dwm3001cdk-lock/ESP builds cannot disagree about which files a role contains.
# Redirected (not piped) so the appends land in this shell under bash 3.2.
unit_srcs_from_role() {
	local _line
	while IFS= read -r _line || [ -n "$_line" ]; do
		_line="${_line%%#*}"
		_line="${_line#"${_line%%[![:space:]]*}"}"
		_line="${_line%"${_line##*[![:space:]]}"}"
		[ -n "$_line" ] || continue
		UNIT_SRCS+=("$ROOT/$_line")
	done < "$1"
}

UNIT_SRCS=(
	# App-layer, deliberately: the grant decision is the only piece of the
	# reader loop that was extracted into a pure function, and it is the one
	# the three firmwares each write out by hand. Testing the FreeRTOS copy
	# where it stands describes the behaviour all three are supposed to have,
	# without moving anything yet.
	"$ROOT/apps/dwm3001cdk-lock-freertos/src/grant.c"
	"$ROOT/modules/ultrawidelock_cred_stack/src/advertising_core.c"
	"$ROOT/modules/ultrawidelock_cred_stack/src/protocol/ble_message.c"
	"$ROOT/modules/ultrawidelock_cred_stack/src/protocol/ble_timeout.c"
	"$ROOT/modules/ultrawidelock_cred_stack/src/protocol/nfc_select.c"
	"$ROOT/modules/ultrawidelock_cred_stack/src/protocol/nfc_auth.c"
	"$ROOT/modules/ultrawidelock_nfc/src/pn532.c"
	"$ROOT/modules/ultrawidelock_nfc/src/pn532_apdu.c"
	"$ROOT/ports/zephyr/log/ultrawidelock_logfmt.c"
	"$ROOT/ports/zephyr/log/ultrawidelock_logquiet.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_tlv.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_msg.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_mrp.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_crypto.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_btp.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_pase.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_spake2p.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_pase_sm.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_exchange.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_im.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_clusters.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_attest.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_fabric.c"
	"$ROOT/modules/ultrawidelock_matter/src/matter_case.c"
	"$ROOT/modules/ultrawidelock_ml/src/ultrawidelock_ml_los.c"
	"$ROOT/modules/ultrawidelock_ml/src/ultrawidelock_ml_lin.c"
	"$ROOT/modules/ultrawidelock_ml/src/ultrawidelock_ml_feat.c"
	"$ROOT/modules/ultrawidelock_ml/src/ultrawidelock_ml_range.c"
	"$ROOT/modules/ultrawidelock_ml/src/ultrawidelock_ml_log2.c"
)

# ultrawidelock_anchor roles. Every one of them: the module is pure integer
# arithmetic over caller-owned structs, so the host suite compiles what the
# Zephyr module compiles across all four Kconfig gates at once.
unit_srcs_from_role "$ANCHOR/roles/anchor_core.list"
unit_srcs_from_role "$ANCHOR/roles/latch.list"
unit_srcs_from_role "$ANCHOR/roles/witness.list"
unit_srcs_from_role "$ANCHOR/roles/wire_codec.list"

# ultrawidelock_cred roles. wire_codecs = the shared step-up/assert codecs (one source;
# ultrawidelock_cred_stack compiles the same files on target) — crypto-free, so no
# ultrawidelock_crypto/prim backend is needed here. hash + reader_policy are the
# host-tested halves of the reader.
unit_srcs_from_role "$CRED/roles/wire_codecs.list"
unit_srcs_from_role "$CRED/roles/hash.list"
unit_srcs_from_role "$CRED/roles/reader_policy.list"

# ultrawidelock_uwb roles. Everything radio-free: the CCC key schedule and STS engine, the
# M1-M4 codec on BOTH ends (ultrawidelock_device = initiator side, tested against the
# real reader codec), the ranging estimator and responder state machine, and the
# flight recorder's replay half. base_driver/responder_driver need a DW3000 and
# are absent by design.
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/ccc_keys.list"
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/ccc_engine.list"
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/ultrawidelock_adapter.list"
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/ultrawidelock_codec.list"
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/ultrawidelock_device.list"
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/base_engine.list"
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/responder_engine.list"
unit_srcs_from_role "$ROOT/modules/ultrawidelock_uwb/roles/flight_recorder.list"

TEST_SRCS=(
	"$HOST/aes_ref.c"
	"$HOST/test.c"
	"$HOST/test_main.c"
	"$HOST/test_matter_tlv.c"
	"$HOST/test_matter_msg.c"
	"$HOST/test_matter_mrp.c"
	"$HOST/test_matter_crypto.c"
	"$HOST/test_matter_btp.c"
	"$HOST/test_matter_pase.c"
	"$HOST/test_matter_spake2p.c"
	"$HOST/test_matter_pase_sm.c"
	"$HOST/test_matter_exchange.c"
	"$HOST/test_matter_im.c"
	"$HOST/test_matter_attest.c"
	"$HOST/test_matter_attest_stub.c"
	"$HOST/test_matter_fabric.c"
	"$HOST/test_matter_thread_stub.c"
	"$HOST/test_matter_case.c"
	"$HOST/test_matter_case_stub.c"
	"$HOST/test_matter_clusters.c"
	"$HOST/test_ultrawidelock_advertising.c"
	"$HOST/test_ultrawidelock_ble.c"
	"$HOST/test_ultrawidelock_nfc.c"
	"$HOST/test_pn532.c"
	"$HOST/test_ccc_kdf.c"
	"$HOST/test_ccc_mac.c"
	"$HOST/test_ccc_sts.c"
	"$HOST/test_ccc_shim.c"
	"$HOST/test_ccc_session.c"
	"$HOST/test_prepoll_schedule.c"
	"$HOST/test_ultrawidelock_uwb_msg_builder.c"
	"$HOST/test_ultrawidelock_uwb_msg_parser.c"
	"$HOST/test_ultrawidelock_uwb_adapter.c"
	"$HOST/test_ultrawidelock_uwb_msg.c"
	"$HOST/test_ultrawidelock_uwb_session.c"
	"$HOST/test_ultrawidelock_prov.c"
	"$HOST/test_ultrawidelock_hash.c"
	"$HOST/test_ultrawidelock_assert.c"
	"$HOST/test_ultrawidelock_device_uwb.c"
	"$HOST/test_cherry.c"
	"$HOST/test_fira.c"
	"$HOST/test_facade.c"
	"$HOST/test_prepoll_gate.c"
	"$HOST/test_prepoll_round.c"
	"$HOST/test_rssi_gate.c"
	"$HOST/test_approach.c"
	"$HOST/test_grant.c"
	"$HOST/test_ultrawidelock_door.c"
	"$HOST/test_ultrawidelock_fusion.c"
	"$HOST/test_ultrawidelock_report.c"
	"$HOST/test_ultrawidelock_satellite.c"
	"$HOST/test_ultrawidelock_side.c"
	"$HOST/test_ultrawidelock_side_replay.c"
	"$HOST/test_ultrawidelock_latch.c"
	"$HOST/test_ultrawidelock_witness_core.c"
	"$HOST/test_ultrawidelock_witness_msg.c"
	"$HOST/test_ultrawidelock_witness_pick.c"
	"$HOST/test_ultrawidelock_slam.c"
	"$HOST/test_ultrawidelock_logfmt.c"
	"$HOST/test_trace.c"
	"$HOST/trace_stub.c"
	"$HOST/test_ccc_shim_wrap.c"
	"$HOST/test_flight_recorder.c"
	"$HOST/fr_replay.c"
	"$HOST/test_ultrawidelock_ml.c"
	"$HOST/test_ultrawidelock_port.c"
)

SHIM_SRCS=(
	"$SHIM/shim.c"
	"$SHIM/dw_rx_stub.c"
	"$HOST/logfake/logfake.c"
	"$HOST/spakefake/spakefake.c"
	# The host OSAL/flash backends double as the test fakes (ultrawidelock_osal.h).
	"$ROOT/tests/host/port/osal_host.c"
	"$ROOT/tests/host/port/flash_host.c"
)

# Include search path: shim first so <zephyr/...> resolves to the stubs;
# logfake supplies the Zephyr logging + CMSIS surface ultrawidelock_logfmt.c needs.
INCS=(
	-I"$SHIM"
	-I"$HOST"
	-I"$HOST/logfake"
	-I"$ROOT/modules/ultrawidelock_cred_stack/src"
	-I"$ROOT/modules/ultrawidelock_cred_stack/src/protocol"
	-I"$ROOT/modules/ultrawidelock_nfc/include"
	-I"$ROOT/modules/ultrawidelock_nfc/src"
	-I"$ROOT/modules/ultrawidelock_uwb/include"
	-I"$SRC/ccc"
	-I"$SRC/driver"
	-I"$SRC/cred"
	-I"$SRC/cred/include"
	-I"$SRC/fira"
	-I"$SRC/facade"
	-I"$ROOT/ports/zephyr/shell"
	-I"$CRED/include"
	-I"$ROOT/modules/ultrawidelock_port/include"
	-I"$ROOT/modules/ultrawidelock_cred/include"
	-I"$ROOT/modules/ultrawidelock_cred/src"
	-I"$ROOT/modules/ultrawidelock_matter/include"
	-I"$ROOT/modules/ultrawidelock_ml/include"
	-I"$ROOT/modules/ultrawidelock_ml/src"
	-I"$ROOT/modules/ultrawidelock_anchor/include"
	-I"$ROOT/modules/ultrawidelock_dw3000/include"
	# grant.h, still in the FreeRTOS app tree. See the note on UNIT_SRCS.
	-I"$ROOT/apps/dwm3001cdk-lock-freertos/src"
)

# The credential path is Kconfig-gated in-tree; the normal build has it on.
# _DEFAULT_SOURCE: glibc hides clock_gettime/CLOCK_MONOTONIC under strict
# -std=c11 without it (feature_test_macros(7)); Darwin headers ignore it.
# ULTRAWIDELOCK_PORT_HOST selects the libc backend in ultrawidelock_port.h / ultrawidelock_log.h; without it
# those headers #error rather than guess a platform.
# MATTER_FEATURE_DL_ALARMS: the DoorLockAlarm event, which the firmware compiles
# in only for the anchor build (modules/ultrawidelock_matter/CMakeLists.txt). The
# host suite always builds it, because the event's encoding is what these tests
# exist to pin down and the alternative is a feature whose only proof is on a
# board. The LockOperation path is compiled identically either way, so the
# suites covering it still describe the default image.
DEFS=(-DCONFIG_ULTRAWIDELOCK_CRED=1 -DCONFIG_ULTRAWIDELOCK_ML_LOS=1 -DCONFIG_ULTRAWIDELOCK_FLIGHT_RECORDER=1 -D_DEFAULT_SOURCE -DULTRAWIDELOCK_PORT_HOST -DMATTER_FEATURE_DL_ALARMS=1)

# PY — the interpreter the python-side suites run under: the repo-local .venv
# when one exists, else the system python3.
PY="$ROOT/.venv/bin/python3"
[ -x "$PY" ] || PY=python3
