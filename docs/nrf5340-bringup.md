# nRF5340 bring-up

Parts on the bench to a healthy first boot; toolchain install is the Quick start
in the [repository README](../README.md).

## Parts

| Part | Role |
|---|---|
| nRF5340 DK | Host SoC: BLE + Matter and the ranging engine |
| DWM3000EVB (DW3110) | UWB radio, on the Arduino header (SPIM4) |
| X-NUCLEO-NFC12A1 (ST25R300) | NFC reader front end for tap (SPIM1) |

Pin assignments are in
[`../apps/nrf5340dk-lock/overlays/dw3000-nfc.overlay`](../apps/nrf5340dk-lock/overlays/dw3000-nfc.overlay),
the source of truth. [nrf5340-wiring.md](nrf5340-wiring.md) transcribes it into
both-ends tables with Arduino header positions and the 9 NFC jumpers.

## Before powering

* 3.3 V only, never 5 V; share a common ground with the DK.
* Check the DWM3000EVB **power-select jumper** first: wrong position means
  SPI fails silently, no device ID, a responder that never listens.

## Build, flash, console

```bash
make nrf-build
make nrf-flash-erase
make nrf-term
```

The image lands in `./build/nrf5340dk/merged.hex`; the console is on VCOM1
(VCOM0 is silent). The first flash and any net-core config change need the
erase; plain `make nrf-flash` otherwise.

## First-boot checks

* `make nrf-selftest` exercises the radio with no phone present, separating a
  wiring problem from a protocol one.
* On the shell: `ultrawidelock status`, `ultrawidelock chip` (DW3110 device ID over SPI),
  `ultrawidelock range`.
* `ultrawidelock factoryreset yes` erases the Matter fabrics, the credential reader identity,
  and every trusted phone key, then reboots. The `yes` is required. Unlike
  `matter device factoryreset` it survives into release images, where
  `prj_release.conf` turns off `CONFIG_CHIP_LIB_SHELL`.
* A healthy release boot is clean on the console and starts BLE advertising.

## Prove the unlock

Commission into Apple Home, then run the pass criteria in
[hardware-validation.md](hardware-validation.md): tap, pocketed approach
unlock, walk-away relock.

## If something fails

SPI and ranging failure modes: [troubleshooting.md](troubleshooting.md).
