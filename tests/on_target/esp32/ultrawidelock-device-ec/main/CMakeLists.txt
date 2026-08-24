# The device self-test + the shared credential-auth core, built with the real
# PSA backend. ULTRAWIDELOCK_DEVICE_HAVE_EC turns on the full ECDH+ECDSA loopback.
# The test body is port-neutral (tests/shared) and the sources are the same
# role manifests the nRF twin of this app reads
# (tests/on_target/zephyr/nrf5340dk-ultrawidelock-device-ec/CMakeLists.txt).
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(CRED "${REPO_ROOT}/modules/ultrawidelock_cred")
set(SHARED "${REPO_ROOT}/tests/shared")

include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
ultrawidelock_role_sources("${CRED}/roles/device.list" ULTRAWIDELOCK_DEVICE_SRCS)
ultrawidelock_role_sources("${CRED}/roles/apdu.list" ULTRAWIDELOCK_APDU_SRCS)
ultrawidelock_role_sources("${CRED}/roles/crypto.list" ULTRAWIDELOCK_CRYPTO_SRCS)
ultrawidelock_role_sources("${CRED}/roles/hash.list" ULTRAWIDELOCK_HASH_SRCS)
ultrawidelock_role_sources("${CRED}/roles/prim_provider.list" ULTRAWIDELOCK_PRIM_PROVIDER_SRCS)

idf_component_register(
	SRCS
		"app_main.c"
		"${SHARED}/test_ultrawidelock_device.c"
		${ULTRAWIDELOCK_DEVICE_SRCS}
		${ULTRAWIDELOCK_APDU_SRCS}
		${ULTRAWIDELOCK_CRYPTO_SRCS}
		${ULTRAWIDELOCK_HASH_SRCS}
		${ULTRAWIDELOCK_PRIM_PROVIDER_SRCS}
	INCLUDE_DIRS "."
	PRIV_INCLUDE_DIRS "${CRED}/include" "${CRED}/src"
	REQUIRES mbedtls
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ULTRAWIDELOCK_DEVICE_HAVE_EC)
