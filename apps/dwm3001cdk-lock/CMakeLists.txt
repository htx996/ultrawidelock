# DWM3001CDK standalone credential lock: the reader engine, the CCC/FiRa responder,
# the DW3110 driver and the BLE stack in one nRF52833 image.
#
# ultrawidelock_uwb and modules/ultrawidelock_dw3000 come in as Zephyr modules because they carry their own
# Kconfig gating. modules/ultrawidelock_cred's reader core is listed
# directly here, the way tests/on_target/zephyr/nrf5340dk-ultrawidelock-device-ec does, so the nRF5340 build
# that consumes that module through CONFIG_ULTRAWIDELOCK_CRED_STACK stays untouched.
cmake_minimum_required(VERSION 3.20.0)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# modules/ultrawidelock_cred is listed for its Kconfig alone: the reader sources use the
# LOG_MODULE the `module = ULTRAWIDELOCK_CRED` template declares, so CONFIG_ULTRAWIDELOCK_CRED_LOG_LEVEL
# has to exist. Its CMakeLists is gated on CONFIG_ULTRAWIDELOCK_CRED_STACK, which stays n
# here, so it contributes no sources and the nRF5340 build is unaffected.
# modules/ultrawidelock_anchor is listed on the same terms: its CMakeLists is gated on
# CONFIG_ULTRAWIDELOCK_ANCHOR, which is default n, so listing it here costs the default
# build nothing and only makes `overlay-anchor.conf` able to turn it on.
# modules/ultrawidelock_ml likewise, gated on CONFIG_ULTRAWIDELOCK_ML_LOS: default n, so the shipping
# image is byte-identical without it and only overlays/mlgate.conf turns it on.
list(APPEND ZEPHYR_EXTRA_MODULES
	"${REPO_ROOT}/modules/ultrawidelock_cred"
	"${REPO_ROOT}/modules/ultrawidelock_anchor"
	"${REPO_ROOT}/modules/ultrawidelock_matter"
	"${REPO_ROOT}/modules/ultrawidelock_ml"
	"${REPO_ROOT}/modules/ultrawidelock_uwb"
	"${REPO_ROOT}/modules/ultrawidelock_dw3000"
	"${REPO_ROOT}/ports/zephyr")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ultrawidelock_reader_dwm3001cdk)

# APPROTECT must never be locked on this board. SystemInit() re-asserts the lock
# on every boot and the only way out is `nrfjprog --recover`, which mass-erases
# flash AND UICR -- taking settings_storage at 0x7c000 with it, and with that the
# reader private key and every iPhone key already provisioned against this board.
#
# This is the configure-time half of the guard: it fails the build of THIS image
# before a hex exists. scripts/check-approtect.sh is the other half and is the
# one that covers MCUboot, whose config lives outside this CMakeLists entirely.
# Both are needed -- the setting can arrive from a board defconfig or an SoC
# default that no grep of this tree would find, which is why the check that
# matters reads the GENERATED .config.
if(CONFIG_NRF_APPROTECT_LOCK OR CONFIG_NRF_SECURE_APPROTECT_LOCK)
	message(FATAL_ERROR
		"APPROTECT lock is enabled for this image. Refusing to build.\n"
		"  CONFIG_NRF_APPROTECT_LOCK        = ${CONFIG_NRF_APPROTECT_LOCK}\n"
		"  CONFIG_NRF_SECURE_APPROTECT_LOCK = ${CONFIG_NRF_SECURE_APPROTECT_LOCK}\n"
		"Locking is a one-way door for this board's DATA: recovering debug "
		"access costs a mass erase of flash and UICR, which destroys "
		"settings_storage and the reader identity. Export it first with "
		"`ultrawidelock export yes` if you genuinely intend this.")
endif()

set(CRED "${REPO_ROOT}/modules/ultrawidelock_cred")
set(PORTZ "${REPO_ROOT}/ports/zephyr")

# Reader-role source sets come from the role manifests (modules/ultrawidelock_cred/
# roles/*.list) — the same lists the ESP32 components, the nRF5340 apps and the
# host suite read, so each role's file set exists in exactly one place.
include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
ultrawidelock_role_sources("${CRED}/roles/reader.list" ULTRAWIDELOCK_READER_SRCS)
ultrawidelock_role_sources("${CRED}/roles/reader_policy.list" ULTRAWIDELOCK_POLICY_SRCS)
ultrawidelock_role_sources("${CRED}/roles/apdu.list" ULTRAWIDELOCK_APDU_SRCS)
ultrawidelock_role_sources("${CRED}/roles/crypto.list" ULTRAWIDELOCK_CRYPTO_SRCS)
ultrawidelock_role_sources("${CRED}/roles/hash.list" ULTRAWIDELOCK_HASH_SRCS)
ultrawidelock_role_sources("${CRED}/roles/dyntag.list" ULTRAWIDELOCK_DYNTAG_SRCS)
ultrawidelock_role_sources("${CRED}/roles/ble_central.list" ULTRAWIDELOCK_BLE_CENTRAL_SRCS)

