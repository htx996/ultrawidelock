# Second anchor: a true UWB satellite

Goal: the side-of-door question answered by two authenticated UWB distances
instead of BLE RSSI correlation. The witness pick, the priming walk, and the
RSSI geometry sensitivity all exist because the dongles cannot range; a second
anchor that can range removes the whole class.

This reopens option A, rejected in [inside-latch.md](inside-latch.md) ("needs a
DWM3001CDK-class board that is not in the bill of materials; multi-anchor
stock-iPhone ranging is explicitly unproven in this tree"). Both reasons are
now void: an nRF5340 DK with a DWM3000EVB shield is on the bench (a second
DWM3001CDK follows later), and the multi-anchor question was in fact half
proven on 2026-07-17 — see below.

## Hardware

| Board | Role now | Role later |
|---|---|---|
| DWM3001CDK (serial 760221694) | main lock, unchanged | main lock |
| nRF5340 DK + DWM3000EVB | satellite, development vehicle | freed for bench work |
| second DWM3001CDK (to be bought) | — | deployed satellite |

The anchor example was written for exactly this migration:
"moving the satellite to a second DWM3001CDK later is a board string rather
than a port" (`examples/zephyr/anchor/src/main.c`). Everything below is
developed on the DK and lands on the CDK by changing `ANCHOR_BOARD`.

## What already exists

| Piece | Where | State |
|---|---|---|
| Two-distance side-of-door + triangle gate | `ultrawidelock_fusion` | built, tested |
| Freshness gate, fail-safe rules | `ultrawidelock_satellite` | built, tested, wired into the lock's PREDICT arm |
| Anchor-to-anchor DS-TWR on these two boards | `examples/zephyr/anchor` (stage A) | built, never run on this bench |
| Full CRED-tier ranging engine on nRF5340+DWM3000EVB | `apps/nrf5340dk-lock` | built (it is a whole lock) |
| Phone accepts `N_Resp = 2` | `ULTRAWIDELOCK_NUM_RESPONDERS`, protocol-research.md §7 | measured 2026-07-17 |
| Sealed lock↔peer link over Thread UDP | `witness_link.c` pattern | built for witnesses, reusable |

What does not exist: `ultrawidelock_satellite_report()` has zero call sites.
Nothing measures `peer_mm` and nothing carries it to the lock. That gap is
this plan.

## How the satellite ranges the phone

The satellite joins the phone's own ranging session as the second responder.
No parallel session, no BLE contact with the phone, no new phone-side anything.

The grounds, all already in this tree (protocol-research.md §7):

1. **The phone builds the bigger round.** Advertising `N_Resp = 2` made the
   phone reserve slot 3 for `Response_1`, move Final to slot 4, and keep its
   SaltedHash key derivation in step — measured over 40 rounds with `cper=0`.
   Slot reservation is solved. What was never done: a second anchor actually
   transmitting in that slot.
2. **A responder can join from the air alone.** Pre-poll recovery: `mUPSK1`
   comes straight off the URSK and is static for the session, so a responder
   holding the session keys decrypts the first pre-poll cold, reads
   `Poll_STS_Index`, derives `URSK_KT → dURSK/dUDSK`, and is armed one slot
   later. No BLE time sync needed — which is exactly what lets a satellite
   that never spoke BLE participate.
3. **The keys are per-session.** The lock hands the satellite the session's
   URSK (or `mUPSK1`+`mURSK`) plus the M2/M3 round parameters over their
   sealed link at session start. This is not the IRK problem: the URSK dies
   with the session, identifies no credential, and opens no lock. A stolen
   satellite can range one live session it was already trusted with — it
   cannot track the phone across sessions and cannot mint an unlock.
4. **A lying satellite cannot open the door.** `ultrawidelock_fusion_may_predict`
   only ever WITHHOLDS: no report and INSIDE both degrade to today's
   single-anchor behaviour. The stage-D latch feed below is the one place a
   forged OUTSIDE could add capability, and it inherits the witness rules
   (sealed reports, and the latch's own dwell/session gates stay in front).

Alternatives considered: **anchor-to-anchor ranging only** never sees the
phone — it calibrates the baseline and nothing else (kept, as stage A).
**Passive TDoA listening** needs the same session keys to receive SP3 frames
plus a solved timebase, and yields worse geometry than an active slot the
phone is already holding open; it is the fallback if stage B's one real
unknown fails, not the first choice.

## Stages

Each stage carries its pass/fail check; a stage failing twice after fixes
stops downstream work.

### A. Run the anchor bench that already exists

`make anchor-pair` → flash DK (initiator) and CDK (responder), tape a known
distance, calibrate `ANT_DLY`, then record jitter.

- Proves the DWM3000EVB shield, wiring, and power jumper (see
  nrf5340-bringup.md — the power-select jumper fails silently).
- Produces the two numbers fusion needs sized from measurement, not guessed:
  `tol_mm` (3 sigma of per-link jitter) and `deadband_mm` (2 sigma).
- Measures the install baseline once the mounting spots are chosen.

Pass: `last_mm` stable within tolerance at the taped distance across a
report interval, raw vs median divergence understood.

Bench cost: flashing the anchor image replaces the CDK's lock image.
Restore with `make flash LATCH=1` (never flash-erase), then wait out the
phone's reconnect before judging anything.

### B. The satellite joins the phone's round — the experiment

The plan's riskiest unknown lives here: the phone reserves the slot, but a
real `Response_1` has never been transmitted, so whether the phone accepts it
and emits its timestamp record (`Final_Data` `nresp=2`) is unproven.

1. Satellite firmware: a new app at the CRED tier — the responder round
   engine (`ccc_shim_rx` and friends, already building for this exact board
   in `apps/nrf5340dk-lock`) minus BLE credential auth, plus key injection.
   It is a lock that is told the keys instead of negotiating them.
2. Key handoff: lock → satellite at session start over a sealed link
   (witness_link pattern over Thread; a UART wire is acceptable for first
   light). Payload: URSK-derived session keys + round parameters. The
   handoff must land before `UWB_Time0`; pre-poll recovery makes late
   arrival cost one block, not the session.
3. Lock built with `ULTRAWIDELOCK_NUM_RESPONDERS=2` (the knob keeps M3 and the
   RangingConfiguration blob in step by construction). Satellite transmits
   `Response_1` in slot 3 at STS index base+3.

Pass: phone's `Final_Data` reports `nresp=2`, and the satellite's computed
range tracks a tape measure. Fail twice: fall back to passive TDoA using the
same key handoff.

### C. Report path and the owed timebase

Satellite → lock: `(block index, peer_mm)` in a sealed report. The block
index IS the timebase alignment `ultrawidelock_satellite.h` says stage C owes —
both boards sit on the same session time grid, so the lock maps block → its
own clock with no extra protocol. Lock calls `ultrawidelock_satellite_report()`
(first call site) with the fusion baseline from stage A; the PREDICT gate
already wired in `main.c` goes live.

Pass: with the phone demonstrably outside, the lock logs
"predict withheld: second anchor puts the phone outside"; with the phone
inside or the satellite off, behaviour is bit-for-bit today's.

### D. Feed the latch, retire the priming walk

Fresh fusion verdicts become the latch's INSIDE/OUTSIDE windows directly:
a distance to the authenticated credential needs no pick, no priming
trajectory, no RSSI geometry. The BLE witnesses remain valid as a
dongle-only tier and as corroboration; the latch's dwell, session, and
clear-run rules stay exactly as proven on 2026-08-21.

Pass: walk-up grant with the witness pick disabled; no grant when walking
out with the phone left inside.

### E. Second DWM3001CDK

Board-string port of the satellite app, DK returns to being a bench tool.
Deferred until the board exists; nothing above depends on it.

## Risks

- Stage B's acceptance question is genuinely open; the fallback (TDoA) is
  real but strictly harder. Budget for stage B to be the long stage.
- `N_Resp` is baked into key derivation, so lock and satellite builds must
  agree and the setting applies from session establishment — a mid-session
  change desyncs every derived key (known, documented at the knob).
- Thread + DW3000 concurrency on the DK is assumed from `apps/nrf5340dk-lock`
  running Matter over Thread beside ranging; first light on stage B2 should
  still use the UART wire so radio coexistence cannot masquerade as a
  protocol failure.
