# ESP32 port

This directory contains ESP-IDF components that connect UltraWideLock's portable
modules to ESP-IDF, NimBLE, NVS, USB, and DW3000 hardware services.

Applications add `ports/esp32/components/` to `EXTRA_COMPONENT_DIRS`. ESP-IDF
then builds only the components named by another component's dependency list.
Each component carries local Component Manager metadata in `idf_component.yml`.

Key component groups are:

- `ultrawidelock_port` for the ESP32 OSAL backend.
- `ultrawidelock_uwb` for DW3000 hardware and SPI integration.
- `ultrawidelock_ble`, `ultrawidelock_ble_central`, and `ultrawidelock_reader` for BLE and storage glue.
- `ultrawidelock_crypto` and `ultrawidelock_device` for provider selection and role assembly.
- `ultrawidelock_anchor` for two-anchor geometry and fusion. It translates nothing: the
  module needs no framework, so the component is only the role manifests
  wrapped for ESP-IDF. No app names it yet.
- `piv_ccid` for the optional USB PIV interface.

Portable protocol behavior belongs in `modules/`; these components should stay
limited to framework and hardware translation.
