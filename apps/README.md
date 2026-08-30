# Applications

Product front ends. Portable protocol lives in `modules/`, OS and chipset glue
in `ports/`.

| Application | Primary build command | Output root |
|---|---|---|
| [`dwm3001cdk-lock/`](dwm3001cdk-lock/) | `make build` | `build/cdk-matter/` |
| [`dwm3001cdk-lock-freertos/`](dwm3001cdk-lock-freertos/) | `make freertos-build` | `build/freertos-nrf52833/` |
| [`nrf5340dk-lock/`](nrf5340dk-lock/) | `make nrf-build` | `build/nrf5340dk/` |
| [`satellite/`](satellite/) | `make sat-build SAT_THREAD=1` | `build/satellite-<board>-thread/` |
| [`esp32-matter-lock/`](esp32-matter-lock/) | `make esp-build APP=matter-lock TARGET=esp32s3` | `build/esp32-matter-lock-esp32s3/` |

A directory carries a board prefix when it is bound to that board.
`dwm3001cdk-lock/` holds one board overlay, a `pm_static.yml` for the nRF52833's
flash layout, and signing keys. `nrf5340dk-lock/` layers modules and patches onto
the Nordic Aliro app that `scripts/nrf5340dk-build.sh` fetches into `workspace/`.
`satellite/` is board-neutral: one `src/`, two overlays under `boards/`, and
`SAT_BOARD` choosing between them, so a third board costs an overlay and a size
baseline rather than a directory.
