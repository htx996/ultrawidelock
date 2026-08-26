<div align="center">

<img src="assets/card.png" width="880" alt="UltraWideLock: an Apple UWB digital key lock that an iPhone or Apple Watch unlocks on approach over UWB or on tap over NFC"/>

**Portable firmware for NFC and UWB smart locks.**

<img src="assets/badges.svg" width="880" alt="v0.3.0 · ISC license · Zephyr, ESP-IDF and FreeRTOS ports · 7,979 host tests"/>

<img src="assets/divider.svg" width="880" alt=""/>

</div>

**UltraWideLock** implements Aliro, the CSA's door-lock credential standard, on
real hardware: BLE, NFC (ECP), and UWB ranging. Three complete locks, plus
reader, initiator, and anchor examples.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Quick start

**No hardware.** A C compiler and `python3` is the whole list.

```sh
git clone https://github.com/ultrawidelock/ultrawidelock.git
cd ultrawidelock
make check                  # 8 host suites, about 2 minutes
```

**With a board.** Default is the Qorvo DWM3001CDK: nRF52833, DW3110 radio, and
a J-Link, all on one part. Nothing to wire.

```sh
make dfu-key                # once per clone   · image-signing key, gitignored
make bootstrap              # once per machine · NCS v3.3.0, several GB
make build                  # -> build/cdk-matter
make flash                  # over the on-board J-Link
make monitor                # console, over RTT
```

`make bootstrap` checks the machine before it fetches anything, names what is missing and
how to get it, and offers to install nRF Util if it is not here. It is resumable: interrupt
it and run it again. `SETUP_AUTO=1` answers yes to that offer without a prompt.

Once per machine, not once per checkout: the workspace lands in a store, named for the
upstream pin and the integration patch set it holds, and `./workspace` is a link to it. A
second clone or worktree runs `make ws-link` and is building in about a second. A branch
that changes a patch gets its own tree, cloned from the nearest one rather than fetched.
`make ws-store` says what the machine is holding and who still links to it.

`make help` lists every target. `make tools` says what this machine is missing.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## How a door opens

```text
    iPhone / Apple Watch              UltraWideLock on one board
   ┌────────────────────┐             ┌──────────────────────┐
   │  Wallet home key   │             │  nRF52833 + DW3110   │
   └──────────┬─────────┘             └───────────┬──────────┘
              │                                   │
   1  BLE     │  credential service 0xFFF2        │
              │ ─────────────────────────────────▶│
   2  Auth    │  AUTH0 → AUTH1 → EXCHANGE         │
              │ ◀────────────────────────────────▶│
              │  both ends now hold the URSK      │
   3  UWB     │  key ladder → STS → DS-TWR        │
              │ ◀────────── ranging ─────────────▶│
   4  Gate    │  range consistency agrees         │
              │      ──  U N L O C K  ──          │
   5  Matter  │  lock state over Thread           └──▶ Apple Home
              ╵
```

Local only. No app, no account, no cloud round trip.

<div align="center">

<img src="assets/hero.gif" width="880" alt="A Wallet home key unlocking the lock on approach, recorded on real hardware"/>

<sub>Real hardware. A real Wallet key. A real walk-up unlock.</sub>

<img src="assets/divider.svg" width="880" alt=""/>

</div>

## The board

One nRF52833, 512 KB flash and 128 KB RAM, runs all of this at once:

| | On the same part |
|:--|:--|
| 📶 | **BLE peripheral** the iPhone talks the credential protocol to |
| 🔑 | **Reader**: AUTH0 / AUTH1 / EXCHANGE, key ladder, STS, DS-TWR |
| 🏠 | **Hand-written Matter node** ([`modules/ultrawidelock_matter`](modules/ultrawidelock_matter/)), not CHIP |
| 🧵 | **OpenThread MTD**, so it joins a real Thread network |
| 📏 | **DW3110 UWB ranging**, over the module's internal SPI |
| 🧠 | **Obstruction classifier** ([`modules/ultrawidelock_ml`](modules/ultrawidelock_ml/)), 776 B of flash |

It fits in 417,684 of 433,664 B flash and 118,312 of 131,072 B RAM, which is
96.3% and 90.3%: **flash is the tighter of the two.** `make cdk-size` reports it.
`make cdk-size-check` cannot gate it today, because the recorded baseline
predates the project rename and refuses to compare across a configuration
change; [troubleshooting](docs/troubleshooting.md) says what to do about that.

**No tap on this board.** No NFC reader IC, and the nRF52833's own NFC is tag
emulation only. Walk-up only here; for a tap use the nRF5340 DK.

Also builds: `make reader` (radio alone, no Matter) and `make selftest`
(DW3110 device ID over SPI at boot).

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Is the door in the way?

Distance alone cannot tell a phone in your hand from a phone through a wall.
A decision tree reads the DW3000's own receive diagnostics and answers:

```sh
make mlgate                 # the classifier, in the unlock path
```

Depth 2, generated by emlearn, no interpreter and no allocation: **776 B of
flash, 0 B of RAM, 28 B of stack.** `tests/host/test_ultrawidelock_ml.c`
certifies the C classifies identically to the trained model.

