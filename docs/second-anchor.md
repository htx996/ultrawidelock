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

## STAGE B PASSES — re-run 2026-08-21 06:25

**The phone reports a second responder.** The 04:30 "answered no" below was our
own defect, and everything from it down is kept only as the record of how a
confident negative got built on a silent buffer drop.

Receipts, one session, satellite transmitting `Response_1` in slot 3:

| Signal | Result |
|---|---|
| `nresp=2` | 47 times, blocks 3 through 39 |
| Records | `resp[0] idx=0 ts=127795319`, `resp[1] idx=1 ts=255590944` |
| `SAT range`, satellite as responder 1 | 28 ranges, 57-63 cm cluster |
| `DIST` | tof 119-129, d = 558-605 mm |
| `SP0 OVERSIZE` | 0 — nothing dropped |

The whole difference was one constant: `g_pp_stash` was 64 bytes and a
two-record `Final_Data` is 65 on air, so every block where the phone reported
BOTH responders was discarded by the size gate before the decode ran. A frame we
delete is indistinguishable from a frame the initiator never sent, which is
exactly the inference stage B made. Being precise about credit: the record
selection fix (by tag rather than array position) did NOT unblock this -- with
two in-order records, position indexing would have worked. It earns its place
for the case where the initiator validates only responder 1.

**This retires B' and B''.** Both anchors now measure in the SAME ranging round,
which is what `ultrawidelock_fusion.h:63` requires -- zero pairing skew, against
alternation's 192 ms and two-round's `O^k x T_Round`. No MAC Mode work, no
second hopping sequence, no shared `Final_Data` hop state. Go to stage C.

Lesson worth keeping: a discard path with no counter and no log is a defect
generator, not a safety measure. The oversize path now counts and logs -- though
only where `DIAGK` survives, so `CONFIG_ULTRAWIDELOCK_PRETTY_SHELL` still blinds
the lock to its own drops. That wants a `LOG_WRN` before this ships.

## The 04:30 run, kept as the record

Answered no, but **only the observation is settled, not its cause.** The phone
builds the grown round faithfully -- Final at POLL+3, `Final_Data` at POLL+4,
key derivation in step -- and still emits `nresp=1` in every `Final_Data`.
Confirmed twice; close-range RF was ruled out separately.

The slot-3 probe (`overlays/bench-2resp-idx1.conf`) put the LOCK's own radio,
the one the phone has granted on for weeks, in slot 3: `tx123` responses, zero
records, a session that FAILED at the phase deadline. That controls for the
RADIO but not for the CODE. Both boards share one index-1 offset path and one
STS derive, so a systematic defect there fails identically on both, and the
probe cannot separate "the phone ignores slot 3" from "our slot-3 frame fails
the phone's validation". Calling it a phone limitation was a step too far.

What IS established: the phone committed to the `N_Resp = 2` round rather than
silently falling back. Its own Final decoded at POLL+3 with `cper=0` under
N=2 derivation and its `Final_Data` at POLL+4, so both its key schedule and its
slot layout used `Number_Responder_Nodes = 2`. CCC v4 obliges an initiator to
report every responder whose response it VALIDATED and gives it no field in
which to declare itself single-responder, so `nresp=1` is spec-legal only if
validation failed. The round arithmetic itself checks out against CCC v4
Table 20-2 (Response_l at POLL+1+l / STS+1+l, Final at POLL+N+1, `Final_Data`
at POLL+N+2), so the layout is not the defect.

MISSING CONTROL, cheap and not yet run: have the second board STS-verify the
first board's `Response_1` with the session keys it already holds. Fails => our
bug, and stage B is recoverable. Verifies => narrows to a phone policy choice,
though not conclusively, since both boards run the same `ccc_kdf.c` and a
shared derive error would agree with itself.

Either way stage B' is the better route and does not wait on this: nothing the
stage was built out of is wasted, and every component below it feeds B'.

> SUPERSEDED by the 06:25 re-run above. B' and B'' below were both designed
> around a limitation that does not exist. They are kept because the reasoning
> in them is sound and the constraints they document -- especially the fusion
> same-round rule -- still bind whatever is built next. The parity knobs remain
> in the tree, inert at their defaults.

### B'. Block-parity alternation — the fallback built only from proven parts

