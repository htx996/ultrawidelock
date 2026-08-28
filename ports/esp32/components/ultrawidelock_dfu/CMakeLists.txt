# Over-the-air update as an ESP-IDF component.
#
# The receiver, the CRC and the wire protocol are modules/ultrawidelock_dfu,
# compiled here verbatim -- the same file the DWM3001CDK and the standalone
# FreeRTOS port build, and the same file 7,546 host checks drive. What this
# component adds is only what names ESP-IDF: the otadata commit hook
# (dfu_esp32.c), the NimBLE GATT service (dfu_ble_esp32.c), and the
# ultrawidelock_flash.h backend, which lives with the other ESP-IDF backends in
# the ultrawidelock_port component.
#
# The applier is NOT here and must not be. It is the detools half that runs
# inside MCUboot on the CDK; this port has two whole slots and applies nothing.
#
# Contributes nothing unless CONFIG_ULTRAWIDELOCK_DFU_ESP32=y, so an image that
# does not ask for over-the-air updates stays byte-identical.

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(DFU "${REPO_ROOT}/modules/ultrawidelock_dfu")

# WHY THE REGISTRATION BELOW IS UNCONDITIONAL AND ONLY THE SOURCES ARE GATED.
#
# ESP-IDF reads every component's CMakeLists TWICE. The first pass builds the
# dependency graph and CONFIG_* is not defined yet; the second is the real
# configure, with sdkconfig loaded. Wrapping idf_component_register() in
# `if(CONFIG_...)` therefore registers an EMPTY component during the pass that
# decides include paths -- the sources then compile in the second pass with none
# of REQUIRES' headers on -I, and the failure reads as "esp_ota_ops.h: No such
# file or directory" on a component that plainly requires app_update.
#
# So: register once, always, with the full REQUIRES, and let the source list be
# the thing that empties out. An image with the feature off still links nothing,
# because there is nothing to link.
set(dfu_srcs "")

if(CONFIG_ULTRAWIDELOCK_DFU_ESP32)

# The signing key. Same contract as every other port: the PRIVATE key never
# enters the build output or the tree, and the public half is derived at
# configure time so the key that signs an image and the key that authorises an
# update cannot drift apart. SIGN_KEY is what mk/esp32.mk and mk/cdk.mk both
# export.
if(NOT ULTRAWIDELOCK_DFU_KEY AND DEFINED ENV{SIGN_KEY})
    set(ULTRAWIDELOCK_DFU_KEY "$ENV{SIGN_KEY}")
endif()
if(NOT ULTRAWIDELOCK_DFU_KEY OR NOT EXISTS "${ULTRAWIDELOCK_DFU_KEY}")
    message(FATAL_ERROR
        "the update-signing key is missing: '${ULTRAWIDELOCK_DFU_KEY}'\n"
        "  CONFIG_ULTRAWIDELOCK_DFU_ESP32=y means this image verifies a signature "
        "before it writes anyone's firmware to flash, and the public half of that "
        "key is compiled in.\n"
        "  Run `make dfu-key` once per clone, or pass SIGN_KEY=/path/to/key.pem.")
endif()

set(PUBKEY_C "${CMAKE_CURRENT_BINARY_DIR}/ultrawidelock_dfu_pubkey.c")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
    OUTPUT "${PUBKEY_C}"
    COMMAND "${Python3_EXECUTABLE}" "${DFU}/scripts/emit_pubkey.py"
            "${ULTRAWIDELOCK_DFU_KEY}" "${PUBKEY_C}"
    DEPENDS "${ULTRAWIDELOCK_DFU_KEY}" "${DFU}/scripts/emit_pubkey.py"
    COMMENT "ultrawidelock_dfu: extracting the update-signing public key")

set(dfu_srcs
    "dfu_esp32.c"
    "dfu_ble_esp32.c"
    "${DFU}/src/dfu_receiver.c"
    "${DFU}/src/dfu_crc.c"
    "${PUBKEY_C}")

endif() # CONFIG_ULTRAWIDELOCK_DFU_ESP32

idf_component_register(
    SRCS ${dfu_srcs}
    INCLUDE_DIRS "include" "${DFU}/include"
    # app_update for esp_ota_ops, bt for NimBLE, esp_partition for the flash
    # backend's peer in ultrawidelock_port.
    REQUIRES app_update bt esp_partition
    # ultrawidelock_crypto provides ultrawidelock_ecdsa_p256_verify over PSA;
    # ultrawidelock_port provides the OSAL, the log contract and flash_esp.c.
    PRIV_REQUIRES ultrawidelock_crypto ultrawidelock_port)

# The receiver logs and locks through modules/ultrawidelock_port's contract, and
# verifies through modules/ultrawidelock_cred's, not through esp_log or mbedtls
# directly. That indirection is what lets one .c file serve three operating
# systems.
#
# Guarded: with no sources, idf_component_register() produces an INTERFACE
# library, and PRIVATE is not a thing an INTERFACE library has.
if(dfu_srcs)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${REPO_ROOT}/modules/ultrawidelock_port/include"
        "${REPO_ROOT}/modules/ultrawidelock_cred/include"
        "${DFU}/src")
endif()
