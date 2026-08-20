# Add the key

Flashing a board is half the job. The image carries no credential: Apple Home mints the
Aliro key during Matter commissioning, and until that happens the lock is a radio with
nothing to say. This page is the commissioning step for every target, from the setup code
to the first walk-up.

If you flashed an ESP32 from the browser, the flasher page walks the same ground for that
one board; come here for the other targets, for what a healthy pairing looks like, and for
what to do when one does not finish.

## Before you start

- An iPhone with a UWB chip. iOS 26 is the validated floor.
- A home hub: a HomePod or an Apple TV. Matter accessories are commissioned through it,
  and without one Home will not finish.
- The network the target actually joins:

| Target | Joins | Also needs |
|---|---|---|
| DWM3001CDK (`make build`) | Thread, as an MTD | A Thread border router the phone already reaches |
| nRF5340 DK | Thread | The same |
| ESP32-S3, C5, C6 | Wi-Fi | A 2.4 GHz network; there is no Improv step, the join happens inside commissioning |

The `make reader` build on the DWM3001CDK is the exception to this whole page. It has no
Matter node, so nothing commissions it and nothing mints a key for it; its credential is
imported over USB instead. See [`../apps/dwm3001cdk-lock/README.md`](../apps/dwm3001cdk-lock/README.md).

## 1. Get the setup code

Where the code comes from is not the same on all three targets, and on the DWM3001CDK it
is the one that surprises people.

| Target | Where the code comes from |
|---|---|
| DWM3001CDK | `make build` and `make monitor` each print it. There is no QR label and the board cannot print one |
| nRF5340 DK | `make nrf-pairing-code`, which `make nrf-term` runs before it attaches |
| ESP32-S3, C5, C6 | Printed on the console at every boot; `codes` at the `matter>` prompt reprints the QR URL and the manual pairing code. The browser flasher page shows the same QR |

On the DWM3001CDK the build prints the line Home is about to ask for:

```
  setup code  3650-503-5696   ·  discriminator 0x0F00, verifier checked
```

**Nothing on that board can print that number**, and that is the protocol working rather
than a missing feature. A Matter device stores the SPAKE2+ *verifier*, never the passcode,
and a verifier cannot be run backwards. So the code is derived on the host from the
`.config` the build just produced, and `verifier checked` means
`scripts/spake2p_verifier.py --from-config` re-derived the verifier and compared it to the
one compiled in. Whatever your own build printed is the authoritative number; a code from
someone else's tree fails at Pake3 looking exactly like a typo.

On the ESP32 the board is the authority instead. If the flasher page and the console ever
disagree, the console is right.

## 2. Add it in Apple Home

Home app, add accessory.

- **With a QR code** (ESP32): scan it with the iPhone you want to carry the key.
- **With digits only** (DWM3001CDK, and any time scanning is awkward): **More options…**
  then **Enter Code**, and type the 11 digits.

Keep the phone next to the board. Commissioning starts over BLE and only then joins Thread
or Wi-Fi, so distance at this stage costs the whole attempt. Expect a minute or two.

## 3. Accept the uncertified-accessory warning

Home warns that this is an uncertified accessory. Add it anyway.

The warning is correct and it is not going away: the images use CHIP's test vendor ID
(`0xFFF1`) and test discriminator (`0xF00`), because a real product needs an allocated
vendor ID and this project has none. The ESP32 images go further and carry Matter's test
setup parameters outright, which is why every board flashed from the browser page has the
same pairing code.

The DWM3001CDK build no longer uses CHIP's published test passcode: it carries this
project's own, generated at 20,000 PBKDF2 iterations rather than the 1,000 floor. That
removes the case where a stranger commissions the lock knowing nothing but the SDK. It is
still **one pairing for every board built from this tree**, so if the lock guards anything
you care about, generate your own passcode and rebuild before commissioning. This is
evaluation firmware either way.

## 4. Wait for the Home Key

There is no separate step for the key and no button to press. Once commissioning
completes, Home recognises the accessory as a Door Lock advertising Aliro support and
provisions the credential into Wallet on its own, over the Matter session it just
established.

Give it a moment after the tile appears, then check Wallet for a key card naming the lock.
Only after that is there anything for a walk-up to authenticate against.

## 5. Share it with Home Assistant

For a DWM3001CDK, first make Home Assistant use the Apple Thread network. In the
iOS Companion app, open **Settings > Devices & services > Thread > Configure**,
send the phone's credentials to Home Assistant, refresh, and make the Apple
network preferred. If Home Assistant runs an OpenThread Border Router, join it
to that network rather than creating a second dataset.