Where B's failure routes, ahead of TDoA. Run the ordinary 1-responder round,
the exact session the phone already grants on. Lock and satellite share the
session keys (proven, B above) and alternate who transmits `Response_0` by
ranging-block parity (both nodes track block indices -- also proven). The
phone sees the session it has always seen. Each node gets a true
authenticated DS-TWR distance to the credential, on its own half of the
blocks, at half the previous rate.

Every phone behaviour this depends on was watched on the bench all night.
CCC v4 covers the mechanism directly: a responder is explicitly permitted to
stay silent in its dedicated response slot, with no retry counter and no round
abort, and the choice of which responders take part in any given round is left
to the initiator side. Alternation is one responder per round, which is the
sanctioned model -- better spec footing than B, which put a second anchor into
a slot the enrolled responder device is the normative occupant of.

1. **RESOLVED: the trust layer tolerates the silent blocks.** `g_range_trust`
   advances and resets only inside the accepted-latch path of
   `fira_session_set_ccc_range_cm()` (`fira_session.c:189-211`), never on a
   block tick or timer, and the run's continuity test is a distance delta, not
   a block-index gap. A silent block produces no latch at all, so it cannot
   clear the run -- it only halves the rate. At K=3 and a 192 ms block, trust
   arrives about 1152 ms after session start instead of 576 ms, worst case
   (the anchor whose parity misses block 0). `overlays/bench-uwb-k2.conf`
   is on the shelf if that half second matters.

   The fail-closed chain behind it: no Response TX means `resp_tx_done` never
   runs, so `g_await_final` stays false, so the Final handler never latches a
   capture, so `Final_Data` finds no fresh round and computes no distance. A
   silent block yields no range rather than a stale-timestamp one. On the
   nRF5340 satellite this requires `CONFIG_ULTRAWIDELOCK_UWB_FINAL_SNAPSHOT=y`:
   its default live-recompute path hardcodes `have_round = true` and would pair
   a stale t3 with this round's t2/t6, latching garbage every second block.
2. **The phone must not mind its range alternating between two nearby
   anchors.** Per block the measured distance steps by up to the install
   baseline. Nothing in the round rejects a range, but a phone-side filter
   could smooth or drop; only the bench answers this.

Pass: with both nodes alternating, the phone's session survives 40+ blocks
with `cper=0`, and each node's range tracks a tape measure on its own blocks.

### B''. Two rounds per block — Aliro's own mechanism for this exact question

Found 2026-08-21 while reading the specs against the bench result. Aliro 1.0
§12.1.1 (`aliro-1.0.txt` l.9571-9582) defines a mechanism aimed precisely at
the question this plan exists to answer, and it is not the multi-responder
route:

> "Two ranging rounds per ranging block enable 'in front of' and 'behind the
> Reader' detection by the Reader. [...] A Reader MAY optionally support two
> ranging rounds per ranging block while **a User Device SHALL support two
> ranging rounds per ranging block.** [...] the responder-device SHALL be
> responsible for mapping responders (at the responder-device) to the
> appropriate ranging rounds."

Two rounds sit at a session-constant non-zero offset `O^k`, chosen by the
Reader at setup and signalled in the MAC Mode attribute (ID 15, l.7589-7600),
running two hopping sequences related by `f_i = h_i + O^k` (l.9585-9591). The
lock takes round 1, the satellite round 2. Details in protocol-research.md §11.

Why this outranks B' on all three axes B' was chosen for:

| | B' alternation | B'' two rounds |
|---|---|---|
| Phone obligation | none; it only ever reserved a slot | "User Device SHALL support" |
| Pairing skew | one whole block, 192 ms | `O^k x T_Round`, inside one block |
| Rate per anchor | halved | unchanged |

The specific hazard (l.9709-9736): if *either* Final_Data goes unheard, the
responder-device unconditionally assumes a hop and sets both `Hop_Flag` to 1.
Two physical units must therefore share Final_Data reception state, not merely
the URSK, or they compute different next-round indices and desynchronise. That
is a real coupling B' does not have, and it is the thing to design first.

Unproven here, and the reason this is a stage rather than a decision: nothing
in this tree implements MAC Mode or a second round today, and the phone's
mandatory support is a spec statement, not a bench measurement. It deserves the
same one-evening probe stage B got.

### Constraints the tree imposes on any alternating scheme

