# nRF5340 DK lock

This application is the nRF5340 DK implementation of UltraWideLock. It combines
UWB approach unlock, NFC tap, and Matter over Thread using the Nordic door-lock
and access-control application as its product shell.

The application itself lives in
[`integrations/nrfconnect-door-lock/matter-aliro-door-lock-app/`](../../integrations/nrfconnect-door-lock/matter-aliro-door-lock-app/):
Nordic's, at a pinned revision, carried here as source we change directly.
Everything it builds against -- the add-on's Aliro subsystem, NCS, Zephyr,
Matter -- is still fetched into the ignored `workspace/` link, and the four
patches that still go into that tree are alongside it in
[`integrations/nrfconnect-door-lock/`](../../integrations/nrfconnect-door-lock/).

This tracked directory owns the UltraWideLock build launcher and product
overlays, and no sources of its own.

## Build

```sh
make bootstrap
make dfu-key
make nrf-build
```

The merged image is written below `build/nrf5340dk/`. Other useful commands are:

| Command | Purpose |
|---|---|
| `make nrf-rebuild` | Force a pristine build |
| `make nrf-selftest` | Build the one-shot UWB self-test |
| `make nrf-flash` | Flash the application image |
| `make nrf-flash-erase` | Erase and flash all required images |
| `make nrf-term` | Open the serial log and shell |

Use the erase-and-flash target after a network-core change. It removes existing
commissioning and credential storage.

## Contents

- `scripts/nrf5340dk-build.sh` resolves the fetched workspace, build options,
  and product checks. It lives in `scripts/` with the other build programs; this
  directory holds only what the product owns.
- `overlays/` contains product-owned Kconfig, devicetree, partition, and
  sysbuild inputs.
