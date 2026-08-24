# The nRF52833 FreeRTOS target build.
#
# Three vendor trees supply this image and none of them is copied into the
# repository, so each is located by a cache variable and pinned by
# platform.lock.yml:
#
#   ULTRAWIDELOCK_QORVO_SDK_DIR  the extracted DW3/QM33 SDK 1.1.1 -- FreeRTOS kernel,
#                      board support, and the DW3000 driver
#   ULTRAWIDELOCK_NCS_WORKSPACE  the west workspace -- the hal_nordic nrfx and MDK the
#                      radio stack is built against, CMSIS, nrfxlib, and the
#                      pinned NCS sources
#   ULTRAWIDELOCK_NIMBLE_DIR     the mynewt-nimble checkout -- the BLE host
#
# The build is assembled in layers and each layer is only added once the one
# below it links, because a first link that reports its section sizes is worth
# more here than a complete graph that does not.

cmake_minimum_required(VERSION 3.20)

project(ultrawidelock_freertos_nrf52833 C ASM)

# Cached rather than a plain variable so ultrawidelock_freertos_image() still resolves it
# when the application calls it from the parent directory scope.
set(ULTRAWIDELOCK_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "FreeRTOS nRF52833 port directory")

foreach(_ultrawidelock_dir ULTRAWIDELOCK_QORVO_SDK_DIR ULTRAWIDELOCK_NCS_WORKSPACE)
  if(NOT ${_ultrawidelock_dir})
    message(FATAL_ERROR "${_ultrawidelock_dir} is not set; see ports/freertos-nrf52833/README.md")
  endif()
  if(NOT IS_DIRECTORY "${${_ultrawidelock_dir}}")
    message(FATAL_ERROR "${_ultrawidelock_dir} is not a directory: ${${_ultrawidelock_dir}}")
  endif()
endforeach()

