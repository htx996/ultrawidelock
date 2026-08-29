# ESP32-S3 gotchas

Non-obvious findings from porting the Aliro UWB door-lock reader to ESP32-S3
(ESP-IDF + esp-matter + NimBLE + DWM3000EVB). Each entry is a trap hit on the bench.

Ports live under [`ports/esp32`](../ports/esp32) (reader / crypto / BLE components and
the standalone bring-up app) and [`apps/esp32-matter-lock`](../apps/esp32-matter-lock) (the Matter
door-lock app hosting the reader). The ranging engine is `modules/ultrawidelock_uwb`, shared
byte-for-byte with the nRF5340 build.

> **Bare `make build`, `make flash` and `make monitor` below are run from inside an app
> directory**, where the local Makefile forwards them to ESP-IDF. At the repository root
> those same names mean the DWM3001CDK; the root spelling is `make esp-build`,
> `make esp-flash` and `make esp-monitor` with `APP=` and `TARGET=`.

> Verification convention below: **VERIFIED** = confirmed on silicon or byte-exact
> against a host KAT; **BENCH-GATED** = built + host-tested but not yet
> confirmed against a live iPhone; **PREDICTED** = derived from vendor source or
> datasheet only, never observed here, so a hazard to measure rather than a finding.

---

## 1. Toolchain, build & flash

### 1.1 `make flash` aborts with `awk: towc: multibyte conversion failure`
**VERIFIED.** The Makefile classifies USB serial ports by piping `ioreg -l -w0` through
`awk`. Some attached USB device ships a non-UTF-8 string descriptor; under a UTF-8 locale
`awk` aborts mid-stream (`towc` conversion), killing **both** port detection and the
SEGGER/J-Link safety guard, so `make flash` reports "no ESP serial port found" with the
board plugged in.

- **Fix:** prefix the `awk` invocation with `LC_ALL=C` (byte-wise, locale-independent).
- **Subtlety:** `LC_ALL=C ioreg | awk …` does **not** work: the env prefix applies only
  to the first command in the pipeline. It must be `ioreg | LC_ALL=C awk …`.
- Applied at both `awk` sites in `apps/esp32-matter-lock/Makefile` (the RESOLVE_PORT table
  build and the `ports` target).

### 1.2 Never flash the SEGGER/J-Link (nRF) port
**VERIFIED.** With both an nRF5340DK and the ESP32-S3 attached, port auto-detection can
see the J-Link (SEGGER, USB vendor `0x1366`). Flashing esptool to it is wrong and can
disrupt the nRF. The Makefile refuses that vendor and auto-selects only the ESP devkit's
bridges: WCH CH343/CH9102 (`0x1A86`) or Espressif native USB (`0x303A`), both
`/dev/cu.usbmodem…`. `ioreg` reports vendor ids in decimal, hence the Makefile's
`4966` / `6790` / `12346`. Keep the guard.

### 1.3 `make flash` port-holder guard
`make flash` refuses if another process holds the port (a stuck `monitor`). Override with
`FORCE=1` only after confirming no other session owns the port. (commit `8ce132a`)

**VERIFIED addendum.** `lsof -t "$port"` silently missed a live `idf.py monitor` on one
Mac, so the guard passed and a `make esp-app-flash` spent a full build before esptool died
on `[Errno 35] Could not exclusively lock port`. Never reproduced, so the guard no longer
relies on naming the holder: when `lsof` finds nothing it also tries to take the exclusive
`flock` that esptool needs, and refuses if that fails. `FORCE=1` still kills a named PID,
and says so when there is a lock but no PID to kill.

### 1.4 A Kconfig `default` does not reach an app that already has an `sdkconfig`
**VERIFIED, at a cost of three bench sessions.** Changing `default` in a `Kconfig` only
seeds a **fresh** `sdkconfig`. An app with an existing one keeps its old value, so the
build silently ships the previous setting and every measurement after it profiles the old
firmware.

- **What hid it:** *new* symbols behave the opposite way. Absent from `sdkconfig`, they do
  track their Kconfig default, so a batch of changes half-applies and reads as "the config
  took" when it did not.
- **Fix:** pin anything that matters in `sdkconfig.defaults`, which this repo treats as
  the tracked source of truth (see the app `.gitignore`), **and** confirm what shipped
  before trusting a bench number:
  ```
  grep ULTRAWIDELOCK_RSSI_GATE apps/esp32-matter-lock/sdkconfig
  ```
- **Do not** `rm sdkconfig` to force regeneration unless everything needed is in
  `sdkconfig.defaults`; menuconfig-only settings are lost with it.

### 1.5 The apps have incompatible flash layouts, and they share your board
**VERIFIED.** Flashing one app over another leaves a bootloader that cannot find the
next app's partition table, and the board sits in a reset loop reporting
`partition 0 invalid magic number 0xffff`.

| app | flash size | partition table |
|---|---|---|
| `apps/esp32-matter-lock` | 4 MB | 0xC000 |
| `examples/esp32/initiator` (HIL) | 2 MB | 0x8000 |

- **Tell:** the boot banner's `SPI Flash Size` disagrees with the app expected, and its
  `compile time` is not from this build.
- **Fix:** a full `make flash`, never `app-flash`. Only the full target rewrites the
  bootloader and partition table together.
- **Related:** the board may have more flash than the image header declares (16 MB part,
  4 MB header). The bootloader honours the header, and the
  `spi_flash: Detected size larger than ... image header` warning is benign.

