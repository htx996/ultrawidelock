# Hardware validation

CI gates the host-side logic (`make ci`: the KAT suite, sanitizers, CBMC, the
tooling gates) and compile-gates four images in the NCS toolchain container: the
`CLIENT=1` build (the tightest configuration in the tree), the shipping image
(`SMP=1 RELEASE=1`, what `make release` wraps), `anchorlink` (the witness
transport, the side gate and the door alarms), and the nRF5340 satellite.
Between them that is nine of the thirteen DWM3001CDK application sources, plus
the satellite app. It does not build the nRF5340 DK lock, the remaining CDK
configurations, the size baseline or anything ESP32, so `make regress` still
compile-gates those on a bench. The `release` workflow does build the DK and all
three ESP32 chips, but only when a release is cut, so those two legs are first
compiled at the moment they ship -- which is the argument for a throwaway tag
ahead of a real one. What none of that can
exercise is the product itself, which runs against a live iPhone.

This checklist is the manual gate: run every applicable item before cutting a
release, and record the results table in the release notes. Part of it is no longer
manual -- `make regress-hil` runs the rows marked automated below and writes
`build/regress-hil/<timestamp>/verdict.txt` naming each one:

| Row | Automated by | Stage |
|---|---|---|
| CDK-4 | `make regress-hil REGRESS_HIL_ARGS=--selftest` | `uwb-selftest` (reflashes the reader) |
| CDK-5..CDK-8 | `make regress-hil` | `walkup`, via `scripts/hitl-run.sh` |

Everything else here, CDK-9, CDK-10 and CDK-14..CDK-18 included, still needs a
person and a phone.

Three hardware paths have recorded bench evidence: the DWM3001CDK, the nRF5340 DK
using the legacy Nordic binary with its default ST25R300/RFAL reader, and ESP32-S3.
The in-tree credential stack is now the nRF default, but it does not inherit the legacy
binary's result. It must pass the nRF checklist before release. A release covering
only one target runs that target's rows and records the others as `n/a`.

ESP32-C5 is built and bundled by the release workflow, but has no hardware
validation record. Mark it build-only in release notes until a C5 checklist is
defined and passed. The PN532 variant likewise has automated evidence only and
does not inherit the ST25R300 checklist result.

## DWM3001CDK

The primary target, and the shortest bench setup there is: one nRF52833 and the
DW3110 in the same module, nothing to wire, on-board J-Link OB. No NFC tap path
exists here and none can, so there is no equivalent of HV-5. The board carries no
reader IC, and the nRF52833's own NFC peripheral is tag-emulation only.

### Test setup

- A DWM3001CDK on USB, over its on-board J-Link OB. No EVB to seat, no ribbon.
- `make dfu-key`, once per clone. Every image on this board is signed and the key
  is gitignored, so a fresh clone or a new git worktree stops at configure until
  it has one of its own.
- A Thread border router the phone already reaches, for the Matter image
  (`make build`). For Apple Home plus Home Assistant, import the Apple Thread
  credentials into Home Assistant and join every border router to that one
  dataset before sharing the accessory. A `make reader` image needs neither a
  border router nor a commissioner.
- An iPhone with the lock's Aliro key in Wallet. Apple Home mints it during
  CDK-7; a `reader` image takes an imported credential instead, per
  [`apps/dwm3001cdk-lock/README.md`](../apps/dwm3001cdk-lock/README.md).
- Console attached with `make monitor`. It is RTT, not UART, so `make nrf-term`
  does not reach this board.

### Checklist

The Recorded column is what this repository has already seen on hardware. It is
not a substitute for running the row: a release records the result you got, not
this one.

