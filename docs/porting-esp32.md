# Porting to ESP32-S3

**Status: shipped and hardware-validated.** Phases 1 through 4 are done; approach unlock
runs end to end on an ESP32-S3 against a live iPhone. Phase 5 (the NFC tap path) was not
attempted. The code is [`ports/esp32`](../ports/esp32): components plus the
bench-reader and matter-lock apps.

The original plan, written before any ESP32 code existed, with what it got right
and wrong. Its estimates are unedited. Bring-up detail it could not have
predicted is [`docs/esp32-gotchas.md`](esp32-gotchas.md).

Target hardware: ESP32-S3-WROOM (N16R8, 16 MB flash / 8 MB octal PSRAM, Wi-Fi + BLE, no
802.15.4) plus a DWM3000EVB for UWB.

## 0. What the plan got wrong

- **The BLE transport was not the dominant rewrite.** The plan called ~650 lines
  of NimBLE GATT + L2CAP the concentrated new work; it was the easy part. The real
  work was the layer the plan assumed it could keep: credential authentication and
  ranging-key derivation, which the reference delegates to a closed ARM-only
  library that cannot link on Xtensa. Reimplemented from the wire up: key
  schedule, two separate GCM channels, wire codec, reader identity.
- **No line item for real-time behaviour.** The DS-TWR responder was treated as
  "already done, just recompile it". The logic was correct, as the nRF proves, but
  the ESP32 has slower SPI and jitterier callback dispatch, and arming each frame
  inside a 2 ms slot took its own campaign: DMA-disabled SPI, an STS key cache,
  and throttling a per-round log that starved the ISR task.
- **The reference source did not have to be a Nordic-licensed study target.** The
  decisive facts are in the published Aliro 1.0 specification, not the add-on's
  C++.

## 1. Summary

Not a spec reverse-engineering project. The DIY Nordic lock already provisions a
credential into Apple Wallet, and the reader that does it is readable, layered
Nordic source: `subsys/aliro/` (about 12 components, ~4.4k relevant lines) plus
the app-side `src/aliro/` (~5.1k lines). The port reimplements that ~10k lines of
C++ on ESP-IDF, tiered by portability. The rewrite is ~650 lines of BLE
transport (Zephyr Bluetooth to NimBLE); everything else is common-SDK,
PSA-portable, or already seam-defined.

Decision: **ESP-IDF + esp-matter**. First engineering is the UWB engine on
silicon (Phase 1). The working nRF closes the old "will Apple provision a DIY
lock" gate.

Constraint held throughout: **the working nRF5340DK stays untouched**, an
observe-only reference with no reflash, reconfigure, reprovision or key readout,
and no connection without sign-off.

Licensing: the add-on is `LicenseRef-Nordic-5-Clause`, restricted to Nordic
devices, so nothing under it can ship in an ESP32 port. The port is built from the
published Aliro 1.0 specification.

## 2. Port map (grounded in the add-on source)

> Historical. The tier assignments below held up for Matter, crypto primitives, and
> storage. They missed the closed-library boundary described in §0.

The engine seam is confirmed. Nordic's credential reader talks to UWB only
through the `UltraWideBand` C++ interface (`subsys/aliro/uwb/.../uwb.h`), and this
repo's engine implements it:
`integrations/nrfconnect-door-lock/patches/custom_impl-uwb.patch` fills
`_ConfigureRangingSession(sessionId, ursk, ...)` to call
`ultrawidelock_uwb_session_set_ursk`, and `_HandleBleMessage(...)` to route the
Aliro UWB messages, backed by `ultrawidelock_uwb_facade`. On ESP32 the engine
keeps implementing the same interface; the reader above it is rebuilt.

Portability tiers (line counts are the Nordic reference, not a copy target):

| Component | ~Lines | Tier | ESP-IDF plan |
|---|---|---|---|
| door_lock_delegate + Matter Aliro cluster | 330 + SDK | A: common SDK | connectedhomeip is shared by NCS and esp-matter, so the Matter/provisioning side ports with modest change. This is why DIY Wallet provisioning already works. |
| crypto_utils + app crypto seam | 327 + part of 1311 | A: PSA-portable | Uses the PSA Crypto API (`psa/crypto.h`). ESP-IDF mbedTLS provides PSA; port at the API level, backend moves CC312 -> mbedTLS. |
| ultrawidelock_service (orchestration) | 975 | B: logic + seams | Portable C++; swap Zephyr workqueue/storage seams. |
| access_manager, disambiguation, access_document | 2171 + 352 + hdr | B: logic + seams | Credential matching / access decision logic; mostly portable. |
| reader_storage, storage, kpersistent_manager | 1374 + 654 + 329 | B: storage seam | Zephyr settings/NVS -> ESP-IDF NVS. |
| aliro_workqueue, time_utils, lock_sim | 83 + 95 + 245 | B: small glue | Zephyr workqueue/time -> FreeRTOS/esp_timer; lock_sim trivial. |
| gatt_server + l2cap_server | 354 + 300 | C: real rewrite | Zephyr `bluetooth/gatt.h` + L2CAP CoC -> NimBLE GATT + L2CAP CoC. The Aliro BLE transaction transport. The dominant rewrite, but only ~650 lines. |
| uwb/custom_impl over ultrawidelock_uwb | small + engine | D: done + engine port | Keep the `UltraWideBand` impl; port the engine per section 4. Do NOT port `uwb/qm35_impl` (the Qorvo coprocessor path, ~3k lines, unused here). |
| platform/nfc (RFAL + ECP) | 647 | E: use esp-aliro | Espressif ships **esp-aliro** (github.com/espressif/esp-aliro), first-party Aliro-over-NFC on ESP32, license-clean. Use it for the tap applet instead of an independent RFAL reimplementation. NFC-only today; BLE + UWB are on its roadmap. |