### 1.6 Host tests are the target's proof
The crypto core (`ultrawidelock_hash.c`) compiles **host == target** so host KATs pin target
behaviour. Run `tests/ports/esp32/run.sh` before believing any crypto change; a compact
AES-256-GCM host double (`ultrawidelock_prim_host.c`) runs the KATs without PSA. Build
success is not proof: the wire/crypto bugs below all built cleanly. The shared
`modules/ultrawidelock_uwb` logic has a second harness, `tests/host/run.sh` (`make test`),
compiling the shared sources **without** `ESP_PLATFORM`: proof that an ESP-only guard did
not regress the nRF path.

---

## 2. Hardware bring-up

### 2.1 DWM3000 EVB power jumper (the multi-day one)
**VERIFIED.** DW3000 SPI comms silently failed until the EVB power jumper was set right.
Before: no valid device ID / responder never listens. After: `start_
ultrawidelock()=0`, responder listening, a 36-byte RX frame arrives. If DW3000 bring-up looks
dead, check the **board power-select jumper first**; it is not a software problem.

### 2.2 Probe the DW3000 at boot, never from the BLE-host callback at M4
**VERIFIED (regression) → BENCH-GATED (fix).** The standalone reader app (`examples/esp32/reader`) probes the
DW3000 at boot (`app_responder_start()` in `main.c`). The `matter-lock` app dropped that,
so the first DW3000 touch happened at M4, inside the NimBLE host callback
(`ultrawidelock_ranging_feed → engine → ultrawidelock_uwb_start_cred → dwt_probe`). There `dwt_probe`
returns `-1` (radio init `-5`): no prior bring-up plus a shallow callback stack. Symptoms:
`dwt_probe failed: -1`, then on reconnect `spi_bus_initialize: SPI bus already
initialized`.

- **Fix:** bring the radio up once in the reader-startup task. `ultrawidelock_ranging_init`
  does a throwaway `ultrawidelock_uwb_start_cred` (zero URSK) + `ultrawidelock_uwb_stop`; `uwb_min`'s
  `g_radio_ready` latches, so M4's `ccc_prepoll_listen` skips the probe and only
  re-applies the negotiated channel.
- **Corollary bug:** `dw3000_spi_init` re-added SPI devices on a second call (leaking the
  old handles); now idempotent (`return 0` if already up). The boot probe means M4 never
  re-enters `dw3000_hw_init`, so the "already initialized" path is avoided.
- **Rule:** heavy SDK bring-up (`dwt_probe` / `dwt_initialise`, OTP reads) belongs in a
  dedicated startup task with a real stack, not a protocol-event callback.

### 2.3 Onboard WS2812 status LED is on GPIO48
**VERIFIED.** The ESP32-S3 dev board's addressable status LED is GPIO48 (single WS2812),
used for bolt-state indication. (commits `ad9a63b`, `ed38895`)

---

## 3. BLE / NimBLE coexistence with Matter

### 3.1 The reader must "attach" to Matter's NimBLE host, not spin up its own
**VERIFIED.** esp-matter already owns the NimBLE host for commissioning; a second BLE
stack instance crashes / reinit-loops. The credential reader runs in **attach mode**,
coexisting on Matter's host rather than initialising its own. (commits `67234fa`,
`996a8d5`; the latter fixed a reader-reinit crash loop by keeping BLE up.)

### 3.2 Keep BLE advertising up across reader restarts
Tearing BLE down and up around the reader lifecycle caused crashes. The reader keeps GAP
advertising alive and re-arms the session instead of cycling the stack. (commit
`996a8d5`)

### 3.3 The Aliro transaction must stay on the nimble_host task
**VERIFIED.** Driving the Aliro APDU exchange from another task races the host. The whole
credential-auth + ranging lifecycle (create / feed / teardown + the engine's transmit &
event callbacks) runs **synchronously on the BLE-host task**, so no locking is needed
there. Only provisioning state shared with the REPL task (`s_trust`, `s_last_cred`) takes
a FreeRTOS mutex. (commit `04bd8cc`)

### 3.4 Advertising service data
The reader advertises the Aliro 0xFFF2 service with a dynamic tag for phone
approach-connect. Separate `DYNAMIC_TAG` staleness trap: approach-unlock dies when the
clock is valid but far behind real time (advertisement expiry tag), first hit on the nRF
side. Handled by a live tag expiry (SNTP-fed wall clock, `now + 900 s`), re-derived at
half the window and immediately on a time step, with the spec's "expiry unavailable" form
as the no-clock fallback. (commit `5a4e6c4`)

---

## 4. Aliro credential-auth crypto & key schedule

**Every fact in this section is published in the Aliro 1.0 specification** (CSA document
26-42802-001, February 18, 2026), distributed free and without registration at
<https://csa-iot.org/wp-content/uploads/2026/02/26-42802-001_Aliro_1.0_specification.pdf>.
Each subsection cites its spec section plus the line number in a `pdftotext` extraction
of that PDF (12100 lines, SHA-256 `3ede12c5b01d7fdf01c24b1ea5f2cd2da25a127c4f12f8f61ff9b208714a7d6a`),
so every citation is independently checkable. Extraction gotcha: the pseudocode
assignment arrow quoted below as `←` is `U+F0DF` (a Symbol-font Private Use Area
codepoint) in the extracted text, so grep the surrounding words, not the arrow.

Values the specification does not fix, such as the `0xA5` proprietary-information TLV in
section 4.3, were observed from an iPhone on the wire. Every fact below is also pinned by
a host KAT.

**For downstream users:** `LicenseRef-Nordic-5-Clause` clause 4 restricts use to Nordic
integrated circuits, and `LicenseRef-QORVO-2` clause 3 restricts the DW3000 driver to
Qorvo ICs. Those field-of-use terms apply to any product built on this tree.