| ID | Procedure | Pass criterion | Recorded |
|---|---|---|---|
| CDK-1 | `make test` on the release commit | Exit 0, all host KATs pass | CI gate |
| CDK-2 | `make dfu-key`, then `make build RELEASE=1 SMP=1 PRISTINE=1` | Exit 0; the production image links and fits the 433,664 B `app` partition and 128 KB RAM | yes, 2026-08-16: 397,360 B flash (91.63%), 120,740 B RAM (92.12%), 10,332 B RAM spare |
| CDK-3 | `make reader PRISTINE=1` | Exit 0; the reader-only image links and fits | yes: 285,664 B (65.87%), 79,908 B RAM (60.96%) |
| CDK-4 | Flash a `make selftest` build, boot with no phone present | `DW3000 raw DEV_ID = 0xdeca0302` on the RTT console | yes |
| CDK-5 | `make flash-erase` with the release image, then boot | Clean boot, `ECDH self-test: PASS`, BLE advertising starts, no faults | yes |
| CDK-6 | Connect from an iPhone with a BLE scanner | 0xFFF2 enumerates with both characteristics and never prompts to pair; the reader-SPSM characteristic reads `00 80 02 01 00 01 01` | yes |
| CDK-7 | Add the accessory in Apple Home with the setup code the build printed | Commissioning completes and the lock tile goes live | yes |
| CDK-8 | Relock, then approach from well outside ranging distance, phone pocketed | Wallet animation plays and the bolt opens, with no phone interaction | yes: four unlocks in one session, 2026-08-02 |
| CDK-9 | Walk away | Bolt relocks past the hysteresis margin and does not oscillate at the boundary | no recorded result; run it |
| CDK-10 | Power-cycle the board, wait for boot, repeat CDK-8 | Unlock works without re-commissioning or re-provisioning | no recorded result; run it |
| CDK-11 | Change something in the tree, then `make dfu`, pressing SW2 when it asks | The delta goes over Bluetooth and the board's flash comes out byte for byte identical to the target image, with a matching CRC | **needs a re-run**: last passed 2026-08-03 (`bca7534`, `ed1780c`) at 17 to 31 s, but that predates the version-2 wire protocol |
| CDK-12 | Repeat CDK-11, opening the window from Apple Home's "Turn On Pairing Mode" instead of pressing SW2 | D10, the blue LED, blinks at 2 Hz while the window is open, and the push is accepted | yes, on a live commissioned lock, for both openers |
| CDK-13 | `make fota`, push the file from a phone with nRF Device Manager's **Images** tab, then `make fota-done` | The board comes back reporting the target image's SHA-256 | yes, on the commissioned lock (`53b2fe1`, `8447e91`) |
| CDK-14 | 100 walk-ups, counting the ones that unlock | 95% or better | **open, never run**; the sample so far is single digits |
| CDK-15 | Cut the power in the middle of a CDK-11 apply, then restore it | The board resumes at the right step and boots the target image | **open, never run** |
| CDK-16 | Hold SW2 for 5 s while the application runs, then upload with `scripts/cdk-dfu.sh` | The board warm-reboots into MCUboot serial recovery and accepts the image | **open**; serial recovery has completed exactly one real upload and is not yet reproducible |
| CDK-17 | Record a walk-up with the flight recorder, histogram the STS quality index, pick a floor above the noise | `ULTRAWIDELOCK_STS_QUALITY_MIN` is set from data rather than left at 0 | **open, never run.** The DWM3001CDK now *enforces* this gate, so an untuned floor is a door that can refuse to open |
| CDK-18 | Walk-up in NLOS: phone pocketed on the far side of the body, and through an interior door | The gate still publishes a range and the bolt opens | **open, never run.** One LOS walk-up passed at `sts_ok=1`, STS index 62, verdict 24, d=107 mm; that is not a calibration |
| CDK-19 | In Home Assistant's Thread integration, send the iPhone's Apple Thread credentials, make that dataset preferred, and join the Home Assistant OTBR to it | Apple and Home Assistant border routers report one Extended PAN ID; no second preferred dataset is created | **open, never run** |
| CDK-20 | With CDK-7 still live, use the Home Assistant iOS app's **Matter > Add device > already in use** path and share from Apple Home | Home Assistant completes commissioning; Apple Home, Home Key, and Home Assistant all operate the same lock; three fabrics are present and two slots remain | **open, never run** |
| CDK-21 | Power-cycle the lock and each border router after CDK-20, then operate it from both controllers and walk up | Both controllers rediscover and operate it without re-pairing; Wallet walk-up still succeeds | **open, never run** |
| CDK-22 | Start another share, abort after `AddNOC`, and wait past the fail-safe | Only the provisional fabric disappears; Apple and Home Assistant remain live and the slot is reusable | **open, never run** |
| CDK-23 | Remove the Home Assistant fabric with **Manage fabrics**, power-cycle, then share it again | Removal survives reboot, Apple and Home Key remain live throughout, and the freed slot is reusable | **open, never run** |
| CDK-24 | Reproduce an SRP duplicate registration, then leave the border router running | The lock retries with a fresh service name and becomes CASE-reachable without restarting the border router | **open, never run** |
| CDK-25 | While Apple Home is live, offer the lock a different Home Assistant Thread dataset | Commissioning refuses that dataset without detaching or replacing the working Apple network | **open, never run** |
| CDK-26 | Cut power once during a fabric commit and once during `RemoveFabric`, reboot after each cut | Each boot loads an old or new valid per-slot record; no torn table, resurrected removal, or damage to another fabric | **open, never run** |