These came out of reading the round engine and the fusion layer on 2026-08-21.
They apply to B'; the first applies to B'' too, in weakened form.

1. **Alternation breaks the one contract `ultrawidelock_fusion_eval` states.**
   `ultrawidelock_fusion.h:63` -- "The two distances MUST be from the same
   round." B' guarantees they never are. Because the anchors are mounted across
   the door plane, the approach axis IS the baseline axis, so `adiff` tends to
   `baseline_mm` for an approaching phone and the only headroom before the
   triangle gate at `ultrawidelock_fusion.c:39` trips is `tol_mm`: 90 by default
   (`ultrawidelock_anchor/Kconfig:253`), 62 at the stage A calibrated value.

   One 192 ms block at 1.4 m/s is 269 mm of radial motion -- 3.0x the default
   headroom, 4.3x the calibrated one. Even a 0.5 m/s shuffle gives 96 mm, still
   over 90. So `geometry_ok` goes false on roughly half the blocks, the half
   whose parity ordering inflates `adiff`, and `ultrawidelock_fusion.c:66` makes
   `may_predict` return false on `!geometry_ok`. It fails CLOSED -- predictive
   unlock suppressed on approach, not an unsafe grant -- and the bias flips sign
   every block, so the verdict chatters rather than settling.

   `stale_ms` will not catch it: 1500 ms default
   (`ultrawidelock_anchor/Kconfig:275`) against a 192 ms skew, so the mispaired
   sample looks fresh and authoritative. Nothing tests it either --
   `tests/host/test_ultrawidelock_fusion.c` covers staleness, swapped anchors
   and negative rejection, but never a moving phone with skewed samples.

   Worth noting the skew is not wholly new: `main.c:901` already pairs
   `approach.last_cm * 10` against whatever report is stored, bounded only by
   `stale_ms`. B' converts that from incidental to structural and
   parity-correlated. B'' shrinks it to `O^k x T_Round` but does not remove it.

2. **`far_silence_ms` is sized for a full-rate feed.** 750 ms
   (`ultrawidelock_approach.c:318`, enforced at `:897-905`) against a 384 ms
   alternating cadence is 2x margin. One dropped block puts it at 576 ms, two at
   768 ms, and it relocks mid-approach. Size this before flashing anything.

3. **Skip at the TX call site, and leave `tr` non-zero.** The narrowest place is
   the single `tx_response_sp3()` call at `ccc_shim_rx.c:1791`; guard it and let
   `tr` keep its `-1` initialiser. Do NOT make `tx_response_sp3` return 0, and
   do NOT skip inside `resp_tx_done`. On a skipped block the deferred Pre-POLL
   decode's only home is `if (tr != 0 && g_pp_pending)` at `:1857-1860`; if it
   does not run, `g_warm_index` never advances and the
   `g_warm_index != g_armed_index` guard at `:1861` suppresses the NEXT block's
   POLL arm -- a one-block skip becomes two, and the stride learn at `:570`
   doubles.

4. **Key the parity off `g_armed_index`.** It is phone-derived and committed at
   `:1864` with no local accumulation, so it is identical on both boards for the
   same block. Not `g_session_block`: it is written in the deferred decode and
   still holds block N-1 at the decision point. Not `g_poll_stride`: it is
   learned, and goes to 192 across a missed Pre-POLL.

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

## STAGE C TRANSPORT PASSES — 2026-08-21 10:29

A sealed distance measured by the satellite reached the lock over the air and
paired with the lock's own measurement of the SAME ranging block: what
`pairjoin.py` did offline after the common-mode test, the lock now does on-board
and live.

BOTH DIRECTIONS ARE ON THE SEALED LINK — 2026-08-21 11:08. Neither half needs a
host any more:

| Direction | Carries | Transport |
|---|---|---|
| satellite → lock | the measured distance + block | sealed Thread UDP (WV3) |
| lock → satellite | the session join parameters | sealed Thread UDP (WV4) |

The handoff replaced a chain that was RTT print → host script → shell paste, so
a walk-up needed a debugger on both boards and the URSK crossed in the clear on
a debug transport. `CONFIG_ULTRAWIDELOCK_SATELLITE_HANDOFF_LOG` still prints
that line and is still bench-only; nothing depends on it now.

Receipts, one walk-up with the injector provably not running:

