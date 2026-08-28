# Changelog

What changed for someone running the firmware, not the commits that got it
there. `scripts/release-notes.sh` reads the section matching a tag and renders
it into that release's notes, so this file is the authored source and the git
log is only the fallback.

Versions follow [semantic versioning](https://semver.org). Dates are the day the
tag was cut.

## [Unreleased]

Nothing yet.

## [0.4.0] - 2026-08-28

The lock stopped needing Zephyr, grew a second anchor so it can tell inside from
outside, and learned to take a firmware update from a browser. 599 commits and
30 pull requests since v0.3.0, across 2,947 files.

### Read this before you update a v0.3.0 board

**Existing pairings do not survive.** The DWM3001CDK settings partition moves
from `0x7e000` to `0x7c000` and the custom Matter record schema becomes `mf2`.
Matter fabrics and the Home Key reader identity are deliberately not migrated:
Apple Home has to add the lock again, and Home Assistant has to be shared again
after that. This is a one-time break, and the partition does not move again.

If you want the old identity off the part first, back it up over SWD before you
flash. `ultrawidelock export` will not do it, because the Matter image compiles
no shell:

```sh
JLinkExe -device nRF52833_xxAA -if SWD -speed 4000 -autoconnect 1 \
  -CommanderScript <(printf 'savebin s.bin 0x7C000 0x4000\nexit\n')
```

Check the result is 16,384 bytes and not all `0xFF` before you trust it. An
erased page saves silently and looks exactly like a backup.

### A third port, and no Zephyr in it

The same lock now builds on **FreeRTOS on the nRF52833**, assembled a layer at a
time and measured as each one landed: NimBLE on the SoftDevice Controller with
MPSL arbitrating flash against the radio, OpenThread on the pinned 802.15.4
driver, Mbed TLS and PSA, the DW3110 over SPIM3, a persistent key-value store,
and the Matter node on a checked shim. The provisioning console runs over USB
CDC ACM and no longer needs SW2 held to reach it.

Signed updates over BLE work on this port, on the oracle's wire protocol, and
have been proven on hardware.

### Two anchors, and a side to be on

A second anchor turns one distance into a side. The satellite ships as a
standalone app on both the ESP32 tier (over an ESP-NOW carrier) and the CDK, it
reports its distance sealed and bound to the ranging block it came from, and the
lock fuses the pair only when both halves come from one block.

- **The side gate fails closed** for passive unlock: no agreeing second opinion,
  no walk-up open. `docs/bench-inside-outside.md` runs the three-board bench.
- **Inside and outside BLE witnesses**, with a proven pick that outlives the
  approach and follows BLE address rotation.
- A lock-owned freshness epoch, so a reboot cannot roll the replay window back.
- Per-board UWB range bias calibration, and a configurable OUTSIDE margin.

### Updates from a browser, on either chip

**[ultrawidelock.com/flash](https://ultrawidelock.com/flash/index.html)** finds
the board, reads what it is running, and sends the one update that applies.
Chrome or Edge on a computer, or Chrome on Android. Neither Web Bluetooth nor
WebSerial exists in Safari, so not from an iPhone.

|  | DWM3001CDK | ESP32-S3 / C5 / C6 |
|---|---|---|
| Transport | mcumgr, over BLE **or** USB | native GATT frames |
| Payload | signed delta, about 11 KB | signed whole image, about 2 MB |
| Time | seconds | several minutes over BLE |

Nothing is written until you open the update window on the board itself: **SW2**
on the CDK, a **double-click** on the ESP32 button. Authenticity is a P-256
signature checked before a byte is written and again by the bootloader.

The CDK also gained **reinstall over the cable**, which is the one that matters
when things have gone wrong: a whole signed image through MCUboot serial
recovery, needing no starting image, no update window, and no working software
on the board.

Hardware status: the browser delivered a delta to a real board over Web
Bluetooth on 2026-08-27 (CDK-30), and serial recovery itself is proven
(CDK-16). The no-delta-applies path (CDK-31) and the cable-versus-radio timing
(CDK-33) have never been run against hardware.

### Matter Door Lock grew up

- `LockOperation` events, `AutoRelockTime`, and both writable Door Lock
  attributes persisted across reboot.
- Apple's **Approach Direction** cluster, and a live **UWB presence** cluster
  that reads only with the Manage privilege.
- **`DoorLockAlarm`**, so the lock can report the door and not just the bolt: a
  forced door from the impact latch, a door left ajar from the swing angle.
  Anchor builds only, and no controller has been seen rendering one yet.
- **A Matter client**, so a walk-up here can open a *second* Matter lock with no
  hub automation in the path: CASE as initiator, the Binding cluster, and a
  `chip-tool` helper that sets up both ends. Off by default. Working on hardware
  since 2026-08-22 against a second lock built from this repo; no commercial
  lock has answered it yet.
- Multi-admin commissioning hardened: five-fabric Apple Home plus Home Assistant
  coexistence on the CDK, one committed Thread dataset, provisional
  commissioning rollback, selective durable `RemoveFabric`, per-fabric state,
  and transactional retries.
- The UWB device id is now derived from the credential, and identifier hashing
  left the hot path.

### Home Assistant

A companion integration, with persistent UWB policy controls and independent
lock action policies, so presence can drive one thing and unlocking another.

### Portability, and gates that keep it

- A **key-value seam addressed by number, not by name**, implemented over Zephyr
  settings, ESP-IDF NVS and the FreeRTOS store, with storage names declared once
  and gated against drift.
- A datagram seam for the sealed link, and AES-128-CCM in the crypto primitive
  seam.
- **`make sdk-export`** for the hardware-agnostic SDK, plus a size gate on every
  port and a port purity gate that now covers the whole tree rather than 64% of
  it.
- CI compiles firmware on every change that can break it: the CDK client image,
  the anchorlink image, the shipping image and the satellite, taking application
  coverage to 9 of 13.
- Static analysis gates for the portable tree, and workspaces shared by content
  hash so a build does not rebuild eleven patches to find out nothing moved.

Host suites now run **9,608 checks** across 18 suites, up from 7,979 at v0.3.0.

### Fixes worth naming

- Matter: resend Sigma3 for a duplicate Sigma2, republish the SRP name after a
  re-pair, chunk large NOC lists, and report operating modes right-side up.
- Credential: hold the bolt through an iOS session flap, relock on a graceful
  close, and meet PSA's output-size contract on the multipart AEAD path.
- Report lock-state truth to Matter even when the phone is gone.
- ESP32: the provisioning namespace fits in NVS again, and four ways the ESP32
  two-anchor gate diverged from the nRF lock are closed.
- Latch: a reboot no longer freezes the entry dwell, and a settings load no
  longer resets the tuning.

Full diff: <https://github.com/ultrawidelock/ultrawidelock/compare/v0.3.0...v0.4.0>

## [0.3.0] - 2026-08-05

Released before this file existed. See the
[v0.3.0 release notes](https://github.com/ultrawidelock/ultrawidelock/releases/tag/v0.3.0).

## [0.2.0] - 2026-07-22

Released before this file existed, under the project's former name.

## [0.1.0] - 2026-07-22

First tagged release, under the project's former name.
