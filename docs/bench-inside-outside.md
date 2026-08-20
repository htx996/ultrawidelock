# Bench runbook: does the inside veto work?

One session, one afternoon. Two lock flashes, two dongle flashes, no Raspberry
Pi, no probe on the dongles, nothing wired to the door.

The question this answers, and the only one: **with the phone inside, does the
lock stay shut, and with the phone outside, does it open fast enough to be
worth having?** Everything else is tuning.

Design and rationale live in `docs/inside-latch.md`. This file is the recipe.

---

## What you need

| | |
|---|---|
| DWM3001CDK | already provisioned with a credential and already on your Thread network. If a walk-up unlocks it today, it is ready |
| 2 × nRF52840 dongle (PCA10059) | no probe needed; they load over their own USB bootloader |
| iPhone | the one enrolled on the lock |
| J-Link | for the lock only, to flash it and to read its log |
| A door | with the lock at it. Bench table first, door second |

Your Thread network needs a router. The lock is a MED and both dongles are
SEDs -- all children, none of them can form a network. A HomePod or Apple TV
is doing this job already if the lock is commissioned.

---

## 1. Enroll the witness keys on the lock

Generate two link keys and keep them where you can paste them:

```
openssl rand -hex 16     # -> the INSIDE dongle's link key
openssl rand -hex 16     # -> the OUTSIDE dongle's link key
openssl rand -hex 16     # -> the group key, SAME on both dongles, NOT on the lock
```

The lock image is a Thread build with `CONFIG_SHELL=n`, so it cannot be told
these itself. Use the reader image, which has a console:

```
make reader
make flash CDK_BUILD=build/cdk-reader
```

Hold **SW2 through reset**. The board comes up with its radios down and a USB
CDC console. Then:

```
ultrawidelock witkey inside  <inside link key>
ultrawidelock witkey outside <outside link key>
```

Both should answer `stored the ... witness link key`. The keys are in the
settings partition now, and the next flash keeps them.

## 2. Flash the lock image and take the Thread dataset off it

One build carries both the latch and the dataset dump:

```
make build LATCH=1 CDK_BUILD=build/cdk-latch \
  CDK_CONF="overlay-thread.conf;overlay-lto.conf;overlay-latch.conf;overlays/thread-dataset-dump.conf"
make flash CDK_BUILD=build/cdk-latch          # NOT flash-erase
```

`flash-erase` would take the keys you just stored, along with the credential
and the Matter fabric. Never use it here.

Nothing to press. The bench build retries the dump from its main loop until a
dataset exists, so it prints itself a second or two after the node attaches.
No controller involved, which matters: the commissioning window was the only
trigger until 2026-08-20, and it needs a controller that is talking to you --
exactly what you do not have when the dataset is what you are missing.

**Do not hold SW2 through reset on this image.** There is no provisioning
console here, so that gesture is a factory reset: credential and Matter fabric
gone. A short press opens the DFU window and is harmless.

The log prints the dataset between two markers:

```
---- BEGIN THREAD DATASET (hex, NNN B) -- CONTAINS THE NETWORK KEY ----
...
---- END THREAD DATASET ----
```

Join those lines into one string. Watch the log with `make monitor`.

That overlay prints your Thread network key on every window open. It is a
bench build. Do not leave it on the door.

## 3. Flash and provision the dongles

```
make witness-build
```

Put a dongle in bootloader mode -- press its RESET button until the LED pulses
red -- then:

```
make witness-flash WITNESS_PORT_DEV=$(ls /dev/tty.usbmodem*)
```

It re-enumerates as a serial console. Open it (`screen /dev/tty.usbmodem* 115200`)
and provision:

```
PROV inside <inside link key> <group key> <dataset hex>
SHOW
```

Repeat for the second dongle with `outside` and its own link key. Same group
key, same dataset, both times.

`make witness-prov-help` prints this whole flow if you lose it.

