# ESP32 port

ESP-IDF components connecting the portable modules to ESP-IDF, NimBLE, NVS, USB
and DW3000 services. Applications add `ports/esp32/components/` to
`EXTRA_COMPONENT_DIRS`; ESP-IDF then builds only what another component's
dependency list names. Each component carries `idf_component.yml`.

- `ultrawidelock_port`, the ESP32 OSAL backend.
- `ultrawidelock_uwb`, DW3000 hardware and SPI.
- `ultrawidelock_ble`, `ultrawidelock_ble_central`, `ultrawidelock_reader`, BLE
  and storage glue.
- `ultrawidelock_crypto`, `ultrawidelock_device`, provider selection and role
  assembly.
- `ultrawidelock_satlink`, the sealed satellite link over an ESP-NOW carrier.
- `ultrawidelock_anchor`, two-anchor geometry and fusion. The module needs no
  framework, so the component is the role manifests wrapped for ESP-IDF.
- `piv_ccid`, the optional USB PIV interface.

Portable protocol behavior belongs in `modules/`.
