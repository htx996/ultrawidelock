# Standalone Zephyr app: run the credential device (initiator) self-test on an
# nRF5340 DK against the real nrf_security/CryptoCell PSA P-256 backend
# (ultrawidelock_prim_psa.c). Console prints PASS/FAIL. No BLE/UWB/iPhone.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ultrawidelock_device_ec_test)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CRED "${REPO_ROOT}/modules/ultrawidelock_cred")
# The device self-test body is port-neutral and lives in tests/shared; the
# ESP32 host suite compiles the same file against the fake-EC prim double,
# while this app links it against the real CryptoCell P-256 backend.
set(TESTDIR "${REPO_ROOT}/tests/shared")

# Device-side shared sources by role manifest (modules/ultrawidelock_cred/roles/*.list)
# — the same lists the initiator app and the ESP32 components read.
include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
ultrawidelock_role_sources("${CRED}/roles/device.list" ULTRAWIDELOCK_DEVICE_SRCS)
ultrawidelock_role_sources("${CRED}/roles/apdu.list" ULTRAWIDELOCK_APDU_SRCS)
ultrawidelock_role_sources("${CRED}/roles/crypto.list" ULTRAWIDELOCK_CRYPTO_SRCS)
ultrawidelock_role_sources("${CRED}/roles/hash.list" ULTRAWIDELOCK_HASH_SRCS)
ultrawidelock_role_sources("${CRED}/roles/prim_provider.list" ULTRAWIDELOCK_PRIM_PROVIDER_SRCS)

target_sources(app PRIVATE
	src/main.c
	${TESTDIR}/test_ultrawidelock_device.c
	${ULTRAWIDELOCK_DEVICE_SRCS}
	${ULTRAWIDELOCK_APDU_SRCS}
	${ULTRAWIDELOCK_CRYPTO_SRCS}
	${ULTRAWIDELOCK_HASH_SRCS}
	${ULTRAWIDELOCK_PRIM_PROVIDER_SRCS}
)
target_include_directories(app PRIVATE ${CRED}/include ${CRED}/src)
target_compile_definitions(app PRIVATE ULTRAWIDELOCK_DEVICE_HAVE_EC)