### 4.1 GCM per-direction counters start at **1**, not 0
**Spec: §8.3.1.12 and §8.3.1.13**: both session-bound counters,
`expedited_device_counter` and `expedited_reader_counter`, are required to be initialized
to `0x00000001`.

**VERIFIED (byte-exact KAT + silicon).** HomeKey starts at 0; do not copy HomeKey here.
The first inbound decrypt (AUTH1Response) uses device_counter = 1. Starting at 0 fails the
GCM tag cleanly. `ultrawidelock_secchan_init` sets both counters to 1.

### 4.2 GCM nonce layout
**Spec: §8.3.1.6 / §8.3.1.7 for the device direction,
§8.3.1.8 / §8.3.1.9 for the reader direction**: `IV ←
0x0000000000000001 || device_counter (unsigned big endian, 4 bytes)` and `IV ←
0x0000000000000000 || reader_counter (unsigned big endian, 4 bytes)` respectively.

So `nonce = 8-byte big-endian direction (0 = reader-seal, 1 = device-open) || 4-byte
big-endian per-direction counter`, non-wrapping per direction.

### 4.3 The salt sub-fields (§8.3.1.13)
**Spec: §8.3.1.13** gives the interface byte and the full `salt_volatile` composition.
The two sibling salts are stated the same way: `salt_fast` in §8.3.1.12 and
`salt_persistent` in §8.3.1.13. Never unresolved in the spec, only in this project's
reading of it.

**VERIFIED.** The AUTH1 tag only decrypts once the session salt is exact:
- salt field 1 = **`reader_group_identifier_key.x` = pub(signingKey).x**, the reader's
  *signing* public key X, **not** the device ephemeral key (that was the bug). Derived via
  `ultrawidelock_ec_p256_pub_from_priv`; refreshed on identity load and on both Matter
  provisioning paths.
- salt item 4 = **interface byte**: `0xC3` when the transaction runs over the BLE
  transport, `0x5E` when it runs over NFC.
- salt item 10 = the phone's **`0xA5` SELECT-response proprietary-info TLV**, required
  per Table 10-2. The value `a5 08 80 02 0000 5c 02 0100` was observed from the phone's
  op-0x05 message on the wire.

### 4.4 Key-block layout (one HKDF expand → 160 bytes)
**Spec: §8.3.1.13** requires 160 bytes of derived key material
(`derived_keys_volatile`) produced by the §8.3.1.5 expand; the field offsets within that
block are enumerated there. `Kdh` comes from §8.3.1.4 (BSI TR-03111 §4.3 ECKA-DH over NIST P-256) and the expand from §8.3.1.5 (RFC 5869 HKDF-SHA-256).

`Z = SHA-256(ecdh_shared(32) || 0x00000001 || txid(16))`; then
`block160 = HKDF-SHA256(salt=CreateSalt transcript, IKM=Z, info=devicePubX(32), L=160)`.
Offsets: `ExpeditedSKReader@0`, `ExpeditedSKDevice@32`, `StepUpSK@64`, **`BleSK@96`**,
**`URSK@128`**. Because it is one expand, **if auth works (ExpeditedSK@0 is right) the
URSK@128 is byte-identical on both sides**.

### 4.5 HKDF salt-vs-info binding (settled by §8.3.1.5)
**Spec: §8.3.1.5** states RFC 5869
explicitly: `Z : input_key_material`, `salt : salt`, `FixedInfo : info`,
`L : key_material_length`.

When a wire trace looks ambiguous, go back to the spec text first.

### 4.6 APDU / wire codec specifics
**Spec, per fact:**

| Fact | Aliro 1.0 |
| --- | --- |
| `AUTH0` INS = `0x80` | Table 8-3 |
| `AUTH1` INS = `0x81` | Table 8-9 |
| `EXCHANGE` INS = `0xC9` | Table 8-14, §8.3.3.5.1 |
| usage separators `0x415D9569` / `0x4E887B4C` | §8.3.3.4.3 |
| 4-byte Aliro message header, big-endian | §11.7, Table 11-10 |
| tag `0x97` absent on BLE success | §8.3.3.5.1 |

ISO7816 case-4 short form wraps AUTH0/AUTH1/EXCHANGE. The ECDSA transcript uses usage
separators `kReaderUsage=415d9569` / `kUserDeviceUsage=4e887b4c`. The L2CAP envelope is
the 4-byte header `[type & 0x3F][opcode][len_be16]`; Table 11-10 names those fields
Protocol Header (bits B5:B0 = protocol type) and Message ID, and §11.7 makes all
multi-octet integer fields big-endian. EXCHANGE payload for the ranging flow is bare
`98 00` (URSK-ready, empty TLV): **VERIFIED**, and §8.3.3.5.1 agrees, admitting tag `0x97`
in a Reader-sent BLE EXCHANGE only on transaction failure.

### 4.7 The credential-auth §14 KAT is member-confidential
A host KAT reproduces the Aliro 1.0 §14 worked example byte-exact (salt, Kdh,
ExpeditedSKReader, BleSK, URSK, the split). It lives in **scratchpad only and is never
committed**: the §14 vectors are CSA member-Confidential. Same for the BleSK message KAT.

---

## 5. Ranging transport (post-auth M1–M4)

The reference reader relied on the **closed `libaliro` stack** to seal/open ranging SDUs
*below* the open glue. Phase 3 replaced it, so each entry below was reimplemented from
scratch, and each was a separate live-iPhone failure.

