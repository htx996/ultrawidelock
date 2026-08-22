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
| [`satellite/`](satellite/) | `make sat-build SAT_THREAD=1` | `build/satellite-<board>-thread/` |
| [`esp32-matter-lock/`](esp32-matter-lock/) | `make esp-build APP=matter-lock TARGET=esp32s3` | `build/esp32-matter-lock-esp32s3/` |

A directory carries a board prefix when it is bound to that board, and drops it
when it is not. `dwm3001cdk-lock/` has one board overlay, a `pm_static.yml`
written for the nRF52833's flash layout, and signing keys; `nrf5340dk-lock/`
holds no sources at all, layering our modules and patches onto the Nordic Aliro
app fetched into `workspace/`, which `scripts/nrf5340dk-build.sh` pins to one
board. `satellite/` is the one that is genuinely board-neutral: one `src/` with no
board `#ifdef`, two overlays under `boards/`, and `SAT_BOARD` choosing between
them. Adding a third board there is an overlay and a size baseline, not a
directory.

These directories are product front ends, not alternate copies of the shared
stack. New reusable behavior belongs in `modules/`; new platform integration
belongs in `ports/`.
