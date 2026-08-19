# nRF52840 BLE witness. One image; role is provisioned, not built in.
cmake_minimum_required(VERSION 3.20.0)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# The witness shares the lock's window accumulator and wire codec, so both ends
# of the link are the same host-tested code rather than two implementations of
# one format that agree until they do not.
list(APPEND ZEPHYR_EXTRA_MODULES
	"${REPO_ROOT}/ports/zephyr"
	"${REPO_ROOT}/modules/ultrawidelock_anchor")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ultrawidelock_ble_witness)

target_sources(app PRIVATE src/main.c)
