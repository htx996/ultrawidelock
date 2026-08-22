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
#
# The source set is roles/*.list -- the same manifests
# ports/esp32/components/ultrawidelock_anchor and tests/host/sources.sh read, so the two
# ports and the suite that is this module's whole correctness story cannot
# disagree about which files a role contains.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ultrawidelock_roles.cmake)
ultrawidelock_role_sources(${CMAKE_CURRENT_SOURCE_DIR}/roles/anchor_core.list ANCHOR_CORE_SRCS)
ultrawidelock_role_sources(${CMAKE_CURRENT_SOURCE_DIR}/roles/wire_codec.list ANCHOR_CODEC_SRCS)
ultrawidelock_role_sources(${CMAKE_CURRENT_SOURCE_DIR}/roles/witness.list ANCHOR_WITNESS_SRCS)
ultrawidelock_role_sources(${CMAKE_CURRENT_SOURCE_DIR}/roles/latch.list ANCHOR_LATCH_SRCS)

if(CONFIG_ULTRAWIDELOCK_ANCHOR)

  zephyr_library_named(ultrawidelock_anchor)
  zephyr_library_sources(${ANCHOR_CORE_SRCS})
  # The wire codec carries BOTH message families -- WV2 witness reports and WV3
  # anchor reports share one version byte, so one file owns it -- and the sealed
  # link needs it whichever consumer is enabled. The advertiser picker and the
  # label core are witness-only and stay behind the witness gate, so an
  # anchor-only image compiles none of them. ULTRAWIDELOCK_ANCHOR alone must
  # still grow no image at all.
  if(CONFIG_ULTRAWIDELOCK_WITNESS_CODEC OR CONFIG_ULTRAWIDELOCK_ANCHOR_MSG)
    zephyr_library_sources(${ANCHOR_CODEC_SRCS})
  endif()
  if(CONFIG_ULTRAWIDELOCK_WITNESS_CODEC)
    zephyr_library_sources(${ANCHOR_WITNESS_SRCS})
  endif()
  if(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
    zephyr_library_sources(${ANCHOR_LATCH_SRCS})
  endif()
  # The LIS2DH12 backend of ultrawidelock_slam_hw.h (CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM) is the one
  # file that was not portable integer logic; it compiles from ports/zephyr
  # (ultrawidelock_zephyr_port) under that same gate.
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

endif()

# The wire codec alone, for builds that are not ULTRAWIDELOCK_ANCHOR at all. The
# satellite encodes a report and fuses nothing -- it has no geometry, no verdict
# and no second distance to compare against -- so requiring the whole fusion
# module to send its own measurement would be backwards.
if(CONFIG_ULTRAWIDELOCK_ANCHOR_MSG AND NOT CONFIG_ULTRAWIDELOCK_ANCHOR)
  zephyr_library_named(ultrawidelock_anchor_msg)
  zephyr_library_sources(${ANCHOR_CODEC_SRCS})
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
