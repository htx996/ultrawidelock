# Satellite responder (docs/second-anchor.md stage B): the CRED-tier CCC
# ranging engine minus BLE credential auth. It is a lock that is told the
# session keys (`sat join` on the shell) instead of negotiating them, and it
# transmits Response_1 in the slot the phone reserves when the lock advertises
# ULTRAWIDELOCK_NUM_RESPONDERS=2.
#
# Same module set as the lock's ranging half, same standalone layout as
# examples/zephyr/anchor: no sysbuild bootloader, flashed over SWD, never
# updated over the air. ultrawidelock_matter is absent, and ultrawidelock_cred is present only
# as the owner of the image's primitive-provider target: this node never
# authenticates a credential.
cmake_minimum_required(VERSION 3.20.0)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

list(APPEND ZEPHYR_EXTRA_MODULES
	"${REPO_ROOT}/modules/ultrawidelock_cred"
	"${REPO_ROOT}/modules/ultrawidelock_uwb"
	"${REPO_ROOT}/modules/ultrawidelock_dw3000"
	"${REPO_ROOT}/modules/ultrawidelock_anchor"
	"${REPO_ROOT}/ports/zephyr")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ultrawidelock_satellite)

# Same guard examples/zephyr/anchor/CMakeLists.txt carries, for the same reason:
# recovering debug access costs a mass erase, and a bench image is exactly the
# kind of thing that would inherit the setting from a stray overlay unnoticed.
if(CONFIG_NRF_APPROTECT_LOCK OR CONFIG_NRF_SECURE_APPROTECT_LOCK)
	message(FATAL_ERROR
		"APPROTECT lock is enabled for this image. Refusing to build.\n"
		"  CONFIG_NRF_APPROTECT_LOCK        = ${CONFIG_NRF_APPROTECT_LOCK}\n"
		"  CONFIG_NRF_SECURE_APPROTECT_LOCK = ${CONFIG_NRF_SECURE_APPROTECT_LOCK}\n"
		"See apps/dwm3001cdk-lock/sysbuild.conf for why these boards never lock.")
endif()

target_sources(app PRIVATE src/main.c)

# main initializes the primitive provider even without Thread, because the UWB
# responder uses it too. The credential module owns exactly one provider TU for
# the image; direct-sourcing the old combined crypto role here duplicated it.
target_link_libraries(app PRIVATE ultrawidelock_prim)
target_include_directories(app PRIVATE
	"${REPO_ROOT}/modules/ultrawidelock_cred/include")

include("${REPO_ROOT}/cmake/ultrawidelock_roles.cmake")

# The sealed link needs Thread. Without it the board still ranges and still
# prints, it simply has nobody to tell.
target_sources_ifdef(CONFIG_OPENTHREAD app PRIVATE src/anchor_link.c)

# The seal and link decisions are shared roles: ONE AES-CCM envelope and ONE
# replay/counter implementation. Read them here rather than leave them to the
# module because this image is ANCHOR_MSG without ANCHOR -- it encodes a report
# and fuses nothing -- and the module's copies are gated on the ANCHOR tier this
# node deliberately does not have.
if(CONFIG_OPENTHREAD)
	ultrawidelock_role_sources("${REPO_ROOT}/modules/ultrawidelock_anchor/roles/seal.list" SEAL_SRCS)
	ultrawidelock_role_sources("${REPO_ROOT}/modules/ultrawidelock_anchor/roles/link.list" LINK_SRCS)
	target_sources(app PRIVATE ${SEAL_SRCS} ${LINK_SRCS})
endif()