### 5.1 Ranging SDUs ride a SEPARATE GCM channel from the AP channel
**VERIFIED (spec §11.8 + silicon).** Proto-1/2/3 ranging SDUs do **not** use the
credential-auth (AP) secure channel. They use their own AES-256-GCM channel:
- Keys `BleSKReader` / `BleSKDevice` = `HKDF-SHA256(ikm = BleSK@96, info =
  "BleSKReader"/"BleSKDevice", salt = reader_supported_versions ||
  user_device_selected_version)` = `01000100` for v1.0-only.
- **Fresh** per-direction counters starting at **1** (not continued from auth).
- Feeding a proto-0 EXCHANGE response raw to the ranging engine yields `protocol 0
  unsupported` (byte 0 = the 0x00 envelope type), the symptom of missing the channel
  split.

### 5.2 Ranging SDU framing: AAD carries the PLAINTEXT length
**VERIFIED (host KAT 15/15).** Wire form:
`[proto][id][(len_plain + 16)_be16][ciphertext || 16-byte tag]`, and the GCM **AAD is the
4-byte header with the *plaintext* length** `[proto][id][len_plain_be16]`, **not** the
wire length. Getting that length field wrong fails the tag with no other signal.
Implemented in `ultrawidelock_msg_seal` / `ultrawidelock_msg_open`.

### 5.3 The missing step: Reader-Status-Access-Protocol-Completed
**VERIFIED against the reference binary.** After EXCHANGE success the reader **must** send
Reader-Status-AP-Completed (proto-2 id-3, BleSK-sealed) or the device stalls ~1.8 s and
disconnects (reason 531). Payload = `02 03 00 04 00 02 20 00` (TLV: attr-id 0, len 2,
value `[cap = 0x20][ReaderState = 0x00 Secured]`). Reader status values are Table 8-18;
these bytes are confirmed byte-for-byte against a live iPhone.

### 5.4 `00 02 00 00` is EXCHANGE SUCCESS, not a ranging cue
The `00 02 00 00` body on the EXCHANGE response is §8.3.3.5.5 success (len `0x0002`,
error `0x0000`). It is not a signal to start ranging; do not hand it to the engine.

### 5.5 THE session-id trap: it is derived from the AUTH0 TXID, not chosen
**Spec: §11.7.2.1.3**: the Attribute Value field carries the session identifier, taken as
the low-order four octets of the Transaction Identifier field (Table 8-4).

The ranging session-id is **not** the reader's choice. The device computes it from the
16-byte AUTH0 transaction id it received:

```
GenerateCryptoMaterials()  →  fills AccessProtocolCrypto[0..16] with a random TXID
GetSessionId()             =  rev(this[0xc])  =  big-endian(txid[12..15])
```

