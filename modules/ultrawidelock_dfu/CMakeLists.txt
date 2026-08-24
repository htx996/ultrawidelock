# Delta update, bootloader half. Contributes nothing unless
# CONFIG_ULTRAWIDELOCK_DFU_APPLIER=y, so every image that does not ask for it -- which is
# every image except MCUboot on the DWM3001CDK -- stays byte-identical.
#
# This module is on the module path of ALL images, not just the application.
# That is how a Zephyr module reaches the bootloader at all: sysbuild builds
# MCUboot as an ordinary Zephyr application, so it loads the same modules, and
# NCS injects its own MCUboot hooks exactly this way
# (nrf/modules/mcuboot/hooks/CMakeLists.txt gates on CONFIG_BOOT_IMAGE_ACCESS_HOOKS).
# The Kconfig gate above is therefore the only thing keeping this out of the
# application image.

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ULTRAWIDELOCK_PORT ${REPO_ROOT}/modules/ultrawidelock_port)
set(ULTRAWIDELOCK_CRED ${REPO_ROOT}/modules/ultrawidelock_cred)
# The Zephyr flash backend lives with the other Zephyr backends, in the port
# tree. It is pulled in BY PATH rather than by depending on that module,
# because the applier half below is built into MCUboot, and ports/zephyr is
# not on the bootloader's module path.
set(ULTRAWIDELOCK_PORT_ZEPHYR ${REPO_ROOT}/ports/zephyr/osal)

if(CONFIG_ULTRAWIDELOCK_DFU_APPLIER)

  set(DETOOLS ${REPO_ROOT}/modules/ultrawidelock_dfu/src/detools)

  zephyr_library_named(ultrawidelock_dfu)
  zephyr_library_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dfu_applier.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dfu_crc.c
    ${ULTRAWIDELOCK_PORT_ZEPHYR}/flash_zephyr.c
    ${DETOOLS}/detools.c
    ${DETOOLS}/heatshrink/heatshrink_decoder.c)

  zephyr_library_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ULTRAWIDELOCK_PORT}/include
    ${DETOOLS}
    ${DETOOLS}/heatshrink)

  # LZMA is the expensive decompressor and nothing here creates lzma patches;
  # dropping it is most of why this fits in the bootloader at all. `none` is
  # kept because it costs almost nothing and makes an uncompressed patch
  # debuggable on the bench. FILE_IO would pull in stdio.
  zephyr_library_compile_definitions(
    DETOOLS_CONFIG_FILE_IO=0
    DETOOLS_CONFIG_COMPRESSION_LZMA=0
    DETOOLS_CONFIG_COMPRESSION_CRLE=0
    DETOOLS_CONFIG_COMPRESSION_NONE=1
    DETOOLS_CONFIG_COMPRESSION_HEATSHRINK=1)

  # Vendored third-party C, not held to this tree's warning level.
  set_source_files_properties(
    ${DETOOLS}/detools.c
    ${DETOOLS}/heatshrink/heatshrink_decoder.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter -Wno-sign-compare")

endif()

# The application half. Never both in one image: the applier belongs to the
# bootloader and the receiver to the application, and each is inert without the
# other.
if(CONFIG_ULTRAWIDELOCK_DFU_RECEIVER)

  # The update-signing key is the IMAGE-signing key, so there is one secret to
  # hold and no way for the two to drift. Passed from the sysbuild command line
  # (mk/cdk.mk) because SB_CONFIG_BOOT_SIGNATURE_KEY_FILE is evaluated a level
  # up and is not visible to an image's CMake.
  # zephyr_get(), not a plain read. A -D on the west command line lands in the
  # SYSBUILD cache, and an image's CMake only sees that through this accessor --
  # which is exactly how EXTRA_ZEPHYR_MODULES reaches the bootloader
  # (zephyr/cmake/modules/zephyr_module.cmake:39). Reading the bare variable
  # finds nothing and reports a missing key that was in fact passed.
  zephyr_get(ULTRAWIDELOCK_DFU_KEY)

  if(NOT ULTRAWIDELOCK_DFU_KEY)
    message(FATAL_ERROR
      "CONFIG_ULTRAWIDELOCK_DFU_RECEIVER is set but ULTRAWIDELOCK_DFU_KEY is not.\n"
      "  The receiver verifies a staged update against the public half of the\n"
      "  image-signing key, so it cannot be built without knowing which key.\n"
      "  Pass -DULTRAWIDELOCK_DFU_KEY=<absolute path to the signing key>.")
  endif()
  if(NOT EXISTS "${ULTRAWIDELOCK_DFU_KEY}")
    message(FATAL_ERROR "ULTRAWIDELOCK_DFU_KEY does not exist: ${ULTRAWIDELOCK_DFU_KEY}\n  Fix: make dfu-key")
  endif()

  set(ultrawidelock_dfu_pubkey_c ${CMAKE_CURRENT_BINARY_DIR}/ultrawidelock_dfu_pubkey.c)
  add_custom_command(
    OUTPUT ${ultrawidelock_dfu_pubkey_c}
    COMMAND ${PYTHON_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/emit_pubkey.py
            "${ULTRAWIDELOCK_DFU_KEY}" ${ultrawidelock_dfu_pubkey_c}
    DEPENDS "${ULTRAWIDELOCK_DFU_KEY}" ${CMAKE_CURRENT_SOURCE_DIR}/scripts/emit_pubkey.py
    COMMENT "ultrawidelock_dfu: extracting the update-signing public key")

  zephyr_library_named(ultrawidelock_dfu)
  zephyr_library_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dfu_receiver.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dfu_crc.c
    ${ULTRAWIDELOCK_PORT_ZEPHYR}/flash_zephyr.c
    ${ultrawidelock_dfu_pubkey_c})
  # The ultrawidelock_osal.h backend this receiver also needs is NOT built here: one
  # image, one definition, and ports/zephyr owns it for every Zephyr image
  # (CONFIG_ULTRAWIDELOCK_DFU_RECEIVER is in that library's gate). The flash backend is
  # named by path above instead, for the reason given at ULTRAWIDELOCK_PORT_ZEPHYR.
  zephyr_library_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ULTRAWIDELOCK_CRED}/include
    ${ULTRAWIDELOCK_PORT}/include)
  # Signature verification reaches PSA only through the image's primitive
  # provider. Linking the named target keeps the backend TU owned in one place.
  zephyr_library_link_libraries(ultrawidelock_prim)

  # The optional SMP image-group front door onto this receiver
  # (CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG) is Zephyr mcumgr glue and compiles from
  # ports/zephyr (ultrawidelock_zephyr_port); see the Kconfig help for why Zephyr's own
  # img_mgmt cannot be used here.

  # zephyr_include_directories, not the _library_ form: the transport lives in
  # the application (ports/zephyr/dfu/dfu_ble_zephyr.c) and has to include
  # ultrawidelock_dfu_rx.h. The _library_ form scopes the path to this module's own
  # library only, and the app then fails to find the header.
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

endif()
