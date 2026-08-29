# nRF5340 DK initiator example

The credential user-device role on an nRF5340 DK, as a bench peer: it scans for
a reader, negotiates the BLE transport, and drives the initiator-side UWB
exchange.

Prepare the NCS workspace and build:

```sh
make bootstrap
make nrf-init-build
```

Flash both application and network cores with:

```sh
make nrf-init-flash
```

Output lands in `build/nrf5340dk-initiator/`, built from the same portable UWB
modules and Zephyr port as the products.
