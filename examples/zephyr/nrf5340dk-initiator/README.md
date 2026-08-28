# nRF5340 DK initiator example

The credential user-device role on an nRF5340 DK: scans for a reader,
negotiates the BLE transport, drives the initiator-side UWB exchange. A bench
peer, not a lock.

Prepare the NCS workspace and build:

```sh
make bootstrap
make nrf-init-build
```

Flash both application and network cores with:

```sh
make nrf-init-flash
```

Output lands in `build/nrf5340dk-initiator/`, from the same portable UWB
modules and Zephyr port the products use.
