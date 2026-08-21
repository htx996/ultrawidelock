# Applications

`apps/` contains the UltraWideLock products and the product ports being brought
up alongside them. Each directory owns the product-specific configuration,
wiring, entry point, and build overlays.
Portable protocol code remains in `modules/`, while OS and chipset glue remains
in `ports/`.

| Application | Primary build command | Output root |
|---|---|---|
| [`dwm3001cdk-lock/`](dwm3001cdk-lock/) | `make build` | `build/cdk-matter/` |
| [`dwm3001cdk-lock-freertos/`](dwm3001cdk-lock-freertos/) | `make freertos-port-test` (foundation only) | `build/freertos-nrf52833-host/` |
| [`nrf5340dk-lock/`](nrf5340dk-lock/) | `make nrf-build` | `build/nrf5340dk/` |
| [`nrf5340dk-satellite/`](nrf5340dk-satellite/) | `make sat-build SAT_THREAD=1` | `build/satellite-<board>-thread/` |
| [`esp32-matter-lock/`](esp32-matter-lock/) | `make esp-build APP=matter-lock TARGET=esp32s3` | `build/esp32-matter-lock-esp32s3/` |

These directories are product front ends, not alternate copies of the shared
stack. New reusable behavior belongs in `modules/`; new platform integration
belongs in `ports/`.
