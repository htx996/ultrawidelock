# ESP32 Matter lock

The UltraWideLock reader plus an esp-matter door lock, on ESP32-S3, C5 or C6
with a DWM3000EVB.

## Build

Install ESP-IDF and esp-matter, then build from the repository root. The bench
builds against ESP-IDF v5.5.4 and esp-matter `93b1680`.

```sh
make esp-build APP=matter-lock TARGET=esp32s3
```

Override the default paths when the SDKs live elsewhere:

```sh
make esp-build APP=matter-lock TARGET=esp32s3 \
  IDF_EXPORT=/path/to/esp-idf/export.sh \
  ESP_MATTER_PATH=/path/to/esp-matter
```

The local Makefile forwards the same targets:

```sh
cd apps/esp32-matter-lock
make build TARGET=esp32s3
```

`make esp-flash APP=matter-lock TARGET=esp32s3` flashes, `make esp-monitor
APP=matter-lock TARGET=esp32s3` opens the console. Variants: `presence`,
`hamqtt`, `piv`; `make help` lists their targets.

## Contents

- `main/`, the Matter application, lock policy, shell and LED adapter.
- `sdkconfig.defaults*`, shared and target-specific ESP-IDF settings.
- `partitions*.csv`, flash layouts.
- Shared components: [`ports/esp32/`](../../ports/esp32/).
