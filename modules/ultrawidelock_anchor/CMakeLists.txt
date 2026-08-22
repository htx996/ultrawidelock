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

# Every source below comes from a role manifest in roles/ — the same lists
# ports/esp32/components/ultrawidelock_anchor and tests/host/sources.sh read. The Kconfig
# tiers stay here; only the membership of each tier moved into the manifests.
set(ROLES ${CMAKE_CURRENT_SOURCE_DIR}/roles)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ultrawidelock_roles.cmake)

if(CONFIG_ULTRAWIDELOCK_ANCHOR)

  ultrawidelock_role_sources(${ROLES}/anchor.list ANCHOR_SRCS)

  zephyr_library_named(ultrawidelock_anchor)
  zephyr_library_sources(${ANCHOR_SRCS})
  # The wire codec carries BOTH message families -- WV2 witness reports and WV3
  # anchor reports share one version byte, so one file owns it -- and the sealed
  # link needs it whichever consumer is enabled. The advertiser picker and the
  # label core are witness-only and stay behind the witness gate, so an
  # anchor-only image compiles none of them. ULTRAWIDELOCK_ANCHOR alone must
  # still grow no image at all.
  if(CONFIG_ULTRAWIDELOCK_WITNESS_CODEC OR CONFIG_ULTRAWIDELOCK_ANCHOR_MSG)
    ultrawidelock_role_sources(${ROLES}/anchor_msg.list ANCHOR_MSG_SRCS)
    zephyr_library_sources(${ANCHOR_MSG_SRCS})
  endif()
  if(CONFIG_ULTRAWIDELOCK_WITNESS_CODEC)
    ultrawidelock_role_sources(${ROLES}/witness_codec.list WITNESS_CODEC_SRCS)
    zephyr_library_sources(${WITNESS_CODEC_SRCS})
  endif()
  if(CONFIG_ULTRAWIDELOCK_INSIDE_LATCH)
    ultrawidelock_role_sources(${ROLES}/inside_latch.list INSIDE_LATCH_SRCS)
    zephyr_library_sources(${INSIDE_LATCH_SRCS})
  endif()
  # The AES-CCM envelope, gated on the link that uses it. Nested inside
  # ULTRAWIDELOCK_ANCHOR because every path to SEALED_LINK goes through it:
  # ANCHOR_LINK depends on ANCHOR outright, and WITNESS_LINK_OT depends on
  # INSIDE_LATCH, which lives inside the same `if ULTRAWIDELOCK_ANCHOR`.
  # apps/satellite is the one consumer that is ANCHOR_MSG without ANCHOR, and
  # it reads roles/seal.list from its own CMakeLists for exactly that reason.
  if(CONFIG_ULTRAWIDELOCK_SEALED_LINK)
    ultrawidelock_role_sources(${ROLES}/seal.list SEAL_SRCS)
    zephyr_library_sources(${SEAL_SRCS})
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
  ultrawidelock_role_sources(${ROLES}/anchor_msg.list ANCHOR_MSG_SRCS)
  zephyr_library_named(ultrawidelock_anchor_msg)
  zephyr_library_sources(${ANCHOR_MSG_SRCS})
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
endif()