```
lock       handoff sent to the second anchor (ctr 1)
satellite  SAT joined from the sealed link sid=0x672da12d ch=9 code=9
lock       32 anchor reports, 19 paired on (session, block)
           median lock − anchor −30 mm, IQR −70..+20, sigma 61 mm
```

ONE KEY, TWO DIRECTIONS, so the CCM nonce spaces have to be disjoint by
construction rather than by luck. The satellite writes its role in nonce byte 0
and `examples/zephyr/satellite/Kconfig` bounds that to `range 1 3`; the lock
writes 0xFF. If a role is ever widened past 3, `HANDOFF_NONCE_ROLE` in
`witness_link.c` must move with it.

OPEN, and NOT to be read as settled from one run: the satellite joined
mid-approach, and `nresp=1` was 22 of 51 frames (57%) against 90% on the
script-relayed run before it. The handoff fires at session start and Thread
delivery costs some tens of ms, so it may systematically cost the first blocks
— or this may be ordinary variation between two walk-ups of different lengths.
One sample cannot tell those apart. Measure it before optimising it.

| | |
|---|---|
| anchor reports received | 145 |
| lock's own ranges | 168 |
| joined on (session, block) | **129** |
| median lock − anchor | **160 mm** |
| IQR | 110 .. 200 mm |
| sigma, core 117/129 | **68 mm** |
| unseal / replay rejections | **0** |
| satellite nresp=2 vs nresp=1 | 219 vs 23 (90%) |

Two sessions, `0c307d45` and `3c2e66bc`. Adjacent blocks read:

```
blk=127  anchor 1870   lock 2050
blk=128  anchor 1870   lock 2050
blk=129  anchor 1880   lock 2040
```

The 160 mm has the same install-constant character as the −200 mm the
common-mode test found; the sign differs only because the approach came from the
other side. Not the same measurement, so do NOT read the two as a drift.

STILL OPEN: this proves the TRANSPORT, not the decision. Nothing yet logs a
fusion verdict, so "the pair arrived" is established and "the pair means
outside" is not. That is what the pass criterion above still asks for, and it
needs a controlled inside/outside walk rather than a single approach.

### Three defects this run found, none of them the transport

1. **OpenThread mutex deadlock.** `thread_bring_up()` held the OT mutex across
   `openthread_run()`, which takes it itself —
   `ports/zephyr/matter/matter_thread_port.c:136` already spells the rule out.
   The shell thread wedged and the console went silent mid-command.
2. **Shell stack overflow.** `sat dataset` blew the default 2 KB stack:
   `ZEPHYR FATAL ERROR 2: Stack overflow ... Current thread: shell_rtt`. A
   254-byte handler buffer, a ~255-byte `otOperationalDatasetTlvs` under it, and
   the OpenThread start path on top. Now `CONFIG_SHELL_STACK_SIZE=8192`.
3. **The stored dataset was inert.** `CONFIG_OPENTHREAD_MANUAL_START=y` and
   nothing called `otThreadSetEnabled()`, so a persisted dataset started
   nothing and `sat dataset` would have needed retyping after every reset.
   `anchor_link_init()` now checks `otDatasetIsCommissioned()` and brings the
   mesh up. Verified: after a bare reset with nothing typed, the satellite logs
   `stored dataset found; bringing the mesh up (rc 0)` and reaches role 2.

DIAGNOSTIC LESSON, worth more than any of the three, and it includes a wrong
answer that stood for hours.

Faults 1 and 2 both present as "the console stopped answering". The UART was
blamed twice before the crash dump was ever seen, because on that backend the
Zephyr fatal-error dump goes to a console whose thread has just died. Moving the
shell to RTT is what made the failure legible.

An earlier version of this section named a third cause — hardware flow control
on the DK's UART0, RTS on P0.19 and CTS on P0.21 per pinctrl. **That was wrong,
and it is worth recording as wrong.** Flow control is OFF on this board at both
ends: the board DTS sets no `hw-flow-control` on `&uart0`, so the driver runs
`NRF_UARTE_HWFC_DISABLED` and the UARTE ignores CTS entirely. All pinctrl leaves
behind is RTS driven high once at init and never lowered, which is harmless when
nothing reads it. Board TX was never gated.

The pinctrl pins were real, so the story was plausible, and nobody grepped the
DTS for the property that actually enables the feature. Pins being routed is not
evidence a feature is on. Two independent causes, one symptom, and a third that
was never a cause at all.

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

