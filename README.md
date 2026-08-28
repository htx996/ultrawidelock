<div align="center">

<img src="assets/card.png" width="880" alt="UltraWideLock: an Apple UWB digital key lock that an iPhone or Apple Watch unlocks on approach over UWB or on tap over NFC"/>

**Portable firmware for NFC and UWB smart locks.**

<img src="assets/badges.svg" width="880" alt="v0.4.0 · ISC license · Zephyr, ESP-IDF and FreeRTOS ports · 9,608 host tests"/>

<img src="assets/divider.svg" width="880" alt=""/>

</div>

Aliro, the CSA's door-lock credential standard, on real hardware: BLE, NFC
(ECP), UWB ranging. Four locks, a second anchor that tells inside from outside,
and standalone reader, initiator, anchor and witness examples.

<details>
<summary><b>Everything in it, on one screen</b></summary>

<br/>

| | |
|:--|:--|
| **Credential** | Aliro / Home Key over BLE: AUTH0, AUTH1, EXCHANGE, key ladder, provisioning, step-up, approach state machine |
| **UWB** | DS-TWR under a CCC STS session, channels 5 and 9, FiRa sessions, flight recorder, CIA/CIR diagnostics |
| **NFC** | ECP and tap unlock on the nRF5340 DK, over ST25R or PN532 (`NFC=st25r\|pn532\|none`) |
| **Side of door** | a second UWB anchor in the phone's own round, or two BLE witness dongles with a door-transition latch. Both fail closed |
| **Obstruction** | depth-2 decision tree over the DW3000's receive diagnostics, 776 B of flash, plus a `[FEAT]` capture path |
| **Matter** | hand-written node, not CHIP: Door Lock with `LockOperation`, `AutoRelockTime`, `DoorLockAlarm`, Approach Direction, a UWB presence cluster, five-fabric multi-admin, and a Matter *client* that opens a second lock |
| **Thread / Wi-Fi** | OpenThread MTD on the nRF parts, Matter over Wi-Fi on ESP32 |
| **Updates** | signed P-256 deltas over BLE or USB through MCUboot, whole signed images on ESP32, serial recovery, a release delta fan, a browser that picks the right one |
| **Ports** | Zephyr, ESP-IDF, Zephyr-free FreeRTOS, behind five HAL seams |
| **Boards** | DWM3001CDK, nRF5340 DK, ESP32-S3 / C5 / C6, nRF52840 dongles |
| **SDK** | CMake package, umbrella header, per-role headers, `make sdk-export` |
| **Web** | the site, the guides, a WASM digital twin, a browser flasher, a subsystem graph |
| **Gates** | 9,608 host checks in 18 suites, plus seam, scope, purity, drift, size, sanitizer, CBMC and static-analysis gates |

</details>

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Quick start

**No hardware.** A C compiler and `python3`.

```sh
git clone https://github.com/ultrawidelock/ultrawidelock.git
cd ultrawidelock
make check                  # 18 host suites, 9,608 checks, about 2 minutes
```

**With a board.** Default is the Qorvo DWM3001CDK: nRF52833, DW3110 radio and a
J-Link on one part. Nothing to wire.

```sh
make dfu-key                # once per clone   · image-signing key, gitignored
make bootstrap              # once per machine · NCS v3.3.0, several GB
make build                  # -> build/cdk-matter
make flash                  # over the on-board J-Link
make monitor                # console, over RTT
```

`make bootstrap` is resumable and names what is missing before it fetches;
`SETUP_AUTO=1` skips its prompt. The workspace is per machine, keyed on the
upstream pin and patch set, and `./workspace` links to it: a second clone runs
`make ws-link` and builds in a second.

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

One nRF52833, 512 KB flash and 128 KB RAM, all at once:

| | On the same part |
|:--|:--|
| 📶 | **BLE peripheral** the iPhone talks the credential protocol to |
| 🔑 | **Reader**: AUTH0 / AUTH1 / EXCHANGE, key ladder, STS, DS-TWR |
| 🏠 | **Hand-written Matter node** ([`modules/ultrawidelock_matter`](modules/ultrawidelock_matter/)), not CHIP |
| 🧵 | **OpenThread MTD**, so it joins a real Thread network |
| 📏 | **DW3110 UWB ranging**, over the module's internal SPI |
| 🧠 | **Obstruction classifier** ([`modules/ultrawidelock_ml`](modules/ultrawidelock_ml/)), 776 B of flash |

417,684 of 433,664 B flash and 118,312 of 131,072 B RAM: 96.3% and 90.3%.
**Flash is the tighter.** `make cdk-size` reports it; `make cdk-size-check`
cannot gate it while the baseline predates the rename
([troubleshooting](docs/troubleshooting.md)).

