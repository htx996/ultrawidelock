# Satellite responder on ESP32 + DWM3000EVB — the ESP-IDF twin of apps/satellite.
#
# Same engine as the lock's ranging half, told its session keys instead of
# negotiating them: no BLE, no credential auth, no Matter. What arrives over the
# sealed link (ESP-NOW here, Thread on the nRF pair) is a URSK and a ranging
# config; what goes back is one sealed WV3 report per accepted range.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../ports/esp32/components")

# TWO RESPONDERS, AND THIS BOARD IS THE SECOND. Set before project.cmake so the
# engine component picks them up. The count is baked into the RangingConfiguration
# SaltedHash the phone independently recomputes, so a board built with the wrong
# one derives a different STS and decodes nothing — which on a bench looks like a
# radio that is not there rather than a configuration error.
set(ULTRAWIDELOCK_NUM_RESPONDERS 2)
set(ULTRAWIDELOCK_RESPONDER_INDEX 1)

# ONLY what main pulls in. EXTRA_COMPONENT_DIRS above makes every component in
# ports/esp32/components discoverable, and ESP-IDF builds all it discovers
# unless told otherwise -- which here drags in ultrawidelock_ble and fails on
# NimBLE headers, because this board has BLE switched off and no use for it. The
# dependency tree of `main` is exactly the right answer: the engine, the sealed
# link, and nothing that authenticates a credential.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ultrawidelock_satellite)