Then open **Settings > Matter > Add device**, answer **Yes, it's already in
use**, select Apple Home, and follow the sharing dialog. This commissions an
additional Matter fabric; it does not replace the Apple fabrics or Home Key.
The exact prerequisites, recovery paths, and five-fabric behavior are in the
[DWM3001CDK multi-admin guide](../apps/dwm3001cdk-lock/README.md#apple-home-plus-home-assistant).
Home Assistant maintains the current UI paths in its
[Matter](https://www.home-assistant.io/integrations/matter/) and
[Thread](https://www.home-assistant.io/integrations/thread/) documentation.

This step is optional for other targets, but the same rule applies to every
Matter-over-Thread port: all border routers must share one Thread dataset.

## 6. What a healthy pairing looks like

Four things, in this order:

1. **Home finishes** and the lock tile goes live: lock and unlock from the app both work.
   On the DWM3001CDK this is checklist row CDK-7 in
   [hardware-validation.md](hardware-validation.md), recorded as passing on hardware.
2. **Wallet holds a key** for the lock.
3. **The BLE advert changes.** Before commissioning the node advertises commissionable;
   after it, the advert gate offers Aliro `0xFFF2` instead. A board still advertising
   commissionable after a successful pairing has not finished one.
4. **Walk up.** From well outside ranging distance, phone pocketed, the Wallet unlock
   animation plays and the bolt opens with no phone interaction, then relocks as you walk
   away.

The bolt moving is **not** the pass criterion. The Wallet animation is, because only that
proves the reader told the phone it granted access rather than actuating locally. The
per-target checklists are in [hardware-validation.md](hardware-validation.md).

## Common failures

| Symptom | Likely cause and fix |
|---|---|
| Home cannot find the accessory | No home hub, Bluetooth off, or the board is already commissioned. If another controller still reaches it, open a commissioning window or share it instead of factory-resetting it |
| The setup code is rejected | A code from a different build. Re-read the one your own `make build`, `make nrf-pairing-code` or `codes` printed |
| Commissioning stalls near the end | The network join. Thread: put Apple and Home Assistant on the same dataset and keep a border router in reach. Wi-Fi: 2.4 GHz is required. Let the fail-safe expire before retrying |
| It sat on "Adding to Home" and then timed out | On old DWM3001CDK images this was often an SRP registration failure. The current image keeps removal objects alive until OpenThread acknowledges them and retries duplicate names; capture the RTT log before restarting a border router |
| A failed share appears to have consumed a fabric | Current DWM3001CDK images roll back only that provisional attempt on fail-safe expiry. If a controller had already completed the fabric, remove it through Home Assistant's **Manage fabrics** view from a surviving administrator |
| The fifth controller fits but a sixth does not | Expected: `SupportedFabrics` is five. Apple Home commonly uses two and Home Assistant one, leaving two spare |
| Same symptom, but only when re-pairing a lock that worked before | A second SRP cause. `otSrpClientAddService` refuses the re-registration with `OT_ERROR_ALREADY` because the name matches a service the client still holds, so no `_matter._tcp` instance is published and nothing resolves. Fixed in `60615646` (the client's service is cleared and re-added); on an older tree, that is the commit to look for |
| Pairing succeeded once, and now the board answers nothing | A commissioning that installs a fabric and then times out leaves that fabric stored, and the advert gate then offers `0xFFF2` instead of commissionable, so the controller can neither discover it nor open a window. On the DWM3001CDK, hold **SW2 through reset** to factory reset; `led0` blinks to confirm the hold registered |
| The tile works but no walk-up ever unlocks | Either the credential never landed or ranging is not running. Check Wallet first, then the radio: [troubleshooting.md](troubleshooting.md) |
| Unlocks worked, then stopped after a re-pairing | The trust store, not the pairing. An Apple home installs two endpoint keys per pairing and they accumulate; see [dwm3001cdk-surgery.md](dwm3001cdk-surgery.md) §6 |
| An upgrade from v0.3 lost Matter and Home Key | Expected once for the `mf2` clean break and settings-partition move. Remove the old controller records and pair the current image again |
| An ordinary reflash lost everything | Not expected. `make flash` preserves current settings; `make flash-erase`, SW2 factory reset, or the v0.3 schema transition intentionally clears them |

Deeper symptom tables, grouped by target: [troubleshooting.md](troubleshooting.md). The
DWM3001CDK's commissioning traps, each with the symptom that exposed it:
[dwm3001cdk-surgery.md](dwm3001cdk-surgery.md). The ESP32's:
[esp32-gotchas.md](esp32-gotchas.md).

## After the key is in

- Prove the unlock properly: [hardware-validation.md](hardware-validation.md).
- Build options and the runtime consoles: [configuring.md](configuring.md).
- What actually happens on air between the phone and the reader:
  [protocol-research.md](protocol-research.md).