The device indexes its URSK by that session-id. Advertise a hardcoded session-id in M1
and the device replies with **GeneralError URSK_Unavailable (code 3)** at M1 and
disconnects: it has no URSK for the session named, even though the URSK *value* matches.
Fix: M1's session-id must be `(txid[12]<<24)|(txid[13]<<16)|(txid[14]<<8)|
txid[15]`. This was the final blocker after auth and transport were correct.

### 5.6 M1 carries no URSK crypto, so `URSK_Unavailable` ≠ wrong URSK value
M1 (proto-1 id-0) contains only config attributes (config-id, pulse-shape, channel,
session-id). It carries no URSK-derived material, so a `URSK_Unavailable` at M1 means the
device has **no URSK installed for that session** (see 5.5), never a URSK value mismatch:
a value mismatch would surface later at M2/M3/M4 STS.

### 5.7 Device-initiated M1
The engine emits M1 when the phone sends its Initiate-Ranging-Session (proto-2 id-1, attr
INIT_RANGING = 0x00), matching the observed iPhone. Do **not** send M1 eagerly on
StartRangingSession. (`ultrawidelock_uwb_msg.c:1164`)

### 5.8 Counter chain across the ranging handshake
Reader-direction (enc): AP-Completed = 1, M1 = 2, M3 = 3.
Device-direction (dec): the phone's first BleSK message (a proto-3 config) = 1,
Initiate-Ranging = 2, its event/M2 = 3 … Each side increments per message it *sends* on
the BleSK channel, including the proto-3 config the phone leads with.

### 5.9 proto-3 SupplementaryService is ignorable
The phone leads ranging with a proto-3 SupplementaryService config blob. **VERIFIED** the
reference's ranging engine (same Qorvo code) also returns "protocol 3 unsupported" for it;
both reference and port ignore it. It is not a skipped step, so do not chase it.

### 5.10 THE Wallet animation gate: send Reader-Status-Changed on grant (step 23)
**VERIFIED byte-exact against a live iPhone**; operation-source values are Table 11-20.
Driving the bolt is not enough: iOS plays the Wallet unlock animation only when the reader
tells the phone it granted access over the BleSK channel, the "Reader Status Changed"
message, Aliro transaction step 23 (the grant-phase sibling of 5.3's AP-Completed).
Without it the port unlocked the bolt locally and Matter posted an *accessory*
notification, but the Wallet never animated. The phone's computed distance is **not** the
gate.

Payload (proto-2 id-2, BleSK-sealed) = `02 02 00 04 00 02 04 01`: one State Attribute
(attr-id 0, len 2) = `[OperationSource = 0x04 (this device, BLE+UWB Aliro flow),
ReaderState = 0x01 Unsecured]`; relock is identical with ReaderState = 0x00 Secured.
The 65-byte access-credential public key is **not** serialized: it selects which
connection to notify and is then dropped.

Send Unsecured on grant, Secured on relock, from the proximity relock task; post it onto
the BLE-host task so it serializes with the other BleSK seals (counter stays monotonic).
(commit `ba612c8`)

---

## 6. UWB DS-TWR ranging engine: the real-time layer

Once M1–M4 negotiate the session, double-sided two-way ranging runs on the DW3000: the
phone drives Pre-POLL → POLL → Response → Final → Final_Data every ~192 ms block, and the
responder must arm each frame inside a ~2 ms slot. **This layer is shared with the nRF5340
port (`modules/ultrawidelock_uwb`), where it already worked, so nothing here is a logic
bug. Every trap below is the ESP32 being a slower, jitterier real-time target than the
nRF.** All **VERIFIED on silicon**: continuous positive DIST every round and the phone
unlocks the door.

### 6.1 The arm-latency-vs-2ms-slot budget is the whole game
Reactive arming fights a fixed deadline: `CCC_RX_SLOT_HI32 − CCC_RX_POLL_LEAD` (= 459000
hi32 units = **1836 µs**; the hi32/`dsys` unit is 4 ns, so `/250 = µs`). If the
Pre-POLL→arm path exceeds it, the delayed RX never opens and the POLL is lost
(`ARM FAIL … LATE`). On nRF this is trivial; on ESP every µs of SPI and dispatch counts,
which is why 6.2–6.7 exist.

### 6.2 DW3000 SPI: disable DMA (the biggest single win)
**VERIFIED.** A boot micro-benchmark (32 back-to-back DEV_ID reads) measured
**~84 µs/transaction** with `SPI_DMA_CH_AUTO`; holding the bus lock saved only ~15 µs, so
the rest is the per-transaction DMA-descriptor + `esp_cache_msync` path. For the small
STS/register writes on the arm critical path that setup dwarfs the bit-time. Fix: init the
bus **`SPI_DMA_DISABLED`** so ≤64-byte transfers use the CPU data registers directly, and
chunk anything larger into ≤64-byte bursts under one CS-low window (the DW3000 streams
sequentially). Only the DW3000 is on SPI2 (the status LED is on RMT), so this is safe.
(commit `d9051c8`)
- Clock is **not** the lever: 2 MHz vs 8 MHz was ~75 vs 84 µs, the ~6 µs of bit-time lost
  in ~70 µs of fixed overhead.

### 6.3 An SP3-ND Final completes with RXFR|CIADONE, not RXFCG
**VERIFIED.** The POLL is an SP3 STS frame carrying FCS (completes `st=0x6700`, has
RXFCG=0x4000). The **Final is SP3-ND (STS only, no PHR/payload)**: it completes
`st=0xa7b0` with RXFR|CIADONE and **no RXFCG**. Any Final-detect that waits on RXFCG hangs
forever; watch CIADONE(0x400). A synchronous busy-wait for the Final *inside* the POLL
callback also fails: the SP3-ND completion lands after the handler returns, and the spin
wedged the receiver / tripped the watchdog. Capture the Final in its own async callback.

### 6.4 "Negative distance" was cross-round mixing, NOT antenna delay
**VERIFIED.** t2/t3/t6 (POLL-RX / Response-TX / Final-RX) are single globals. On ESP the
phone's SP0 Final_Data lands only after the *next* round has overwritten t2/t3, so
recomputing the DS-TWR intervals in `final_data_decode` mixed this round's t6 with the
next round's t3 → km-scale garbage, or a **plausible-looking but wrong negative distance
that still passed the STS gate**. The `-783 mm` / `-333 mm` readings looked like an
uncalibrated antenna delay and cost a calibration hunt. Fix: snapshot `reply1 = t3−t2`,
`round2 = t6−t3` at Final capture (same round), consume once (`g_final_round_valid`). With
correct pairing the distances are **positive and realistic with no antenna calibration**
(~0 mm at contact, ~21 cm for a phone at ~21 cm). ESP-guarded so the nRF path keeps its
original recompute-from-live-globals (nRF processes Final_Data before the overwrite).
(commit `d9051c8`) **Lesson: a constant-looking offset that passes the integrity gate can
be a data-pairing bug, not a physical calibration.**

### 6.5 THE Final_Data trap: blocking `printk` in the hot path starves the ISR task
**VERIFIED, the last blocker before the door opened.** `DIAGK` → `printk`, which is
**blocking** on ESP. `POLL result` + `FINAL result` printed **every round** (~185 chars);
at 115200 that cannot drain inside the ~6 ms burst of per-round callbacks, so the DW3000
ISR task (which runs `dwt_isr` + the callbacks) backs up. One cause, two symptoms:
- The **Final callback dispatches late** → the SP0 revert re-opens RX *after* the phone's
  Final_Data slot (~1 slot ≈ 2 ms behind the Final) → Final_Data never caught → **no
  distance computed**, even though POLL/Response/Final were clean.
- The sustained backlog **trips the task watchdog** → spontaneous reset mid-session.

Fix: throttle `POLL result` and `FINAL result` to the first `CCC_RX_PREPOLL_LOG` (16)
rounds so the steady state is print-free (PREPOLL and RESP-txdone were already gated).
Moving a print *within* a callback cannot help: the callback itself was firing late.
**A per-round blocking log in a 2-ms-deadline path is a real-time bug, not just noise.**
(commit `677cfd1`)

### 6.6 Time-critical FIRST: revert to SP0 before the (throttled) print
The Final handler reverts SP3→SP0 (`revert_to_sp0_listen`) to catch the Final_Data; do it
**before** the diagnostic print, mirroring the POLL handler which arms the Response before
its stsq read + printk. (Redundant once 6.5 throttles the print, but the correct
ordering.) (commit `677cfd1`)

### 6.7 STS key cache: the dURSK is per ranging CYCLE
**VERIFIED.** The STS key (dURSK) is constant across a ranging cycle (POLL, Response,
Final, and every block in the cycle share it), but each arm re-wrote all four STS_KEY
registers (~258 µs, ~40 % of the arm). Cache the loaded dURSK and skip
`dwt_configurestskey` when unchanged; the registers persist across the per-frame
IV/loadiv/mode reprogramming. Helps the Response/Final arms (same-cycle key = cache hit);
the POLL arm's key changes per block so it misses. ESP-guarded. (commit `d9051c8`)

### 6.8 Auto-relock: drive relock from proximity, not a fixed timer
`create_auto_relock_time(door_lock_cluster, 5)` made the bolt relock 5 s after an unlock,
so a successful approach-unlock re-locked while the phone was still there. A fixed timer
cannot both "relock after N s" and "stay unlocked while the peer is present." Fix: set
**`AutoRelockTime = 0`**, which makes CHIP's `DoorLockServer` skip scheduling
(`VerifyOrReturnError(0 != autoRelockTime, true)`), and drive relock from proximity in
`ultrawidelock_reader_task`: unlock at `<= ULTRAWIDELOCK_UNLOCK_RANGE_CM` (100 cm), hold
while present, relock past `ULTRAWIDELOCK_RELOCK_RANGE_CM` (150 cm, hysteresis stops
boundary flapping) or when the ranging session drops. Re-unlock within one session works
too (the old code debounced until disconnect). Not a ranging fault; a lock-policy
choice.

### 6.9 ESP vs nRF: the logic is shared and proven; ESP is the real-time port
The credential/CCC/DS-TWR stack in `modules/ultrawidelock_uwb` is compiled by **both** the nRF5340
Qorvo reference app and this ESP port. Two consequences: (a) lean on the nRF reference to
confirm intent before re-deriving on hardware; (b) any ESP-only tweak to the shared file
must be `#if defined(ESP_PLATFORM)`-guarded so the nRF build keeps the original path. The
snapshot (6.4) and STS-key cache (6.7) are both guarded, verified by the host suite
(`tests/host/run.sh`, 558/558) compiling the file *without* `ESP_PLATFORM`. (commit
`d9051c8`)