The same image prints a `[FEAT]` CSV row per ranging block, which is how the
next model gets its data: 19 columns of first-path geometry, dB values in
centi-units because floating-point printf does not fit an 8 KB RTT ring.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Other boards

| Application | Hardware | Connectivity |
|---|---|---|
| [`apps/dwm3001cdk-lock/`](apps/dwm3001cdk-lock/) | DWM3001CDK | UWB, Matter over Thread |
| [`apps/dwm3001cdk-lock-freertos/`](apps/dwm3001cdk-lock-freertos/) | DWM3001CDK, no Zephyr | UWB, Matter over Thread |
| [`apps/nrf5340dk-lock/`](apps/nrf5340dk-lock/) | nRF5340 DK, DWM3000EVB, NFC12A1 | UWB and NFC, Matter over Thread |
| [`apps/esp32-matter-lock/`](apps/esp32-matter-lock/) | ESP32-S3 / C5 / C6 with DWM3000EVB | UWB, Matter over Wi-Fi |

```sh
make nrf-build && make nrf-flash && make nrf-term    # nRF5340 DK
make esp-go APP=matter-lock TARGET=esp32s3           # ESP32: build, flash, monitor
make freertos-build && make freertos-flash           # the Zephyr-free port
make hitl                                            # unattended end-to-end bench
```