**No tap here**: no NFC reader IC, and the nRF52833's own NFC is tag emulation
only. Use the nRF5340 DK. Also builds `make reader` (radio alone) and
`make selftest` (DW3110 device ID over SPI at boot).

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Is the door in the way?

Distance cannot tell a phone in your hand from a phone through a wall. A
decision tree reads the DW3000's own receive diagnostics.

```sh
make mlgate                 # the classifier, in the unlock path
```

Depth 2, generated by emlearn, no interpreter and no allocation: **776 B of
flash, 0 B of RAM, 28 B of stack.** `tests/host/test_ultrawidelock_ml.c`
certifies the C matches the trained model. The image also prints a `[FEAT]` CSV
row per block: 19 columns of first-path geometry, dB in centi-units because
floating-point printf does not fit an 8 KB RTT ring.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Inside, or outside?

One distance is a radius, not a side. Two routes answer it, both fail closed:
no agreeing second opinion, no passive unlock.

**A second UWB anchor.** The satellite ([`apps/satellite/`](apps/satellite/))
joins the phone's own round as responder 1 and reports its distance sealed and
bound to the block it was measured in; the lock fuses the pair only when both
halves carry the same block. CDK, nRF5340 DK, or the ESP32 tier over ESP-NOW.

```sh
make anchorlink                       # the lock half   · BENCH=1 for the desk
make flash CDK_BUILD=build/cdk-anchorlink
make sat-build SAT_THREAD=1           # the satellite half
make sat-flash
```

Never `make flash-erase` an anchorlink board: the anchor key lives in the
settings partition a full erase takes with it.

**Two BLE witness dongles.** No extra UWB radio: one nRF52840 inside, one
outside, a differential read taken at the one moment it is reliable, and a
door-transition latch holding the answer between crossings. A dead dongle,
dropped mesh, reboot or corrupt storage each leave the lock shut from inside.

```sh
make witness-build && make witness-flash   # one image, role set at install
make witness-prov-help                     # the line to paste per dongle
```

Also: a lock-owned freshness epoch, so a reboot cannot roll the replay window
back; per-board range bias calibration; a configurable OUTSIDE margin.

| Read this | For |
|---|---|
| [inside latch](docs/inside-latch.md) | why the veto is a latch, and what each failure does |
| [second anchor](docs/second-anchor.md) | the UWB satellite, and what is built versus wired |
| [bench runbook](docs/bench-inside-outside.md) | the three-board afternoon, start to finish |
| [range integrity](docs/range-integrity.md) | what a distance may be trusted for |

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Other boards

| Application | Hardware | Connectivity |
|---|---|---|
| [`apps/dwm3001cdk-lock/`](apps/dwm3001cdk-lock/) | DWM3001CDK | UWB, Matter over Thread |
| [`apps/dwm3001cdk-lock-freertos/`](apps/dwm3001cdk-lock-freertos/) | DWM3001CDK, no Zephyr | UWB, Matter over Thread |
| [`apps/nrf5340dk-lock/`](apps/nrf5340dk-lock/) | nRF5340 DK, DWM3000EVB, NFC12A1 | UWB and NFC, Matter over Thread |
| [`apps/esp32-matter-lock/`](apps/esp32-matter-lock/) | ESP32-S3 / C5 / C6 with DWM3000EVB | UWB, Matter over Wi-Fi |
| [`apps/satellite/`](apps/satellite/) | CDK, nRF5340 DK, or the ESP32 tier | UWB responder, sealed link to the lock |

```sh
make nrf-build && make nrf-flash && make nrf-term    # nRF5340 DK
make esp-go APP=matter-lock TARGET=esp32s3           # ESP32: build, flash, monitor
make freertos-build && make freertos-flash           # the Zephyr-free port
make sat-build && make sat-flash                     # the satellite
make hitl                                            # unattended end-to-end bench
```

One role at a time:

| Example | What it is | Build |
|---|---|---|
| [`examples/zephyr/anchor/`](examples/zephyr/anchor/) | two-board anchor-to-anchor DS-TWR bench | `make anchor-pair` |
| [`examples/zephyr/ble-witness/`](examples/zephyr/ble-witness/) | the inside/outside dongle | `make witness-build` |
| [`examples/zephyr/nrf5340dk-initiator/`](examples/zephyr/nrf5340dk-initiator/) | a DK standing in for the phone | `make nrf-init-build` |
| [`examples/esp32/reader/`](examples/esp32/reader/) | the credential reader alone, no Matter | `make esp-build APP=reader` |
| [`examples/esp32/initiator/`](examples/esp32/initiator/) | an ESP32 BLE initiator peer | `make esp-build APP=initiator` |
| [`examples/esp32/satellite/`](examples/esp32/satellite/) | the satellite on the ESP32 tier | `make esp-build APP=satellite` |
| [`examples/cmake/consumer/`](examples/cmake/consumer/) | an out-of-tree C consumer of the SDK | `make sdk-check` |