#### The port is done in software — 2026-08-21

Deployed target: **one DWM3001CDK lock, one DWM3001CDK satellite**, DK retired
to the bench. Hardware bring-up waits for the board; everything below was
verified by build and host tests alone.

It was a board string, as the anchor example promised. The app C did not change:
`examples/zephyr/satellite/src/` has no board `#ifdef` and no `dwt_*`, and
`anchor_link.c` already stamped `CONFIG_ULTRAWIDELOCK_ANCHOR_ROLE` into every
report. What the port needed was three files and one real bug.

| Piece | What it took |
|---|---|
| Pin map | `boards/decawave_dwm3001cdk.overlay`, copied from the anchor bench's, body byte-identical |
| Thread radio | `overlay-thread.conf` is board-neutral again; `CONFIG_NRF_802154_SER_HOST` and the net-core sysbuild image moved to `overlay-thread-nrf5340.conf`, layered by `mk/satellite.mk` for the DK only |
| Size gate | `make sat-size` / `sat-size-check` / `sat-size-baseline`, one baseline per board+Thread variant |

Measured from the linker's own region report, first green builds:

| Image | Flash | RAM |
|---|---|---|
| CDK, no Thread | 116,256 B of 504 KB (22.53%) | 68,160 B of 128 KB (52.00%) |
| CDK, Thread | 272,160 B of 504 KB (52.73%) | 108,032 B of 128 KB (82.42%) |
| DK, Thread (unchanged) | 256,336 B of 1008 KB (24.83%) | 119,096 B of 448 KB (25.96%) |

Both CDK rows are committed as baselines --
`examples/zephyr/satellite/size-baseline-decawave_dwm3001cdk.json` and
`-thread.json`, one per variant because the four builds are four different
images -- and `make sat-size-check SAT_BOARD=decawave_dwm3001cdk SAT_THREAD=1`
is what fails on a regression against them.

RAM was the risk and it is not the binding one: the CDK Thread satellite has
23,040 B free, against a lock on the same part that ships at 84.70%. The obvious
reclamation is `CONFIG_MBEDTLS_HEAP_SIZE=16384` in `prj.conf` -- the lock
measured a 228 B peak on its own PSA path and now runs 1024. NOT TRIMMED HERE,
because this board's PSA workload is the CCC key ladder rather than the lock's,
and the number that settles it is a heap peak read off hardware. Measure it at
bring-up, then cut; the whole point of the baseline above is that the cut can
then be shown to be free.

#### The one-satellite assumption that was really there

The audit was owed on the lock's ingest, and it found two faults, one of them a
security fault rather than a functional one. Both are fixed; neither changes
anything for a lock with one satellite.

1. **Replay protection was disabled outright by a second satellite.**
   `witness_link.c` kept ONE `struct ultrawidelock_witness_seen` for every
   anchor. `ultrawidelock_seen_accept_ctr()` compares counters only when the
   boot id matches, and two satellites have different boot ids -- so alternating
   reports each reset the window for the other and every counter was accepted,
   including a captured one replayed between two genuine reports. Now one window
   per role, indexed by the role inside the seal.

2. **A second satellite's distance was fused as if the first had measured it.**
   The anchor callback dropped `am.role`, so both boards' distances landed in
   one `struct ultrawidelock_satellite`. Nothing fails: the side verdict simply
   inverts. The callback now carries the role, and
   `ultrawidelock_satellite_set` holds one slot per role.

The fold across roles is fail-safe and, with one satellite, is exactly the
single-peer behaviour: silent roles are absent and absent permits; a role in its
dead band abstains rather than vetoes; roles naming opposite sides return
`{UNKNOWN, geometry_ok = false}` rather than voting; `may_predict` is the AND,
so one satellite with real evidence withholds whatever the others say.

What holding three roles costs the lock, MEASURED by building this branch and
HEAD side by side in the anchor-link bench config (`LATCH=1 SIDE=1`,
`bench-2resp` + `bench-anchorlink` + LTO):

| | HEAD | this branch | delta |
|---|---|---|---|
| FLASH | 430,488 B (99.27%) | 430,728 B (99.32%) | **+240 B** |
| RAM | 120,808 B (92.17%) | 121,256 B (92.51%) | **+448 B** |

