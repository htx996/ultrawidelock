# The DWM3001CDK lock image.
#
# The binding constraint on this port is the 512 KB flash and 128 KB RAM budget.
# The current Zephyr production oracle fits at 120,740 B of RAM; the FreeRTOS
# image is still measured independently because its task and storage layout is
# different. Every layer added below the application gets measured here.

cmake_minimum_required(VERSION 3.20)

project(dwm3001cdk_lock_freertos C ASM)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../ports/freertos-nrf52833" port)

add_executable(dwm3001cdk-lock-freertos
  "${CMAKE_CURRENT_LIST_DIR}/src/main.c"
  "${CMAKE_CURRENT_LIST_DIR}/src/grant.c"
  "${CMAKE_CURRENT_LIST_DIR}/src/status_led.c"
)
target_link_libraries(dwm3001cdk-lock-freertos PRIVATE ultrawidelock_freertos_port)
target_compile_options(dwm3001cdk-lock-freertos PRIVATE -Wall -Wextra -Werror)

ultrawidelock_freertos_image(dwm3001cdk-lock-freertos)
