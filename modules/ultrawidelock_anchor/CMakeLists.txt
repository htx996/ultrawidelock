# Two-anchor geometry and fusion as a Zephyr module.
#
# Contributes nothing unless CONFIG_ULTRAWIDELOCK_ANCHOR=y, and nothing sets that yet: the
# logic is finished and host-tested ahead of the hardware, so every existing
# image stays byte-identical. Wiring up the satellite transport is what
# turns it on, and that step owes a RAM budget against the CDK Matter image.
#
# Pure integer arithmetic over caller-owned structs. No allocation, no threads,
# no platform dependency -- so it links anywhere and the host suite is the whole
# correctness story until real distances exist.

if(CONFIG_ULTRAWIDELOCK_ANCHOR)

  zephyr_library_named(ultrawidelock_anchor)
  zephyr_library_sources(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_door.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_fusion.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_report.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_satellite.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_side.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_side_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_slam.c)
  # The inside veto and its witness link. Gated again inside the module
  # Kconfig: ULTRAWIDELOCK_ANCHOR alone must not grow the image.
  if(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
    zephyr_library_sources(
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_latch.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_witness_core.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_witness_msg.c
      ${CMAKE_CURRENT_SOURCE_DIR}/src/ultrawidelock_witness_pick.c)
  endif()
  # The LIS2DH12 backend of ultrawidelock_slam_hw.h (CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM) is the one
  # file that was not portable integer logic; it compiles from ports/zephyr
  # (ultrawidelock_zephyr_port) under that same gate.
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

endif()