### 6.10 On the single-core C6, priority 23 no longer isolates the DW3000 worker
**PREDICTED: derived from ESP-IDF v5.5.4 source and docs. No C6 has been on a bench with
a DWM3000 yet, so this is a hazard to measure before it bites, not a failure that was
seen.** (The rest of section 6 is VERIFIED on silicon; this entry is the exception.)

On the S3 the DW3000 IRQ worker owns a core: `dw3000_hw.c` pins it to core 1 via
`ULTRAWIDELOCK_DW3000_TASK_CORE` at priority 23 while IDF's radio tasks sit on core 0, so
its priority never has to win an argument. The C6 has one core, so
`ULTRAWIDELOCK_DW3000_TASK_CORE` is 0, priority is the only isolation, and 23 does not
clear the traffic it must beat:

| task | prio | source |
| --- | --- | --- |
| DW3000 IRQ worker | 23 | `xTaskCreatePinnedToCore(…, 23, …)` in `dw3000_hw.c` |
| Wi-Fi | 23 | IDF `docs/en/api-guides/performance/speed.rst`, single-core list |
| BT/BLE controller | 23 | `ESP_TASK_BT_CONTROLLER_PRIO` = `ESP_TASK_PRIO_MAX - 2`, and `configMAX_PRIORITIES` is 25 |
| esp_timer | 22 | `ESP_TASK_TIMER_PRIO` |
| NimBLE host | 21 | speed.rst, as above |
| esp_event | 20 | `ESP_TASKD_EVENT_PRIO` |
| lwIP TCP/IP | 18 | `ESP_TASK_TCPIP_PRIO` |

The C6 controller takes that constant:
`components/bt/include/esp32c6/include/esp_bt.h` sets
`.controller_task_prio = ESP_TASK_BT_CONTROLLER_PRIO` in its default config, so the UWB
worker ties with the BLE controller, and with Wi-Fi if it is up.

Two FreeRTOS details turn the tie into latency rather than resolving it:
- **An equal-priority wake does not preempt.** The GPIO ISR's `xSemaphoreGiveFromISR`
  reaches `xTaskRemoveFromEventList`, whose single-core build (`configNUMBER_OF_CORES == 1`)
  yields only when `pxUnblockedTCB->uxPriority > pxCurrentTCBs[0]->uxPriority`, strictly
  greater. At equal priority the worker goes to the *back* of the priority-23 ready list
  and the controller keeps the CPU.