set(ULTRAWIDELOCK_NRF5_SDK "${ULTRAWIDELOCK_QORVO_SDK_DIR}/SDK_BSP/Nordic/SDK_17_1_0")
set(ULTRAWIDELOCK_FREERTOS_SRC "${ULTRAWIDELOCK_NRF5_SDK}/external/freertos")
set(ULTRAWIDELOCK_NRFX "${ULTRAWIDELOCK_NCS_WORKSPACE}/modules/hal/nordic/nrfx")
set(ULTRAWIDELOCK_BSP "${ULTRAWIDELOCK_NRFX}/bsp/stable")
set(ULTRAWIDELOCK_MDK "${ULTRAWIDELOCK_BSP}/mdk")
get_filename_component(ULTRAWIDELOCK_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(ULTRAWIDELOCK_MODULES_DIR "${ULTRAWIDELOCK_REPO_ROOT}/modules")
set(ULTRAWIDELOCK_CMSIS "${ULTRAWIDELOCK_NCS_WORKSPACE}/modules/hal/cmsis/CMSIS/Core/Include")
include("${ULTRAWIDELOCK_REPO_ROOT}/cmake/ultrawidelock_roles.cmake")

foreach(_ultrawidelock_path "${ULTRAWIDELOCK_FREERTOS_SRC}/source/tasks.c" "${ULTRAWIDELOCK_MDK}/nrf52833.h"
                  "${ULTRAWIDELOCK_CMSIS}/core_cm4.h")
  if(NOT EXISTS "${_ultrawidelock_path}")
    message(FATAL_ERROR "expected vendor file is missing: ${_ultrawidelock_path}")
  endif()
endforeach()

# ----------------------------------------------------------------------------
# Which product this image is, declared before anything is built.
# ----------------------------------------------------------------------------
# Position is for readers, NOT for correctness, and the difference is worth
# stating because the two blocks below look like this one and do not behave like
# it.
#
#   add_compile_definitions  sets a directory property that CMake reads when it
#                            GENERATES. Every target in this directory gets it,
#                            including targets created above the call.
#   add_compile_options      is captured when each target is CREATED. Targets
#                            above the call never see it.
#
# Verified with a two-target scratch project rather than reasoned about: a
# library created before both calls received the definition and did not receive
# the option. So this could sit where it used to, 830 lines down beside the
# OpenThread subdirectory whose config reads it, and ultrawidelock_cred would still have
# compiled with ULTRAWIDELOCK_HAVE_MATTER defined. It is up here because a symbol that
# says which product the image is belongs before anything is built, not because
# moving it fixed anything -- it fixed nothing, and an earlier version of this
# comment claimed otherwise.
#
# The app gets the same symbol a second way, through ultrawidelock_freertos_port's
# INTERFACE definitions further down. That one IS load-bearing: the app is a
# different directory, and no directory property here reaches it.
#
# OpenThread still constrains this. Its subdirectory is added below and its
# config reads the symbol, so the definition has to exist by then -- which any
# position in this file satisfies, and a target-scoped definition would not.
if(ULTRAWIDELOCK_MATTER)
  add_compile_definitions(ULTRAWIDELOCK_HAVE_MATTER=1)
endif()

# ----------------------------------------------------------------------------
# Code generation applied to the WHOLE program.
# ----------------------------------------------------------------------------
# Same placement argument as the LTO block below, and the same defect behind it:
# these four flags were a hand-maintained nine-target foreach 400 lines down,
# which left out ultrawidelock_thread, ultrawidelock_matter,
# ultrawidelock_matter_transport, ultrawidelock_dfu, ultrawidelock_usb
# and the OpenThread subdirectory -- libopenthread-mtd.a being the largest
# archive in the image. Without -ffunction-sections there, --gc-sections can
# only drop whole objects, never individual functions.
#
#   -ffunction-sections   one section per function, so the linker can drop them
#   -fdata-sections       the same for objects
#   -fno-common           tentative definitions become real ones, so two
#                         translation units defining the same name is an error
#                         rather than a silent merge
#   -fno-strict-aliasing  the vendored radio and kernel sources punch through
#                         types in ways the optimiser is otherwise entitled to
#                         assume cannot happen
#
# WHAT HOISTING THEM WAS WORTH, measured on the Matter image: NOTHING. Byte for
# byte identical at 373,288 flash before and after, because with ULTRAWIDELOCK_LTO=ON the
# IR-level dead-code elimination has already dropped everything --gc-sections
# would have. The change is kept because it removes the second hand-maintained
# list from this file and because it is the ULTRAWIDELOCK_LTO=OFF build that needs it --
# but that build has NOT been measured, so this is a correctness argument and
# not a size claim. Do not quote a saving for it.
add_compile_options(
  -ffunction-sections
  -fdata-sections
  -fno-common
  -fno-strict-aliasing
  # Without this, configASSERT and the vendor asserts bake absolute source
  # paths into .rodata: 12 strings carrying the developer's username and
  # machine layout in every shipped image, and a binary that differs by
  # checkout location. Maps both __FILE__ and debug info.
  "-ffile-prefix-map=${ULTRAWIDELOCK_REPO_ROOT}=."
  "-fmacro-prefix-map=${ULTRAWIDELOCK_REPO_ROOT}=."
)

# ----------------------------------------------------------------------------
# Link-time optimisation, applied to the WHOLE program.
# ----------------------------------------------------------------------------
# This block sits here, ahead of every add_library() and every
# add_subdirectory(), because add_compile_options() is directory-scoped and
# applies only to targets created after it. Declaring the option further down --
# which is where it used to live -- silently limited LTO to the handful of
# targets that happened to be defined below it, and that is the defect this
# placement fixes rather than a style preference. See the measurement below.
#
# WHY WHOLE-PROGRAM AND NOT A LIST. The list was the bug. An earlier form of
# this named ten ultrawidelock_* targets, which read like a considered subset and was in
# fact an accident of file order: OpenThread, tcplp, the Matter node and the
# Matter transport were never in it. MEASURED from the two linker map files,
# Matter configuration, 2026-08-14:
#
#   Zephyr oracle       313,572 B of flash inside the LTO set,  74.3%
#   this port           174,143 B,                              36.6%
#
# The Zephyr image's entire non-LTO remainder is prebuilt binary: the
# SoftDevice Controller, MPSL, nrf_oberon, the 802.15.4 SL and libc. It
# optimises everything it has source for, and so should this. The single
# largest line in the difference was libopenthread-mtd.a at 102,396 B compiled
# per-file here and inside the oracle's LTO set there.
#
# WHAT THE FIX WAS WORTH, measured on the linked images, 2026-08-14, text for
# flash and data+bss for RAM:
#
#   Matter, console off, log level 2   446,852 -> 390,924 flash   -55,928
#                                      129,576 -> 123,076 RAM      -6,500
#   default, no Matter                 417,348 -> 348,636 flash   -68,712
#                                      112,196 -> 105,680 RAM      -6,516
#
# The Matter image had been 13,188 B OVER the 433,664 B slot and is now 42,740 B
# inside it. A product decision -- drop UWB from the Matter image, or ship Matter
# and the credential reader as separate images -- was put to the user on the strength of that
# overflow. It was never a real constraint; it was this misplacement.
#
# A 56 KB drop has the same shape as the failure this port has already paid for
# twice: a link that succeeds by deleting the product. VERIFIED ON THE ELF
# rather than inferred from the green build -- 384 mangled ot:: symbols with
# ot::Srp::Client, ot::Mle and ot::MeshForwarder among them, 42 matter_*,
# 48 ultrawidelock_*, 174 nrf_802154*, 60 psa_*, 24 ble_gatts_*, 14 dw3000*, and one
# each of vTaskSwitchContext and SystemInit. The otSrpClient* and otThread* C
# wrappers are gone BY NAME because their callers are now inside the set and the
# calls inlined, which is the optimisation working rather than the node
# vanishing. Ask the artifact what it does.
#
# add_compile_options() rather than CMAKE_INTERPROCEDURAL_OPTIMIZATION: the
# latter does not reach a vendored subdirectory that sets its own properties,
# and OpenThread's CMake is exactly such a subdirectory.
option(ULTRAWIDELOCK_LTO "Build the image with link-time optimisation" OFF)

# 1to1 keeps code generation on translation-unit boundaries. It was forced by an
# assembler "offset out of range" -- the assembler, not the linker -- when -Os
# inlining across merged units produced functions past Thumb branch range.
#
# THAT IS NO LONGER WHY IT IS HERE. Re-measured on the whole-program set,
# Matter configuration, 2026-08-14: balanced links clean, passes the vector
# check, and is 628 B LARGER (391,552 vs 390,924 flash) for 80 B less RAM. An
# earlier run on the old partial set put balanced 920 B behind. Two measurements
# on different sets agree on the direction, so this stays 1to1 because it is
# smaller, not because the assembler forbids the alternative.
#
# Left as a cache variable because the finding is worth re-testing whenever the
# set changes again -- and it should be re-tested by BUILDING, not reasoned
# about from what LTO is supposed to do.
set(ULTRAWIDELOCK_LTO_PARTITION "1to1" CACHE STRING "GCC -flto-partition mode")

if(ULTRAWIDELOCK_LTO)
  # -ffat-lto-objects is not optional, and it is not about build speed.
  #
  # Without it every archive member is IR only, and ld's plugin decides a symbol
  # is "IR only, no non-IR reference" while the archive holding the non-IR
  # caller has not been scanned yet. It then never generates code for it.
  # Measured on this tree: 167 symbols went missing from libultrawidelock_thread alone --
  # the whole Mbed TLS surface OpenThread calls, all of nrf_802154, every
  # ultrawidelock_* entry point -- each reported as
  #
  #   undefined reference to `X'
  #   (X): Unknown destination type (ARM/Thumb) ... dangerous relocation
  #
  # A fat object carries real machine code and a real symbol table beside the
  # IR, so ordinary archive resolution works and the plugin still optimises
  # across what it can see. Only one of the two forms is ever linked, so the
  # final image is unaffected.
  #
  # It is also what retires the old exclusion list. ultrawidelock_soc held SystemInit,
  # whose only caller is the non-LTO startup file; ultrawidelock_thread held the otPlat*
  # callouts reached from libopenthread-mtd.a. Both were symptoms of thin LTO
  # objects and of OpenThread sitting outside the set, and both premises are
  # gone here.
  add_compile_options(-flto -ffat-lto-objects)
endif()

# ----------------------------------------------------------------------------
# Device layer: CMSIS, the pinned MDK, and the nrfx hal.
#
# This is the hal_nordic tree the radio stack is built against, deliberately
# not the older nrfx bundled with the Qorvo SDK. Two register maps in one image
# is a class of bug that does not announce itself, and the radio's requirement
# is the harder one, so it wins.
# ----------------------------------------------------------------------------
add_library(ultrawidelock_soc STATIC
  "${ULTRAWIDELOCK_MDK}/system_nrf52833.c"
)
target_include_directories(ultrawidelock_soc PUBLIC
  "${ULTRAWIDELOCK_CMSIS}"
  "${ULTRAWIDELOCK_MDK}"
  "${ULTRAWIDELOCK_NRFX}"
  "${ULTRAWIDELOCK_NRFX}/hal"
  "${ULTRAWIDELOCK_NRFX}/drivers"
  "${ULTRAWIDELOCK_NRFX}/drivers/include"
  "${ULTRAWIDELOCK_NRFX}/bsp"
  "${ULTRAWIDELOCK_NRFX}/bsp/stable"
  "${ULTRAWIDELOCK_NRFX}/bsp/stable/templates"
  "${ULTRAWIDELOCK_PORT_DIR}/board/nrfx_config"
)
target_compile_definitions(ultrawidelock_soc PUBLIC
  NRF52833_XXAA
  __HEAP_SIZE=0
  __STACK_SIZE=0
)

# ----------------------------------------------------------------------------
# Kernel layer: FreeRTOS V10.0.0 and its Cortex-M4F port, both unmodified.
#
# port_cmsis_systick.c is deliberately absent. It drives RTC1 through
# nrf_drv_clock, which fights MPSL for the low-frequency clock, and it prescales
# the counter board/time_freertos.c reads at full rate.
# board/tick_freertos.c replaces it and supplies the same two entry points.
# ----------------------------------------------------------------------------
add_library(ultrawidelock_kernel STATIC
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/croutine.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/event_groups.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/list.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/queue.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/stream_buffer.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/tasks.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/timers.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/portable/MemMang/heap_4.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/portable/GCC/nrf52/port.c"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/portable/CMSIS/nrf52/port_cmsis.c"
)
target_include_directories(ultrawidelock_kernel PUBLIC
  "${ULTRAWIDELOCK_FREERTOS_SRC}/source/include"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/portable/GCC/nrf52"
  "${ULTRAWIDELOCK_FREERTOS_SRC}/portable/CMSIS/nrf52"
  "${ULTRAWIDELOCK_PORT_DIR}/board"
  "${ULTRAWIDELOCK_PORT_DIR}/kernel/kernel_compat"
)
target_link_libraries(ultrawidelock_kernel PUBLIC ultrawidelock_soc)

# heap_4 coalesces adjacent free blocks, which matters because Mbed TLS
# allocates and frees bignums of varying size for the duration of the product.
# heap_1, which the Qorvo baseline is closest to using, cannot free at all.

# The vendor kernel predates several warnings this repository builds with, and
# it is compiled unmodified on purpose, so they are relaxed for it alone.
target_compile_options(ultrawidelock_kernel PRIVATE
  -Wno-unused-parameter
  -Wno-sign-compare
)

# ----------------------------------------------------------------------------
# Board layer: the parts of the port that touch the chip directly.
#
# Only the files whose dependencies are already in the graph are listed. The
# rest of the board -- entropy, flash, temperature, logging -- arrives with the
# radio layer, because they call MPSL.
# ----------------------------------------------------------------------------
add_library(ultrawidelock_board STATIC
  "${ULTRAWIDELOCK_PORT_DIR}/board/startup_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/kernel_hooks_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/tick_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/time_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/fault_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/log_rtt_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/gpiote_freertos.c"
  # The OSAL sits here rather than in a layer of its own: it is the port's
  # implementation of the shared modules/ultrawidelock_port contract, it needs nothing but
  # the kernel, and everything above the board uses it.
  "${ULTRAWIDELOCK_PORT_DIR}/osal/osal_freertos.c"
)
target_include_directories(ultrawidelock_board PUBLIC
  "${ULTRAWIDELOCK_PORT_DIR}/include"
  "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_port/include"
)
target_compile_definitions(ultrawidelock_board PUBLIC ULTRAWIDELOCK_PORT_FREERTOS)
target_link_libraries(ultrawidelock_board PUBLIC ultrawidelock_kernel)

# A real cycle, declared rather than worked around. The kernel calls back into
# the board for configASSERT, the stack-overflow hook, and the static memory the
# idle and timer tasks are built from; the board calls into the kernel for
# everything else. CMake resolves a static-library cycle by repeating the
# archives on the link line, which is what this needs.
target_link_libraries(ultrawidelock_kernel PRIVATE ultrawidelock_board)

# ----------------------------------------------------------------------------
# Crypto layer: Mbed TLS 3.6.6, standalone with the PSA core on.
#
# Built from the library sources directly rather than through upstream's CMake,
# because upstream's project builds three libraries, a test framework, and its
# own programs, and this image wants one of the three with a configuration this
# port supplies wholesale.
#
# Only libmbedcrypto's sources are here. TLS and X.509 are not compiled at all:
# nothing in this product speaks either, and the whole reason OpenThread was
# left on its upstream OPENTHREAD_CONFIG_CRYPTO_LIB_MBEDTLS default rather than
# moved to PSA was to keep the surface this small.
#
# The configuration is forced in rather than selected by macro, so there is one
# answer to what this library was built with and it is a file in this tree.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_MBEDTLS_DIR "${ULTRAWIDELOCK_NCS_WORKSPACE}/modules/crypto/mbedtls")
if(NOT EXISTS "${ULTRAWIDELOCK_MBEDTLS_DIR}/library/psa_crypto.c")
  message(FATAL_ERROR "pinned Mbed TLS is missing: ${ULTRAWIDELOCK_MBEDTLS_DIR}")
endif()

file(GLOB ULTRAWIDELOCK_MBEDTLS_SOURCES "${ULTRAWIDELOCK_MBEDTLS_DIR}/library/*.c")
list(FILTER ULTRAWIDELOCK_MBEDTLS_SOURCES EXCLUDE REGEX "/(ssl_|x509|net_sockets|debug|mps_)")

add_library(ultrawidelock_mbedtls STATIC
  ${ULTRAWIDELOCK_MBEDTLS_SOURCES}
  "${ULTRAWIDELOCK_PORT_DIR}/crypto/mbedtls_threading_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/crypto/mbedtls_platform_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/crypto/crypto_init_freertos.c"
)
if(ULTRAWIDELOCK_MATTER)
  # PSA's Internal Trusted Storage, over the port's key-value store. Only a
  # Matter build has a persistent key to hold -- OpenThread's SRP signing key --
  # and MBEDTLS_PSA_CRYPTO_STORAGE_C is only defined for that build, so
  # compiling this unconditionally would be four entry points nothing calls.
  target_sources(ultrawidelock_mbedtls PRIVATE "${ULTRAWIDELOCK_PORT_DIR}/crypto/psa_its_freertos.c")
  # The NATIVE ITS path, which is the supported way to supply a custom backend:
  # psa_crypto_storage.c takes it whenever MBEDTLS_PSA_ITS_FILE_C is absent, and
  # then expects public <psa/error.h> and <psa/internal_trusted_storage.h>.
  # Neither ships in this tree, so the port provides them.
  #
  # MBEDTLS_PSA_ITS_FILE_C was tried first and is NOT the door: it selects the
  # internal interface but requires MBEDTLS_FS_IO, and there is no filesystem
  # here. Mbed TLS's own psa_its_file.c self-guards on that symbol, so with it
  # undefined the file compiles to nothing and cannot collide.
  target_include_directories(ultrawidelock_mbedtls PRIVATE "${ULTRAWIDELOCK_PORT_DIR}/crypto/psa_native_its")
  target_link_libraries(ultrawidelock_mbedtls PRIVATE ultrawidelock_storage)
endif()
target_include_directories(ultrawidelock_mbedtls PUBLIC
  "${ULTRAWIDELOCK_MBEDTLS_DIR}/include"
  "${ULTRAWIDELOCK_PORT_DIR}/crypto"
)
# The NCS fork keeps the library-internal headers under include/library rather
# than beside the sources, so both are on the private path.
target_include_directories(ultrawidelock_mbedtls PRIVATE
  "${ULTRAWIDELOCK_MBEDTLS_DIR}/library"
  "${ULTRAWIDELOCK_MBEDTLS_DIR}/include/library"
)
target_compile_definitions(ultrawidelock_mbedtls PUBLIC
  MBEDTLS_CONFIG_FILE="mbedtls_config_freertos.h"
  MBEDTLS_PSA_CRYPTO_CONFIG_FILE="psa_crypto_config_freertos.h"
)
target_link_libraries(ultrawidelock_mbedtls PUBLIC ultrawidelock_board)

# ----------------------------------------------------------------------------
# BLE host layer: Apache NimBLE.
#
# Built from the upstream checkout with the port's own syscfg, which is why
# ble/nimble_syscfg comes first on the include path: upstream ships its own
# syscfg/syscfg.h under porting/nimble/include and the first one found wins.
# The port's selection is peripheral-only -- no central, no observer, no
# extended advertising, one connection, one credit-based L2CAP channel.
#
# The host and its porting layer are taken by directory rather than by file.
# That is the upstream build's own granularity: every porting example lists
# these directories wholesale, and picking files by hand would mean tracking
# upstream's internal factoring at every re-pin for no reduction in image size,
# since --gc-sections already drops what the configuration does not reach.
# ----------------------------------------------------------------------------
if(NOT ULTRAWIDELOCK_NIMBLE_DIR)
  message(FATAL_ERROR "ULTRAWIDELOCK_NIMBLE_DIR is not set; see ports/freertos-nrf52833/README.md")
endif()
if(NOT EXISTS "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/npl/freertos/src/npl_os_freertos.c")
  message(FATAL_ERROR "ULTRAWIDELOCK_NIMBLE_DIR is not a mynewt-nimble checkout: ${ULTRAWIDELOCK_NIMBLE_DIR}")
endif()

file(GLOB ULTRAWIDELOCK_NIMBLE_SOURCES
  "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/nimble/src/*.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/npl/freertos/src/*.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/src/*.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/src/*.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/util/src/*.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/services/gap/src/*.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/services/gatt/src/*.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/transport/src/*.c"
)

# ble_gatts_lcl.c dumps the local GATT database over Mynewt's console package,
# which this build does not have and does not want: the port's log sink is
# SEGGER RTT. Upstream's own porting examples leave it out for the same reason.
list(REMOVE_ITEM ULTRAWIDELOCK_NIMBLE_SOURCES "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/src/ble_gatts_lcl.c")

# hal_timer.c and the cputime sources are the link layer's timebase. The
# controller here is Nordic's, reached over HCI, so nothing in this image drives
# them and their only effect would be to claim a hardware timer that
# peripherals.yml has already given to someone else.
list(REMOVE_ITEM ULTRAWIDELOCK_NIMBLE_SOURCES
  "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/nimble/src/hal_timer.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/nimble/src/os_cputime.c"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/nimble/src/os_cputime_pwr2.c")

add_library(ultrawidelock_nimble STATIC ${ULTRAWIDELOCK_NIMBLE_SOURCES})
target_include_directories(ultrawidelock_nimble PUBLIC
  "${ULTRAWIDELOCK_PORT_DIR}/ble/nimble_syscfg"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/nimble/include"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/porting/npl/freertos/include"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/include"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/include"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/util/include"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/services/gap/include"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/host/services/gatt/include"
  "${ULTRAWIDELOCK_NIMBLE_DIR}/nimble/transport/include"
)
target_link_libraries(ultrawidelock_nimble PUBLIC ultrawidelock_kernel ultrawidelock_mbedtls)

# Upstream is compiled unmodified and predates some of the warnings this
# repository builds with. -Wundef in particular cannot survive contact with
# NimBLE's headers: its configuration scheme tests MYNEWT_VAL_* and
# NIMBLE_CFG_* names that are meant to evaluate to zero when unset, so the
# warning fires on the mechanism rather than on a mistake. It is relaxed on the
# INTERFACE side as well, because the port's own files that include those
# headers hit the same thing, and it would be worse to drop the warning for the
# whole port than to scope it to the files that talk to NimBLE.
target_compile_options(ultrawidelock_nimble
  PRIVATE -Wno-sign-compare
  PUBLIC -Wno-undef -Wno-unused-parameter)

# ----------------------------------------------------------------------------
# Radio layer: MPSL and the SoftDevice Controller.
#
# Both are supplied as binaries built for this part and this float ABI, which is
# the reason the toolchain file makes hard float non-negotiable. The peripheral
# variant of the controller is linked, not multirole: this product advertises
# and accepts one connection and never initiates one, and the central and
# multirole archives carry link-layer code it would never execute.
#
# The board files that arrive with this layer do so because they call MPSL:
# temperature reads through mpsl_temperature_get, since MPSL owns TEMP and
# calibrates the low-frequency clock with it.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_NRFXLIB "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrfxlib")
set(ULTRAWIDELOCK_MPSL_LIB "${ULTRAWIDELOCK_NRFXLIB}/mpsl/lib/nrf52/hard-float/libmpsl.a")
set(ULTRAWIDELOCK_SDC_LIB
  "${ULTRAWIDELOCK_NRFXLIB}/softdevice_controller/lib/nrf52/hard-float/libsoftdevice_controller_peripheral.a")

# MPSL calls a front-end module API unconditionally, so something has to answer
# it even though the DWM3001CDK has no front-end module: the module is a bare
# nRF52833 driving the DW3110, with no PA or LNA in the radio path. The common
# archive is that answer -- it is the dispatch layer, and linking it without any
# of the device-specific FEM archives is how "no FEM" is expressed.
set(ULTRAWIDELOCK_FEM_LIB "${ULTRAWIDELOCK_NRFXLIB}/mpsl/fem/common/lib/nrf52/hard-float/libmpsl_fem_common.a")

foreach(_ultrawidelock_lib "${ULTRAWIDELOCK_MPSL_LIB}" "${ULTRAWIDELOCK_SDC_LIB}" "${ULTRAWIDELOCK_FEM_LIB}")
  if(NOT EXISTS "${_ultrawidelock_lib}")
    message(FATAL_ERROR "pinned radio binary is missing: ${_ultrawidelock_lib}")
  endif()
endforeach()

add_library(ultrawidelock_radio STATIC
  "${ULTRAWIDELOCK_PORT_DIR}/radio/radio_start_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/radio/mpsl_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/entropy_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/board/temperature_freertos.c"
  # The pinned NCS opcode dispatcher, compiled unmodified against ble/hci_compat
  # exactly as make freertos-ncs-source-check compiles it, plus the port half
  # that adapts it. The controller's command API is opcode-specific, so an HCI
  # command has to be decoded before it reaches the controller.
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/subsys/bluetooth/controller/hci_internal.c"
  "${ULTRAWIDELOCK_PORT_DIR}/ble/hci_dispatcher_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/ble/nimble_sdc_transport.c"
  "${ULTRAWIDELOCK_PORT_DIR}/ble/nimble_host_freertos.c"
)
target_include_directories(ultrawidelock_radio PUBLIC
  "${ULTRAWIDELOCK_NRFXLIB}/mpsl/include"
  "${ULTRAWIDELOCK_NRFXLIB}/mpsl/fem/include"
  "${ULTRAWIDELOCK_NRFXLIB}/softdevice_controller/include"
  "${ULTRAWIDELOCK_PORT_DIR}/ble/hci_compat"
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/subsys/bluetooth/controller"
)
target_link_libraries(ultrawidelock_radio PUBLIC ultrawidelock_board ultrawidelock_nimble "${ULTRAWIDELOCK_MPSL_LIB}" "${ULTRAWIDELOCK_SDC_LIB}"
  "${ULTRAWIDELOCK_FEM_LIB}")

# The dispatcher reads its configuration from a forced include, the same way the
# source-check script compiles it, so the vendor file needs no edit.
set_source_files_properties(
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/subsys/bluetooth/controller/hci_internal.c"
  PROPERTIES COMPILE_OPTIONS
  "-include;${ULTRAWIDELOCK_PORT_DIR}/ble/hci_compat/ultrawidelock_freertos_hci_config.h")

# The second declared cycle, and the same reasoning as the kernel's. The vector
# table in board/startup_freertos.c is the only thing that references the radio
# entry points, so without this the archive is scanned before those references
# exist and radio_start_freertos.c is never pulled in.
target_link_libraries(ultrawidelock_board PUBLIC ultrawidelock_radio)

# ----------------------------------------------------------------------------
# Storage layer: the persistent key-value store and its two readers.
#
# board/flash_freertos.c belongs here rather than with the rest of the board,
# because it arbitrates NVMC against the radio with MPSL timeslots and so
# cannot be linked without MPSL.
# ----------------------------------------------------------------------------
add_library(ultrawidelock_storage STATIC
  "${ULTRAWIDELOCK_PORT_DIR}/board/flash_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/storage/kv_flash_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/storage/ultrawidelock_prov_kv.c"
  # ultrawidelock_prov.c -- the portable half of the provisioning record -- was listed
  # here while this was the only layer that needed it. It is manifested in
  # ultrawidelock_cred/roles/reader_policy.list and now comes in with the rest of that
  # role, below: naming it in two places would put one object in two archives.
)
# The provisioning backend implements a contract owned by the shared credential
# module, not by this port, so the module's public headers are on the path. The
# key-value store is the same arrangement one layer down: ultrawidelock_kv.h is
# the seam every port implements, and ultrawidelock_freertos_kv.h now includes
# it. PUBLIC because everything that reaches the store -- the credential
# backend, OpenThread's settings, Matter's fabric records, PSA's trusted
# storage -- links this target to get there.
target_include_directories(ultrawidelock_storage PUBLIC
  "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_cred/include"
  "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_port/include"
)
target_link_libraries(ultrawidelock_storage PUBLIC ultrawidelock_radio)

# The flash driver refuses writes below this address, so the window has to cover
# every partition anything is expected to write -- and the DFU staging partition
# is BELOW the key-value store, which is the driver's default lower bound. Left
# at the default, the receiver could not erase its own staging area and every
# update died with "a flash write or erase failed" (measured on hardware
# 2026-08-15).
#
# 0x74000 is __ultrawidelock_patch_staging_start in board/nrf52833_lock.ld, which
# is the one place the map is decided. Restated here because the value has to
# reach the preprocessor, and checked against the linker's own symbol at
# start-up by ultrawidelock_flash_area_open() so the two cannot drift in silence.
target_compile_definitions(ultrawidelock_storage
  PRIVATE ULTRAWIDELOCK_FREERTOS_FLASH_WRITABLE_BASE=0x00074000u)

# ----------------------------------------------------------------------------
# Primitive provider: the image's single PSA implementation.
#
# Kept separate from the credential library because UWB and DFU consume the
# primitive contract directly. Defining it before UWB also avoids manufacturing
# a UWB <-> credential target cycle merely to obtain a crypto backend.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_CRED_ROLES "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_cred/roles")
ultrawidelock_role_sources(
  "${ULTRAWIDELOCK_CRED_ROLES}/prim_provider.list"
  ULTRAWIDELOCK_PRIM_PROVIDER_SRCS)
add_library(ultrawidelock_prim STATIC ${ULTRAWIDELOCK_PRIM_PROVIDER_SRCS})
target_include_directories(ultrawidelock_prim PUBLIC
  "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_cred/include")
target_link_libraries(ultrawidelock_prim PUBLIC ultrawidelock_mbedtls)

# ----------------------------------------------------------------------------
# UWB layer: the ranging engine, the vendored decadriver, and the DW3110
# platform backends. Its own file, because its source set comes from the shared
# role manifests rather than from a list kept here.
# ----------------------------------------------------------------------------
include("${ULTRAWIDELOCK_PORT_DIR}/uwb/uwb.cmake")

# ----------------------------------------------------------------------------
# Credential reader: the transaction engine, its BLE transport, and the ranging glue.
#
# After the UWB layer because it calls into it: ultrawidelock_ranging.c drives the
# responder through the contract in <ultrawidelock/uwb.h>.
#
# Every source here comes from a role manifest, including the NimBLE transport
# backend -- that file names no OS and is the same one ports/esp32 builds, so it
# is shared rather than copied. Only ble/ultrawidelock_ble_freertos.c is this port's,
# and it is short because ultrawidelock_radio already owns the host: see its header for
# why bonding is off and what turning it on would cost.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_CRED_ROLE_LISTS
  "${ULTRAWIDELOCK_CRED_ROLES}/reader.list"
  "${ULTRAWIDELOCK_CRED_ROLES}/reader_policy.list"
  "${ULTRAWIDELOCK_CRED_ROLES}/ble_transport.list"
  "${ULTRAWIDELOCK_CRED_ROLES}/apdu.list"
  "${ULTRAWIDELOCK_CRED_ROLES}/crypto.list"
  "${ULTRAWIDELOCK_CRED_ROLES}/hash.list"
  "${ULTRAWIDELOCK_CRED_ROLES}/dyntag.list"
  "${ULTRAWIDELOCK_CRED_ROLES}/wire_codecs.list"
)

set(ULTRAWIDELOCK_CRED_SOURCES)
foreach(_ultrawidelock_list IN LISTS ULTRAWIDELOCK_CRED_ROLE_LISTS)
  if(NOT EXISTS "${_ultrawidelock_list}")
    message(FATAL_ERROR "credential role manifest is missing: ${_ultrawidelock_list}")
  endif()
  ultrawidelock_role_sources("${_ultrawidelock_list}" _ultrawidelock_role_srcs)
  list(APPEND ULTRAWIDELOCK_CRED_SOURCES ${_ultrawidelock_role_srcs})
endforeach()

add_library(ultrawidelock_cred STATIC
  ${ULTRAWIDELOCK_CRED_SOURCES}
  "${ULTRAWIDELOCK_PORT_DIR}/ble/ultrawidelock_ble_freertos.c"
)
target_include_directories(ultrawidelock_cred PUBLIC
  "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_cred/include"
)
if(ULTRAWIDELOCK_MATTER)
  # The advertiser has to ask two questions it cannot answer itself: whether the
  # node already holds a fabric, and what the commissionable payload is. Headers
  # only, and PRIVATE -- linking ultrawidelock_cred against
  # ultrawidelock_matter_transport would be a cycle, since ultrawidelock_matter
  # already links ultrawidelock_cred. The symbols resolve when the executable is
  # linked, which is the one place both archives are present.
  #
  # apps/dwm3001cdk-lock/src is the other product's tree and carries a
  # status_led.h that would be wrong for this image. No ultrawidelock_cred source
  # includes it, which is why this is safe here and is checked rather than
  # assumed -- the same hazard is called out where ultrawidelock_matter_transport
  # takes the same directory.
  target_include_directories(ultrawidelock_cred PRIVATE
    "${ULTRAWIDELOCK_PORT_DIR}/matter"
    "${ULTRAWIDELOCK_REPO_ROOT}/apps/dwm3001cdk-lock/src"
  )
endif()
# ultrawidelock_radio for the NimBLE host and its hooks, ultrawidelock_storage for the provisioning
# backend the identity is read through, ultrawidelock_uwb for the ranging
# contract. The primitive provider is private composition, not part of the
# credential API surface.
target_link_libraries(ultrawidelock_cred
  PUBLIC ultrawidelock_radio ultrawidelock_storage ultrawidelock_uwb
  PRIVATE ultrawidelock_prim)

# ----------------------------------------------------------------------------
# The Matter node.
# ----------------------------------------------------------------------------
# modules/ultrawidelock_matter is a hand-written minimal Matter node -- CHIP does not fit
# this part -- and it is platform-agnostic C11 with no operating system
# dependency at all. The Zephyr build compiles these same fourteen files, and so
# does tests/host. Nothing here is a port of the protocol; the port is the glue
# that carries its messages, which lives in this directory and not in modules/.
#
# Its only outside dependency is ultrawidelock_cred: matter_crypto.c uses ultrawidelock_hash.h
# for SHA-256 and HKDF, and the crypto_aes_ecb_encrypt seam it declares itself.
# Both are already linked into any image that has the reader, so this is an
# include path rather than new code.
#
# OFF by default while the transport glue is unfinished. A protocol that
# compiles is not a node that commissions.
option(ULTRAWIDELOCK_MATTER "Build the Matter commissioning node into the image" OFF)

if(ULTRAWIDELOCK_MATTER)
  set(ULTRAWIDELOCK_MATTER_SRC "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_matter/src")
  add_library(ultrawidelock_matter STATIC
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_tlv.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_msg.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_mrp.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_crypto.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_btp.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_pase.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_spake2p.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_pase_sm.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_exchange.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_im.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_clusters.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_attest.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_fabric.c"
    "${ULTRAWIDELOCK_MATTER_SRC}/matter_case.c"
  )
  target_include_directories(ultrawidelock_matter PUBLIC
    "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_matter/include"
  )
  target_link_libraries(ultrawidelock_matter PUBLIC ultrawidelock_cred)

  # --------------------------------------------------------------------------
  # The Thread transport, compiled from the Zephyr port tree unmodified.
  # --------------------------------------------------------------------------
  # Not a copy, and not a rewrite. matter_thread_port.c is 1,227 lines of which
  # the Zephyr-specific part is three headers and four openthread_*() helpers;
  # everything else is OpenThread's own API, which is the same library this
  # image already runs. matter_compat/ supplies the three headers and maps the
  # four helpers onto this port's equivalents.
  #
  # Compiling it in place keeps one copy of behaviour that was expensive to
  # learn -- the SRP host-name suffix that dodges a 14-day key-lease collision
  # is the example that costs a fortnight to rediscover. Its proper home is
  # modules/, and moving it means editing the Zephyr module's CMakeLists, which
  # this build cannot verify; scripts/freertos-matter-source-check.sh holds the
  # seam closed until then.
  add_library(ultrawidelock_matter_transport STATIC
    "${ULTRAWIDELOCK_REPO_ROOT}/ports/zephyr/matter/matter_thread_port.c"
    "${ULTRAWIDELOCK_PORT_DIR}/matter/matter_settings_freertos.c"
    # The BLE half is written, not shimmed: Zephyr's bt_* GATT and NimBLE's
    # differ in shape, not spelling. It carries the oracle's hard-won behaviour
    # across rather than its code -- see the four numbered lessons in the file.
    "${ULTRAWIDELOCK_PORT_DIR}/matter/matter_ble_freertos.c"
    # The commissioning layer, also compiled unmodified. It is 3,417 lines and
    # its entire operating-system surface is work items, logging and settings --
    # everything else is PSA crypto and this project's own modules. That makes
    # it the same kind of file as the Thread transport: shared behaviour wearing
    # a Zephyr include block. It lives under apps/ rather than ports/ only
    # because that is where the Zephyr build wanted it.
    "${ULTRAWIDELOCK_REPO_ROOT}/apps/dwm3001cdk-lock/src/matter_commission.c"
    "${ULTRAWIDELOCK_REPO_ROOT}/ports/zephyr/store/matter_fab_settings.c"
  )
  target_include_directories(ultrawidelock_matter_transport PUBLIC
    "${ULTRAWIDELOCK_PORT_DIR}/matter"
    # matter_commission.h, which the application includes to call
    # matter_commission_init(). PUBLIC for that header alone; the app's own
    # quoted includes still resolve to its own directory first, so putting the
    # other product's src tree on the path cannot capture them.
    "${ULTRAWIDELOCK_REPO_ROOT}/apps/dwm3001cdk-lock/src"
  )
  target_include_directories(ultrawidelock_matter_transport PRIVATE
    "${ULTRAWIDELOCK_PORT_DIR}/matter/matter_compat"
    # The fabric store's own header, and the app's LED contract. Which
    # status_led.h a given file gets is decided by that file's own directory,
    # not by this list; see the note below.
    "${ULTRAWIDELOCK_REPO_ROOT}/ports/zephyr/store"
    # Both app trees carry a status_led.h declaring the same functions and the
    # same signal enum. The -I order below is NOT what decides which one
    # matter_commission.c gets: a quoted include resolves relative to the
    # INCLUDING file's own directory before any -I, and matter_commission.c
    # lives in the Zephyr app tree, so it always reads the Zephyr header. That
    # header compiles fine here -- matter_compat supplies zephyr/sys/util.h --
    # and it hides its declarations behind
    # IS_ENABLED(CONFIG_ULTRAWIDELOCK_STATUS_LED), turning every LED call into
    # a silent no-op when the macro is missing.
    # That was found on hardware, not in review: a Home tile tap that answered
    # SUCCESS and reported LockState while the lock LED never moved.
    "${ULTRAWIDELOCK_REPO_ROOT}/apps/dwm3001cdk-lock-freertos/src"
    "${ULTRAWIDELOCK_REPO_ROOT}/apps/dwm3001cdk-lock/src"
  )
  # See the include-order note above: matter_commission.c reads the ZEPHYR
  # status_led.h no matter what, so this build must satisfy that header's
  # config gate or lose every LED call in the file to the no-op stub. The
  # declarations behind the gate match this port's status_led.c exactly, and
  # the linker binds them to it.
  target_compile_definitions(ultrawidelock_matter_transport PRIVATE CONFIG_ULTRAWIDELOCK_STATUS_LED=1)
  # --------------------------------------------------------------------------
  # SPAKE2+ over P-256, from Nordic's oberon library.
  # --------------------------------------------------------------------------
  # matter_spake2p.c and matter_pase_sm.c call ocrypto_spake2p_p256_*. Mbed TLS
  # does not implement SPAKE2+ -- it is not a general primitive, it is the
  # augmented PAKE Matter commissioning is defined on -- so this is the one
  # binary this image takes for crypto. The Zephyr oracle lands on the same
  # library through PSA's nrf_oberon backend, because nRF52833 has no
  # CryptoCell.
  #
  # HARD-FLOAT, matching ULTRAWIDELOCK_ARCH_FLAGS. The soft-float variant links without
  # complaint on many toolchains and passes arguments in the wrong registers.
  set(ULTRAWIDELOCK_OBERON_LIB
    "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrfxlib/crypto/nrf_oberon/lib/cortex-m4/hard-float/liboberon_3.0.19.a")
  if(NOT EXISTS "${ULTRAWIDELOCK_OBERON_LIB}")
    message(FATAL_ERROR
      "nrf_oberon is missing: ${ULTRAWIDELOCK_OBERON_LIB}. Matter commissioning needs "
      "SPAKE2+ over P-256 and no other library in this image provides it.")
  endif()
  add_library(ultrawidelock_oberon STATIC IMPORTED GLOBAL)
  set_target_properties(ultrawidelock_oberon PROPERTIES IMPORTED_LOCATION "${ULTRAWIDELOCK_OBERON_LIB}")
  target_include_directories(ultrawidelock_oberon INTERFACE
    "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrfxlib/crypto/nrf_oberon/include")

  # ultrawidelock_dfu because the commissioning gesture opens the update window alongside
  # the commissioning one, and asks ultrawidelock_dfu_rx for it directly.
  target_link_libraries(ultrawidelock_matter_transport
    PUBLIC ultrawidelock_matter ultrawidelock_thread ultrawidelock_nimble ultrawidelock_radio ultrawidelock_dfu ultrawidelock_oberon)
  target_link_libraries(ultrawidelock_matter PUBLIC ultrawidelock_oberon)
  # The port's own files in this library are held to the port's standard. The
  # shared transport is not: it is compiled unmodified and belongs to a build
  # with different warning flags, so -Werror here would make an unrelated
  # change over there fail this image for style.
  set_source_files_properties(
    "${ULTRAWIDELOCK_PORT_DIR}/matter/matter_ble_freertos.c"
    "${ULTRAWIDELOCK_PORT_DIR}/matter/matter_settings_freertos.c"
    TARGET_DIRECTORY ultrawidelock_matter_transport
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-Wshadow"
  )
  # The Kconfig symbols the file selects its behaviour with. This port has no
  # Kconfig, so they are forced on from a header, exactly as the pinned radio
  # platform's are.
  target_compile_options(ultrawidelock_matter_transport PRIVATE
    "-include;${ULTRAWIDELOCK_PORT_DIR}/matter/matter_compat/ultrawidelock_freertos_matter_config.h")
endif()

# ----------------------------------------------------------------------------
# 802.15.4 layer: Nordic's radio driver, its service-layer binary, and the
# platform files this port already supplies in radio/.
#
# Built from an explicit source list rather than through nrfxlib's own CMake.
# That CMake builds five interface targets whose compile definitions come from
# Zephyr Kconfig, and adding it would pull the Kconfig dependency this port
# exists to avoid. The list below is driver/CMakeLists.txt's, read at the pinned
# revision; tests/ports/freertos-nrf52833/nrf_802154_sources_check.sh fails if
# the two drift apart, which is the same guard the UWB manifests get.
#
# The service layer is a binary. It arbitrates the radio with MPSL -- timeslot
# requests, the low-power timer, RSSI and CCA timing -- and there is no source
# form to compile, only the per-core, per-float-ABI archive selected here.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_802154_DIR "${ULTRAWIDELOCK_NRFXLIB}/nrf_802154")
set(ULTRAWIDELOCK_802154_SL_LIB
  "${ULTRAWIDELOCK_802154_DIR}/sl/sl/lib/nrf52833/hard-float/libnrf-802154-sl.a")
if(NOT EXISTS "${ULTRAWIDELOCK_802154_SL_LIB}")
  message(FATAL_ERROR "802.15.4 service layer is missing: ${ULTRAWIDELOCK_802154_SL_LIB}")
endif()

set(ULTRAWIDELOCK_802154_SOURCES)
foreach(_ultrawidelock_src
    nrf_802154.c
    nrf_802154_aes_ccm_acc_ccm.c
    nrf_802154_aes_ccm_acc_ecb.c
    nrf_802154_co.c
    nrf_802154_core.c
    nrf_802154_core_hooks.c
    nrf_802154_critical_section.c
    nrf_802154_debug.c
    nrf_802154_debug_gpio.c
    nrf_802154_encrypt.c
    nrf_802154_notification_swi.c
    nrf_802154_pib.c
    nrf_802154_queue.c
    nrf_802154_request_swi.c
    nrf_802154_rssi.c
    nrf_802154_rx_buffer.c
    nrf_802154_stats.c
    nrf_802154_swi.c
    nrf_802154_swi_callouts_weak.c
    nrf_802154_trx.c
    nrf_802154_trx_ppi.c
    nrf_802154_tx_work_buffer.c
    nrf_802154_tx_power.c
    mac_features/nrf_802154_csma_ca.c
    mac_features/nrf_802154_csma_ca_backoff.c
    mac_features/nrf_802154_delayed_trx.c
    mac_features/nrf_802154_filter.c
    mac_features/nrf_802154_frame_parser.c
    mac_features/nrf_802154_ie_writer.c
    mac_features/nrf_802154_imm_tx.c
    mac_features/nrf_802154_security_pib_ram.c
    mac_features/nrf_802154_security_writer.c
    mac_features/nrf_802154_precise_ack_timeout.c
    mac_features/nrf_802154_tx_timestamp_provider.c
    mac_features/ack_generator/nrf_802154_ack_data.c
    mac_features/ack_generator/nrf_802154_ack_generator.c
    mac_features/ack_generator/nrf_802154_enh_ack_generator.c
    mac_features/ack_generator/nrf_802154_imm_ack_generator.c)
  list(APPEND ULTRAWIDELOCK_802154_SOURCES "${ULTRAWIDELOCK_802154_DIR}/driver/src/${_ultrawidelock_src}")
endforeach()

# Four of upstream's sources are deliberately absent, and each omission is a
# selection rather than a trim:
#
#   nrf_802154_bsim_utils.c      BabbleSim host simulation, not a target build
#   nrf_802154_notification_direct.c \  the direct variants are the SL_OPENSOURCE
#   nrf_802154_request_direct.c      /  build's; this image uses the SWI ones,
#                                       which is what the shared EGU0 handler in
#                                       radio/ is wired for
#   nrf_802154_trx_dppi.c        DPPI belongs to nRF53/nRF91. nRF52833 has PPI,
#                                and linking both would define the same symbols

add_library(ultrawidelock_802154 STATIC
  ${ULTRAWIDELOCK_802154_SOURCES}
  # Shared timestamp conversions, from nrfxlib's common tree rather than the
  # driver's. radio_nrf5.c calls them directly for its CSL and delayed-TX
  # timing, so they belong to the layer even though upstream files them apart.
  "${ULTRAWIDELOCK_802154_DIR}/common/src/nrf_802154_common_utils.c"
  # The platform half of the driver: everything nrfxlib declares under
  # platform/ and leaves to whoever integrates it. Under Zephyr these come
  # from the nrfxlib module glue; here they are the port's, and they are what
  # keeps this driver sharing CLOCK, the low-power timer and the interrupt
  # controller with MPSL instead of fighting it for them.
  "${ULTRAWIDELOCK_PORT_DIR}/radio/nrf_802154_clock_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/radio/nrf_802154_irq_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/radio/nrf_802154_lptimer_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/radio/nrf_802154_misc_freertos.c"
  # No code, only the peripheral collision assertions. Here because this is
  # the only library whose include path reaches both the driver's private
  # masks and the UWB layer's board_pins.h.
  "${ULTRAWIDELOCK_PORT_DIR}/radio/peripheral_asserts_freertos.c"
  # The high-precision timer, a pinned NCS source compiled unmodified. It is an
  # 802.15.4 platform file rather than a Thread one despite living under
  # nrf/modules: the service layer's timer coordinator calls it, and nothing in
  # OpenThread does.
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/nrfxlib/nrf_802154/sl/platform/nrf_802154_hp_timer.c"
)
target_include_directories(ultrawidelock_802154 PUBLIC
  "${ULTRAWIDELOCK_802154_DIR}/common/include"
  "${ULTRAWIDELOCK_802154_DIR}/driver/include"
  "${ULTRAWIDELOCK_802154_DIR}/sl/include"
  "${ULTRAWIDELOCK_802154_DIR}/sl/sl/include"
)
target_include_directories(ultrawidelock_802154 PRIVATE
  "${ULTRAWIDELOCK_802154_DIR}/driver/src"
  "${ULTRAWIDELOCK_PORT_DIR}/uwb"
)
target_compile_definitions(ultrawidelock_802154 PUBLIC
  # This core has the driver; nothing is serialized to a network core.
  NRF_802154_SERIALIZATION_HOST=0
  # The port's configuration, forced in by name the same way Mbed TLS's is, so
  # there is one answer to what this driver was built with.
  NRF_802154_PROJECT_CONFIG="ultrawidelock_freertos_802154_config.h"
)
target_include_directories(ultrawidelock_802154 PUBLIC "${ULTRAWIDELOCK_PORT_DIR}/radio")

# The service layer as an imported target rather than a bare path, so the cycle
# below can be declared. It is a real cycle and not an ordering mistake: the SL
# archive's timer coordinator calls the platform's high-precision timer, and the
# driver's core calls back into the SL for timeslots and timing. A single scan
# in either order leaves the other half unresolved.
add_library(ultrawidelock_802154_sl STATIC IMPORTED)
set_target_properties(ultrawidelock_802154_sl PROPERTIES IMPORTED_LOCATION "${ULTRAWIDELOCK_802154_SL_LIB}")
target_link_libraries(ultrawidelock_802154 PUBLIC ultrawidelock_radio ultrawidelock_802154_sl)
set_property(TARGET ultrawidelock_802154_sl APPEND PROPERTY INTERFACE_LINK_LIBRARIES ultrawidelock_802154)

# Vendor source, compiled unmodified, so the port's warning set is not imposed
# on it -- the same treatment the FreeRTOS kernel gets above.
target_compile_options(ultrawidelock_802154 PRIVATE
  -ffunction-sections
  -fdata-sections
  -fno-common
  -fno-strict-aliasing
  -Wno-unused-parameter
)

# ----------------------------------------------------------------------------
# Thread layer: OpenThread as an MTD, plus the port's platform adapters.
#
# Built through upstream's own CMake with OT_PLATFORM=external, which is the
# supported way to supply a platform from outside the tree: it links ot-config
# and leaves otPlat* to whoever provides them. That is this port.
#
# OT_EXTERNAL_MBEDTLS points at the crypto layer above, so there is one Mbed TLS
# in the image rather than OpenThread bringing its own. This is also where
# leaving OPENTHREAD_CONFIG_CRYPTO_LIB at its upstream MBEDTLS default pays for
# itself: OpenThread compiles its own otPlatCrypto from crypto_platform.cpp
# against that library, so the port implements none of that surface.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_OPENTHREAD_DIR "${ULTRAWIDELOCK_NCS_WORKSPACE}/modules/lib/openthread")
if(NOT EXISTS "${ULTRAWIDELOCK_OPENTHREAD_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "pinned OpenThread is missing: ${ULTRAWIDELOCK_OPENTHREAD_DIR}")
endif()

set(OT_PLATFORM "external" CACHE STRING "" FORCE)
set(OT_BUILD_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(OT_MTD ON CACHE BOOL "" FORCE)
set(OT_FTD OFF CACHE BOOL "" FORCE)
set(OT_RCP OFF CACHE BOOL "" FORCE)
set(OT_APP_CLI OFF CACHE BOOL "" FORCE)
set(OT_APP_NCP OFF CACHE BOOL "" FORCE)
set(OT_APP_RCP OFF CACHE BOOL "" FORCE)
set(OT_EXTERNAL_MBEDTLS "ultrawidelock_mbedtls" CACHE STRING "" FORCE)
set(OT_MBEDTLS_THREADING ON CACHE BOOL "" FORCE)
set(OT_PROJECT_CONFIG "openthread_project_config.h" CACHE STRING "" FORCE)

# The project config is found by name, so its directory has to be reachable
# from every OpenThread translation unit.
include_directories("${ULTRAWIDELOCK_PORT_DIR}/thread")

# ULTRAWIDELOCK_HAVE_MATTER is what the project config gates the SRP client on, and
# OpenThread is built by its own CMake in the subdirectory below -- so it has to
# be a GLOBAL definition rather than a target-scoped one, which would leave the
# config reading it as undefined and silently build an OpenThread with no SRP
# client. That fails at link with otSrpClient* missing rather than quietly, but
# only after a full build of the wrong thing.
#
# It is set at the top of this file. Anywhere above this line would do; see the
# block up there for why position is a readability choice and not a functional
# one for definitions.

add_subdirectory("${ULTRAWIDELOCK_OPENTHREAD_DIR}" openthread EXCLUDE_FROM_ALL)

add_library(ultrawidelock_thread STATIC
  # Nordic's OpenThread radio platform and the service layer's high-precision
  # timer, both pinned NCS sources compiled unmodified against thread/ot_compat
  # -- exactly as make freertos-ncs-source-check compiles them. They are the
  # otPlatRadio surface: without them OpenThread has no radio and every
  # otPlatRadio* entry point is undefined.
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/openthread/platform/radio_nrf5.c"
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/nrfxlib/nrf_802154/sl/platform/nrf_802154_hp_timer.c"
  "${ULTRAWIDELOCK_PORT_DIR}/thread/openthread_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/thread/ot_kernel_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/thread/ot_radio_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/thread/ot_alarm_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/thread/ot_misc_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/thread/ot_log_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/thread/ot_settings_freertos.c"
)
if(ULTRAWIDELOCK_MATTER)
  # Nordic's PSA crypto platform, a pinned NCS source compiled unmodified --
  # the same arrangement as radio_nrf5.c above.
  #
  # It is required, not optional, once OPENTHREAD_CONFIG_CRYPTO_LIB is PSA.
  # OpenThread's own crypto_platform.cpp then contributes only 71 lines of weak
  # ECDSA stubs and expects the PLATFORM to supply otPlatCrypto -- import,
  # export, destroy, HMAC, AES, random, the whole surface. Without this file the
  # link fails on otPlatCryptoImportKey and a dozen others, which is exactly
  # what it did.
  #
  # Writing that surface by hand was the alternative. Nordic already maintains
  # it against the same OpenThread and the same PSA core, and the oracle ships
  # it, so this is the file the Zephyr build uses too.
  target_sources(ultrawidelock_thread PRIVATE
    "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/openthread/platform/crypto_psa.c")
  # NOT matter_compat on this target's include path. Both shim trees publish
  # zephyr/kernel.h and zephyr/logging/log.h, and putting the second one here
  # let it shadow ot_compat's for radio_nrf5.c -- which then failed to compile
  # against a struct it had been reading correctly for months. Two shim trees on
  # one include path is a trap regardless of order.
  #
  # crypto_psa.c needs one declaration out of Zephyr's settings API, so
  # ot_compat carries its own one-function header instead.
endif()
target_include_directories(ultrawidelock_thread PUBLIC
  "${ULTRAWIDELOCK_PORT_DIR}/thread/ot_compat"
  "${ULTRAWIDELOCK_OPENTHREAD_DIR}/include"
  "${ULTRAWIDELOCK_OPENTHREAD_DIR}/examples/platforms"
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/openthread/platform"
)
target_link_libraries(ultrawidelock_thread PUBLIC ultrawidelock_storage ultrawidelock_802154 openthread-mtd ot-config)

# The third declared cycle in this file, and the same shape as the other two.
# OpenThread's core calls otPlatSettings*, otPlatLog and otPlatReset, which this
# port implements in thread/; this port calls into OpenThread's core. Whichever
# archive is scanned first leaves the other's references open.
set_property(TARGET openthread-mtd APPEND PROPERTY INTERFACE_LINK_LIBRARIES ultrawidelock_thread)

# The vendor sources read their configuration from a forced include, the same
# way the source-check script compiles them, so none of them needs an edit.
#
# THIS LIST IS NAMED, so a file added to ultrawidelock_thread and not added here compiles
# with every CONFIG_* guard evaluating false -- silently, because an #if on an
# undefined symbol is 0 rather than an error. That is what happened when
# crypto_psa.c was added: it compiled to almost nothing and the link failed on
# otPlatCryptoEcdsaGenerateAndImportKey, three functions that were sitting
# inside `#if defined(CONFIG_OPENTHREAD_ECDSA)` in a file that had never been
# told the symbol was set.
set(ULTRAWIDELOCK_OT_VENDOR_SOURCES
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/openthread/platform/radio_nrf5.c"
  "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/nrfxlib/nrf_802154/sl/platform/nrf_802154_hp_timer.c")
if(ULTRAWIDELOCK_MATTER)
  list(APPEND ULTRAWIDELOCK_OT_VENDOR_SOURCES
    "${ULTRAWIDELOCK_NCS_WORKSPACE}/nrf/modules/openthread/platform/crypto_psa.c")
endif()
set_source_files_properties(${ULTRAWIDELOCK_OT_VENDOR_SOURCES}
  PROPERTIES COMPILE_OPTIONS
  "-include;${ULTRAWIDELOCK_PORT_DIR}/thread/ot_compat/ultrawidelock_freertos_ot_config.h;-Wno-unused-parameter")

# ----------------------------------------------------------------------------
# DFU receiver: take a signed delta patch, verify it, stage it, reboot.
#
# The receiver itself is already portable -- modules/ultrawidelock_dfu/src/dfu_receiver.c
# names PSA, ultrawidelock_flash, ultrawidelock_osal and ultrawidelock_log and nothing else -- so what this
# layer adds is the flash backend for the two partitions and the signing key.
#
# The APPLIER half is deliberately not here. It runs in MCUboot, because an
# application cannot rewrite the flash it is executing from, and MCUboot is a
# separate image this port does not build. See board/nrf52833_lock.ld.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_DFU_DIR "${ULTRAWIDELOCK_MODULES_DIR}/ultrawidelock_dfu")

# The same key the Zephyr image signs with, reached the same way: the top-level
# Makefile computes SIGN_KEY and exports it, and mk/freertos-nrf52833.mk passes
# it through. Falling back to the environment covers a bare cmake invocation.
if(NOT ULTRAWIDELOCK_DFU_KEY AND DEFINED ENV{SIGN_KEY})
  set(ULTRAWIDELOCK_DFU_KEY "$ENV{SIGN_KEY}")
endif()
if(NOT ULTRAWIDELOCK_DFU_KEY OR NOT EXISTS "${ULTRAWIDELOCK_DFU_KEY}")
  message(FATAL_ERROR
    "the update-signing key is missing: '${ULTRAWIDELOCK_DFU_KEY}'\n"
    "  This image verifies delta patches against its public half, so it cannot\n"
    "  be built without one.\n"
    "  Fix: make dfu-key   (once per clone; it refuses to overwrite)\n"
    "  Or:  pass -DULTRAWIDELOCK_DFU_KEY=<absolute path>.")
endif()

# The public key is generated into the image rather than committed, for the same
# reason the Zephyr build does it: the private key is the thing that must not be
# in the tree, and deriving the public half at configure time means the two
# cannot drift.
set(ULTRAWIDELOCK_DFU_PUBKEY_C "${CMAKE_CURRENT_BINARY_DIR}/ultrawidelock_dfu_pubkey.c")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
  OUTPUT "${ULTRAWIDELOCK_DFU_PUBKEY_C}"
  COMMAND "${Python3_EXECUTABLE}" "${ULTRAWIDELOCK_DFU_DIR}/scripts/emit_pubkey.py"
          "${ULTRAWIDELOCK_DFU_KEY}" "${ULTRAWIDELOCK_DFU_PUBKEY_C}"
  DEPENDS "${ULTRAWIDELOCK_DFU_KEY}" "${ULTRAWIDELOCK_DFU_DIR}/scripts/emit_pubkey.py"
  COMMENT "ultrawidelock_dfu: extracting the update-signing public key")

add_library(ultrawidelock_dfu STATIC
  "${ULTRAWIDELOCK_DFU_DIR}/src/dfu_receiver.c"
  "${ULTRAWIDELOCK_DFU_DIR}/src/dfu_crc.c"
  "${ULTRAWIDELOCK_PORT_DIR}/storage/flash_area_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/dfu/dfu_ble_freertos.c"
  "${ULTRAWIDELOCK_DFU_PUBKEY_C}"
)
target_include_directories(ultrawidelock_dfu PUBLIC
  "${ULTRAWIDELOCK_DFU_DIR}/include"
)
target_include_directories(ultrawidelock_dfu PRIVATE
  "${ULTRAWIDELOCK_DFU_DIR}/src"
)
# ultrawidelock_storage for the flash driver underneath the area backend and
# ultrawidelock_nimble for the transport in front of it. The receiver reaches
# signature verification only through the primitive contract; PRIVATE supplies
# the image's one provider without exporting it as part of the DFU API.
target_link_libraries(ultrawidelock_dfu
  PUBLIC ultrawidelock_storage ultrawidelock_nimble
  PRIVATE ultrawidelock_prim)

# ----------------------------------------------------------------------------
# USB device: the provisioning console's transport.
#
# The nRF5 SDK's app_usbd framework and CDC ACM class, compiled unmodified,
# against the nrfx this image already links rather than the one they shipped
# with. usb/usb_compat/ carries the three headers that makes possible:
# nrf_drv_usbd.h absorbs a return-type change between nrfx 1.9.0 and 4.2.1, and
# nrf_drv_clock.h / nrf_drv_power.h replace two SDK drivers that would each have
# taken POWER_CLOCK away from MPSL.
#
# NOT TinyUSB or a hand-written stack. USB enumeration is the kind of code whose
# bugs only appear against a particular host, and this tree already contains a
# stack that Nordic ships, tests on this silicon, and that the Qorvo SDK pins.
# ----------------------------------------------------------------------------
set(ULTRAWIDELOCK_NRF5_USBD "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/usbd")
set(ULTRAWIDELOCK_NRF5_NRFX "${ULTRAWIDELOCK_NRF5_SDK}/modules/nrfx")

# ----------------------------------------------------------------------------
# The provisioning console is optional, and a Matter build does not have one.
# ----------------------------------------------------------------------------
# It costs 18,151 bytes of flash and 10,249 of RAM -- measured on this tree, and
# the RAM is the larger number in a part where RAM is what runs out. All of it
# serves a mode that runs only when SW2 is held through reset.
#
# A Matter node self-provisions: the commissioner supplies the fabric, so
# typing a credential over a serial port is the thing Matter replaces. The
# Zephyr oracle reaches the same conclusion in prj.conf, where the console is
# off for the same reason and with the note that reader + console + Thread
# overflows RAM by 1,752 bytes regardless.
#
# ON by default, because that is the image that exists today and the one that
# provisions a board with no Matter fabric to join.
option(ULTRAWIDELOCK_PROV_CONSOLE "Build the USB provisioning console into the image" ON)

# ----------------------------------------------------------------------------
# The one SDK source this image does not compile verbatim, and why.
#
# app_usbd_core.c builds its device descriptor with bDeviceClass,
# bDeviceSubClass and bDeviceProtocol all zero -- "ask the interfaces". Linux
# does ask: cdc_acm binds off the interface class and a port appears. macOS does
# not. AppleUSBCDCACM binds a two-interface CDC function only when the DEVICE
# descriptor carries the Interface Association triple 0xEF/0x02/0x01, which is
# what marks the control and data interfaces as one function. With zeros it sees
# two unrelated interfaces of an unknown device and binds nothing.
#
# The failure has no error in it anywhere. The board enumerates, ioreg shows
# "Provisioning Console" with the right strings and serial, the firmware logs
# "USB CDC ACM up" -- and there is no /dev/cu.* node. Zephyr sets
# USB_BCC_MISCELLANEOUS here, which is why the same console works on the same
# laptop from the oracle build. A permissive host hid a descriptor that was
# always wrong, which is this port's recurring bug in a new place.
#
# THE VENDORED TREE IS NOT EDITED. A corrected copy is generated into the build
# directory and compiled instead. The three substitutions are asserted present
# first, so an SDK whose descriptor no longer looks like this stops the build
# rather than silently producing an image that enumerates into nothing -- the
# same failure the patch exists to remove.
#
# A linker --wrap on app_usbd_core_setup_rsp was tried first and is NOT
# sufficient: the descriptor is sent from setup_device_req_get_descriptor in the
# same translation unit, so the call never becomes a cross-object reference and
# --wrap does not see it. That version linked cleanly, defined the wrapper, and
# still shipped zeros. Checked by disassembly, not by the build succeeding.
# ----------------------------------------------------------------------------
if(ULTRAWIDELOCK_PROV_CONSOLE)

set(ULTRAWIDELOCK_USBD_CORE_SRC "${ULTRAWIDELOCK_NRF5_USBD}/app_usbd_core.c")
set(ULTRAWIDELOCK_USBD_CORE_GEN "${CMAKE_CURRENT_BINARY_DIR}/app_usbd_core_iad.c")
file(READ "${ULTRAWIDELOCK_USBD_CORE_SRC}" _ultrawidelock_usbd_core_txt)
foreach(_pair
    ".bDeviceClass = 0,|.bDeviceClass = 0xEF,"
    ".bDeviceSubClass = 0,|.bDeviceSubClass = 0x02,"
    ".bDeviceProtocol = 0,|.bDeviceProtocol = 0x01,")
  string(REPLACE "|" ";" _p "${_pair}")
  list(GET _p 0 _from)
  list(GET _p 1 _to)
  string(FIND "${_ultrawidelock_usbd_core_txt}" "${_from}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR
      "app_usbd_core.c no longer contains '${_from}'. The device descriptor "
      "changed shape in this SDK. Re-check the Interface Association triple "
      "against the new source: without it macOS enumerates the console and "
      "creates no serial port, with no error anywhere.")
  endif()
  string(REPLACE "${_from}" "${_to}" _ultrawidelock_usbd_core_txt "${_ultrawidelock_usbd_core_txt}")
endforeach()
file(WRITE "${ULTRAWIDELOCK_USBD_CORE_GEN}" "${_ultrawidelock_usbd_core_txt}")

add_library(ultrawidelock_usb STATIC
  # The pinned vendor stack, unmodified.
  "${ULTRAWIDELOCK_NRF5_NRFX}/drivers/src/nrfx_usbd.c"
  # The exclusive-access atomics nrfx_usbd's endpoint bitmasks are built on.
  # The pinned nrfx inlines these; this one does not, so the source is needed.
  "${ULTRAWIDELOCK_NRF5_NRFX}/soc/nrfx_atomic.c"
  "${ULTRAWIDELOCK_NRF5_USBD}/app_usbd.c"
  # Generated above from app_usbd_core.c: the device-class triple only.
  "${ULTRAWIDELOCK_USBD_CORE_GEN}"
  "${ULTRAWIDELOCK_NRF5_USBD}/app_usbd_string_desc.c"
  "${ULTRAWIDELOCK_NRF5_USBD}/app_usbd_serial_num.c"
  "${ULTRAWIDELOCK_NRF5_USBD}/class/cdc/acm/app_usbd_cdc_acm.c"
  # The SDK libraries it leans on. nrf_atfifo is the event queue -- see
  # usb/usb_power_freertos.c for why its being lock-free is load-bearing here.
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/atomic_fifo/nrf_atfifo.c"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/atomic/nrf_atomic.c"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/util/app_util_platform.c"
  # This port's half.
  "${ULTRAWIDELOCK_PORT_DIR}/usb/usb_power_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/usb/usb_cdc_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/usb/prov_console_freertos.c"
  "${ULTRAWIDELOCK_PORT_DIR}/usb/sdk_error_freertos.c"
)
target_include_directories(ultrawidelock_usb PUBLIC
  # First, so the three compat headers shadow the SDK's own.
  "${ULTRAWIDELOCK_PORT_DIR}/usb/usb_compat"
  "${ULTRAWIDELOCK_NRF5_USBD}"
  "${ULTRAWIDELOCK_NRF5_USBD}/class/cdc"
  "${ULTRAWIDELOCK_NRF5_USBD}/class/cdc/acm"
)
# THE nrfx THIS ONE LIBRARY SEES IS THE SDK'S OWN, and it is the only place in
# the image where that is true. Everywhere else the rule at the top of this file
# holds: hal_nordic's nrfx, because the radio stack is built against it.
#
# The rule cannot hold here, and the reason is measured rather than assumed.
# Between nrfx 1.9.0 and 4.2.1 the USBD *driver* API is nearly unchanged -- three
# return types, absorbed by usb_compat/nrf_drv_usbd.h -- but the *hal* underneath
# it gained a peripheral-instance argument on every function: this SDK calls
# nrf_usbd_setup_brequest_get(), the new hal wants nrf_usbd_setup_brequest_get(NRF_USBD).
# app_usbd.c and app_usbd_core.c do that in dozens of places, and shimming a hal
# means redeclaring functions the newer nrfx also defines, in an image that
# compiles both.
#
# So the vendor stack gets to be internally consistent instead: driver, hal and
# framework all from SDK 17.1, on include paths PRIVATE to this library. What is
# deliberately NOT taken is ${ULTRAWIDELOCK_NRF5_NRFX}/mdk -- nrf.h still resolves to the
# pinned MDK through ultrawidelock_soc, so there is still exactly one register map and
# one device header in the image, which was the property the rule was protecting.
target_include_directories(ultrawidelock_usb PRIVATE
  "${ULTRAWIDELOCK_NRF5_SDK}/integration/nrfx"
  "${ULTRAWIDELOCK_NRF5_NRFX}"
  "${ULTRAWIDELOCK_NRF5_NRFX}/hal"
  "${ULTRAWIDELOCK_NRF5_NRFX}/drivers/include"
  "${ULTRAWIDELOCK_NRF5_NRFX}/soc"
  # nrf_error.h, from the no-SoftDevice variant. This image has no SoftDevice --
  # the controller is the SoftDevice Controller library, which is a different
  # thing -- so the s132/s112 copies would be the wrong ones.
  "${ULTRAWIDELOCK_NRF5_SDK}/components/drivers_nrf/nrf_soc_nosd"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/util"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/atomic"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/atomic_fifo"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/log"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/log/src"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/experimental_section_vars"
  "${ULTRAWIDELOCK_NRF5_SDK}/components/libraries/delay"
  "${ULTRAWIDELOCK_NRF5_SDK}/external/utf_converter"
)
# ultrawidelock_radio for the MPSL clock and the POWER vector fan-out; ultrawidelock_board for the
# logger and the device headers underneath both.
# ultrawidelock_cred for the provisioning console, which reads and writes the same
# identity the reader does -- through ultrawidelock_prov_* and the reader's own commit
# path, never through a second copy of that logic.
target_link_libraries(ultrawidelock_usb PUBLIC ultrawidelock_board ultrawidelock_radio ultrawidelock_cred)

endif(ULTRAWIDELOCK_PROV_CONSOLE)

add_library(ultrawidelock_freertos_port INTERFACE)
target_link_libraries(ultrawidelock_freertos_port INTERFACE
  ultrawidelock_storage ultrawidelock_uwb ultrawidelock_thread ultrawidelock_cred ultrawidelock_dfu)
if(ULTRAWIDELOCK_PROV_CONSOLE)
  target_link_libraries(ultrawidelock_freertos_port INTERFACE ultrawidelock_usb)
endif()
# Carried to every consumer rather than tested as a CMake variable in the app,
# so the application source says in C which mode it was built for. A build
# without the console must not merely fail to start it -- main() has a whole
# branch that does not apply, and a branch that cannot run should not be in
# the image being measured.
target_compile_definitions(ultrawidelock_freertos_port INTERFACE
  ULTRAWIDELOCK_HAVE_PROV_CONSOLE=$<BOOL:${ULTRAWIDELOCK_PROV_CONSOLE}>
  ULTRAWIDELOCK_HAVE_MATTER=$<BOOL:${ULTRAWIDELOCK_MATTER}>)
if(ULTRAWIDELOCK_MATTER)
  target_link_libraries(ultrawidelock_freertos_port INTERFACE ultrawidelock_matter_transport)
endif()

# ----------------------------------------------------------------------------
# Warnings and code generation, applied to the port's own code only.
# ----------------------------------------------------------------------------
foreach(_ultrawidelock_target ultrawidelock_board ultrawidelock_radio ultrawidelock_storage ultrawidelock_thread)
  target_compile_options(${_ultrawidelock_target} PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wundef
  )
endforeach()

# The credential BLE layer gets the same treatment minus -Wundef, which it cannot
# have: NimBLE's public headers test MYNEWT_VAL_* names that are meant to
# evaluate to zero when unset, and ultrawidelock_nimble already relaxes it PUBLIC for that
# reason. The rest still applies -- this is the port's own code and the shared
# backend, not vendor source.
target_compile_options(ultrawidelock_cred PRIVATE
  -Wall
  -Wextra
  -Werror
  -Wshadow
)
target_compile_options(ultrawidelock_prim PRIVATE
  -Wall
  -Wextra
  -Werror
  -Wshadow
)

# -ffunction-sections and friends used to be a nine-target foreach here. They
# are now in the global block at the top of this file, for the same reason the
# LTO flags are: the list left out ultrawidelock_thread, ultrawidelock_matter,
# ultrawidelock_matter_transport, ultrawidelock_dfu, ultrawidelock_usb and the
# whole OpenThread subdirectory, so --gc-sections could only drop entire objects
# from the largest archive in the image rather than individual functions. Two
# hand-maintained lists in one file, with the same defect, found by a review
# comparing this port against the oracle.

# ----------------------------------------------------------------------------
# Link-time optimisation.
# ----------------------------------------------------------------------------
# Measured on this tree: 418,664 bytes of flash without it and 348,228 with,
# a saving of 70,436 -- which is the difference between an image with no room
# for the Matter commissioning stack and one with more headroom than the
# Zephyr build that ships it. RAM falls 10,912 bytes on the same link.
#
# LTO IS NOT FREE HERE, and the risk is specific rather than general. This
# image's vector table is DATA holding weak aliases that each owner is expected
# to override. Whole-program optimisation is exactly the pass that can decide an
# override is unreachable, and the result links cleanly and resolves to
# default_handler -- an infinite loop the first interrupt falls into. That is
# the failure this port already paid for twice, on RNG and RTC2.
#
# What makes it safe to turn on is not confidence in the optimiser: it is that
# freertos-vector-check.sh reads the LINKED IMAGE and asserts every vector
# peripherals.yml gives an owner resolves somewhere other than default_handler.
# It runs on every build. Ask the artifact what it does, not the toolchain
# whether it succeeded.
#
# The second cost is timing, and it is NOT yet retired: the DW3110 response-arm
# deadline of ~1.836 ms has never been measured, and inlining moves code across
# the boundary the deadline lives on. Until that measurement exists this stays
# an option rather than the default.
# ----------------------------------------------------------------------------
# How much of the log survives compilation.
# ----------------------------------------------------------------------------
# 1 error, 2 warning, 3 info, 4 debug, matching enum ultrawidelock_freertos_log_level.
# The gate is at the call site (see ultrawidelock_freertos_platform.h), so a level below a
# call's own removes its format string from the image as well as its output --
# which is the whole point, and was not true until that gate moved.
#
# 3 keeps the boot log this port has been debugged against. A build that needs
# the flash takes 2 and loses the narration, not the diagnosis: every failure
# path in this port reports at error or warning.
set(ULTRAWIDELOCK_LOG_LEVEL "3" CACHE STRING "Highest log level compiled in (1 error .. 4 debug)")
add_compile_definitions(ULTRAWIDELOCK_FREERTOS_LOG_MAX_LEVEL=${ULTRAWIDELOCK_LOG_LEVEL})

# How much of a boot the RTT up-buffer can hold, in bytes of static RAM.
#
# The default is the file's own, and the file explains why raising it is a bench
# decision: the buffer DROPS writes when full rather than overwriting, so on a
# board with nothing draining it the log stops dead partway through boot and
# says nothing about having stopped. 1 kB reaches the end of bring-up and no
# further, which is enough for every failure this port had until commissioning
# -- a BTP handshake is far more talkative than a boot.
#
# It is spent from roughly 8 kB of headroom, so this is not a shipping value and
# there is no default here that spends it.
set(ULTRAWIDELOCK_LOG_RTT_BUFFER "" CACHE STRING "RTT up-buffer size in bytes; empty keeps the built-in 1024")
if(ULTRAWIDELOCK_LOG_RTT_BUFFER)
  add_compile_definitions(ULTRAWIDELOCK_FREERTOS_LOG_RTT_BUFFER_BYTES=${ULTRAWIDELOCK_LOG_RTT_BUFFER}u)
endif()

if(ULTRAWIDELOCK_LTO)
  # TWO FILES ARE HELD OUT OF THE WHOLE-PROGRAM SET, and only two. Both are
  # reached from something the optimiser cannot read, so neither exclusion is
  # caution: each was a measured failure.
  #
  # THE FREERTOS PORT LAYER. vTaskSwitchContext's only caller is the inline
  # assembly in the Cortex-M PendSV handler, and assembly is outside the LTO set
  # by construction -- the optimiser reads IR, never an asm string. LTO does not
  # merely drop the function, it may LOCALISE AND RENAME it, which is why
  # forcing the symbol with -Wl,--undefined does not help and was tried. Only
  # port.c is held out, not the whole kernel: tasks.c, queue.c and the rest have
  # every caller in C and belong in the set.
  set_source_files_properties("${ULTRAWIDELOCK_FREERTOS_SRC}/portable/GCC/nrf52/port.c"
    TARGET_DIRECTORY ultrawidelock_kernel
    PROPERTIES COMPILE_OPTIONS "-fno-lto"
  )

  # THE STARTUP FILE MUST NOT BE OPTIMISED ACROSS THE PROGRAM, and the reason
  # is not caution -- it was measured. With this file in LTO the link says:
  #
  #   ld: warning: cannot find entry symbol Reset_Handler; defaulting to 0000a200
  #
  # and produces an ELF with no default_handler and no vector table. Nothing in
  # C ever references Reset_Handler or .isr_vector; only the linker script does.
  # The LTO plugin decides which symbols are live BEFORE code generation emits
  # any section, so the script's KEEP(*(.isr_vector)) has nothing to hold: KEEP
  # operates on input sections, and at that point the section does not exist
  # yet. Whole-program analysis therefore concludes the entire startup object is
  # dead and drops it, which is a correct answer to the question it was asked.
  #
  # The result links with one warning and is a brick. Compiling this one file
  # normally leaves a real object with real symbols, and the handlers it names
  # in other translation units are then live because a non-LTO object refers to
  # them. freertos-vector-check.sh is what caught this, on the linked image.
  set_source_files_properties("${ULTRAWIDELOCK_PORT_DIR}/board/startup_freertos.c"
    TARGET_DIRECTORY ultrawidelock_board
    PROPERTIES COMPILE_OPTIONS "-fno-lto"
  )
endif()

# ----------------------------------------------------------------------------
# A helper the application uses to turn a linked ELF into something flashable
# and to print what it cost.
# ----------------------------------------------------------------------------
function(ultrawidelock_freertos_image target)
  if(ULTRAWIDELOCK_LTO)
    # The link needs the flag too: with LTO the code generator runs HERE, so
    # the optimisation level has to be restated on the link line or the final
    # pass runs at -O0 and gives most of the saving back. The arch flags do not
    # need restating -- CMAKE_EXE_LINKER_FLAGS_INIT in the toolchain file
    # already carries -mcpu/-mfpu, and without them this link would quietly
    # generate soft-float objects for a hard-float image.
    target_compile_options(${target} PRIVATE -flto)
    target_link_options(${target} PRIVATE -flto -Os)

    # KEEP CODE GENERATION ON THE ORIGINAL TRANSLATION-UNIT BOUNDARIES.
    #
    # The default partitioning merges translation units before generating code,
    # and with -Os inlining across the whole program that produced functions
    # large enough to break Thumb branch range. The assembler, not the linker,
    # is what failed:
    #
    #   ccydKnE6.s:2017: Error: offset out of range
    #
    # 1to1 keeps each unit's code generation to itself while still allowing the
    # cross-unit analysis that produces the saving. It also makes the build
    # reproducible unit by unit, which matters when something has to be
    # disassembled to find out what the image actually does.
    target_link_options(${target} PRIVATE -flto-partition=${ULTRAWIDELOCK_LTO_PARTITION})
  endif()
  target_link_options(${target} PRIVATE
    "-T${ULTRAWIDELOCK_PORT_DIR}/board/nrf52833_lock.ld"
    "-L${ULTRAWIDELOCK_PORT_DIR}/board"
    --specs=nano.specs
    --specs=nosys.specs
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    "-Wl,-Map=$<TARGET_FILE_DIR:${target}>/${target}.map"
  )
  set_target_properties(${target} PROPERTIES
    LINK_DEPENDS "${ULTRAWIDELOCK_PORT_DIR}/board/nrf52833_lock.ld"
    SUFFIX ".elf"
  )
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O ihex "$<TARGET_FILE:${target}>"
            "$<TARGET_FILE_DIR:${target}>/${target}.hex"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:${target}>"
    VERBATIM
  )
endfunction()
