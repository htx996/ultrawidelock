# ports/zephyr — ultrawidelock_zephyr_port: the Zephyr platform backends, one module.
# Mirror of ports/esp32/components/*, which implements the same contracts on
# ESP-IDF; modules/ stays the single portable source tree. Contract headers
# stay with their modules (dw3000_hw.h / dw3000_spi.h in
# modules/ultrawidelock_dw3000/include, pn532_bus.h in modules/ultrawidelock_nfc/include); this tree
# carries the Zephyr .c backends plus the app-target glue below.
# Contributes nothing unless a consuming CONFIG is y: an image that selects no
# feature below gets zero bytes from this module.

# ultrawidelock_shell.h is the one header living here (the `ultrawidelock` console's seam to
# the application); published wherever modules/ultrawidelock_uwb used to publish its
# shell dir, because the add-on factory-reset patch includes it.
if(CONFIG_ULTRAWIDELOCK_UWB)
	zephyr_include_directories(shell)
endif()

# The seam headers, published to every image on this OS rather than to this
# module's own sources. modules/ultrawidelock_uwb also publishes them, which is
# enough for the three images that carry the ranging engine -- and was not
# enough for examples/zephyr/ble-witness, which carries no UWB module at all and
# still opens the sealed link. A contract's headers belong with the tree that
# implements it, so they are published here and reach every consumer.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_port/include)

# ---- ultrawidelock_zephyr_port library: backends of module library contracts ----------
if(CONFIG_DW3000
   OR CONFIG_ULTRAWIDELOCK_PRETTY_SHELL
   OR CONFIG_ULTRAWIDELOCK_UWB_SHELL
   OR (CONFIG_ULTRAWIDELOCK_CRED AND CONFIG_SOC_NRF5340_CPUAPP)
   OR CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG
   OR CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM
   OR CONFIG_ULTRAWIDELOCK_DFU_RECEIVER
   # An image with OpenThread gets the ultrawidelock_dgram.h backend below, and
   # needs this library to exist to hold it. ble-witness selects none of the
   # conditions above -- no DW3000, no shell, no DFU -- so before this line it
   # built no port library at all and would have linked the sealed link's send
   # against nothing.
   OR CONFIG_OPENTHREAD)

zephyr_library_named(ultrawidelock_zephyr_port)

# Zephyr backend of ultrawidelock_osal.h, for every image on this OS. It lives here and
# nowhere else: modules/ calls the contract (uwb_rxdiag.c, uwb_selftest.c,
# dfu_receiver.c) and this tier is the one thing all four Zephyr images carry
# on ZEPHYR_EXTRA_MODULES. It used to ride modules/ultrawidelock_dfu, which meant an
# image without the DFU module -- the nRF5340DK door lock -- linked the callers
# with nothing behind them. MCUboot is the exception and needs no backend: the
# applier is synchronous and takes only ultrawidelock_flash.
zephyr_library_sources(osal/osal_zephyr.c)
# Zephyr backend of ultrawidelock_kv.h, here for the same reason as the OSAL
# above: it is the contract's implementation for this OS and belongs to the tier
# every image on it carries, not to whichever application happened to need it
# first. An image that calls nothing in it pays nothing -- the linker drops what
# nobody reaches -- and compiling it everywhere is what stops it rotting.
zephyr_library_sources(store/kv_zephyr.c)
# Zephyr backend of ultrawidelock_dgram.h, on OpenThread UDP. NOT unconditional
# like the two above: it calls otUdpOpen and takes openthread_mutex, so an image
# built without OpenThread has nothing to compile it against.
#
# CONFIG_OPENTHREAD, not CONFIG_NET_L2_OPENTHREAD, for the reason
# matter/matter_thread_port.c already states: every call in the backend is
# either OpenThread's own API or one of the openthread_*() helpers the
# standalone module provides, and the L2 is not what supplies either. All three
# images that open the sealed link -- the lock, the satellite and ble-witness --
# are standalone by choice; each overlay says so at length. Guarding on the L2
# would have compiled this backend into no image at all.
zephyr_library_sources_ifdef(CONFIG_OPENTHREAD net/dgram_openthread.c)
zephyr_library_include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_port/include)

# Zephyr GPIO/SPI/trace backend of the dw3000_hw.h + dw3000_spi.h contract
# (ESP-IDF backend: ports/esp32/components/ultrawidelock_uwb/port/dw3000_{hw,spi}.c).
zephyr_library_sources_ifdef(CONFIG_DW3000
	dw3000/dw3000_hw.c
	dw3000/dw3000_spi.c
	dw3000/dw3000_spi_trace.c)

