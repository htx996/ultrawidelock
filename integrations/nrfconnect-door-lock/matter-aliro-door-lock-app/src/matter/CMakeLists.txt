#
# Copyright (c) 2025 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

# Enable GNU STD support.
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)

# NORDIC SDK APP START

# Include all source files that are located in the Matter common directory.
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)

if(NOT CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH)
  if(CONFIG_DOOR_LOCK_BLE_UWB)
    # Two data models, one seam: upstream already selects the ZAP file by
    # variable, so the Home Assistant variant is a directory rather than the
    # pair of ordered patches it used to be. See CONFIG_ULTRAWIDELOCK_HA.
    if(CONFIG_ULTRAWIDELOCK_HA)
      set(CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH "${APPLICATION_CONFIG_DIR}/src/matter/zap_uwb_ha/lock.zap")
    else()
      set(CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH "${APPLICATION_CONFIG_DIR}/src/matter/zap_uwb/lock.zap")
    endif()
  else()
    set(CONFIG_NCS_SAMPLE_MATTER_ZAP_FILE_PATH "${APPLICATION_CONFIG_DIR}/src/matter/zap_nfc/lock.zap")
  endif()
endif()

# Include Data Model utils
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)

include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
ncs_get_zap_parent_dir(ZAP_PARENT_DIR)
zephyr_include_directories(${ZAP_PARENT_DIR})

target_include_directories(app PRIVATE
  .
  ${ZAP_PARENT_DIR}
)

target_sources(app PRIVATE
  app_task.cpp
  bolt_lock_manager.cpp
  init.cpp
  time_sync_delegate.cpp
  zcl_callbacks.cpp
)

# Do not treat warnings as errors while the variable may be uninitialized for this sample data model.
# In the door-lock-server implementation, there is a warning that the C++ "optional"
# variable may be uninitialized, but actually, it is wrongly interpreted by the Zephyr toolchain.
target_compile_options(matter-data-model PRIVATE -Wno-error=maybe-uninitialized)

if(CONFIG_NCS_DOOR_LOCK_APP_EXTENSIONS_MATTER)
  include(${ZEPHYR_NCS_DOOR_LOCK_APP_EXTENSIONS_MODULE_DIR}/matter/matter_extensions.cmake)
  ncs_add_extended_data_model_sources(app matter-data-model)
  ncs_configure_extended_data_model()
else()
  ncs_configure_data_model()
endif()

# NORDIC SDK APP END