Provisioning and crypto are the portable parts: common connectedhomeip, PSA. The
concentrated new work is BLE transport on NimBLE.

## 3. Decision: ESP-IDF, not Zephyr

Apple Home + Wallet requires Matter (no sideload path to Wallet). On ESP32-S3 (no 802.15.4) that is
Matter over Wi-Fi, which Apple Home accepts; the transport is irrelevant to Apple (the nRF used
Thread). esp-matter is the only viable ESP32 Matter path and shares connectedhomeip with NCS,
so the door_lock_delegate + Aliro cluster (Tier A) carries over. ESP-IDF also gives native NimBLE
(the L2CAP transaction), mbedTLS-PSA (the crypto), NVS (storage), FreeRTOS core-pinning, and PSRAM.
Espressif also ships first-party Aliro (esp-aliro, NFC today; BLE + UWB roadmapped).

The decision held: NimBLE's L2CAP CoC carried the transaction, mbedTLS-PSA covered
the crypto, NVS covered storage, and core-pinning mattered for the ranging task.

### Toolchain

- `make esp-bootstrap` installs both on a machine with neither: ESP-IDF at the
  version esp-matter recommends, then esp-matter on that tree, each stage asked
  for separately. The Makefiles pin only paths (`IDF_EXPORT`, `ESP_MATTER_PATH`)
  and check only that the export scripts exist, so a self-managed install is
  untouched and `make tools` reports whichever the build will use. CI pins its
  own copy: an ESP-IDF container digest plus a bench-validated esp-matter
  revision, in `.github/workflows/firmware-builds.yml` and `release.yml`.
- Stage the install if disk is tight: plain ESP-IDF with the `esp32s3` target
  covers the bench reader app (a few GB). The matter-lock app also needs
  esp-matter, which is much larger.

## 4. Engine port surface (Phase 1, concrete)

The UWB engine (`modules/ultrawidelock_uwb`) already compiles as pure C on host (`tests/host/sources.sh`,
13 core files). Minimal ESP-IDF port:

- OS seam: extend `modules/ultrawidelock_uwb/src/facade/ultrawidelock_alloc.h` (already wraps `k_malloc` and monotonic
  microseconds) into a `ultrawidelock_os` seam with a FreeRTOS backend. Real call sites are few:
  `uwb_min.c` (`k_uptime_get` busy-wait deadlines ~188-307, `k_msleep` 31/122/362) and
  `fira_session.c` (`k_uptime_get` 71/125). `k_work`/`k_timer` uses are diagnostics/self-test/log
  only (`uwb_rxdiag.c`, `uwb_selftest.c`, `ultrawidelock_logfmt.c`) and stub or defer.
- DW3000 platform shim, rewrite on ESP-IDF SPI-master + GPIO: `deps/dw3000/platform/dw3000_spi.c`
  (241), `dw3000_hw.c` (298), `deca_port.c` (60). Leave `deca_compat.c` (1352, vendor) logic intact.
- STS seam: the four helpers in `modules/ultrawidelock_uwb/include/uwb_seam.h`. Compile-time, no
  linker feature, nothing toolchain-specific to verify. The port
  omits `uwb_rxdiag.c`, which is `k_work`-based, and supplies `ultrawidelock_uwb_set_callbacks` and
  `ultrawidelock_uwb_configure_phy` from `port/ultrawidelock_seam_stubs.c`; `ccc_shim_rx.c` and `ccc_shim_wrap.c`
  carry `ultrawidelock_uwb_arm_rx` and `ultrawidelock_uwb_set_sts_iv` across unchanged.
- Crypto seam: `ccc_crypto_prim.c`, routed through the primitive provider over
  ESP-IDF's Mbed TLS PSA implementation.
- Placement: pin engine + DW3000 SPI to core 1, BLE/Wi-Fi on core 0. Hot buffers in internal SRAM,
  not PSRAM (jitter).

