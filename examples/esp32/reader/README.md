# ESP32 reader example

A standalone credential reader for ESP32-S3, C5 or C6 with a DWM3000EVB,
exercising the reader, provisioning, BLE and UWB paths without esp-matter.

Build from the repository root:

```sh
make esp-build APP=reader TARGET=esp32s3
```

The local Makefile forwards the same targets:

```sh
cd examples/esp32/reader
make build TARGET=esp32s3
```

`make esp-flash APP=reader TARGET=esp32s3` flashes, `make esp-monitor
APP=reader TARGET=esp32s3` opens the console. The optional `presence` variant
adds its diagnostics in a separate build tree.
