# two-anchor geometry and fusion as an ESP-IDF component — the mirror of the
# Zephyr module in modules/ultrawidelock_anchor. Sources come from the role manifests in
# modules/ultrawidelock_anchor/roles/ (the same lists the Zephyr module and tests/host
# read), so the three consumers cannot drift about which files a role holds.
#
# REQUIRES nothing, deliberately: every source here includes only its own
# headers plus <errno.h>/<string.h>. Integer arithmetic over caller-owned
# structs — no allocation, no threads, no platform call — which is exactly why
# the module ports without a seam of its own.
#
# All four roles compile together, where the Zephyr module puts each behind its
# own Kconfig. There is no ESP-IDF Kconfig for the anchor yet and inventing four
# would be four symbols nothing sets. Nothing costs an image anything either
# way: no app REQUIREs this component, so ESP-IDF does not build it at all. The
# day one does, that app owes the per-role gating and a measured RAM budget,
# the same debt CONFIG_ULTRAWIDELOCK_ANCHOR carries on the Zephyr side.
#
# Which also means no gate compiles these sources on this port yet. The host
# suite is what proves they build and behave; this file exists so that turning
# the anchor on for an ESP app is a REQUIRES line rather than a new source list.
#
# The LIS2DH12 backend of ultrawidelock_slam_hw.h has no ESP-IDF implementation: an app
# that wants CONFIG_ULTRAWIDELOCK_ANCHOR_SLAM on this port owes one, the way ports/zephyr
# supplies the Zephyr half.

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(ANCHOR "${REPO_ROOT}/modules/ultrawidelock_anchor")

include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")
ultrawidelock_role_sources("${ANCHOR}/roles/anchor_core.list" ULTRAWIDELOCK_ANCHOR_CORE_SRCS)
ultrawidelock_role_sources("${ANCHOR}/roles/wire_codec.list" ULTRAWIDELOCK_ANCHOR_CODEC_SRCS)
ultrawidelock_role_sources("${ANCHOR}/roles/witness.list" ULTRAWIDELOCK_ANCHOR_WITNESS_SRCS)
ultrawidelock_role_sources("${ANCHOR}/roles/latch.list" ULTRAWIDELOCK_ANCHOR_LATCH_SRCS)

idf_component_register(
	SRCS
		${ULTRAWIDELOCK_ANCHOR_CORE_SRCS}
		${ULTRAWIDELOCK_ANCHOR_CODEC_SRCS}
		${ULTRAWIDELOCK_ANCHOR_WITNESS_SRCS}
		${ULTRAWIDELOCK_ANCHOR_LATCH_SRCS}
	INCLUDE_DIRS "${ANCHOR}/include"
)