The RAM is where the three slots live: `struct ultrawidelock_satellite_set` is
720 B of `.bss` against the single peer's 240, and the per-role replay windows
36 B against 12. Most of it is this node's sample ring kept three times over.
Sharing one ring would save ~380 B and would mean reaching into the pairing
rule that has been on hardware, which is not a trade worth making with 9,816 B
free.

**That bench image is at 99.3% of flash with or without this change** -- 2,936 B
free here, 3,176 B at HEAD. The tightness is pre-existing and is the next thing
that will bite; it is why the build fails outright without `overlay-lto.conf`.

#### Can the real Matter lock carry this? Yes, at RELEASE

Measured on the shipping DWM3001CDK image -- `make build`, so Matter over
Thread, BLE, the reader and LTO, which `CDK_LTO` defaults on (`mk/cdk.mk:159`):

All four cells, because three of them invite the wrong comparison:

| flash / RAM | debug default | `RELEASE=1` |
|---|---|---|
| **without** second anchor | 417,956 B (96.38%) / 118,376 B (90.31%) | 393,872 B (90.82%) / 111,080 B (84.75%) |
| **with** second anchor | 430,728 B (99.32%) / 121,256 B (92.51%) | 405,752 B (93.56%) / 113,960 B (86.94%) |

Read the COLUMNS for what the feature costs and the ROWS for what the release
profile costs. The feature is +12,772 B of flash at debug and +11,880 B at
release -- the same ~12 KB either way -- and +2,880 B of RAM in both. The
release profile is worth ~24 KB of flash and 7,296 B of RAM in both rows.

The two axes are independent, and mixing them flatters the result: a release
image carrying the second anchor (93.56%) does read smaller than the debug image
without it (96.38%), but that comparison credits the feature with a saving that
has nothing to do with it. Adding the second anchor never buys headroom. The
question "does it fit" is answered in a column, not a diagonal -- and it does
fit, with 27,912 B of flash spare at release.

And `RELEASE=1` is not free, it is on-board debugging given up:
`overlay-release.conf` cuts the RTT up-buffer from 8,376 B to 1,024 (which is
where nearly all of its RAM saving comes from), sets `INIT_STACKS=n`, and drops
every wrn/inf/dbg format string. That file's own header says not to flash it to
a board being brought up, and stage E is bring-up -- so the bench checklist
above runs on the DEBUG default, which is also what every size figure in this
document was measured against. `RELEASE=1` is a decision for after the boards
are working, not a way to make the feature fit.

**Why 12,772 B, when the geometry is about 1 KB?** Because `ANCHOR_LINK` cannot
be built on its own yet, and the chain that forces the rest is all in Kconfig:
`bench-anchorlink.conf` requires `overlay-latch.conf` (witness_link.c still
holds the WV2 consumer inline), `overlay-latch.conf` sets `ANCHOR` +
`SIDE_GATE` + `INSIDE_LATCH` + `WITNESS_LINK_OT` together, and `INSIDE_LATCH`
depends on `SIDE_GATE` and selects `WITNESS_CODEC`. 3,254 lines of C get newly
compiled and only 395 of them -- `fusion.c` and `satellite.c` -- are the second
anchor. The rest is the sealed transport (OpenThread UDP, PSA AES-CCM both
directions, replay windows, key settings, the WV4 handoff), the side classifier
and temporal filter, the inside latch, AND the retired BLE-witness receive path
with its advertiser picker.

`ULTRAWIDELOCK_ANCHOR_LINK`'s own help already books this as a deferral. Now
that the satellite replaces the dongles, carving the WV2 consumer and the picker
out of witness_link.c is dead-code removal rather than a feature cut, and it is
where the flash is. Deliberately not bundled here: it is a refactor of a
security-sensitive file and wants its own change.

`make build` -- the shipping default -- is unaffected:
`CONFIG_ULTRAWIDELOCK_ANCHOR is not set` there, so
`ultrawidelock_satellite.c` is not compiled at all
(`modules/ultrawidelock_anchor/CMakeLists.txt` gates the whole library on it)
and every changed region in `main.c` and `witness_link.c` is inside an
`IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR*)` guard.

