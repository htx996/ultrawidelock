# Two-anchor geometry, the WV3 wire codec and the sealed link's envelope as an
# ESP-IDF component. Additive: reuses modules/ultrawidelock_anchor unchanged.
# Source set = the role manifests in modules/ultrawidelock_anchor/roles/ — the
# SAME lists modules/ultrawidelock_anchor/CMakeLists.txt and tests/host/sources.sh
# read — so the ESP32 lock, the ESP32 satellite, the nRF pair and the host suite
# cannot disagree about what a tier contains. Nothing here is ESP32-specific.
#
# WHY ALL THREE TIERS IN ONE COMPONENT. A lock takes the geometry and the codec;
# a satellite takes the codec and the envelope and fuses nothing. Splitting that
# into two components would put the tier choice in two places (here and each
# app's REQUIRES) when the manifests already own it, and ESP-IDF links with
# --gc-sections, so the satellite image drops the fusion it never calls. The
# manifests stay the boundary; this component is just the delivery vehicle.
#
# The logic is pure integer C over caller-owned structs: no allocation, no
# threads, no platform. ultrawidelock_seal.c is the one file with a dependency —
# psa/crypto.h, the same seam ultrawidelock_prim_psa.c sits on, backed here by
# ESP-IDF's mbedTLS-PSA provider. That provider needs AES-CCM: MBEDTLS_PSA_CRYPTO_C
# comes from mbedtls_config.h and esp_config.h gates MBEDTLS_CCM_C / MBEDTLS_AES_C
# on CONFIG_MBEDTLS_CCM_C / CONFIG_MBEDTLS_AES_C, both default y. An image that
# turns either off fails to link this component rather than silently sending
# datagrams no peer can open.

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(ANCHOR "${REPO_ROOT}/modules/ultrawidelock_anchor")
set(ULTRAWIDELOCK_PORT "${REPO_ROOT}/modules/ultrawidelock_port/include")

include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
# geometry + fusion + side gate + satellite pairing (the lock's half)
ultrawidelock_role_sources("${ANCHOR}/roles/anchor.list" ANCHOR_SRCS)
# the WV2/WV3/WV4 wire codec, on both ends of the link
ultrawidelock_role_sources("${ANCHOR}/roles/anchor_msg.list" ANCHOR_MSG_SRCS)
# the AES-CCM envelope, shared with the nRF pair so a mixed bench stays possible
ultrawidelock_role_sources("${ANCHOR}/roles/seal.list" ANCHOR_SEAL_SRCS)
# the link's decisions (compose, authenticate, replay-check) with no carrier in
# them. NOT taken by the Zephyr module: those apps still carry their own copies
# inline, and compiling a second unused implementation into images that have
# committed size baselines would cost flash for nothing.
ultrawidelock_role_sources("${ANCHOR}/roles/link.list" ANCHOR_LINK_SRCS)

idf_component_register(
	SRCS
		${ANCHOR_SRCS}
		${ANCHOR_MSG_SRCS}
		${ANCHOR_SEAL_SRCS}
		${ANCHOR_LINK_SRCS}
	INCLUDE_DIRS
		"${ANCHOR}/include"      # fusion, satellite, side, the codec and the seal
		"${ULTRAWIDELOCK_PORT}"  # ultrawidelock_port.h / ultrawidelock_log.h
	REQUIRES
		# ultrawidelock_port carries the ESP-IDF backend of the platform contract.
		# Named rather than left to the include path above: ESP-IDF only builds a
		# component something requires, so without this the backend is discovered
		# and then silently skipped.
		ultrawidelock_port
		mbedtls
)

# Kconfig symbols the in-tree build gates on, supplied as compile defs. The
# geometry reads none of these — it is arithmetic over caller-owned structs —
# but ultrawidelock_witness_core.c sizes its table from ULTRAWIDELOCK_WITNESS_MAX,
# and that file is witness-only and NOT in any tier above, so nothing here needs
# it. The tier gates are what the sources' own headers check.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
	CONFIG_ULTRAWIDELOCK_ANCHOR=1
	CONFIG_ULTRAWIDELOCK_ANCHOR_MSG=1
	CONFIG_ULTRAWIDELOCK_SEALED_LINK=1
)
