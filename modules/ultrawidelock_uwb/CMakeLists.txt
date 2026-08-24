# UltraWideLock UWB engine as a Zephyr module for the Nordic door-lock add-on.
# The CCC STS substitution rides include/uwb_seam.h: every caller reaches the
# radio through ultrawidelock_uwb_arm_rx(), which programs the CCC key/IV on every RX-arm,
# and its three siblings. tests/tooling/uwb_seam_check.sh keeps the seam intact.
# The DS-TWR responder runs on modules/ultrawidelock_dw3000.
# Contributes nothing unless CONFIG_ULTRAWIDELOCK_UWB=y (stock build byte-identical).

if(CONFIG_ULTRAWIDELOCK_UWB)

  set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set(ROLES ${CMAKE_CURRENT_SOURCE_DIR}/roles)

  # Every shared source below comes from a role manifest in roles/ — the same
  # lists ports/esp32/components/ultrawidelock_uwb, examples/zephyr/nrf5340dk-initiator and
  # tests/host/sources.sh read. The Kconfig tiers stay here; only the membership
  # of each tier moved into the manifests.
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ultrawidelock_roles.cmake)
  ultrawidelock_role_sources(${ROLES}/base_driver.list UWB_BASE_DRIVER_SRCS)
  ultrawidelock_role_sources(${ROLES}/base_engine.list UWB_BASE_ENGINE_SRCS)

  zephyr_library_named(ultrawidelock_uwb)
  # ds_twr.c sits at the BASE tier, not under the responder, because the
  # anchor-to-anchor example ranges another board with no CCC engine in
  # its image and needs the same estimator the credential responder uses.
  zephyr_library_sources(
    ${UWB_BASE_DRIVER_SRCS}
    ${UWB_BASE_ENGINE_SRCS})

  if(CONFIG_ULTRAWIDELOCK_UWB_RESPONDER)
    # The responder runs entirely on modules/ultrawidelock_dw3000 in ccc_shim_rx.c
    # (ccc_prepoll_listen). The codec uses only qmalloc/qcalloc/qfree + qrtc_get_us,
    # provided as thin Zephyr wrappers by src/facade/ultrawidelock_alloc.h.
    ultrawidelock_role_sources(${ROLES}/responder_driver.list UWB_RESP_DRIVER_SRCS)
    ultrawidelock_role_sources(${ROLES}/responder_engine.list UWB_RESP_ENGINE_SRCS)
    zephyr_library_sources(
      ${UWB_RESP_DRIVER_SRCS}
      ${UWB_RESP_ENGINE_SRCS}
    )

    if(CONFIG_ULTRAWIDELOCK_CRED)
      # The add-on hands us the plaintext URSK in _ConfigureRangingSession, so we
      # only need the CCC STS engine (ccc_keys = key schedule/MAC/session config,
      # ccc_engine = STS generation + RX-arm shim + facade).
      ultrawidelock_role_sources(${ROLES}/ccc_keys.list UWB_CCC_KEYS_SRCS)
      ultrawidelock_role_sources(${ROLES}/ccc_engine.list UWB_CCC_ENGINE_SRCS)
      zephyr_library_sources(
        ${UWB_CCC_KEYS_SRCS}
        ${UWB_CCC_ENGINE_SRCS}
        # Diagnostic (removable): tallies on-air RX events. Single-consumer
        # literal (allowlisted as NOT_MANIFESTED in
        # tests/tooling/port_purity_check.sh): the ESP port omits it and stubs
        # the two seams it would supply.
        ${SRC}/driver/uwb_rxdiag.c)

      # The CCC/Matter AES-ECB compatibility symbol is now only a thin adapter;
      # all platform crypto reaches the provider behind ultrawidelock_prim.
      ultrawidelock_role_sources(${ROLES}/crypto_prim.list UWB_CRYPTO_PRIM_SRCS)
      zephyr_library_sources(${UWB_CRYPTO_PRIM_SRCS})
      zephyr_library_include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../ultrawidelock_cred/include)
      zephyr_library_link_libraries(ultrawidelock_prim)

      # The PRETTY log backend (ultrawidelock_logfmt/ultrawidelock_logquiet, CONFIG_ULTRAWIDELOCK_PRETTY_SHELL)
      # and the `ultrawidelock` console (ultrawidelock_shell, CONFIG_ULTRAWIDELOCK_UWB_SHELL) are Zephyr
      # glue and compile from ports/zephyr (ultrawidelock_zephyr_port) under the same
      # gates; their Kconfig symbols stay in this module's Kconfig.

      # Credential UWB ranging-setup adapter: composes the M1-M4 handshake the add-on's
      # ultrawidelock_stack drives via the facade, over the cherry_ccc_* seam that
      # cherry_ccc_shim.c implements on our MAC (fira_session.c + ccc_shim).
      ultrawidelock_role_sources(${ROLES}/ultrawidelock_adapter.list UWB_ULTRAWIDELOCK_ADAPTER_SRCS)
      ultrawidelock_role_sources(${ROLES}/ultrawidelock_codec.list UWB_ULTRAWIDELOCK_CODEC_SRCS)
      zephyr_library_sources(
        ${UWB_ULTRAWIDELOCK_ADAPTER_SRCS}
        ${UWB_ULTRAWIDELOCK_CODEC_SRCS})
      # Diagnostic: one-shot boot self-test of the UWB init path (gated, default
      # n). Single-consumer literal (allowlisted as NOT_MANIFESTED in
      # tests/tooling/port_purity_check.sh); no other port builds it.
      if(CONFIG_ULTRAWIDELOCK_UWB_SELFTEST)
        zephyr_library_sources(${SRC}/driver/uwb_selftest.c)
      endif()

      # CIA/CIR diagnostics: per-reception first-path summary + windowed CIR tap dump
      # (gated, default n; the RX-shim call sites no-op when unset).
      if(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG)
        ultrawidelock_role_sources(${ROLES}/diag_cir.list UWB_DIAG_CIR_SRCS)
        zephyr_library_sources(${UWB_DIAG_CIR_SRCS})
      endif()
      # Flight recorder: record a live walk-up into a RAM ring for host replay
      # (gated, default n; the ccc_shim_rx.c call sites no-op when unset).
      if(CONFIG_ULTRAWIDELOCK_FLIGHT_RECORDER)
        ultrawidelock_role_sources(${ROLES}/flight_recorder.list UWB_FLIGHT_SRCS)
        zephyr_library_sources(${UWB_FLIGHT_SRCS})
      endif()
    endif()

    # ultrawidelock_alloc.h (in ${SRC}/facade, on the path below) covers the qmalloc/qrtc
    # surface the codec uses. deca_device_api.h comes from the dw3000 module.
  endif()

  # Public contracts are global; private layer folders stay on this library.
  # The flat #include "sibling.h" style still resolves across private layers.
  # The platform contract (ultrawidelock_port.h / ultrawidelock_log.h) lives in modules/ultrawidelock_port;
  # the shell seam header (ultrawidelock_shell.h) moved to ports/zephyr/shell.
  zephyr_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../ultrawidelock_port/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
  zephyr_library_include_directories(
    ${SRC}/driver ${SRC}/fira ${SRC}/ccc ${SRC}/facade ${SRC}/cred)

endif()