target_sources(app PRIVATE
	src/main.c
	src/ml_feed.c
	# Platform seams the reader engine links against, re-homed to ports/zephyr
	# with the rest of the Zephyr glue. Listed by path rather than compiled by
	# the ultrawidelock_zephyr_port module because they are unconditional app glue with
	# no Kconfig gate to ride — this app IS the gate.
	${PORTZ}/ble/ultrawidelock_ble_zephyr.c
	${PORTZ}/store/ultrawidelock_prov_settings.c
	# Reader engine: roles reader + reader_policy + apdu + crypto + hash + dyntag.
	${ULTRAWIDELOCK_READER_SRCS}
	${ULTRAWIDELOCK_POLICY_SRCS}
	${ULTRAWIDELOCK_APDU_SRCS}
	${ULTRAWIDELOCK_CRYPTO_SRCS}
	${ULTRAWIDELOCK_HASH_SRCS}
	${ULTRAWIDELOCK_DYNTAG_SRCS}
)
# Provisioning console (USB CDC-ACM, provisioning mode only).
target_sources_ifdef(CONFIG_ULTRAWIDELOCK_SIDE_GATE app PRIVATE src/side_feed.c)
# Measurement only; CONFIG_ULTRAWIDELOCK_THREAD_GATE defaults n so this is absent from
# every shipping image.
target_sources_ifdef(CONFIG_ULTRAWIDELOCK_THREAD_GATE app PRIVATE src/thread_gate.c)
# The DFU receiver's BLE transport (ports/zephyr/dfu/dfu_ble_zephyr.c) and the
# rest of the gated Zephyr glue compile via the ultrawidelock_zephyr_port module under
# the same CONFIG symbols.
# The board's only output. NOT gated on the DFU receiver any more: the update
# window was the first thing worth showing, not the only one, and the `reader`
# image has no receiver and used to light nothing at all.
target_sources_ifdef(CONFIG_ULTRAWIDELOCK_STATUS_LED app PRIVATE src/status_led.c)
target_sources_ifdef(CONFIG_ULTRAWIDELOCK_CIRDIAG_CAPTURE app PRIVATE src/cirdiag_capture.c)
target_sources_ifdef(CONFIG_ULTRAWIDELOCK_CASE_BENCH app PRIVATE src/case_bench.c)
# Initiator (User-Device role) transport: ultrawidelock_zephyr_port compiles the central
# mirror of ultrawidelock_ble_zephyr.c under this gate; the platform-free advert/READ
# decoder it calls is the same translation unit the ESP32 port and the host
# suite compile.
target_sources_ifdef(CONFIG_ULTRAWIDELOCK_BLE_CENTRAL app PRIVATE ${ULTRAWIDELOCK_BLE_CENTRAL_SRCS})
# The commissioning policy half; its transport, Thread/fabric ports and fabric
# table store live in ports/zephyr/{matter,store} under the same gate.
target_sources_ifdef(CONFIG_ULTRAWIDELOCK_MATTER_BLE app PRIVATE src/matter_commission.c)

# CONFIG_MBEDTLS_MEMORY_DEBUG=y is a NO-OP in this build and sets no define.
# nrf_security only wires that symbol through configs/legacy_crypto_config.h.template
# (`#cmakedefine MBEDTLS_MEMORY_DEBUG`, line 1268); a PSA-only build
# (CONFIG_OBERON_ONLY_PSA_ENABLED) generates nrf-config.h from a different
# template that has no slot for it, so the Kconfig is accepted and silently
# dropped. The app then compiles against a declaration the library never
# defined and the link fails on mbedtls_memory_buffer_alloc_max_get.
#
# Define it for every target instead. Safe because the only struct it changes,
# buffer_alloc_ctx, is file-local to mbedtls/library/memory_buffer_alloc.c, so
# there is no layout to get out of step across compilation units.
if(CONFIG_ULTRAWIDELOCK_HEAP_PROBE)
	zephyr_compile_definitions(MBEDTLS_MEMORY_DEBUG)
endif()
# src for matter_commission.h (included by the re-homed ultrawidelock_ble_zephyr.c);
# ${PORTZ}/{matter,store} for the re-homed matter_ble_zephyr.h and
# matter_fab_settings.h, which main.c and matter_commission.c include
# regardless of CONFIG_ULTRAWIDELOCK_MATTER_BLE.
target_include_directories(app PRIVATE ${CRED}/include ${CRED}/src src
	${PORTZ}/matter ${PORTZ}/store)
