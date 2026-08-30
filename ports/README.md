# Platform ports

Thin platform backends. Each port may name exactly one operating system or
framework.

| Port | Integration model |
|---|---|
| [`zephyr/`](zephyr/) | one Zephyr module providing Bluetooth, DFU, device, storage, shell, and OSAL backends |
| [`esp32/`](esp32/) | ESP-IDF components selected through component dependencies |
| [`freertos-nrf52833/`](freertos-nrf52833/) | standalone DWM3001CDK port on the Qorvo board/UWB base, with a custom upstream OpenThread radio integration |

A port translates a platform API into a contract declared by a portable module,
then returns control to the shared implementation. Product policy and protocol
behavior do not belong here.

The host OSAL backend is under `tests/host/port/`: it also serves as the test
fake.

[`PORTING.md`](../PORTING.md) has the five-seam chipset contract and the
integration flow.
