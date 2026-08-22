# Inside veto — build, provision, place

The lock must never passively unlock while the credentialed phone is inside.
This is the operator flow for the two-dongle system that enforces that.

Design and safety argument: `docs/inside-latch.md`. Read it before changing a
default; every threshold here has a reason recorded next to it.

Running it for the first time: `docs/bench-inside-outside.md` is the linear
recipe, with the pass/fail for each step and the point at which to stop.

## What runs where

| device | image | job |
|---|---|---|
| DWM3001CDK | `make build LATCH=1` | owns every decision; holds the veto |
| nRF52840 dongle x2 | `make witness-build` | reports what it hears; decides nothing |

One dongle inside, one outside, mirrored across the door plane, each on a USB
charger. No Raspberry Pi, no debug probe, no wiring between devices.

## Baseline (unchanged Matter image)

```
make dfu-key          # once per checkout
make build
make cdk-size CDK_SIZE_REPORTS=0
```

## The LATCH image

```
make build LATCH=1 CDK_BUILD=build/cdk-latch
make cdk-size CDK_SIZE_REPORTS=0 CDK_BUILD=build/cdk-latch
```

MEASURED 2026-08-19, against `make build` on the same tree:

| region | baseline | LATCH=1 | delta |
|--------|---------:|--------:|------:|
| FLASH | 417,684 | 424,544 | **+6,860 B** |
| RAM | 118,312 | 119,464 | **+1,152 B** |

The default image is unchanged: built at commit 588459f5 and at this branch's
HEAD, both 417,684 B, differing in 4 bytes — the `__TIME__` string inside
OpenThread's version banner.

`RELEASE=1 SMP=1 LATCH=1` measures 406,536 B flash (93.74%), 115,944 B RAM.

## Host tests

```
make check
```

Suites: `ultrawidelock_latch` (the veto and every evidence-loss case),
`ultrawidelock_witness_msg` (wire format, replay), `ultrawidelock_witness_core`
(which advertisers get reported), `ultrawidelock_witness_pick` (which one is
the phone), plus the existing `ultrawidelock_side` gate suite.

## Provisioning the dongles

Once each, over USB, before mounting:

```
make witness-prov-help      # prints the PROV line and what each field is
```

Three secrets, and the difference between them matters:

- **link key** — one per dongle, also stored on the lock. Seals that
  witness's reports. The lock's copy goes in on the reader image, which is
  the one with a console (`ultrawidelock witkey inside <hex32>`); reflash the
  Thread image with `make flash`, never `flash-erase`, or the keys go with it.
  See docs/inside-latch.md section 6.1.
- **group key** — the same on both dongles and NOT on the lock. Labels
  advertisers so the two dongles can be compared without the lock ever
  learning an address.
- **Thread dataset** — your existing network. Get it from the lock, which is
  already joined: `overlays/thread-dataset-dump.conf` + SW2. See
  docs/inside-latch.md section 6.

Generate keys with `openssl rand -hex 16`.

## What it does with no witnesses

Refuses every passive unlock. That is the fail-closed behaviour the design
asks for, not a regression. NFC Express Mode, Apple Home commands and
mechanical operation are never gated and keep working.

## Recovering

One deliberate unlock — NFC tap, app, key — re-seeds the latch record. That is
the recovery from a factory boot, corrupt storage, a new credential, or an exit
through a door nothing observed.

## Unsupported / not proven

- BLE observer + Thread SED coexistence on the nRF52840: builds, not yet
  measured on hardware. This is the first thing to test.
- Every picker and latch threshold: set from geometry, not from a capture.
- Multi-anchor stock-iPhone UWB ranging (unrelated to this path, still unproven).
- Fail-open passive unlock on UNKNOWN — explicitly rejected.
- Unlock authority on a witness — forbidden by construction.
