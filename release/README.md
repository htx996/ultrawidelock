# Release bundle support

Documentation templates and flash helpers packaged with published firmware,
consumed by `make release`, `make nrf-release` and `make esp-release`.

Each board directory holds:

- `README.txt`, the shortest release summary.
- `FLASH.md`, board-specific wiring and flashing.
- `flash.sh` for that release image.

Generated bundles are written below `build/release/`.