# Contract headers + decadriver API; modules/ultrawidelock_dw3000 also publishes these
# via zephyr_include_directories, listed here so this library does not depend
# on that module's processing order.
zephyr_library_include_directories(
	${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_dw3000/include
	${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_dw3000/dwt_uwb_driver)

# PRETTY (reversible): DWT-cycle-counter timestamps + compact colored log line
# (ultrawidelock_logfmt), plus runtime-mute of benign upstream Matter/BLE error spam
# (ultrawidelock_logquiet). Owns the whole log line via LOG_CUSTOM_FORMAT_SUPPORT, set in
# apps/nrf5340dk-lock/overlays/ultrawidelock-pretty.conf, active only under PRETTY=1.
zephyr_library_sources_ifdef(CONFIG_ULTRAWIDELOCK_PRETTY_SHELL
	log/ultrawidelock_logfmt.c
	log/ultrawidelock_logquiet.c)

# `ultrawidelock` UART console (status/rx/range/chip/selftest/log/version). Needs the
# shell subsystem, which an NCS/Matter dependency force-selects. Engine headers
# (ccc_shim.h, fira_session.h, ...) come from modules/ultrawidelock_uwb's global include
# dirs, alive whenever CONFIG_ULTRAWIDELOCK_UWB is.
if(CONFIG_ULTRAWIDELOCK_UWB_SHELL)
	zephyr_library_sources(shell/ultrawidelock_shell.c)
	# Short commit SHA of THIS repo, baked in for `ultrawidelock version`. Captured at
	# CMake configure time (git -C works from a worktree too); refreshed on a
	# pristine/reconfigure build, so a stale-but-clean incremental keeps the
	# prior value. "unknown" if git is unavailable (e.g. a source export).
	execute_process(
		COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
		OUTPUT_VARIABLE ULTRAWIDELOCK_GIT_SHA
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET)
	if(NOT ULTRAWIDELOCK_GIT_SHA)
		set(ULTRAWIDELOCK_GIT_SHA "unknown")
	endif()
	set_source_files_properties(shell/ultrawidelock_shell.c PROPERTIES
		COMPILE_DEFINITIONS "ULTRAWIDELOCK_GIT_SHA=\"${ULTRAWIDELOCK_GIT_SHA}\"")
endif()

# nRF5340 app-core HFCLK boost, split out of ultrawidelock_uwb_facade.c (which keeps the
# portable ultrawidelock_hfclk_ensure_128mhz() seam). Compiled exactly when the facade
# is (CONFIG_ULTRAWIDELOCK_CRED) on the SoC it exists for.
if(CONFIG_ULTRAWIDELOCK_CRED AND CONFIG_SOC_NRF5340_CPUAPP)
	zephyr_library_sources(uwb/ultrawidelock_hfclk_boost.c)
endif()

# Optional second front door onto the ultrawidelock_dfu receiver: the SMP image group,
# for clients that only speak mcumgr. Rides the receiver's library contract
# (ultrawidelock_dfu.h / ultrawidelock_dfu_rx.h / ultrawidelock_flash.h).
if(CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG)
	zephyr_library_sources(dfu/dfu_smp_img.c)
	zephyr_library_include_directories(
		${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_dfu/include
		${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_dfu/src
		${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_port/include)
endif()

# LIS2DH12 wake-on-motion backend of ultrawidelock_slam_hw.h: writes an I2C part's
# registers and attaches a GPIO callback, so it needs a board whose devicetree
# has an accel0 — which is why CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM is a separate symbol
# from CONFIG_ULTRAWIDELOCK_ANCHOR (the geometry links anywhere, this does not).
if(CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM)
	zephyr_library_sources(drivers/ultrawidelock_slam_lis2dh12.c)
	zephyr_library_include_directories(
		${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_anchor/include
		${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_port/include)
endif()

endif()

# ---- app-target glue --------------------------------------------------------
# Compiled straight into the application target, the same mechanism
# modules/ultrawidelock_nfc uses for its transports: these files include app-owned
# headers (matter_commission.h) or need the app/add-on include paths, so they
# ride the app's compile context rather than the library's. The ULTRAWIDELOCK_* /
# ULTRAWIDELOCK_DFU_RECEIVER gates are defined by the consuming app's Kconfig and simply
# do not exist elsewhere, so every other image gets nothing.

# Zephyr SPI backend of the PN532 bus contract.
if(CONFIG_ULTRAWIDELOCK_NFC_TRANSPORT_PN532)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/nfc/pn532_bus_spi.c)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../../modules/ultrawidelock_nfc/include)
endif()

# The 0xFFF6 Matter commissioning transport + Thread/fabric ports + the fabric
# table store. matter_commission.c stays in apps/dwm3001cdk-lock/src (it is the app's
# policy half); its headers here reach it via apps/dwm3001cdk-lock/CMakeLists.txt's app
# include dirs.
if(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
	target_sources(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/matter/matter_ble_zephyr.c
		${CMAKE_CURRENT_SOURCE_DIR}/matter/matter_thread_port.c
		${CMAKE_CURRENT_SOURCE_DIR}/store/matter_fab_settings.c)
endif()

# Resolving somebody ELSE, which only a client build does. Separate from
# matter_thread_port.c rather than guarded inside it, so an image without the
# client carries no otDnsClient at all -- the measured cost of that library is
# 4,052 B of flash, which is not a rounding error on this part.
if(CONFIG_ULTRAWIDELOCK_MATTER_CLIENT)
	target_sources(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/matter/matter_dns_port.c)
endif()

# Initiator (User-Device role) BLE central transport, the mirror of
# ble/ultrawidelock_ble_zephyr.c. examples/zephyr/nrf5340dk-initiator lists this file by path
# instead: it carries no Kconfig of its own to define this gate.
if(CONFIG_ULTRAWIDELOCK_BLE_CENTRAL)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ble/ultrawidelock_ble_central_zephyr.c)
endif()

# BLE transport of the delta-update receiver (ultrawidelock_dfu supplies ultrawidelock_dfu_rx.h
# globally while CONFIG_ULTRAWIDELOCK_DFU_RECEIVER=y).
if(CONFIG_ULTRAWIDELOCK_DFU_RECEIVER)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dfu/dfu_ble_zephyr.c)
endif()
