# Standalone Zephyr app: the credential initiator (User-Device role) on an nRF5340 DK,
# the bench stand-in for an iPhone. Scans for a reader's 0xFFF2 advert, opens the
# L2CAP channel and runs the Access Protocol to a shared URSK. No UWB: ranging on
# that URSK is Stage 1b and needs the DWM3000EVB driver linked in as well.
#
# Layout follows on_target_ec/ next door: a normal Zephyr app that lists the
# modules/ultrawidelock_cred sources it needs directly, rather than a Zephyr module gated
# on CONFIG_ULTRAWIDELOCK_CRED_STACK. That module deliberately omits the transport, and
# the source list here is the same one examples/esp32/initiator links through
# its ultrawidelock_{crypto,device,ble_central} components.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ultrawidelock_initiator_nrf5340dk)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(CRED "${REPO_ROOT}/modules/ultrawidelock_cred")
set(UWB_INC "${REPO_ROOT}/modules/ultrawidelock_uwb/include")
set(UWB "${REPO_ROOT}/modules/ultrawidelock_uwb/src/cred")
set(CCC "${REPO_ROOT}/modules/ultrawidelock_uwb/src/ccc")

# Shared sources by role manifest (modules/{ultrawidelock_cred,ultrawidelock_uwb}/roles/*.list) —
# the same lists the ESP32 components and the host suite read.
include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
ultrawidelock_role_sources("${CRED}/roles/ble_central.list" ULTRAWIDELOCK_BLE_CENTRAL_SRCS)
ultrawidelock_role_sources("${CRED}/roles/device.list" ULTRAWIDELOCK_DEVICE_SRCS)
ultrawidelock_role_sources("${CRED}/roles/apdu.list" ULTRAWIDELOCK_APDU_SRCS)
ultrawidelock_role_sources("${CRED}/roles/crypto.list" ULTRAWIDELOCK_CRYPTO_SRCS)
ultrawidelock_role_sources("${CRED}/roles/hash.list" ULTRAWIDELOCK_HASH_SRCS)
ultrawidelock_role_sources("${CRED}/roles/prim_provider.list" ULTRAWIDELOCK_PRIM_PROVIDER_SRCS)
set(UWBROLES "${REPO_ROOT}/modules/ultrawidelock_uwb/roles")
ultrawidelock_role_sources("${UWBROLES}/ultrawidelock_device.list" UWB_DEVICE_SRCS)
ultrawidelock_role_sources("${UWBROLES}/ultrawidelock_codec.list" UWB_CODEC_SRCS)
ultrawidelock_role_sources("${UWBROLES}/ccc_keys.list" UWB_CCC_KEYS_SRCS)
ultrawidelock_role_sources("${UWBROLES}/crypto_prim.list" UWB_CRYPTO_SRCS)

target_sources(app PRIVATE
	src/main.c
	# BLE transport. The Zephyr backend lives with the reader's peripheral
	# mirror in ports/zephyr/ble so the two central/peripheral halves stay side
	# by side; it depends on no symbol from apps/dwm3001cdk-lock/Kconfig, which is what
	# makes it linkable from here. The ble_central role is the platform-free
	# decoder half, the same translation unit the ESP32 port and the host suite
	# compile.
	${REPO_ROOT}/ports/zephyr/ble/ultrawidelock_ble_central_zephyr.c
	${ULTRAWIDELOCK_BLE_CENTRAL_SRCS}
	# Access Protocol, device side (roles device + apdu), plus the key schedule
	# and the PSA primitive backend (roles crypto + hash + prim_provider).
	${ULTRAWIDELOCK_DEVICE_SRCS}
	${ULTRAWIDELOCK_APDU_SRCS}
	${ULTRAWIDELOCK_CRYPTO_SRCS}
	${ULTRAWIDELOCK_HASH_SRCS}
	${ULTRAWIDELOCK_PRIM_PROVIDER_SRCS}
	# UWB ranging SETUP, device side. Roles, not the ultrawidelock_uwb Zephyr module:
	# that module's CMakeLists gates everything on CONFIG_ULTRAWIDELOCK_UWB_RESPONDER and
	# builds the reader's session, adapter and CCC engine, none of which a device
	# uses. ultrawidelock_device (ultrawidelock_device_uwb.c) is the M1/M3 parser and M2/M4
	# builder, host-tested by tests/host/test_ultrawidelock_device_uwb.c against the real
	# reader codec; ultrawidelock_codec is the TLV builder and parser it is written on.
	# No radio here.
	src/ranging.c
	${UWB_DEVICE_SRCS}
	${UWB_CODEC_SRCS}
	# UWB initiator role, radio side. Again not the ultrawidelock_uwb module: its CCC
	# block is gated on CONFIG_ULTRAWIDELOCK_CRED, which `depends on ULTRAWIDELOCK_UWB_RESPONDER`
	# and would drag in the reader's session, adapter and RX engine. The CCC
	# key schedule and MAC (role ccc_keys) are role-neutral -- nothing in
	# ccc_kdf.c or ccc_mac.c knows which end it is on -- so the initiator links
	# the same translation units the reader does. crypto_prim is the thin
	# AES-ECB adapter over the provider already compiled above.
	src/prepoll_tx.c
	${UWB_CCC_KEYS_SRCS}
	${UWB_CRYPTO_SRCS}
)
# The app target also compiles selected module sources, so their private
# sibling headers remain on this target's private include path.
target_include_directories(app PRIVATE ${CRED}/include ${CRED}/src
	# The UWB codec's own headers, the adapter/cherry headers its message struct
	# is declared in, and the two platform facades it compiles against
	# (qmalloc -> k_malloc, LOG_MODULE_DECLARE).
	${UWB_INC} ${UWB}
	${REPO_ROOT}/modules/ultrawidelock_uwb/src/facade
	${REPO_ROOT}/modules/ultrawidelock_port/include
	# The CCC key schedule and MAC headers, plus src/driver for uwb_seam.h
	# (ultrawidelock_uwb_configure_phy). CONFIG_ULTRAWIDELOCK_UWB already puts src/driver on the
	# path for the module's own library, but this app's sources are compiled
	# into `app`, which does not inherit it.
	${CCC}
	${REPO_ROOT}/modules/ultrawidelock_uwb/src/driver)

# Enrolled or not, decided here rather than with __has_include in main.c.
# semgrep cannot parse __has_include, and a file it cannot parse is one that
# NONE of its rules run against -- which would silently drop SAST coverage of
# the translation unit that holds the credential key. Configure-time detection
# means a freshly written header needs a re-configure to be picked up; that is
# what scripts/ultrawidelock-enroll.py tells you to do when it finishes.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/bench_identity.h")
	message(STATUS "credential initiator: ENROLLED identity (src/bench_identity.h)")
	target_compile_definitions(app PRIVATE ULTRAWIDELOCK_BENCH_ENROLLED)
else()
	message(STATUS "credential initiator: DEV fallback identity, run scripts/ultrawidelock-enroll.py")
endif()