Geometry is per role now (`ULTRAWIDELOCK_ANCHOR_BASELINE_2_MM`, `_3_MM`,
default 0 = not installed), because the baseline is the distance from the lock
to THAT board and two boards are not in the same place. Role 1 keeps
`ULTRAWIDELOCK_ANCHOR_BASELINE_MM` unrenamed, so a one-satellite deployment
keeps the setting it already has.

Three is the ceiling and it is not a free number: it is `range 1 3` on
`ULTRAWIDELOCK_ANCHOR_ROLE`, it is `enum ultrawidelock_witness_role`, and it is
what makes `0xFF` safe as the lock's own nonce prefix for the WV4 handoff. Those
move together or the AES-CCM nonce spaces stop being disjoint.

The WV4 handoff needed nothing: it goes to mesh-local all-nodes sealed under the
one anchor key, so it already reaches every satellite, and the satellites' nonce
spaces are disjoint because each writes its own role into nonce byte 0.

#### Bench checklist, for when the boards arrive

Nothing here has been run on hardware. Stage E is not passed until it is.

1. **Flash.** `make sat-build SAT_BOARD=decawave_dwm3001cdk SAT_THREAD=1`, then
   `make sat-flash SAT_BOARD=decawave_dwm3001cdk SAT_THREAD=1`. The chip for
   `probe-rs` is **nRF52833_xxAA** (`mk/satellite.mk` picks it from the board;
   attaching with the DK's target reads like a dead board). Two probes on one
   machine enumerate in a different order twenty minutes apart -- pass
   `PROBE=` rather than trusting the order.
   **Never `--erase` a provisioned board**: the link key lives in settings.
2. **First light on the UART, radio off.** `make sat-term`, confirm the shell
   answers, before any Thread dataset goes in. This is the wire that separates a
   protocol failure from radio coexistence, and it is why the plain build exists.
3. **Per-unit provisioning**, one board at a time, and write down which unit got
   which:
   - **role** -- build-time, `CONFIG_ULTRAWIDELOCK_ANCHOR_ROLE`. A MOUNTING
     FACT. Backwards it fails no test and inverts the side verdict. One
     satellite = role 2 (outside).
   - **link key** -- the anchor key, shared with the lock (`uwl/anc`), which is
     safe across satellites only because the role is in the nonce.
   - **Thread dataset** -- `sat dataset <tlvs>`, 226 characters; the RX rings on
     both transports are sized for it.
4. **Baseline.** Measure lock-to-satellite centre-to-centre and set
   `CONFIG_ULTRAWIDELOCK_ANCHOR_BASELINE_MM` from the tape, not from the
   default. Every triangle test is relative to it.
5. **Heap peak.** With `CONFIG_ULTRAWIDELOCK_HEAP_PROBE`, read the mbedTLS peak
   after a full join, then decide the `MBEDTLS_HEAP_SIZE` trim against the
   committed size baseline.
6. **Re-baseline the size** once the config is final:
   `make sat-size-baseline SAT_BOARD=decawave_dwm3001cdk SAT_THREAD=1`.

**Stage E passes when:** the nRF5340 DK is unplugged and out of the deployment,
and one CDK satellite reports distances the lock pairs into the same ranging
block (`anchor role=2 report blk=... mm=...` beside the lock's own `pair
sid=... blk=...` for that block), with a walk-up grant and a correct withhold
from the inside.

**Adding satellite 2 and 3 later** is provisioning, not a patch: a free role in
1..3, its own place in the lock's `k/<role>`-style enrolment (the anchor key is
shared, the role keeps the nonces apart), the Thread dataset, and the matching
`ULTRAWIDELOCK_ANCHOR_BASELINE_2_MM` / `_3_MM`. The lock's ingest already holds
three roles and is host-tested for two and three reporting in one block. The
lock does need a rebuild for the new baseline value, since it is Kconfig; it
does not need a code change.

## The transport is not settled: BLE probably beats Thread

Recorded 2026-08-21, deliberately, because the current answer is INHERITED
rather than chosen. The satellite reports over Thread only because
`witness_link` already existed for the retired BLE dongles, and the pipe was
there. Nothing about a UWB anchor argues for it.

The case against Thread is one line: **Thread has no per-node revocation.**
Every node shares one network key, so "stop trusting the satellite" means
re-keying the entire mesh -- and since the commissioner owns that dataset, that
plausibly means re-commissioning the Thread network and everything on it.
Putting a second board on the home mesh is close to one-way.

BLE does not have that problem. A bond is per device; unpair one and nothing
else is touched. It also needs no dataset, so no bench-only dataset tooling goes
anywhere near a shipping path, and the satellite could drop OpenThread entirely.
Latency is better, not worse: a 7.5-30 ms connection interval against a
multicast that has to be inside the 8-block pairing ring.

What is NOT a difference: the DW3110 is a separate radio on SPI, so UWB never
contends with BLE or 802.15.4 for the air. The contention is CPU and interrupt
time on the lock's single core -- the same constraint that forces the per-frame
trace off before ranging starts. The lock already runs BLE and Thread
concurrently under MPSL, so a second BLE connection is more of an existing load
rather than a new class of it. Whether it costs measurable arm margin is a
MEASUREMENT, not a guess; the equivalent question about OpenThread turned out to
cost nothing.

Cost to switch: a GATT characteristic or L2CAP CoC carrying the same sealed WV3
payload. The codec, the seal, the replay window and the block pairing are all
transport-independent and would not change -- only what carries the bytes.

Thread stays as the fallback if BLE scheduling ever does hurt the margin.

## Risks

- Stage B's acceptance question is CLOSED, POSITIVE (2026-08-21 06:25): a stock
  iPhone does report a second responder, `nresp=2` across 47 frames. The earlier
  negative in this same list was our own 64-byte stash discarding the 65-byte
  two-record frame. B', B'' and TDoA are all retired by it.
- The live risk is now in stage C, and it is pairing rather than acceptance.
  Two anchors only mean something together if their distances describe the SAME
  ranging block, which is why the report carries one and the verdict enforces
  equality. One block of slack is 192 mm at 1.0 m/s against `tol_mm` 90, so the
  temptation to widen the match when reports arrive late must be refused -- that
  is the same arithmetic that killed B'.
- Deciding on ONE stored range means a report that arrives even one block late
  has nothing to match. `stale_ms` 1500 already permits 7.8 blocks, so a short
  ring of `(block, mm)` is the shape that fits the window we allow.
- The satellite's distance is not trust-gated. Ours passes
  `FIRA_RANGE_TRUST_K` = 3 consecutive agreeing blocks; nothing equivalent
  guards the peer's. It fails safe -- a bad peer reading withholds, so the door
  stays shut -- but that is a usability failure that will get blamed on fusion
  rather than on the missing gate.
- RESOLVED 2026-08-21: the differential error is a CONSTANT, so the two-anchor
  approach stands. Phone on the perpendicular bisector, where the true
  difference is zero by construction; 452 pairs joined on (session, block)
  across four independent sessions:

  | session | n | median | IQR |
  |---|---|---|---|
  | 0465bb25 | 95 | -200 mm | 90 |
  | 57cf247c | 120 | -200 mm | 100 |
  | a0330e5b | 124 | -210 mm | 80 |
  | fb77ba1f | 113 | -200 mm | 50 |

  The same offset four times over, varying by 10 mm. Remove it and the anchors
  agree to **sigma 39 mm** over 79% of pairs -- inside the 60 mm dead band,
  which is the number that had to be beaten. A wandering differential would
  have been unfixable; a constant is an install-time calibration.

  What remains is a ~20% TAIL, and that is a categorically easier problem: it
  is what median filtering and the K-consecutive trust layer already exist for.

  Two honest limits. A positioning error and a calibration bias both produce a
  constant, so the -200 mm cannot be attributed to the radios; it does not
  matter, because the question was constant-versus-wandering and the sigma is
  independent of where the centre sits. And this is one geometry at one
  distance -- the tail fraction may differ at an install baseline.

  Earlier single-anchor captures could not see any of this. A COMMON-MODE shift
  cancels exactly in `sign(d_inside - d_outside)`, so the four 420 mm clusters
  that looked alarming were never evidence either way. Only a pair can tell.
- `N_Resp` is baked into key derivation, so lock and satellite builds must
  agree and the setting applies from session establishment — a mid-session
  change desyncs every derived key (known, documented at the knob).
- Thread + DW3000 concurrency on the DK is assumed from `apps/nrf5340dk-lock`
  running Matter over Thread beside ranging; first light on stage B2 should
  still use the UART wire so radio coexistence cannot masquerade as a
  protocol failure.
