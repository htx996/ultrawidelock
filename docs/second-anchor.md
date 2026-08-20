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
| Anchor-to-anchor DS-TWR on these two boards | `examples/zephyr/anchor` (stage A) | run and calibrated 2026-08-21 |
| CRED ranging engine as a standalone app, no Matter workspace | `examples/zephyr/satellite` (stage B) | built 129 KB, joins a live session from air |
| Full CRED-tier ranging engine on nRF5340+DWM3000EVB | `apps/nrf5340dk-lock` | built (it is a whole lock) |
| Phone builds the `N_Resp = 2` layout | `ULTRAWIDELOCK_NUM_RESPONDERS`, protocol-research.md §7 | measured 2026-07-17; slot layout only, see stage B |
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
phone is already holding open. It was written here as stage B's fallback;
after the 2026-08-21 run it is the *second* fallback, behind block-parity
alternation (stage B', below), which needs no new timebase and no phone
behaviour that has not been watched on this bench.

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

PASSED 2026-08-21, first run, stock `ANT_DLY_DTU=4092`: at a taped 1.000 m,
400 samples read mean 959.5 mm / median 957 / sigma 20.5, ~95% completion at
10 Hz, first-path index steady at 735-744. Sigma matches the calibration
reference run, so: `tol_mm` = 62 (3 sigma), `deadband_mm` = 41 (2 sigma),
bench values pending the install geometry. The -40 mm bias is the tape's
reference point, not the radio's, and is deliberately not fitted -- fusion
consumes the difference of two links and the constant cancels.

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

RUN 2026-08-21, ~04:15-04:30. Every question the stage asked about OUR
hardware answered yes; the one question it asked about the PHONE answered no.

Proven, each on a console this night:

- **The satellite joins from air alone.** Given only the lock's session keys,
  it decrypted the Pre-POLLs cold, tracked block indices, decoded POLL and
  Final with `cper=0` at `stsq` ~60, and decoded `Final_Data`. The whole
  URSK -> mURSK -> dURSK/dUDSK ladder runs on a second board that never
  touched BLE. Receipt: `FINALDATA-2RESP blk=…` plus a `cper=0` FINAL on the
  satellite's own console. Ground 2 above stops being an inference.
- **The key handoff works end to end and can be fully automatic.** The lock
  prints the session line at credential start; a host-side watcher injected
  it into the satellite's shell at ~0 s lag and the satellite joined
  mid-session. Late joins cost one block, exactly as pre-poll recovery
  predicts. Receipt: auto-injected `sid=…` -> `SAT joined sid=…`.
- **Precision TX in an arbitrary slot works.** `Response_1` fired every round
  with ~3 ms arm margin, no `HPDWARN`, correct STS index. Receipt:
  `RESPTX r=0`, 16+ `RESP txdone`.

Answered no: **the phone does not report a second responder.** It builds the
grown round faithfully -- Final at POLL+3, `Final_Data` at POLL+4, key
derivation in step -- and still emits `nresp=1` in every `Final_Data`. The
discriminator was the slot-3 probe (`overlays/bench-2resp-idx1.conf`): the
LOCK's own radio, the one the phone has granted on for weeks, answered from
slot 3 and got `tx123` responses, zero records, and a session that FAILED at
the phase deadline. The transmitter is not the variable; the slot is.
Confirmed twice, and close-range RF was ruled out separately.

VERDICT: a stock iPhone honors `N_Resp = 2` as slot LAYOUT only -- its
receive-and-report path is single-responder. Stage B as specified cannot
pass, and the `nresp=2` pass criterion in the satellite README is unreachable
on stock phone firmware. Nothing the stage was built out of is wasted: every
component below it is proven and feeds stage B'.

### B'. Block-parity alternation — the fallback built only from proven parts

Where B's failure routes, ahead of TDoA. Run the ordinary 1-responder round,
the exact session the phone already grants on. Lock and satellite share the
session keys (proven, B above) and alternate who transmits `Response_0` by
ranging-block parity (both nodes track block indices -- also proven). The
phone sees the session it has always seen. Each node gets a true
authenticated DS-TWR distance to the credential, on its own half of the
blocks, at half the previous rate.

Every phone behaviour this depends on was watched on the bench all night.
The two unproven pieces are both in our own firmware:

1. **The lock's K-consecutive trust layer must tolerate its silent blocks.**
   Either K counts only the blocks the lock owns, or the window doubles.
   Cheap to check in the module tests before any board is flashed.
2. **The phone must not mind its range alternating between two nearby
   anchors.** Per block the measured distance steps by up to the install
   baseline. Nothing in the round rejects a range, but a phone-side filter
   could smooth or drop; only the bench answers this.

Pass: with both nodes alternating, the phone's session survives 40+ blocks
with `cper=0`, and each node's range tracks a tape measure on its own blocks.

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

- Stage B's acceptance question is CLOSED, negative (2026-08-21): a stock
  iPhone will not report a second responder. The live risk moved to stage B',
  where it is smaller and lives in our own code -- the K-consecutive trust
  layer over silent blocks, and whether the phone tolerates an alternating
  range. TDoA remains behind B' and is still strictly harder: it needs
  ns-grade cross-anchor time transfer for a worse measurement.
- `N_Resp` is baked into key derivation, so lock and satellite builds must
  agree and the setting applies from session establishment — a mid-session
  change desyncs every derived key (known, documented at the knob).
- Thread + DW3000 concurrency on the DK is assumed from `apps/nrf5340dk-lock`
  running Matter over Thread beside ranging; first light on stage B2 should
  still use the UART wire so radio coexistence cannot masquerade as a
  protocol failure.