## 4. Link check, on a table, before anything goes near the door

Both dongles powered, 3 m apart, phone in the middle, lock nearby. `make monitor`.

**Want to see:**

```
witness link on UDP 49180 (2 enrolled)
```

**Must not see:**

```
witness datagram no enrolled key opened (N B); check the link keys match
```

That warning means a key differs between a dongle and the lock. Retype it.

LED on each dongle: **solid** = attached and reporting. Slow blink = provisioned
but not attached (dataset wrong, or no router in reach). Fast blink = not
provisioned.

> **This step is the plan invalidator.** If reports do not arrive, stop. It
> means BLE scanning and Thread cannot share the radio on this part, and no
> result from step 6 means anything. Say so and come back to the design.

## 5. Mount

Inside dongle on the inside wall, outside dongle on the outside wall, both near
the door, both mains powered. Roughly symmetric about the door; the design reads
the *difference* between them, so a lopsided mounting biases every window.

## 6. The two tests

Watch `make monitor` throughout. Twenty runs each, alternating, and write down
what happened.

**Test A -- inside. This is the one that matters.**
Phone in your pocket, start 5 m inside, walk to the door, stand at it 30 s,
walk away.

> **Pass: zero unlocks. One unlock is a failure of the whole spike.**

Expect `passive unlock withheld: inside latch (why=0x..)` in the log.

**Test B -- outside.**
Start 8 m outside, walk to the door at normal pace.

> **Pass is a number, not a verdict:** record the grant rate and roughly where
> you were standing when it fired.

A low rate here is a tuning result. Do not fix it by loosening anything until
Test A has 20 clean runs.

## Reading `why=`

The bitmask says which condition refused. Several bits can be set at once.

| bit | name | means |
|---|---|---|
| `0x01` | NO_SESSION | no credential session, or a different credential |
| `0x02` | NO_RECORD | this credential has never unlocked the door |
| `0x04` | NO_OPPORTUNITY | no door opening since it was last confirmed inside |
| `0x08` | DWELL | inside the 60 s after the last grant |
| `0x10` | WINDOWS | fewer than 3 agreeing OUTSIDE windows |
| `0x20` | STALE | the agreeing windows aged out (10 s) |

`why=0x10` during Test A is the healthy case: the lock has a session, believes
the phone could be either side, and the witnesses are simply not saying
OUTSIDE. That is the discrimination working.

`why=0x04` on every approach from both sides means no grant has been recorded
since boot -- tap NFC once and it goes away.

`why=0x08` for a minute after every unlock is the entry dwell, working.

## How fast can Test B be?

Floor is about **6 s of agreeing evidence**: three windows at
`CONFIG_WITNESS_WINDOW_MS=2000`. Only the first must be at 3 m or more; the run
then goes stale if 10 s pass without another OUTSIDE window. At walking pace
from 8 m you have around 6.7 s, so a grant should land about as you arrive --
if the outside dongle hears the phone from 8 m.

If you are standing at the door waiting, the knobs, in the order to try them:

1. Move the outside dongle to where it hears the phone sooner.
2. `CONFIG_ULTRAWIDELOCK_LATCH_CLEAR_WINDOWS=2` (`overlay-latch.conf`).
3. `CONFIG_WITNESS_WINDOW_MS=1500` -- but check packets per window first; below
   about 3 the means are noise.

Do not touch `CLEAR_MIN_MM`. It is what keeps a run from starting at the door,
where the differential's sign is worth least.

## Stop conditions

- No reports at step 4 after retyping the keys once. Design review, not tuning.
- Any unlock during Test A. Stop, record what the log said, do not tune around it.
- Two failures of the same step after fixes. Stop and report both attempts.

## What this does not test

- Anything about power. Both dongles are mains powered here.
- The accelerometer door-swing source (P7). Not built.
- Long-term drift. That is P8, a weekend, and a second credential.
