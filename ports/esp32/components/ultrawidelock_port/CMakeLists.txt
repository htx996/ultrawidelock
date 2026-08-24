# The ESP-IDF backend of the ultrawidelock_port contract, mirror of ports/zephyr/osal.
# modules/ultrawidelock_port carries the headers; every .c that names an OS lives in the
# port tree for that OS, and this is the ESP-IDF one.
#
# Registered even though no ESP image calls the OSAL today: uwb_rxdiag and
# uwb_selftest are the callers and both are deliberately excluded from this
# port (tests/ports/esp32/verify_port.sh asserts they stay out). Before this
# component existed the backend was compiled by nothing at all, so it could rot
# unnoticed. Building it here costs an image nothing, because the linker drops
# what nobody calls.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS "osal_esp.c" "kv_nvs.c"
    INCLUDE_DIRS "${REPO_ROOT}/modules/ultrawidelock_port/include"
    # nvs_flash for kv_nvs.c, which is the ultrawidelock_kv.h backend on this
    # port. Same reasoning as the OSAL above: no image calls it yet, and the
    # linker drops what nobody calls, but compiling it here is what stops it
    # rotting between now and the first caller.
    REQUIRES freertos esp_timer nvs_flash)