CDK-8 is this target's EV-7, and it is faked the same way: the bolt moving is not a
pass. The Wallet animation is, because that is what proves the reader told the phone
it granted access rather than just actuating locally.
| CDK-27 | With a second administrator on and a Matter DoorLock peer bound, walk up once and read the RTT log | The peer's bolt moves, and the log reaches `the bound lock UNLOCKED` | **PASSED 2026-08-22**, against `apps/nrf5340dk-lock` on a Home Assistant fabric. Took five runs and five interoperability fixes; `docs/matter-binding-bench.md` records each fault and its signature. Never run against a commercial peer |
| CDK-28 | After CDK-27, walk away and let the departure gate relock | Both bolts close, and the log reaches `the bound lock LOCKED` | **PASSED 2026-08-22.** Home Assistant showed both locks unlocked and then both locked within the same second |
| CDK-29 | Open https://ultrawidelock.com/flash/index.html in Chrome, pick the DWM3001CDK, and connect | The chooser lists the board, and the page names the image it is running before asking for anything | **open, never run** |
| CDK-30 | Continue CDK-29 on a board whose image the release publishes a delta from, pressing SW2 when the page asks | The delta goes over Web Bluetooth, the board reboots, and the page reconnects and reports the target SHA-256 | **open, never run.** This is CDK-13 driven from a browser instead of a phone, and the firmware is the same `SMP=1` image, so what is untested is the JavaScript and nothing else |
| CDK-31 | Repeat CDK-30 on a board running a build the release ships no delta from | The page says no over-the-air update applies, names the single-slot reason, points at the J-Link, and sends nothing | **open, never run.** Proven against a fake board in the `fotawire` suite; never against a real one |
| CDK-32 | Repeat CDK-30 with **USB cable** picked instead of Bluetooth, on the J-Link port | The same delta goes over uart0 as mcumgr serial frames, the board reboots, and the page reports the target SHA-256 without the port ever closing | **open, never run.** The firmware side is `CONFIG_MCUMGR_TRANSPORT_UART` and nothing else -- same receiver, same signature check, same window gate as the radio |
| CDK-33 | Time CDK-30 and CDK-32 on the same delta | The cable is materially faster; 384-byte chunks against 105 | **open, never run.** The ratio is arithmetic, the wall-clock is not: per-request latency over a UART is unmeasured |
| CDK-34 | With the application running normally, pick **reinstall everything** and try to write | The page says the application answered rather than the bootloader, writes nothing, resets nothing, and does not tell anyone to press SW2 | **open, never run.** Covered against a fake board; the point is that rc=11 means something different on this path and must not be read as "open the window" |
| CDK-35 | Hold SW2 for 5 s, then pick **reinstall everything** and write the published whole image | MCUboot accepts ~400 KB over uart0, verifies it, and the board comes back on the published SHA-256 | **open, and gated on CDK-16.** This is CDK-16's upload driven from a browser instead of the Go `mcumgr` binary. Worth running FOR that reason: a different host implementation on the same board is the one experiment that separates "the board does not answer" from "that client does not get an answer" |
| CDK-36 | Erase the application only (leave MCUboot), then run CDK-35 with no button press at all | `CONFIG_BOOT_SERIAL_NO_APPLICATION=y` leaves the board already waiting in recovery, and the page installs onto a board with no working software | **open, never run.** This is the row that decides whether the probe is genuinely optional after the first flash |

