# Examples

Independently buildable firmware demonstrating one role or bench setup, built
from the same `modules/` and `ports/` as the products under `apps/`.

| Example | Purpose | Build command |
|---|---|---|
| [`zephyr/anchor/`](zephyr/anchor/) | Two-board DS-TWR anchor bench | `make anchor-build` |
| [`zephyr/ble-witness/`](zephyr/ble-witness/) | Inside/outside BLE witness dongle, role set at install | `make witness-build` |
| [`zephyr/nrf5340dk-initiator/`](zephyr/nrf5340dk-initiator/) | nRF5340 DK credential initiator | `make nrf-init-build` |
| [`esp32/reader/`](esp32/reader/) | Standalone ESP32 credential reader | `make esp-build APP=reader` |
| [`esp32/initiator/`](esp32/initiator/) | ESP32-S3 BLE initiator peer | `make esp-build APP=initiator TARGET=esp32s3` |
| [`esp32/satellite/`](esp32/satellite/) | Second-anchor responder on the ESP32 tier | `make esp-build APP=satellite` |
| [`cmake/consumer/`](cmake/consumer/) | Installed C API and TLV package consumer | `make sdk-check` |
