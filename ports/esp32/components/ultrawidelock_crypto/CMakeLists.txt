# Credential key schedule plus the image's primitive provider as an ESP-IDF
# component.
# The sources come from the role manifests in modules/ultrawidelock_cred/roles/ (the
# same lists the nRF builds and the host suite read); nothing here is
# ESP32-specific except the mbedTLS-PSA provider that backs ultrawidelock_prim_psa.c.

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(CRED "${REPO_ROOT}/modules/ultrawidelock_cred")

include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
ultrawidelock_role_sources("${CRED}/roles/hash.list" ULTRAWIDELOCK_HASH_SRCS)
ultrawidelock_role_sources("${CRED}/roles/crypto.list" ULTRAWIDELOCK_CRYPTO_SRCS)
ultrawidelock_role_sources("${CRED}/roles/dyntag.list" ULTRAWIDELOCK_DYNTAG_SRCS)
ultrawidelock_role_sources("${CRED}/roles/prim_provider.list" ULTRAWIDELOCK_PRIM_PROVIDER_SRCS)

idf_component_register(
	SRCS
		${ULTRAWIDELOCK_HASH_SRCS}
		${ULTRAWIDELOCK_CRYPTO_SRCS}
		${ULTRAWIDELOCK_DYNTAG_SRCS}
		${ULTRAWIDELOCK_PRIM_PROVIDER_SRCS}
	INCLUDE_DIRS "${CRED}/include"
	PRIV_INCLUDE_DIRS "${CRED}/src"
	REQUIRES mbedtls
)
