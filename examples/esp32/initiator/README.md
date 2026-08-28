# ESP32-S3 initiator example

The ESP32-S3 bench peer for a credential reader: the BLE central and
user-device transport path. BLE-only; no UWB initiator.

Build from the repository root with an installed ESP-IDF:

```sh
make esp-build APP=initiator TARGET=esp32s3
```

`make esp-flash APP=initiator TARGET=esp32s3` flashes, `make esp-monitor
APP=initiator TARGET=esp32s3` opens the console. No esp-matter required.