CDK-14 through CDK-26 are the open rows, and none has ever been run to
completion. CDK-16, CDK-17 and CDK-18 cover recovery, STS quality and NLOS
walk-up. CDK-19 through CDK-26 are the Apple Home plus Home Assistant release
gate added with the five-fabric transaction work: host tests and the target
build cover their local state machines, but none inherits a hardware result
from those tests. Do not describe multi-admin operation as hardware-robust
until all eight of those rows pass. CDK-27 and CDK-28 are the binding, and both
PASSED on 2026-08-22 -- which incidentally settles CDK-20 in passing, since a
second administrator had to exist before anything could write a binding at all.
Note what that does NOT settle: the binding was proven against this repo's own
DK, and every one of the five faults it took to get there was a place the code
agreed with itself and not with another implementation. A commercial peer has
still never answered. CDK-14 is
the only rate on this list: everything above it has been demonstrated at least once,
and none of it at a rate. CDK-15 is the resumable apply, whose step counter is
exercised by design and by host test but has never met a real power cut.

CDK-29 through CDK-31 are the browser update path, and they are the cheapest
open rows on this list to close: the firmware is the same `SMP=1` release image
CDK-13 already passed by hand, so the only thing that has never met a board is
the JavaScript. CDK-31 is the one worth running even though it sounds like a
non-event -- a board with no applicable delta is the normal state of anything
more than a few releases old, and a page that handled it badly would be the
first thing most people saw.

CDK-32 through CDK-36 are the cable. CDK-32 and CDK-33 are low risk and cost one
build: the transport changed, nothing above it did, and a host suite already
compares the framing byte for byte across three implementations -- the standard
library's CRC-16/XMODEM and base64, the CLI client, and the browser's copy.

**None of these rows needs a browser.** `ultrawidelock_smp.py --serial` speaks
the same protocol from the command line, which is the cheaper way to run them
and the one that isolates the firmware:

```sh
make flash SMP=1 RELEASE=1               # the image with the UART transport in it
make ota-smp-list OTA_SERIAL=auto        # CDK-32, the identify half
make ota-smp      OTA_SERIAL=auto        # CDK-32, the upload half
```

Running the CLI first is worth the extra step rather than a detour. If it works
and the page does not, the fault is in JavaScript; if neither works, the fault
is in the firmware or the wire, and no amount of reading the page will find it.
That split is otherwise expensive to make.

CDK-35 and CDK-36 are the ones that matter, and CDK-35 should be run before
anything else on this list is attempted, because of what it can settle. It has a
command-line form too -- `ultrawidelock_smp.py --serial PORT --chunk 128` against
a board held in recovery -- which is the same second opinion without the browser
in the way. CDK-16
has been open since 2026-08-02 with a symptom nobody has explained: MCUboot sits
in its recovery window on a UART that is measurably working, and does not
answer. Everything ruled out so far was ruled out from the board's side. The
browser is a SECOND, INDEPENDENT HOST IMPLEMENTATION of the same protocol on the
same wire -- so if it gets an answer, the fault was never the board, and if it
does not, the fault is not the Go client. That is a real bisection of a stuck
bug, available for the cost of one page load, and it is the reason CDK-35 is
worth running even though CDK-16 is expected to fail.

