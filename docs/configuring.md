# Configuring

Three layers: build options on the make command line, the Kconfig overlays behind
them, and runtime consoles on the running reader.

Bare make targets mean the DWM3001CDK, the primary board; the nRF5340 DK is
`nrf-` prefixed, the ESP32 `esp-`. `make` prints the grouped list; each target's
comment block in `mk/*.mk` is the authority.

## Build options (DWM3001CDK)

On the command line, e.g. `make build RELEASE=1 SMP=1`:

| Option | Effect |
|---|---|
| `PRISTINE=1` | force a from-scratch build. Supported option changes reconfigure in place; use this only when the cache itself must be replaced |
| `LTO=0` | opt out of link-time optimisation, which is on by default and worth 41,084 B. Use it when a stack trace has to name every frame |
| `RELEASE=1` | trade the 8 KB RTT ring for 7,168 B of RAM, and set errors-only logging to save 20,568 B of flash. Codegen is identical either way |
| `SMP=1` | add mcumgr over Bluetooth **and** over `uart0`, which is what nRF Device Manager and the browser both speak. `make build SMP=1` is a valid debug configuration and leaves 12,764 B free. `RELEASE=1` remains the shipping configuration |
| `IMAGE_VERSION=x.y.z` | the version the BOARD reports in its image list. Defaults to the repository's `VERSION` file. Left unset entirely, Zephyr's default is `0.0.0+0` and every board reports `v0.0.0.0`, which identifies nothing: the SHA-256 stays authoritative either way, but it is the version an operator actually reads |
| `DFU_LOG=1` | make the bootloader narrate what it does with a staged patch. Read it with MCUboot's own ELF, not the application's |
| `ANCHOR=1` | layer `overlay-anchor.conf`: the second-anchor geometry, the door-swing angle and the LIS2DH12 impact latch, plus the two DoorLockAlarm events those feed. Default off, and the default image is byte-identical without it. Every threshold it turns on is a placeholder; see below |
| `CDK_BUILD=<dir>` | which build directory `flash`, `flash-erase` and `monitor` mean. Default `build/cdk-matter` |
| `CDK_RTT_BUILD=<dir>` | point `monitor` at a different image without moving what the flash targets write |
| `CDK_KEY=<path>` | the image-signing key for this build only. Must be absolute. Defaults to `SIGN_KEY`, which both Zephyr ports share; `make release` uses it to point one build at a production key |
| `CDK_DEPLOYED=<hex>` | the record of what the board is running, which every delta is computed against |
| `OTA_NAME=<name>` | the advertised name `make dfu` and `make ota-smp` connect to |
| `FOTA_VERSION=<x.y.z>` | the version stamped into the file `make fota` leaves for a phone |
| `PREV_HEXES='a.hex …'` | `make ota-fan` builds one delta from each of these to the release build. They are the `zephyr.signed.hex` shipped in each earlier release bundle: a board running something not in that list has no over-the-air path at all |
| `CDK_OTA_DIR=<dir>` | where `make ota-fan` leaves the fan and `ota-index.json`. Defaults under `build/release/ota`, which is where `web/build.py` looks |
| `ESP_OTA_DIR=<dir>` | the same, for `make esp-ota`'s signed per-chip images |

`LTO=0` no longer fits the flash map: 446,380 B against a 433,664 B `app`
partition, so the build fails rather than ships. Derivation in
[`../apps/dwm3001cdk-lock/pm_static.yml`](../apps/dwm3001cdk-lock/pm_static.yml).

`IMAGE_VERSION` changes the image hash by design: the version sits in the MCUboot
header, which the SHA-256 TLV covers, so two builds differing only in version
are different images and the delta between them is real. The first build after
setting it needs the deployed record re-recorded; `make flash` and the `ota-*`
targets do that.

Serial recovery belongs to MCUboot, not the application: see
[`../apps/dwm3001cdk-lock/sysbuild/mcuboot.conf`](../apps/dwm3001cdk-lock/sysbuild/mcuboot.conf).
It accepts a **whole** image where everything else takes a delta, because the
slot is free while the application is not running: the only path onto a board
whose software does not boot. `CONFIG_BOOT_SERIAL_NO_APPLICATION=y`
means such a board already sits in recovery with nothing to press, and
`make ota-fan` publishes the whole image beside the deltas.

