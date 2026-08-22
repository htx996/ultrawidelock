# DWM3001CDK lock

This application turns one DWM3001CDK into a credential reader and Matter lock. It
runs the portable credential, UWB, and Matter modules on the board's nRF52833 and
DW3110.

## Build

Prepare the NCS workspace and the checkout-specific MCUboot key once:

```sh
make bootstrap
make dfu-key
```

Build the default Matter over Thread lock from the repository root:

```sh
make build
```

Useful related images are:

| Command | Image |
|---|---|
| `make reader` | credential reader without Matter |
| `make selftest` | One-shot UWB initialization self-test |
| `make cirdiag` | Matter lock with unattended CIR capture |
| `make mlgate` | Lock with LOS/NLOS classification in the unlock path |

Use `make flash` to program the built image and `make monitor` to open the RTT
console. `make flash-erase` also erases commissioning and reader state.

## Apple Home plus Home Assistant

The lock supports five Matter fabrics. Apple Home normally consumes two and
Home Assistant consumes one, leaving two spare. All administrators must use one
Thread dataset. Do not commission the board onto a new Home Assistant Thread
network after Apple Home has already put it on the Apple network.

### Before pairing

- Run the supported Home Assistant OS Matter app and current iOS Companion app.
- Keep an Apple home hub that provides Thread online and on the same LAN as Home
  Assistant.
- In Home Assistant, open **Settings > Devices & services > Thread > Configure**.
  On the iPhone, send the Apple Thread credentials to Home Assistant, refresh,
  and make that network preferred. If Home Assistant runs its own OpenThread
  Border Router, join it to those credentials instead of creating another
  Thread network. Home Assistant's current UI and limitations are documented in
  its [Thread integration guide](https://www.home-assistant.io/integrations/thread/).

### Commission and share

1. Flash the current image. An upgrade from v0.3 uses a new Matter settings
   schema and flash layout, so remove the old accessory from controllers and
   pair it again. The credential reader identity is deliberately not migrated by
   this clean break. Moving to a build carrying the fabric label field costs one
   further re-pair: the fabric record grew, so records written by earlier images
   fail the loader's length check.
2. Add the lock to Apple Home using the code printed by `make build`, then wait
   for the Home Key to appear in Wallet. The full Apple path is in
   [`../../docs/add-the-key.md`](../../docs/add-the-key.md).
3. In the Home Assistant iOS app, open **Settings > Matter > Add device**,
   answer **Yes, it's already in use**, select Apple Home, and complete the
   sharing dialog. Do not reset the lock or scan its original setup code as a
   new accessory. Home Assistant documents this under
   [Adding a Matter device commissioned to another controller](https://www.home-assistant.io/integrations/matter/#adding-a-matter-device-commissioned-to-another-controller).
4. Operate the lock from both apps, power-cycle it, and repeat both operations.
   Confirm the Wallet key still unlocks on approach.

No `HA=1` build option is required. That option belongs only to the nRF5340
generated data-model variant; DWM3001CDK multi-admin is standard Matter.

### Recovery without a factory reset

| Symptom | Recovery |
|---|---|
| Apple works but Home Assistant cannot discover the shared lock | Confirm the iPhone and Home Assistant prefer the Apple Thread dataset and the Home Assistant border router joined that dataset. Re-share from Apple Home after fixing Thread. |
| The border router logs an SRP duplicate | Leave the lock and border router running. This image retains SRP objects until OpenThread completes removal and retries a rejected registration with a fresh service name. Restarting the border router is a diagnostic fallback, not the normal repair. |
| A commissioning attempt times out | Let the Matter fail-safe expire, then retry. Only the provisional fabric and staged Thread data are rolled back; existing Apple and Home Assistant fabrics remain live. |
| A controller was removed locally but its fabric remains on the lock | From a controller that still reaches the lock, use **Settings > Matter > Devices > _lock_ > Share device > Manage fabrics** and remove the stale controller. `RemoveFabric` is authenticated, durable, and scoped to that fabric. |
| No surviving controller can reach the lock | Hold **SW2 through reset**. The LED blinks and all Matter/Home Key state is erased. This is the last resort because every controller and Wallet key must then be paired again. |

Removing the last fabric clears the Home Key trust state and returns the board to
commissionable advertising. A failed `AddNOC` does not consume a persistent
slot. The persistent `mf2` records commit one fabric at a time and tombstone a
removal before success is returned, so an interrupted update loads either the
old or new valid record rather than a half-written table.

### What is verified

- Portable host tests cover five fabrics, Apple-like two-fabric state plus a
  failed or successful Home Assistant attempt, selective fail-safe rollback,
  authenticated removal, per-fabric ACL/session/subscription cleanup, Thread
  dataset staging, SRP duplicate recovery, response replay, and power-cut
  persistence cases. Also `UpdateFabricLabel` and its label conflict, an
  idempotent CommissioningComplete retransmission, the acknowledgement a
  subscription report's StatusResponse needs, and a real controller's ACL entry
  pinned by its captured bytes including a CASE Authenticated Tag subject.
- The Zephyr production image builds and links for
  `decawave_dwm3001cdk/nrf52833` with NCS v3.3.0 and fits the board.
- Live Apple Home plus Home Assistant commissioning and fault injection are
  explicit rows in [`../../docs/hardware-validation.md`](../../docs/hardware-validation.md).
  They are not recorded as passed yet.

### Verified size

Both sides of this comparison were pristine builds of revision `3dee6531`,
using NCS v3.3.0, Zephyr 4.3.99, LTO, `RELEASE=1`, and `SMP=1`. The only
difference was this worktree's multi-admin changes.

| Region | Clean `main` | This worktree | Increase | Free now |
|---|---:|---:|---:|---:|
| RAM | 115,748 B | 120,740 B | **4,992 B** | 10,332 B |
| Application flash | 388,800 B | 397,360 B | 8,560 B | 36,304 B |

The RAM change is 2,649 B of BSS plus 2,352 B of initialized data, offset by
9 B less linker padding. The largest named costs are 2,280 B for the larger
five-fabric/per-fabric state, 1,400 B for exact-response replay across seven
exchanges, 496 B for the bounded persistence serializer, 272 B for its request
snapshot, and 264 B for the larger SRP registry.

Reproduce the current half after `make bootstrap` with:

```sh
make build RELEASE=1 SMP=1 PRISTINE=1
make cdk-size CDK_SIZE_REPORTS=0
```

## Contents

- `src/` contains the product entry point and product policy.
- `boards/` and `overlays/` describe board wiring and optional build profiles.
- `sysbuild/` configures MCUboot.
- `keys/` holds the ignored local signing key generated by `make dfu-key`.
  Regenerating it is free only until a bootloader is flashed. After that, MCUboot
  on that board trusts one key, and losing it means the board takes no further
  image over DFU -- it needs the bootloader replaced over SWD. Because the key is
  gitignored it exists in exactly one working directory and no clone, worktree or
  push carries it, so pruning a worktree or moving a checkout can destroy it.
  Back it up somewhere that is not this tree.