- **It then waits for a time slice.** `configUSE_TIME_SLICING` is 1, and both apps set
  `CONFIG_FREERTOS_HZ=1000`, so the ready list rotates on the next 1 ms tick (10 ms at
  IDF's default 100 Hz, which is why that setting is not optional here).

Against 6.1's 1836 µs arm deadline, one tick is over half the budget before any SPI runs.
Expected symptom: intermittent `ARM FAIL … LATE` and lost POLLs on C6 from firmware clean
on S3, correlating with BLE activity, continuous during a credential session because the
L2CAP channel carries M1–M4.

**Do not raise the worker to 24 without measuring first.** That puts UWB above the radio
controller, which Espressif advises against ("it is not recommended to set task priorities
higher than the built-in … operations as starving them of CPU may make the system
unstable"), and BLE is load-bearing here: a starved controller drops the session the
ranging belongs to. The latch is already in the hot path: `dw3000_hw.c` writes
`g_dw_cyc_gpio` at GPIO-ISR entry and `g_dw_cyc_work` at worker entry, so their difference
*is* this latency. Only the code behind `#if DIAG_HOT` in
`deps/dw3000/dwt_uwb_driver/dw3000/dw3000_device.c` reads them, and that is
`#define DIAG_HOT 0`. Flip it, take the distribution on C6 and on S3, and let the two
histograms decide. `g_dw_cyc_per_us` initialises from `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`,
160 on C6 and 240 on the Matter S3 build, so the cycle counts convert on both.

---

## 7. Reader state-machine & diagnostics

### 7.1 The GeneralError short-circuit must not fire during ranging
**VERIFIED.** The reader had a blanket rule: a proto-2 / id-0 Notification-Event mid-auth
is a device GeneralError, so read `payload[2]` as the code. During `PH_ESTABLISHED` those
events are **BleSK-encrypted**, so `payload[2]` is a ciphertext byte: the reader reported
`GeneralError 0x28` one run and `0xf6` the next and killed the session before decrypting
the real event. Guard the short-circuit to pre-ranging phases; in `PH_ESTABLISHED` always
BleSK-open + dump + feed the engine. That surfaced the real `general error 3`.
(`ultrawidelock_reader.c` ~line 583)

### 7.2 Read the *decrypted* error, and log it
General-error codes: `0 UNKNOWN, 1 RESOURCE_UNAVAILABLE, 2 WRONG_PARAMS, 3
URSK_UNAVAILABLE`. `WRONG_PARAMS (2)` points at M1 attribute values; `URSK_UNAVAILABLE
(3)` at the session-id / URSK-install path (§5). The codes are meaningful only once the
SDU is opened.

---

## 8. Provisioning & identity

### 8.1 A per-boot random reader key changes the reader's identity every reboot
**VERIFIED.** The reader originally generated a random dev P-256 key at boot, so its
reader-id (and therefore the salt's reader_group_key) changed each power cycle and the
phone's stored credential no longer matched. Fixed by a **fixed, non-secret dev identity**
loaded from NVS (`uwl_prov` namespace, key `blob`): reader-id = signing pub.X.
(commit `ca937e2`)

### 8.2 DEV identity is dev-open + loud, not a bypass
With a DEV identity and no trust anchors, the reader accepts the presented credential
**and logs a warning**: an interim seam for real issuer-chain validation, not a silent
allow. Bench commands: `ultrawidelock-prov` (show id / trust / last cred), `ultrawidelock-trust`
(persist the last-presented credential key). Phase-4 Matter `SetAliroReaderConfig` writes
the same blob.

### 8.3 Zephyr-reserved NVS id wiped external NVS (cross-port cousin)
On the nRF side, a rollback unique-id mirror at Zephyr-reserved NVS id `0xFFFF` wiped
external NVS every boot (fixed `0xFFFF → 0xFFFE`). If ESP32 external-NVS credentials
vanish across reboot, suspect an id collision with a reserved slot.

### 8.4 An NVS namespace over 15 characters is "never provisioned" on read and `0x1109` on write
**VERIFIED (2026-08-15).** NVS caps namespace *and* key names at
`NVS_NS_NAME_MAX_SIZE - 1` = 15 characters, and does not fail symmetrically. A read-only
`nvs_open` of a longer name can never match an existing namespace, so it misses as
`ESP_ERR_NVS_NOT_FOUND` and the loader reports "no provisioning yet"; only the read-write
open that would *create* the namespace reports `ESP_ERR_NVS_KEY_TOO_LONG` (`0x1109`). The
project rename took the provisioning namespace to 18 characters, and a freshly reset lock
then paired "successfully" while UWB never worked:

```
W ultrawidelock_reader: using DEV reader identity ...; 0 trust anchor(s)      <- read side: silent
W ultrawidelock_prov: nvs_open(rw) err=0x1109                                 <- SetAliroReaderConfig's store
W ultrawidelock_reader: advertisement NOT refreshed: GRK is still all-zero
E chip[ZCL]: [SetAliroReaderConfig] Aliro reader verification key was not read or is not null.
W ultrawidelock_prov: nvs_open(rw) err=0x1109                                 <- SetCredential's trust store
```

`ultrawidelock_reader_provision_identity` refuses to commit an identity it could not
persist, so `s_id` stayed the dev default (all-zero GRK, unresolvable advertisement) even
though the Matter delegate had accepted the command; the retry then hit `InvalidInState`
because the delegate's RAM said configured. Fixed by naming the namespace `uwl_prov` and
`_Static_assert`ing both names against the IDF caps in `ultrawidelock_prov_nvs.c`; the
host fake (`tests/ports/esp32/sdkfake/fake_nvs.c`) enforces the same cap the same
asymmetric way, so the suite fails where the bench used to. A device provisioned under the
old namespace re-provisions on its next Apple Home add. Nothing migrates.

---

## 9. Debugging an opaque peripheral: the probe that ended three wrong theories

**VERIFIED (2026-07-25).** The channel-impulse CIR tap dump returned a fixed non-physical
blob for most receptions: byte-identical at every accumulator offset, saturated at the
`int16` limits. Six bench runs and three failed hypotheses before the cause: **the DW3000
accumulator cannot be read while the receiver is up.** The capture chained to the blob's RX
handler, and on the Final that handler re-arms an SP0 listen, so the receiver overwrote the
accumulator underneath the read.

The wrong turns are the reusable part:

| Theory | Killed by |
|---|---|
| The 97-byte read splits across the ESP32's 64 B non-DMA SPI limit | Splitting into 8-tap sub-reads made it **worse** (0/16 windows real, against 3/16 and 4/16 before). Reverted |
| `dwt_readcir`'s ACC clock forcing was not sticking | `CLK_CTRL` read back identical (`11796992`) in both the working and the broken case |
| The RSSI power gate was closing and dropping the link | No gate-close line anywhere in the log; the gate only ever held AP-Completed |

The probe that cracked it is `lab cir probe` (`uwb_cirdiag_probe`): read the same window
at three different accumulator offsets, repeat the first offset, then repeat it once more
in `DWT_CIR_READ_FULL` so the raw 24-bit words are visible before the reduced modes
saturate them. It reads as:

- passes at different offsets identical → the sample offset is ignored
- first and repeated pass differ at the same offset → the read is racing something
- `clk` without the ACC enable bits → the clock forcing did not stick

Idle reads are perfect (offsets differ, the repeat is byte-identical, values sit at a
physical noise floor); only live-session reads are corrupt. That pointed at the receiver.
**An A/B that varies one address and repeats one measurement beats any amount of reasoning
about the bus.**

Two lessons that generalise past this chip:

- **A fix that makes the symptom worse is a result, not a setback.** The sub-read split
  going from 4/16 to 0/16 was what ruled out the entire SPI layer.
- **Instrument the boundary you cannot see.** Every hypothesis here was about the
  transport; none survived a probe that could distinguish "addressing wrong" from "data
  wrong" from "timing wrong."

---

## 10. Current status: approach-unlock works end to end

- **VERIFIED on a live iPhone (2026-07-20):** the full approach-unlock, with the Wallet
  unlock animation playing on approach and a relock on departure. The final gate was the
  reader→phone Reader-Status-Changed grant message (5.10).
- **VERIFIED on silicon (full path):** credential-auth (discovery / L2CAP CoC / AUTH0 /
  AUTH1 decrypt / device-signature / credential-trust / URSK) → M1–M4 ranging setup →
  **live DS-TWR**: continuous positive DIST every round, tracking the phone (d ~6–45 cm at
  arm's length, growing as it withdraws), then `trusted range NN cm (<= 100):
  unlocking` and auto-relock. No watchdog resets. Against a live iPhone.
- **No antenna calibration was needed**, see 6.4.
- **Open polish (non-blocking):** per-round DIST jitter (the AccessManager medians and
  filters it enough to unlock cleanly); auto-relock time (6.8); the phone's
  `general error 0` at end-of-session is post-unlock and benign.

### Commit map (newest first)
| Commit | What |
|---|---|
| `cf0c73d` | document the Reader-Status-Changed grant (message 23) gotcha |
| `ba612c8` | tell the phone the grant so the Wallet unlock animation plays (§5.10) |
| `7b892b6` | land the DS-TWR RESPONSE on the phone's negotiated slot |
| `6ba874b` | capture DS-TWR ranging bring-up gotchas (unlock working) |
| `677cfd1` | throttle hot-path ranging logs so DS-TWR closes and unlocks (§6.5, §6.6) |
| `d9051c8` | DMA-disable SPI, fence ESP-only ranging tweaks from nRF (§6.2, §6.4, §6.7, §6.9) |
| `1ef8946` | real-time DS-TWR responder: POLL→Response→Final→distance on silicon |
| `a517d71` | DW3000 radio bring-up (boot probe + wakeup-settle) + tooling (§2.2) |
| `84c2875` | Aliro ranging session-id + BleSK channel: M1-M4 green on silicon (§5.1, §5.5) |
| `f628beb` | ranging-setup diagnostic (hold M1, decrypt post-EXCHANGE SDUs) |
| `5f91235` | AUTH1 secure-channel decrypt: credential-auth green on silicon |
| `cef8255` | correct AUTH0 phase encoding: full ECDH auth accepted |
| `04bd8cc` | keep the Aliro transaction on the nimble_host stack |
| `f937f01` | wrap Aliro auth commands in ISO7816 APDUs (AUTH0 acceptance) |
| `69d24bb` | credential reader BLE discovery + L2CAP CoC for approach-unlock |
| `ed38895` | host test for the bolt-state LED policy |
| `5a4e6c4` | Aliro 0xFFF2 advertisement + dynamic tag (phone approach-connect) |
| `ad9a63b` | bolt-state LED on the onboard WS2812 (GPIO48) |
| `67234fa` | attach-mode credential reader: coexist on Matter's NimBLE host |
| `2aea20f` | make front door for the esp32-matter port (isolated) |
| `996a8d5` | interactive console + keep-BLE-up (fixes reader-reinit crash loop) |
| `af7a3a3` | print commissioning onboarding codes at boot |
| `d7131d3` | Phase 4.3 merge reader+UWB into Matter app (provision→handoff→range) |
| `14446c8` | Phase 4.2 credential reader delegate (BLE+UWB Home Key provisioning) |
| `9eba9c9` | Phase 4.1 esp-matter door-lock baseline (commissions to Apple Home) |
| `19b6239` | Phase 3.5 M1–M4 ranging-setup wiring (negotiated params) |
| `ca937e2` | Phase 3.4 reader provisioning seam (identity + credential trust) |

Earlier Phase-3.1/3.2/3.3 crypto and auth work (key schedule, wire codec, URSK handoff)
landed before this range; its gotchas are in §4. Everything above is merged to `main`.