Apple Home and Home Assistant on one CDK without splitting its Thread network:
the [multi-admin guide](apps/dwm3001cdk-lock/README.md#apple-home-plus-home-assistant).

<details>
<summary>ESP32 toolchain paths</summary>

<br/>

```sh
make esp-bootstrap APP=reader     # ESP-IDF only, about 5 GB
make esp-bootstrap                # ESP-IDF + esp-matter, about 20 GB and an hour
make esp-build APP=matter-lock TARGET=esp32s3
```

Neither is pinned by the build. Both default under `$HOME/esp`, overridden with
`IDF_EXPORT` and `ESP_MATTER_PATH`; an existing install keeps working. From
nothing, `esp-bootstrap` installs what the bench builds against: ESP-IDF v5.5.4
and esp-matter `93b1680`. `IDF_VER` and `ESP_MATTER_REV` choose others.

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

## New in v0.4.0

Full notes: [`CHANGELOG.md`](CHANGELOG.md). A Zephyr-free FreeRTOS port, a
second anchor, browser updates on either chip, a grown-up Matter Door Lock, and
a Matter client that opens a second lock.

> **v0.3.0 pairings do not survive this update.** The settings partition moves
> and the record schema becomes `mf2`: Apple Home must add the lock again, and
> Home Assistant must be shared again after that. One-time; the partition does
> not move again.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Update over the air

No probe. Either chip, from a browser:
**[ultrawidelock.com/flash](https://ultrawidelock.com/flash/index.html)** finds
the board, reads what it is running, and sends the one update that applies.
Chrome or Edge on a computer, Chrome on Android; neither Web Bluetooth nor
WebSerial exists in Safari.

Nothing is written until you open the update window on the board: **SW2** on the
CDK, a **double-click** on the ESP32's button. That press is the whole
availability gate. Authenticity is a P-256 signature, checked before a byte is
written and again by the bootloader.

| | DWM3001CDK | ESP32-S3 / C5 / C6 |
|---|---|---|
| Transport | mcumgr, over BLE **or** USB | native GATT frames |
| Payload | signed delta, ~11 KB | signed whole image, ~2 MB |
| Why | one MCUboot slot in 512 KB | two full OTA slots |
| Time | seconds | several minutes over BLE |

One MCUboot slot means a CDK delta is cut against the *exact* bytes on the part,
so a release ships one file per earlier release still in the field.

The cable is the same update through a different pipe: `uart0` is the J-Link
OB's VCOM, enumerates as USB CDC-ACM, and WebSerial opens it. About three times
faster, because Web Bluetooth will not report an MTU.

The CDK can also **reinstall everything** over that cable: a whole signed image
through MCUboot serial recovery, needing no starting image, no update window and
no working software on the board (`CONFIG_BOOT_SERIAL_NO_APPLICATION=y` keeps a
dead application in recovery). The J-Link is needed once, to place the
bootloader.

> Serial recovery is [CDK-16](docs/hardware-validation.md), open: it completed
> one upload in August 2026 and has not reproduced. The browser is a second,
> independent host implementation on the same wire, which is why running it is
> worth doing even though it is expected to fail.

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

## In a browser, without a board

[**ultrawidelock.com**](https://ultrawidelock.com) is built from this tree by
stdlib Python. `make docs-serve` puts it on `localhost:8080`.

| Page | What it does |
|---|---|
| [Guides](https://ultrawidelock.com/docs/index.html) | every file in [`docs/`](docs/), with search, a contents rail and a reading order |
| [Digital twin](https://ultrawidelock.com/twin/index.html) | the ranging engine in WASM: walk a phantom phone, add noise, fire a Ghost-Peak spoof, single-step a DS-TWR round |
| [Flash](https://ultrawidelock.com/flash/index.html) | install an ESP32 over the cable, or update either chip over the air |
| [Graph](https://ultrawidelock.com/graph/index.html) | the subsystem graph, generated from the source tree |

The twin is not a re-implementation: it compiles the untouched
`modules/ultrawidelock_uwb` sources against the host shim the test suite links,
so every block is a real CCM*-encrypted exchange decoded by the firmware's own
RX state machine. `make test-twin` replays its scenarios.

`make docs-check` fails on a dead internal link, and on drift in three constant
tables re-read from the C they cite: the twin's, the hero's tick rate and unlock
bound, and the flasher's setup code.

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## What the tree checks about itself

```sh
make check                  # 18 host suites, 9,608 checks, no hardware
make ci                     # every pull-request gate, in CI's order
make regress                # everything a machine can check without a board
make regress-hil            # and then on air, against a live reader
```

| Gate | Refuses |
|---|---|
| `make seam` | a call that reaches the radio past the CCC STS seam |
| `make scope` | a vendor radio API named outside the DW3000 engine file set |
| `make purity` | an OS named in `modules/`, or a port tree naming another port's |
| `make drift` | a constant or integration patch set that has quietly moved |
| `make cdk-size-check` | an image that lost flash or RAM headroom |
| `make lint` / `make sca` | cppcheck and Clang Static Analyzer over the portable tree |
| `make cbmc` | a wire parser that cannot be proved memory-safe |
| `make test-san` | the host suite again under ASan and UBSan |
| `make coverage` | nothing, but prints 0% rows for what no suite reaches |

`make sdk-check` builds an out-of-tree C consumer against the installed CMake
package. `make app-diff` diffs this tree's door-lock app against pinned upstream.
`make instrument` serves a latency dashboard captured from a real board.

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
the root `VERSION`. `add_subdirectory` works too, and `make sdk-check` verifies
both paths against [`examples/cmake/consumer/`](examples/cmake/consumer/).

Full firmware is consumed through the Zephyr module or the ESP-IDF components.
`<ultrawidelock/ultrawidelock.h>` pulls in every declaration.

</details>

<details>
<summary><b>All 25 guides</b></summary>

<br/>

| | |
|:--|:--|
| **Start** | [configuring](docs/configuring.md) · [add the key](docs/add-the-key.md) · [troubleshooting](docs/troubleshooting.md) |
| **Boards** | [ESP32 bring-up](docs/esp32-bringup.md) · [ESP32 gotchas](docs/esp32-gotchas.md) · [nRF5340 bring-up](docs/nrf5340-bringup.md) · [nRF5340 wiring](docs/nrf5340-wiring.md) · [DWM3001CDK surgery](docs/dwm3001cdk-surgery.md) · [hardware validation](docs/hardware-validation.md) |
| **Porting** | [porting](docs/porting.md) · [porting to ESP32](docs/porting-esp32.md) · [chipset memory](docs/chipset-memory.md) |
| **Protocol** | [notes](docs/protocol-notes.md) · [research](docs/protocol-research.md) · [range integrity](docs/range-integrity.md) · [approach direction](docs/approach-direction.md) · [UWB MAC login](docs/uwb-mac-login.md) · [Door Lock events](docs/matter-door-lock-events.md) · [Matter binding](docs/matter-binding.md) · [binding bench](docs/matter-binding-bench.md) · [BodyCal](docs/bodycal-falsification.md) |
| **Side of door** | [inside latch](docs/inside-latch.md) · [second anchor](docs/second-anchor.md) · [bench runbook](docs/bench-inside-outside.md) |
| **Reference** | [reference](docs/reference.md) |

</details>

<div align="center"><img src="assets/divider.svg" width="880" alt=""/></div>

## Repository layout

```text
ultrawidelock/
├── apps/           complete lock products
├── examples/       independently buildable role and bench examples
├── modules/        the portable protocol, with no OS in it
│   ├── ultrawidelock_cred/       credential sessions, TLV, key ladder
│   ├── ultrawidelock_cred_stack/ that stack, adapted into the Nordic app
│   ├── ultrawidelock_uwb/        ranging engine behind the STS seam
│   ├── ultrawidelock_dw3000/     DW3000 driver integration
│   ├── ultrawidelock_anchor/     side of door: fusion, latch, witnesses, seal
│   ├── ultrawidelock_matter/     the hand-written Matter node
│   ├── ultrawidelock_ml/         on-device classifiers
│   ├── ultrawidelock_nfc/        ECP and reader transports
│   ├── ultrawidelock_dfu/        signed delta updates
│   └── ultrawidelock_port/       the OS, flash, log and byte-order contracts
├── ports/          zephyr · esp32 · freertos-nrf52833
├── integrations/   patches for external upstream applications
├── tests/          host, shared, port, tooling, sdk, on-target
├── include/        SDK umbrella and public-API ownership
├── cmake/          shared CMake helpers
├── mk/             what sits behind each Make target
├── scripts/        setup, release, DFU, sizing, device utilities
├── docs/           the guides, read on GitHub and published to the site
├── web/            the site, the flasher, the WASM twin, the graph
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