`make fota` and `make ota-smp` set `SMP=1 RELEASE=1` themselves and build in
their own directory: a board without SMP does not speak mcumgr, so a bare
`make`'s defaults would build the wrong image and diff the board against it.

## Kconfig overlays (DWM3001CDK)

In [`../apps/dwm3001cdk-lock`](../apps/dwm3001cdk-lock), selected by the options above:

- `overlay-thread.conf`: always applied by `make build`. The Matter node,
  OpenThread MTD/MED and SRP. `make reader` omits it, the only difference
  between the two images.
- `overlay-release.conf`, `overlay-smp.conf`, `overlay-lto.conf`: `RELEASE=1`,
  `SMP=1` and the default `LTO=1`. Later files win.

  `overlay-smp.conf` turns on two transports. Bluetooth serves nRF Device
  Manager and the flasher page's radio path. UART
  (`CONFIG_MCUMGR_TRANSPORT_UART`) binds `zephyr,uart-mcumgr`, which the board
  DTS points at `uart0`, the J-Link OB's VCOM: it enumerates as USB CDC-ACM and
  the application leaves it alone, its console being RTT. Both reach the same
  handler in
  [`ports/zephyr/dfu/dfu_smp_img.c`](../ports/zephyr/dfu/dfu_smp_img.c), so
  cable and radio get the same signature check and update window. UART transport
  costs **+2,408 B flash, +464 B RAM**.
- `overlays/uwb-selftest.conf`: the `make selftest` image; reads the DW3110's
  `DEV_ID` at boot and stops.
- `overlay-anchor.conf`: `ANCHOR=1`. Turns on `ULTRAWIDELOCK_ANCHOR` and the
  impact latch, and with them the DoorLockAlarm events in
  [`matter-door-lock-events.md`](matter-door-lock-events.md). Every number in it
  is a placeholder.
- `sysbuild/mcuboot.conf`: the bootloader's own configuration, a separate image
  that does not inherit the application's.

### Overlays no `make` target selects

The rest are applied by hand: they answer a question rather than build a shipping
image. Override `CDK_CONF`, which `make build` hands to `-DEXTRA_CONF_FILE`,
repeating the overlays still wanted (`;`-separated, later files win). The CDK
recipes reconfigure the existing build directory:

```sh
make build \
  CDK_CONF="overlay-thread.conf;overlay-lto.conf;overlays/bench.conf;overlays/bench-uwb-k2.conf"
```

- `overlays/bench.conf` plus the `bench-*` family: the A/B arms for the
  speed work: `bench-ble-dle251.conf`, `bench-ble-phy2m.conf`,
  `bench-ble-dle251-phy2m.conf`, `bench-cred-o2.conf`, `bench-uwb-dwell1.conf`,
  `bench-uwb-k2.conf`, `bench-uwb-spi-fused.conf`, `bench-uwb-spi-metrics.conf`.
  Their savings are hypotheses, not measurements.
- `overlays/latency.conf`: the `ULTRAWIDELOCK_LAT_TRACE` timing histograms.
- `overlays/dw3110-spi-8.overlay`, `-16`, `-32`: SPI clock arms for the DW3110.
- `overlays/cirdiag.conf`, `overlays/mlgate.conf`: selected by `make cirdiag`
  and `make mlgate` respectively.
- `overlays/ble-verbose.conf`, `overlays/thread-dataset-dump.conf`: debug aids.

**`ULTRAWIDELOCK_BENCH` is a required acknowledgement, not a convenience.** Two
bench arms weaken the range-integrity evidence (`RANGE_TRUST_K` below 3 and
`APPROACH_NEAR_DWELL` below 2), so `apps/dwm3001cdk-lock/CMakeLists.txt` fails
configuration unless `ULTRAWIDELOCK_BENCH` *and* `ULTRAWIDELOCK_RANGE_GATE_STRICT`
are both set. A bench image is not a shipping image; see
[`range-integrity.md`](range-integrity.md).

### Kconfig worth knowing about

