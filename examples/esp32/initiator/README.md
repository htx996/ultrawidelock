# ESP32-S3 initiator example

The ESP32-S3 bench peer for a credential reader: the BLE central and user-device
transport path, BLE only.

Build from the repository root. ESP-IDF alone is enough:

```sh
make esp-build APP=initiator TARGET=esp32s3
```

`make esp-flash APP=initiator TARGET=esp32s3` flashes, `make esp-monitor
APP=initiator TARGET=esp32s3` opens the console.