CDK-36 is the claim the page now makes to first-time owners in as many words:
that the J-Link is needed once and then never again. Until it has been run, that
sentence is a design intention rather than a measured fact.

## nRF5340 DK

### Test setup

- nRF5340 DK with DWM3000EVB (Arduino header) and X-NUCLEO-NFC12A1, wired per
  [`apps/nrf5340dk-lock/overlays/dw3000-nfc.overlay`](../apps/nrf5340dk-lock/overlays/dw3000-nfc.overlay).
- `make dfu-key`, once per clone, the same key the DWM3001CDK uses. `DFU=1` is
  this board's default and the build refuses to give MCUboot a key it does not
  own. `DFU=0` builds the older no-bootloader layout and needs none.
- An iPhone (or Apple Watch) with the lock's Aliro key provisioned in Wallet.
- Serial console attached (`make nrf-term`) to observe logs.

### Checklist

| ID | Procedure | Pass criterion |
|---|---|---|
| HV-1 | `make test` on the release commit | Exit 0, all host KATs pass |
| HV-2 | `make rebuild` (pristine) | Exit 0, image links and fits flash |
| HV-3 | Flash a `make selftest` build, boot with no phone present | Boot self-test reports pass on the console |
| HV-4 | Flash the release image (`make flash-erase` for a first flash), boot | Clean boot, `credential source stack enabled` appears with no errors, BLE advertising starts |
| HV-5 | Tap the phone on the NFC reader (Express Mode, screen off) | Lock actuates to unlocked; console logs the granted access |
| HV-6 | Relock, then approach from well outside ranging distance, phone pocketed | Lock unlocks on approach with no phone interaction |
| HV-7 | Walk away from the lock | Lock relocks after passing the hysteresis margin, and does not oscillate at the boundary |
| HV-8 | Power-cycle the DK, wait for boot, repeat HV-5 and HV-6 | Both unlock paths work without re-provisioning the key |

## ESP32-S3

No NFC tap path exists on this target, so there is no equivalent of HV-5.

### Test setup

- ESP32-S3 dev board with a DWM3000EVB wired per
  [`docs/esp32-bringup.md`](esp32-bringup.md), including the EVB's
  power-select jumper.
- An iPhone with a key provisioned in Wallet for *this* reader identity. A key minted
  against a different reader will not authenticate.
- Serial console attached (`make monitor` from `apps/esp32-matter-lock`).

### Checklist

