# Release bundle support

Documentation templates and flash helpers packaged with published firmware.
Source material for `make release`, `make nrf-release` and `make esp-release`,
not a build-output directory.

Each board directory holds:

- `README.txt`, the shortest release summary.
- `FLASH.md`, board-specific wiring and flashing.
- `flash.sh` for that release image.

Generated bundles are written below `build/release/`.