To use one DWM3001CDK in Apple Home and Home Assistant without splitting its
Thread network, follow the
[multi-admin commissioning guide](apps/dwm3001cdk-lock/README.md#apple-home-plus-home-assistant).

<details>
<summary>ESP32 toolchain paths</summary>

<br/>

ESP32 needs an installed ESP-IDF, and the Matter lock also needs esp-matter.
`make esp-bootstrap` installs both, asking before each stage:

```sh
make esp-bootstrap APP=reader     # ESP-IDF only, about 5 GB
make esp-bootstrap                # ESP-IDF + esp-matter, about 20 GB and an hour
make esp-build APP=matter-lock TARGET=esp32s3
```

Neither is pinned by the build; both default under `$HOME/esp`, overridden with
`IDF_EXPORT` and `ESP_MATTER_PATH`, and an install you already have keeps
working untouched. What `esp-bootstrap` installs when it starts from nothing is
the pair the bench builds against: ESP-IDF v5.5.4 and esp-matter `93b1680`,
which is the version esp-matter itself asks for at that revision. `IDF_VER` and
`ESP_MATTER_REV` choose others.

</details>

<div align="center">
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/grid-demo-dark.webp">
  <source media="(prefers-color-scheme: light)" srcset="assets/grid-demo-light.webp">
  <img src="assets/grid-demo.webp" width="880" alt="Home Key setup, Approach Direction, provisioning, NFC tap, and lock-state notifications on live hardware"/>
</picture>
<br/>
<sub>Home Key · Approach Direction · provisioning · NFC tap · live lock state</sub>

<img src="assets/divider.svg" width="880" alt=""/>

</div>

## New since v0.3.0

- **Five-fabric Apple Home plus Home Assistant coexistence** on the
  DWM3001CDK: one committed Thread dataset, provisional commissioning rollback,
  selective durable `RemoveFabric`, per-fabric state, SRP duplicate recovery,
  and power-cut-safe `mf2` records.
- **A Zephyr-free FreeRTOS port** of the same lock on the nRF52833: NimBLE,
  MPSL and the SoftDevice Controller, OpenThread, Mbed TLS, and the Matter
  node, added a layer at a time and measured as each one lands.
- **The obstruction classifier** above, and `make mlgate` to run it live.
- **Matter Door Lock** grew up: `LockOperation` events, `AutoRelockTime`,
  persisted writable attributes, and Apple's Approach Direction cluster.
- **`DoorLockAlarm`**, so the lock can report the door and not just the bolt:
  a forced door from the impact latch, a door left ajar from the swing angle.
  Anchor builds only, and no controller has been seen rendering one yet.
  [Door Lock events](docs/matter-door-lock-events.md).
- **A Matter client**, so a walk-up at this lock can open a *second* Matter
  lock directly, with no hub automation in the path: CASE as initiator, the
  Binding cluster, and a `chip-tool` helper that sets up both ends.
  Off by default, and working on hardware since 2026-08-22 against a second
  lock built from this repo: [unlocking another
  lock](docs/matter-binding.md) sets it up, [bringing it
  up](docs/matter-binding-bench.md) is the bench procedure. No commercial lock
  has answered it yet.
- **A capture path for the classifier**: one `[FEAT]` CSV row per ranging block
  over RTT, carrying the first-path index and peak the model cannot currently
  see. Data collection only, no decision reads it.
- **A carry-mode seam** under the classifier, so a phone in a pocket could one
  day get its own threshold. The shipped tree is two-class, so the per-class
  table is empty and behaviour is unchanged; the capture that would fill it is
  [BodyCal](docs/bodycal-falsification.md).
- **Signed updates over BLE on the FreeRTOS port**, proven on hardware.
- **Inside and outside BLE witnesses**, with a fail-closed side gate for
  passive unlock (`make witness-trio`).
- **`make sdk-export`** for the hardware-agnostic SDK, and a size gate on
  every port.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Update over the air

No cable, no probe. Either chip, from a browser:

**[ultrawidelock.com/flash](https://ultrawidelock.com/flash/index.html)** finds the
board over Web Bluetooth, reads what it is running, and sends the one update that
applies. Chrome or Edge on a computer, or Chrome on Android — Web Bluetooth does
not exist in Safari, so not from an iPhone. Nothing is written until you open the
update window on the board itself: **SW2** on the CDK, a **double-click** on the
ESP32's button. That press is the whole availability gate; authenticity is a
P-256 signature checked before a byte is written and again by the bootloader.

The two chips travel differently, and the page hides the difference:

| | DWM3001CDK | ESP32-S3 / C5 / C6 |
|---|---|---|
| Transport | mcumgr over BLE | native GATT frames |
| Payload | signed delta, ~11 KB | signed whole image, ~2 MB |
| Why | one MCUboot slot in 512 KB | two full OTA slots |
| Time | seconds | several minutes over BLE |

One MCUboot slot means a CDK delta is cut against the *exact* bytes on the part,
so a release ships one file per earlier release still in the field, and a board
running something older than that fan has no over-the-air path at all — the page
says so and points at the J-Link.

From the command line instead:

```sh
make dfu                    # CDK: build, diff, sign, push
make fota                   # CDK: one file a phone can install
make fota-done              # after every phone push
make ota-fan  PREV_HEXES=…  # the delta fan + the index the web page reads
make esp-ota                # ESP32: sign each chip's app image
```

`make fota-done` is not optional. The delta is cut against the exact bytes on
the board, and only the build host keeps that record.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Before you rely on it

- **Console is RTT, not UART.** `make monitor` attaches with the ELF you
  flashed. The ring survives reset, so the first block is the previous run.
- **`make flash-erase` costs the commissioning.** Apple Home has to add the
  lock again, and Home Assistant must be shared again afterward.
- **Never lock APPROTECT.** Recovery needs a mass erase, which takes the
  reader's private key and every phone key on it.
  `scripts/check-approtect.sh` checks a part.
- **These are bench defaults.** Do not secure valuables with it.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Use as an SDK

Include only your role:

```c
#include <ultrawidelock/reader.h>     // reader
#include <ultrawidelock/device.h>     // initiator
#include <ultrawidelock/tlv.h>        // codec only
```

Ports implement the five seams in `<ultrawidelock/ultrawidelock_hal.h>`: DW3000
GPIO/IRQ, DW3000 SPI, reader BLE, central BLE, credential storage. New board or
chipset: [`PORTING.md`](PORTING.md).

<details>
<summary>Plain CMake consumers</summary>

<br/>

```sh
cmake -S . -B build/sdk -DCMAKE_INSTALL_PREFIX="$PWD/build/sdk-install"
cmake --build build/sdk
cmake --install build/sdk
```

Exports `UltraWideLock::headers` and `UltraWideLock::tlv`; version comes from
the root `VERSION`. Working example in
[`examples/cmake/consumer/`](examples/cmake/consumer/), and `add_subdirectory`
works too. `make sdk-check` verifies both paths.

Full firmware is consumed through the Zephyr module or the ESP-IDF components.
The all-in-one `<ultrawidelock/ultrawidelock.h>` pulls in every declaration.

</details>

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Repository layout

```text
ultrawidelock/
├── apps/           complete lock products
├── examples/       independently buildable role and bench examples
├── modules/        the portable protocol, with no OS in it
│   ├── ultrawidelock_cred/     credential sessions, TLV, key ladder
│   ├── ultrawidelock_uwb/      ranging engine behind the STS seam
│   ├── ultrawidelock_dw3000/   DW3000 driver integration
│   ├── ultrawidelock_matter/   the hand-written Matter node
│   ├── ultrawidelock_ml/       on-device classifiers
│   ├── ultrawidelock_nfc/      ECP and reader transports
│   └── ultrawidelock_dfu/      signed delta updates
├── ports/          zephyr · esp32 · freertos-nrf52833
├── integrations/   patches for external upstream applications
├── tests/          host, shared, port, tooling, on-target
├── include/        SDK umbrella and public-API ownership
├── cmake/          shared CMake helpers
├── mk/             what sits behind each Make target
├── scripts/        setup, release, DFU, sizing, device utilities
└── release/        templates and scripts for release bundles
```

Contributing: [`CONTRIBUTING.md`](CONTRIBUTING.md). Coding agents:
[`AGENTS.md`](AGENTS.md).

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## License

Project-original code is Copyright (c) 2026 asxeem and UltraWideLock
contributors under the ISC license ([LICENSE](LICENSE)). The DW3000 integration
in `modules/ultrawidelock_dw3000` is ISC (Bruno Randolf).

The vendored Qorvo UWB driver it wraps is LicenseRef-QORVO-2, which permits use
only with Qorvo integrated circuits, so **binaries built with UWB support
inherit that hardware restriction**. Full mapping:
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES).