| ID | Procedure | Pass criterion |
|---|---|---|
| EV-1 | `make check` on the release commit | Exit 0, all host suites pass |
| EV-2 | `make rebuild` in `apps/esp32-matter-lock` | Exit 0; `verify_port.sh` reports the link seam intact and the app fits its partition |
| EV-3 | `make flash-erase`, then boot | Clean boot, onboarding codes printed, no watchdog resets |
| EV-4 | Commission into a home with the printed code | Commissioning completes; `status` shows the fabric |
| EV-5 | Confirm a key lands in the phone's wallet | Key appears, tied to this reader |
| EV-6 | `ultrawidelock prov` on the console | Reports a provisioned identity, not the dev-identity fallback warning |
| EV-7 | Approach from well outside ranging distance, phone pocketed | Wallet unlock animation plays and the bolt opens, with no phone interaction |
| EV-8 | Watch the console through EV-7 | Continuous positive distances tracking the approach; no watchdog reset |
| EV-9 | Walk away | Bolt relocks past the hysteresis margin and does not oscillate at the boundary |
| EV-10 | Re-approach within the same session | Unlocks again without a reconnect |
| EV-11 | Power-cycle the board, wait for boot, repeat EV-7 | Unlock works without re-commissioning or re-provisioning |
| EV-12 | `lab on`, then approach from beyond BLE range and watch the trace | `gate.hold` appears and no `rrx`/`rtx` follows until `gate.open`; the radio stays dark while the phone is far |
| EV-13 | Loiter out of range for ~10 s during EV-12 | Repeated `session.start` / `gate.hold` / `session.end` cycles are correct, not a fault; the phone gives up at ~1.9 s and retries |
| EV-14 | Unlock, then stand still at the door for 10 s | No `relock.sent`, bolt does not cycle. iOS pauses ranging when still; a relock here is the regression |
| EV-15 | Unlock, then leave briskly | `relock.sent` appears **before** `session.end`, and the phone shows locked as you go |
| EV-16 | Re-approach after EV-15 | No `relock.sent` between `ph.apc` and the grant, i.e. the Wallet does not flash locked then unlocked |
| EV-17 | Score any capture that reached UWB-active. The bench scoring script this used is not in this repository | The `order` check passes. `ph.m1` before `ph.m2` and `ph.m3` before `ph.m4rx`: setup stamps follow message identity, not arrival order |
| EV-18 | Read the `ranging setup:` line of that report | Reads `rrx SUPPL id=0, rrx IRS, rtx M1, rrx M2, rtx M3, rrx M4`. A bare `rrx id=` with no protocol means pre-fix firmware |
| EV-19 | Double-click the board's button while the application runs | The update window opens, and the long press still opens a commissioning window rather than this | **open, never run** |
| EV-20 | With the window open, install from https://ultrawidelock.com/flash/index.html in Chrome | The whole ~2 MB image goes over Bluetooth, the board reboots into it, and it commissions and unlocks as before | **open, never run.** The entire ESP32 update path is new firmware that has compiled and never executed |
| EV-21 | Cut power partway through EV-20, then restore it | The board boots the image it was already running; otadata was never pointed at a half-written slot | **open, never run** |
| EV-22 | Install an image signed with a different key | The board refuses it with the signature error and writes nothing | **open, never run** |
| EV-23 | Install an image that links but panics before the end of `app_main` | The bootloader rolls back to the previous slot at the next boot, and the lock still answers | **open, never run.** This is the row that decides whether a bad update is recoverable without a cable |

EV-7 is the row that matters most and the one most easily faked: the bolt moving is not
a pass. The Wallet animation is the pass criterion, because that is what proves the
reader told the phone it granted access rather than just actuating locally.

EV-12 to EV-16 gate the RSSI power gate and the relock policy. The guide that
explained each measurement and its thresholds documented bench tooling that is no
longer in this repository, so it is not included here. EV-14 and EV-16 are regression rows: both behaviours were
shipped broken once and are invisible unless specifically looked for.

EV-17 and EV-18 are the third such row. The ranging-setup latency stamps used to be
assigned by arrival order, and the phone sends a proto-3 (supplementary-service) SDU
ahead of Initiate-Ranging-Session, so every device-to-reader label sat one frame early
— the report claimed M2 arrived before M1 was sent. Nothing in the protocol depended
on it, but every setup timing read from those captures was wrong. Measured on the
fixed firmware, the setup exchange is IRS +2.0 ms M1, +27.8 ms M2, +2.4 ms M3,
+27.7 ms M4; the old labelling reported that as a 29.7 ms IRS-to-M4 span, which was
really IRS to M2.

EV-19 through EV-23 are the over-the-air update path, and none of it has ever
executed: unlike the DWM3001CDK, which already spoke mcumgr, this is entirely
new firmware. EV-23 is the row that decides whether the feature is safe to have
at all. An update that links and then panics is not hypothetical, and without a
working rollback the recovery for it is a cable -- which is the one thing the
whole path exists to avoid. EV-21 and EV-22 are the other two ways it can go
wrong quietly: a half-written slot that the bootloader is nonetheless pointed
at, and an image that was never signed by the release key.

## Recording results

Copy the relevant tables into the release notes with a Result column (`pass` / `fail` /
`n/a`), plus: firmware commit hash, toolchain version (NCS, or ESP-IDF and esp-matter),
board revision, phone model, and iOS version. A release ships only when every applicable
row is `pass`.