| Symbol | Where | What it does |
|---|---|---|
| `ULTRAWIDELOCK_RANGE_GATE_STRICT` | `prj.conf`, **on** for this board | Drops a block that fails the STS-quality floor instead of latching it |
| `ULTRAWIDELOCK_STS_QUALITY_MIN` | `modules/ultrawidelock_uwb` | The floor itself. 0 means "defer to the driver"; never yet sized from captures |
| `ULTRAWIDELOCK_RANGE_TRUST_K` | `modules/ultrawidelock_uwb` | Consensus blocks, 2-3. **3 is the shipping floor** |
| `ULTRAWIDELOCK_APPROACH_NEAR_DWELL` | `apps/dwm3001cdk-lock` | Blocks held near before the lock opens |
| `ULTRAWIDELOCK_MCUBOOT_RECOVERY_HOLD_MS` | `apps/dwm3001cdk-lock` | SW2 hold that reboots into MCUboot serial recovery |
| `ULTRAWIDELOCK_CRED_DEV_TRUST` | `modules/ultrawidelock_cred` | **LAB ONLY.** Lets the built-in development identity authenticate against an empty trust store. Must be off in anything you ship |
| `DW3000_SPI_METRICS`, `DW3000_STS_BULK_WRITE_EXPERIMENT` | `modules/ultrawidelock_dw3000` | Instrumentation and an unproven fast path. Both default `n` |
| `ULTRAWIDELOCK_ANCHOR` | `modules/ultrawidelock_anchor` | The door-swing angle, the two-anchor fusion and the side gate's logic. Default `n`; `ANCHOR=1` sets it |
| `ULTRAWIDELOCK_ANCHOR_SLAM` | `modules/ultrawidelock_anchor` | The LIS2DH12 impact and tamper interrupt. Needs a board whose devicetree has an `accel0` alias with `irq-gpios`; the DWM3001CDK has both |
| `ULTRAWIDELOCK_ANCHOR_SLAM_THRESHOLD_MG` | `modules/ultrawidelock_anchor` | What counts as a strike, after the high-pass filter. **Placeholder at 2000.** The number that matters is "louder than closing this door normally", which is a property of the door and its hinges |
| `ULTRAWIDELOCK_ANCHOR_AJAR_DWELL_S` | `modules/ultrawidelock_anchor` | How long the leaf may stand away from CLOSED, bolt thrown, before a DoorAjar alarm. **Placeholder at 60**, chosen long: guessing high costs a late alarm, guessing low teaches the owner to ignore the event |
| `ULTRAWIDELOCK_ANCHOR_DOOR_HINGE_FRAME_MM`, `..._HINGE_LEAF_MM`, `..._OFFSET_MDDEG` | `modules/ultrawidelock_anchor` | The install geometry the swing angle is solved from. **All placeholders.** Solve the offset from three measured points (shut, ~45, ~90 degrees), not with a tape to the hinge pin: the hinge axis is not where it looks like it is |
| `ULTRAWIDELOCK_ML_LOS` | `modules/ultrawidelock_ml` | The obstruction classifier, and with it the carry-mode seam and the per-class widening table. `make mlgate` sets it |
| `ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_LEAN` | `modules/ultrawidelock_uwb` | Drops three separate register reads on a deadline. It does NOT drop the first-path index or the peak: those come out of one diagnostic burst whose length is fixed, so latching them costs 0 SPI bytes |

## Build options (nRF5340 DK)

On the command line, e.g. `make nrf-build PRETTY=1 CHIP=dw3720`:

| Option | Effect |
|---|---|
| `CHIP=dw3720` | build for the DW3720 (default: DW3000) |
| `PRETTY=1` | curated, quiet serial console |
| `SELFTEST=1` | radio TX/RX self-test at boot, no iPhone needed |
| `STRICT=1` | drop suspect UWB range blocks |
| `HA=1` | nRF5340-only Home Assistant data-model variant. One flag: it sets `CONFIG_ULTRAWIDELOCK_HA`, which selects the `zap_uwb_ha` data model inside the application, and no longer needs a separately bootstrapped workspace. It is not needed to share a DWM3001CDK with Home Assistant |
| `ULTRAWIDELOCK_SOURCE=0` | use the legacy Nordic Aliro binary instead of the default in-tree stack; diagnostic comparison only |
| `ULTRAWIDELOCK_TRACE=1` | declared temporary BLE/session boundary trace; currently unavailable because the required vendor integration patch is absent; see [Capture safety](#capture-safety) |
| `NFC=st25r` | use the default X-NUCLEO-NFC12A1/ST25R300 RFAL path; hardware-validated |
| `NFC=pn532` | use the in-tree PN532 SPI transport; driver and APDU layers are host-tested, not hardware-validated |
| `NFC=none` | build without an NFC reader; BLE/UWB remains enabled |
| `CIR=1` | compile CIA/CIR diagnostics; arm at runtime with `ultrawidelock cir on`, `ultrawidelock cir dump on`, or `ultrawidelock cir probe` |
| `LTO=0` | opt out of link-time optimisation, which is **on by default**. It saves 77,452 B of app-core flash and costs 1,920 B of RAM, and is hardware-validated 2026-08-03 (approach unlock, NFC tap, Apple Home commissioning and tile control). Turn it off when a stack trace has to name every frame |
| `DFU=0` | opt out of MCUboot plus Matter OTA, which are **on by default**, and get the old no-bootloader bench layout back. The default costs 33,280 B of app-core flash (LTO more than covers it) and moves `external_nvs` from `0x0` to `0x12f000`, so a board switched between the two loses its credential reader storage. It also requires `make dfu-key`, whose key the bootloader is signed with. Hardware-validated 2026-08-03 as a working lock; installing an OTA update is still unexercised |
| `PRISTINE=1` | force a clean rebuild |
| `SIGN_KEY=<path>` | the MCUboot image-signing key, used when `DFU=1`. Must be absolute. Default `apps/dwm3001cdk-lock/keys/mcuboot_ec_p256.pem`, created by `make dfu-key`. The same variable and the same key serve the DWM3001CDK |

### Kconfig overlays (nRF5340 DK)

In [`../apps/nrf5340dk-lock/overlays`](../apps/nrf5340dk-lock/overlays), layered
over the stock Nordic app; each file documents its settings.

- `ultrawidelock-cred.conf`: always applied. UWB heap and threads, BLE time-sync,
  the Apple ECP Express tap, log levels.
- `st25r.conf` or `pn532.overlay`: selected by `NFC=st25r|pn532`; `NFC=none`
  selects neither.
- `ultrawidelock-pretty.conf`, `ultrawidelock-ha.conf`: opt-in via `PRETTY=1` / `HA=1`.
- `lto.conf`: opt-in via `LTO=1`. Two Kconfig symbols, both required:
  `CONFIG_LTO=y` alone is a silent no-op. The build reads the pair back out of
  the linked image rather than trusting the request.
- `dfu.conf` plus `sysbuild-dfu.conf`: opt-in via `DFU=1`. The sysbuild file
  REPLACES `sysbuild-ultrawidelock.conf` rather than layering over it, and the flash map
  becomes the add-on's `pm_static_nrf5340dk_nrf5340_cpuapp.yml`, not
  `pm_static.yml`.
- `diag-cirdiag.conf`: opt-in via `CIR=1`; reading a CIR window costs walk-up
  latency while armed, so arm it only for a capture run.
- `diag-latency.conf`: diagnostic only (`LAT=1` to `make nrf-build`),
  Matter debug logs for timing notification delays.

`CONFIG_ULTRAWIDELOCK_CRED_SOURCE_STACK=y` is the nRF default. `make nrf-build` sets it
explicitly and verifies the link map contains no member from `libultrawidelock_ble.a`.
Use `ULTRAWIDELOCK_SOURCE=0` for comparison and regression isolation, not as the normal build.

## ESP32-S3, ESP32-C5, and ESP32-C6

One `idf.py menuconfig` option, **Enable Aliro over BLE + UWB** (default on),
advertises the Aliro features so Apple Home can put a key in Wallet.
Commissioning is standard Matter over Wi-Fi; `codes` reprints the QR URL and
pairing code.

ESP32-S3 is hardware-validated. ESP32-C5 has source and release-build support but
no recorded hardware validation. ESP32-C6 is hardware-validated for direct-SPI
BU04 bring-up with `ST_NRST` held low.

### Over-the-air updates

| Symbol | What it does |
| --- | --- |
| `CONFIG_ULTRAWIDELOCK_DFU_ESP32` | on by default. Adds the GATT service the browser writes a signed image to, and the commit hook that points the bootloader at the slot it landed in. Without the hook an update is received, verified, written: and then ignored at the next boot, because nothing wrote `otadata` |
| `CONFIG_ULTRAWIDELOCK_DFU_WINDOW_MS` | how long a **double-click** on the board's button leaves the update window open. 300000 (5 min) |
| `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` | on by default. An update that does not reach the end of `app_main` is rolled back to the previous slot at the next boot. Without it, a bad image is simply what the lock now runs, and the only way back is a cable |

This is **not** `CONFIG_ENABLE_OTA_REQUESTOR`, which is also on. That is Matter
OTA: BDX from a commissioned provider node over Wi-Fi, right for a fleet and
useless with a board and a browser, because a web page cannot join a Matter
fabric. The two share the `ota_0`/`ota_1` slots and nothing else.

The long press stays the commissioning window: overloading it would make
everyone who opens one spend five minutes accepting firmware from whoever is in
radio range.

## Runtime consoles

None needs a reflash.

**DWM3001CDK** (`make monitor`): read-only RTT over `probe-rs`, not a serial
port; `make nrf-term` does not reach it. No UART console: on a single-core part
the DW3110's delayed-transmit reply window cannot afford a blocking console
write. The Matter image has no shell
(`CONFIG_ULTRAWIDELOCK_PROV_CONSOLE=n`, `CONFIG_SHELL=n`), so
`ultrawidelock export` and friends do not exist there. Back up
`settings_storage` over SWD instead.

**DWM3001CDK, `make reader` only**: hold **SW2 and tap RESET** for provisioning
mode, a USB CDC-ACM console on the second USB port with the radios down.
Commands: `ultrawidelock prov`, `ultrawidelock import <hex>`, `ultrawidelock export yes`,
`ultrawidelock erase yes`. Walkthrough in
[`../apps/dwm3001cdk-lock/README.md`](../apps/dwm3001cdk-lock/README.md).

**nRF5340 DK** (`make nrf-term`): the `ultrawidelock` command group: `status`, `rx`,
`range`, `chip`, `selftest`, `log`, `frames`, `version`.

`make nrf-term` prints this image's Matter pairing code and QR payload before
attaching, because the Matter build may show nothing else: the add-on sets
`CONFIG_LOG_DEFAULT_LEVEL=0`, enables no UART log backend and drops the shell
(`CONFIG_SHELL=n`), so an empty terminal is expected, not a fault.
`make nrf-pairing-code` prints it alone. The payload is generated at build time
and merged into the hex, so it describes the build, not the board; the
discriminator and passcode are fixed in Kconfig, bench credentials rather than
per-device secrets.

**ESP32 Matter lock** (`make esp-monitor APP=matter-lock`): `status`, `lock`,
`unlock`, `codes`, `range`, `factoryreset`, `ultrawidelock <prov|trust|clear>`.

**ESP32 reader** (`make esp-monitor APP=reader`): `status`, `range`,
`ultrawidelock-start` / `ultrawidelock-stop` (demo responder, no phone needed), `ultrawidelock-prov`,
`ultrawidelock-trust`.

`ultrawidelock trust` / `ultrawidelock-trust` persist the last-seen credential to NVS;
`factoryreset` and `esp-flash-erase` drop it.

## Capture safety

With its missing integration patch restored, `ULTRAWIDELOCK_TRACE=1` logs protocol
states, message metadata, device or credential identifiers, and a truncated URSK
fingerprint. It does not log the raw URSK, but the trace is a bring-up artifact:
do not ship it in production firmware or publish a capture without review. In the
current tree, selecting it stops before the firmware build because the vendor
trace patch is absent, as `make help` reports.

Flight-recorder data is more sensitive. Raw serial logs containing `[FREC]`
records and binary `.frc` files include the full ephemeral URSK. Keep them
private and delete unneeded copies. The fuzz corpus exported by
`tools/flight_recorder.py` contains received frames only, no URSK. See
[`SECURITY.md`](../SECURITY.md).

## Where the defaults are

Reader identity and the trust store are NVS-backed, created on first boot;
inspect or reset them from the consoles, nothing on disk to hand-edit.