## 5. Phased plan and outcomes

| Phase | Plan | Outcome |
|---|---|---|
| 1: engine on ESP-IDF | Compile the engine behind an OS seam, write a DW3000 backend, range against a second board with a canned URSK. | **Done.** The compat layer let `modules/ultrawidelock_uwb/src` and `deps/dw3000` compile unchanged, and the `--wrap` link-time interposer the seam used at the time behaved as on any GNU ld. (That interposer is gone: the seam is now the compile-time one in `uwb_seam.h`, which is why the section above no longer mentions the linker.) The one surprise was hardware, not software: an EVB power-select jumper hid the radio for days. |
| 2: BLE transport | Reimplement GATT + L2CAP CoC on NimBLE. Estimated the dominant rewrite. | **Done, and easier than planned.** `ports/esp32/components/ultrawidelock_ble`. Advertising, the SPSM/version characteristics, and the CoC came up quickly. |
| 3: reader logic and the ranging key | Port the reference's reader logic and storage; the derived key enters the engine at the existing seam. | **Done, and far larger than planned.** The reference does not derive the key in portable code at all: a closed ARM-only library does. This phase became a from-scratch reimplementation of the credential authentication, key schedule, secure channels, and wire codec, plus a provisioning seam. |
| 4: Matter provisioning | esp-matter door lock over Wi-Fi with the Aliro cluster and delegate, so the lock self-commissions and provisions a key into the wallet. | **Done.** `apps/esp32-matter-lock`. The Tier-A bet paid off: the cluster and delegate came across with modest change. The reader attaches to esp-matter's NimBLE host rather than starting its own. |
| 4.5: real-time ranging | Not in the plan. | **Done, and unplanned.** Making the DS-TWR responder hold a 2 ms slot on ESP32 was its own campaign. Nothing here was a logic bug; the shared engine was already correct. |
| 5: NFC tap | Integrate a first-party Aliro-over-NFC stack rather than reimplementing RFAL. | **Not attempted.** No NFC reader was sourced. The ESP32 target is approach-unlock only. |

## 6. Hardware bill of materials

- ESP32-S3-WROOM N16R8 dev board.
- DWM3000EVB (DW3110) for UWB on SPI2, eleven jumpers. Current pin map:
  [`ports/esp32/components/ultrawidelock_uwb/port/board_pins.h`](../ports/esp32/components/ultrawidelock_uwb/port/board_pins.h),
  wiring table in [`docs/esp32-bringup.md`](esp32-bringup.md).
- No NFC reader. Phase 5 was not attempted: PN532-class parts are assumed too
  limited for Express / ECP timing, and the reference uses ST25R.
- Matter over Wi-Fi on the S3 is not low-power like the nRF Thread sleepy end device.
  Fine for a mains-powered reader, wrong for a battery lock.

## 7. Risks and assumptions, resolved

- **Confirmed.** The Aliro Door Lock cluster is in connectedhomeip
  (`SetAliroReaderConfig` / `ClearAliroReaderConfig` and the feature flag), so esp-matter
  has it and the Tier-A carry-over worked.
- **Confirmed.** NimBLE's L2CAP CoC and GATT cover what the transaction needs, given a
  larger host task stack: the 4096-byte default overflows during software P-256.
- **Confirmed.** ESP-IDF's mbedTLS-PSA covers the ECDH, ECDSA, and AES-GCM the reader
  needs.
- **Confirmed, then made moot.** `--wrap` on `xtensa-esp32s3-elf-ld` did behave as on any GNU
  ld. The risk no longer exists in that form: the seam moved to compile time
  (`uwb_seam.h`), so no linker feature is involved. `make seam`, the `uwb-seam`
  gate in `make check`, guards it now and runs on every host sweep and on CI.
  `verify_port.sh` also checks the seam, but only in a developer shell with
  ESP-IDF sourced: `make check` sets `ULTRAWIDELOCK_NO_TARGET_BUILD=1` and CI's runner has no ESP-IDF,
  so it is not what stands behind this.
- **Confirmed on S3, with a caveat.** BLE and UWB coexist on one S3, but not for free:
  the ranging task is pinned to core 1 and the console to core 0, and the transaction
  runs synchronously on the BLE host task because driving it from elsewhere races the
  host. The hardware-validated C6 shares its single core between BLE/Wi-Fi and the
  priority-23 UWB worker.
- **Held.** Wire behavior follows the published Aliro 1.0 specification, with
  values it does not fix observed from a real phone; no restricted source was
  copied.

## 8. Before starting a port

Read [`docs/esp32-gotchas.md`](esp32-gotchas.md) first: forty-odd traps, each
with its console symptom and its fix. Three cost more than a day each: an EVB
power jumper, a ranging session id that is derived rather than chosen, and a
per-round log line that starved the DW3000 ISR task.
