# UltraWideLock credential-auth stack and primitive provider as a Zephyr module.
# The credential library contributes nothing unless
# CONFIG_ULTRAWIDELOCK_CRED_STACK=y. CONFIG_ULTRAWIDELOCK_PRIM independently
# contributes the image's one explicitly linked provider. The same src/ is
# compiled by the ESP-IDF ports through
# ports/esp32/components/ultrawidelock_{crypto,reader}/CMakeLists.txt.
#
# Every source here is platform-agnostic: the k_*/LOG_* calls are Zephyr's own
# API, which the ESP-IDF side backs with the ultrawidelock_uwb compat headers. What is NOT
# here is the BLE transport (ultrawidelock_ble.h has no Zephyr backend yet) and the
# ultrawidelock_prov storage backend, so linking this alone does not yield a reader.

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(ROLES ${CMAKE_CURRENT_SOURCE_DIR}/roles)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ultrawidelock_roles.cmake)

# One provider target per Zephyr image. It deliberately lives outside the
# credential-stack gate: sealed links, CCC ranging and DFU signature checks use
# the same primitive contract without necessarily building the credential
# protocol. Consumers link this target explicitly.
if(CONFIG_ULTRAWIDELOCK_PRIM)

  ultrawidelock_role_sources(${ROLES}/prim_provider.list PRIM_PROVIDER_SRCS)

  zephyr_library_named(ultrawidelock_prim)
  zephyr_library_sources(${PRIM_PROVIDER_SRCS})
  zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

endif()

if(CONFIG_ULTRAWIDELOCK_CRED_STACK)

  ultrawidelock_role_sources(${ROLES}/crypto.list CRYPTO_SRCS)

  zephyr_library_named(ultrawidelock_cred)

  # Credential composition uses the image's one primitive provider. The
  # provider itself is compiled above, from its own role manifest.
  zephyr_library_sources(
    ${SRC}/ultrawidelock_hash.c
    ${CRYPTO_SRCS}
    ${SRC}/ultrawidelock_advtag.c
    # BER-TLV / ISO7816 / BLE-envelope wire codec.
    ${SRC}/ultrawidelock_apdu.c
    # Reader identity + raw-key trust store (storage backend is per-platform).
    ${SRC}/ultrawidelock_prov.c
  )

  # NOT listed yet: src/ultrawidelock_reader.c and src/ultrawidelock_ranging.c. They no longer
  # contain any ESP-IDF call (both are now written to Zephyr's k_*/LOG_* API),
  # but they have never been compiled by a Zephyr toolchain, and ultrawidelock_ranging.c
  # additionally needs modules/ultrawidelock_uwb's facade/cherry/adapter headers on the
  # include path. Add them here in the same change that first builds them.

  zephyr_library_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SRC}
  )
  zephyr_library_link_libraries(ultrawidelock_prim)

  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

endif()
