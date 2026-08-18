# Inside latch: never a passive unlock from inside, on two BLE dongles

The DWM3001CDK must never passively unlock while the credentialed phone is
inside the door. This document is the design for meeting that goal with the
hardware already on the bench -- the lock plus two nRF52840 BLE witness
dongles -- connected entirely wirelessly: no Raspberry Pi, no J-Link held
open, no USB host in steady state, no new sensor types.

> Convention (matches `approach-direction.md`): **VERIFIED** = confirmed
> against this tree or on silicon; **MEASURED** = a dated bench observation
> recorded in a source comment; **UNMEASURED** = required by the design but
> not yet observed; **LIKELY** = consistent with vendor documentation from
> memory, to be verified before it is load-bearing.

Two consequences of the goal, restated because every decision below leans on
them:

- Failing to unlock outside is not a defect. The user retries, taps NFC, or
  uses the app. No inside-safety is traded for outside-availability.
- Losing evidence must not unlock. Dead dongle, dropped mesh, reboot,
  corrupt storage: each must leave the lock refusing to open from inside.

---

## 1. Route decision

The three candidate routes from the task brief, evaluated against the owner
constraint "BLE dongles only, wireless, no Pi":

| route | verdict | why |
|---|---|---|
| A: second UWB anchor | rejected | needs a DWM3001CDK-class board that is not in the bill of materials; multi-anchor stock-iPhone ranging is explicitly unproven in this tree |
| B: door-transition latch | **core of the chosen design** | nothing to train; a dead sensor while latched INSIDE leaves the latch INSIDE |
| C: hybrid (latch + live classification at crossings) | **chosen, with BLE in place of UWB** | the latch is the veto authority; the BLE differential is consulted only at the one moment it is reliable |

The chosen design is route C with one structural change to how the BLE
witnesses are used. They are not a continuous classifier -- the 2026-08-11
bench notes in `ultrawidelock_side.c` record why that fails (49% of windows
in the dead band at the door plane, 37 refusals against 2 grants on one
walk). Instead the witnesses are a **walk-up notary**: their evidence is
consulted only to CLEAR the inside veto during a live approach, at 3-8 m out
where the differential is unambiguous, and the cleared state then persists
through the dead band as state rather than as an expiring reading. This
retires the `outside_hold_ms` hack rather than tuning it.

Answers to the brief's three explicit questions:

1. **Devices beyond the lock:** two nRF52840 dongles (~$10 class), one
   inside, one outside, each on a USB charger. The threshold role remains in
   the firmware but is optional. No Pi, no UWB satellite, no door sensor.
2. **Wireless:** yes. Reports ride Thread (the lock already runs standalone
   OpenThread as a Matter MTD); one-time enrollment rides BLE. No probe, no
   USB data connection, ever.
3. **What removes per-session learning and the address handshake:** identity
   matching moves to the lock. The witnesses report keyed-hash tuples for
   the loudest advertisers they hear; the lock, which already holds the live
   credential peer address for the session, computes the same keyed hash and
   matches. `LEARN` and `ADDR` retire. Role, Thread dataset, and link key
   are written once at enrollment and persist in the witness's NVS, so both
   dongles run one firmware image and cold-boot into a working state.

---

## 2. Architecture: two layers with different failure semantics

The single most important property: the two layers fail in opposite
directions, and the safe layer is the authority.

**Layer 1 -- the latch (authority).** Per-credential persistent state on the
lock. Its resting state is INSIDE. It needs no RF evidence to hold, so
evidence loss cannot move it. While it holds, passive unlock is vetoed
unconditionally at the same call site the SIDE gate uses today
(`main.c`, UNLOCK_PREDICT / UNLOCK_THRESHOLD); deliberate paths (NFC
Express, Home commands, mechanical) never enter that call site and stay
ungated.

**Layer 2 -- the witnesses (clear evidence).** Consulted only during a live
credential session, only to clear the veto for that one approach. Absence of
witness evidence means the clear does not happen; it never means anything
else. A dead dongle degrades the product to "NFC-only lock", never to
"unlocks from inside".

Asymmetric freshness follows from the asymmetric goal: evidence in the veto
direction may be lenient, evidence in the clear direction must be
challenge-bound (section 5), because a replayed genuine "outside" report
from yesterday is the one dangerous forgery.

---

## 3. The state machine

### 3.1 Persistent record, per credential

Keyed by a derived, non-identifying credential id: a truncated
`ultrawidelock_hash` over install-local credential material (fabric index +
credential slot). No address, IRK, or other phone identifier is persisted or
derivable from the record.

```
struct latch_rec {
    uint32_t cred_id;              /* derived, non-identifying */
    int64_t  confirmed_inside_ms;  /* when last pessimistically latched */
    bool     crossing_opportunity; /* any door-crossing-capable event since */
    /* CRC over the record; bad CRC or missing record reads as
     * { INSIDE now, opportunity = false } -- the safest possible state. */
};
```

Persisted via the settings subsystem (already in the lock image for Matter
fabric storage -- VERIFIED, `test_matter_fab_settings.c` and `settingsfake`
exist). Written on change only; a few writes per day, so flash wear is not a
factor.

### 3.2 Events

1. **Any unlock grant, passive or deliberate, any path:** re-latch INSIDE at
   `t + entry_dwell` (default 60 s, covering the walk-in), then
   `opportunity = false`. Pessimism is the resting state: after any door
   opening, the phone plausibly went inside, and the design assumes it did.
2. **Opportunity events** set `crossing_opportunity = true`: any unlock
   command after the dwell, and -- if stage P7 measures out -- an LIS2DH12
   door-swing event. Attributable events (a specific credential's unlock)
   set that credential's flag; non-attributable events (door motion,
   mechanical operation if ever sensed) set every credential's flag, because
   there is no identity at a door event. The safety cost of the broad set is
   bounded: opportunity alone clears nothing (see 3.3).
3. **Reboot:** records restore from settings. Corruption degrades to INSIDE
   with no opportunity.

### 3.3 The clear: five conditions, all in one approach

Passive unlock for credential C is permitted only when every one of these
holds:

1. A live credential session exists and the lock holds C's current peer
   address (in RAM only -- section 7).
2. `C.crossing_opportunity == true` -- there has been a door-crossing
   opportunity since C was last confirmed inside. A pure RF misread through
   the door can therefore never clear the latch by itself; there must also
   have been a moment the phone could physically have left.
3. Witness reports echo the current challenge nonce, their counters are
   monotonic per (witness, boot_id), and their age is inside the staleness
   bound. This is `ultrawidelock_satellite`'s staleness rule plus the nonce.
4. The keyed-hash-matched differential reads OUTSIDE by margin for N
   consecutive paired windows (default 3), while the UWB range is closing
   from beyond a minimum distance (default 3 m). The existing
   `ultrawidelock_side` classifier and temporal filter compute this;
   the latch consumes only decisions that `ultrawidelock_side_may_passive_unlock`
   already accepts.
5. The grant then fires, and event 1 immediately re-latches INSIDE.

Failure of any condition leaves the veto standing. NFC, app, and mechanical
paths are unaffected and double as the universal recovery: after storage
loss, first boot, a new credential, or an unobserved mechanical exit, one
deliberate unlock re-seeds the record and restores normal walk-up behaviour.

### 3.4 The cases the brief asked about

- **Phone leaves without an observed door-open** (thumbturn exit, no sensed
  event): `opportunity` stays false; the next walk-up is vetoed; the user
  taps NFC once. Annoying, never unsafe. The optional accelerometer
  opportunity source (P7) exists to shrink exactly this case.
- **Multi-credential, one leaves and one stays:** latches are per
  credential. B inside stays latched; A outside clears and unlocks on
  approach. Opening for A while B is inside is the normal household case,
  and B's latch is untouched by A's grant.
- **Door held open, or opened and closed with nobody passing:** either sets
  `opportunity` at most. Clearing still requires live confident-outside
  evidence, which a phone that stayed inside does not produce.
- **Resident walks to the door from inside:** latched INSIDE. If no
  opportunity since their entry, the clear is impossible regardless of RF.
  With opportunity set (someone else used the door), the clear additionally
  needs N consecutive confident-OUTSIDE windows plus a closing UWB
  trajectory from beyond 3 m -- a through-door misread must survive all of
  that to cause harm. Residual, stated in section 8.

---

## 4. Transport: Thread for reports, BLE for one-time enrollment

Candidates for the witness-to-lock link, evaluated on the lock side:

| option | lock-side cost | credential-band impact | verdict |
|---|---|---|---|
| Thread UDP | one more `otUdpSocket` + codec | none beyond existing Thread | **chosen** |
| BLE connections | 2 more central links on the credential controller | contends with the ranging arm deadline (~1836 us, `thread_gate.c`) | no |
| bare 802.15.4 / ESB | fight the OT-owned radio driver, MPSL timeslot work | none | no |

VERIFIED in-tree: the lock runs standalone OpenThread (`overlay-thread.conf`,
`OPENTHREAD_MTD=y`, MED not SED, `NET_SOCKETS=n`, every datagram through
`otUdpSend` / `otUdpSocket` callbacks). A second socket is a small addition
to an existing stack, not a new stack.

**Witnesses join the home's Thread network as SEDs** polling at ~500 ms.
SED, not MED, and mains-powered regardless (USB chargers): the choice is
about radio time, not battery. BLE scanning wants the radio; a SED gives
Thread the radio only at poll instants, so the scan duty cycle survives.
Uplink reports are child-initiated and suffer no poll latency; only the
challenge nonce rides the downlink, bounded by the poll period, which is
well inside the staleness budget.

Witness -> lock traffic routes child -> parent router -> lock (the lock is
itself an MED child); two mesh hops, tens of milliseconds against a 1500 ms
staleness bound.

The witness firmware becomes dual-stack: BLE observer plus OpenThread SED
under MPSL dynamic multiprotocol. LIKELY supported on nRF52840 (Nordic
ships BLE/Thread dynamic multiprotocol samples for this SoC); this is the
plan's riskiest assumption and is stage P0, first, on hardware.

---

## 5. Wire protocol (WV2)

One UDP datagram per witness window, AES-CCM sealed with the per-witness
link key. Nothing in it identifies a phone.

```
WV2 payload (before sealing):
  ver        u8      protocol version, 2
  role       u8      inside / outside / threshold
  boot_id    u32     random per witness boot
  ctr        u32     monotonic per boot
  echo_nonce u64     latest challenge nonce heard from the lock
  window_ms  u16     summarisation window length
  n_tuples   u8      up to K = 4
  tuples[]           per advertiser, loudest first:
    hash24   u24     truncated keyed hash of AdvA (per-witness key)
    mean_dbm i8
    n_pkts   u8
CCM: key = 128-bit per-witness link key, nonce = witness_id || boot_id || ctr,
     tag = 8 bytes. Total on the wire ~50-60 B, one 802.15.4 frame.
```

Rules the lock enforces (all lock-side; witnesses hold no authority):

1. CCM must verify under that witness's key, or the datagram never existed.
2. `ctr` must be strictly monotonic per (witness, boot_id). A new `boot_id`
   resets the counter and is accepted -- a witness reboot costs at most a
   brief veto, never a lockout.
3. Age since arrival must be inside the staleness bound
   (`ultrawidelock_satellite`'s rule; default 1500 ms).
4. **Clear-direction evidence additionally requires `echo_nonce` to be the
   current epoch.** The lock rotates the nonce when a credential session
   opens and every 30 s while one is live, sending it unicast to each
   enrolled witness. Reports echoing a stale nonce still count toward the
   veto direction; they never count toward a clear. Replay is therefore
   structurally impossible in the dangerous direction.
5. Windows from the two dongles are paired by nonce epoch plus arrival
   time. Alignment error is bounded by the poll period (~500 ms); the
   outside margin must absorb the residual smear (bench-sized in P8).

Identity matching: the witness computes `hash24 = trunc24(CMAC(link_key,
AdvA))` for the loudest K advertisers. The lock computes the same over the
live session peer address and matches. 24 bits keeps ambient collisions
negligible at household scale; a collision at worst contributes one wrong
tuple to one window, and the N-consecutive-windows rule plus the paired
second witness absorb it.

---

## 6. Enrollment: once, wireless, then never again

Steady state has no operator action and no host. The one-time enrollment is
wireless too:

1. Fresh witness (no NVS record) boots into enrollment mode: BLE
   connectable advertising as `UWLW`, fast LED blink. Role is declared on
   the witness by button taps before enrollment (1 tap = outside, 2 =
   inside; LED pattern confirms) -- a mounting fact, declared once, exactly
   like `ULTRAWIDELOCK_ANCHOR_SELF_INSIDE`.
2. The installer opens a short enrollment window on the lock (user button
   on the DWM3001CDK; board DT check in P5). The lock scans, connects as
   central, and opens an L2CAP CoC on the witness's enrollment PSM --
   reusing the central + CoC machinery the credential path already has.
3. Ephemeral P-256 ECDH (both ends already carry PSA ECC), derive a wrap
   key, and transfer one sealed record: Thread operational dataset,
   128-bit link key, witness id, window parameters. The witness persists it
   to NVS and reboots into steady state: join Thread, start scanning,
   report. Solid LED when attached, heartbeat while reporting.
4. Re-enrollment = hold the witness button 5 s to wipe and return to step 1.

The Thread dataset is the home's real network secret, so it is never sent
in the clear. Residual: no out-of-band binding, so an active MITM present
during the seconds of enrollment could intercept it. Enrollment happens
once, at install, with both devices in hand; the residual is stated in
section 8 rather than engineered away.

A USB CDC `ENROLL` command remains in the witness as a bench fallback
(the hardware has the port anyway), but nothing depends on it.

---

## 7. Privacy

- The credential peer address exists in lock RAM for the life of the
  session, exactly as it already must for the connection itself. It is
  never logged (the `SIDE_PEER_EMIT` bench logger stays default n and off
  in this design), never persisted, never transmitted -- the lock sends
  nonces, not addresses.
- Witnesses transmit 24-bit keyed truncations. Without the per-witness
  link key an observer cannot map them to addresses, and the address a
  hash refers to is a resolvable private address with a bounded lifetime.
- Latch records are keyed by a derived id from install-local credential
  material. Nothing in NVS, on either device, identifies a phone.

---

## 8. Safety argument: every way to lose evidence

| # | loss | mechanism | outcome |
|---|---|---|---|
| 1 | one or both witnesses dead | clear conditions 3-4 unmeetable | veto stands; NFC works |
| 2 | Thread mesh down / border router reboot | reports stop arriving | veto stands |
| 3 | witness reboot | `boot_id` changes, counters reset, fresh nonce echo required | brief veto at worst |
| 4 | lock reboot | latch restored from settings | latch holds |
| 5 | settings corrupt / first boot / new credential | record reads INSIDE + no opportunity | veto until one deliberate unlock re-seeds |
| 6 | phone stops advertising or rotates its RPA mid-approach | condition 4 fails | veto; retry or NFC |
| 7 | replayed reports | nonce epoch (clear direction) + counters | rejected |
| 8 | forged reports without the key | CCM | rejected |
| 9 | witness link key stolen | attacker can fabricate outside evidence | needs opportunity set AND the real phone closing on UWB from 3 m out; possessing a witness means being inside the home already. Residual, stated |
| 10 | through-door RF misread while an opportunity is set | must survive N consecutive confident-OUTSIDE paired windows plus a closing UWB trajectory | residual, stated; the margin and N are the knobs, sized on the bench in P8 |

Every row degrades to "the door does not open passively", except 9 and 10,
which are stated residuals with their preconditions -- neither is reachable
by evidence LOSS, only by evidence FORGERY plus independent conditions.

Load-bearing physical assumption, called out as the one that can sink the
design: **the phone keeps advertising with the session's AdvA during the
approach.** MEASURED 2026-08-11 on this bench (3-8 filtered packets per 2 s
window, recorded in `ultrawidelock_side.c`); re-verify after iOS updates
(P0/P8). If it stops holding, the fallback is the lock's own connection
RSSI plus a single inside dongle -- a weaker discriminator, deliberately
not designed here.

---

## 9. What this retires

- **The Raspberry Pi collector**: the lock does its own correlation.
- **`LEARN` / `ADDR` / per-role images** on the witnesses: replaced by
  lock-side keyed-hash matching and role-in-NVS.
- **The RTT SF1 feed** (`ULTRAWIDELOCK_SIDE_FEED_RTT`): demoted to bench
  debug; the deployed path is WV2 over Thread.
- **`outside_hold_ms` as a load-bearing mechanism**: the cleared latch is
  state and survives the dead band by construction. The side-module
  defaults are not changed by this design; the latch simply stops relying
  on that one.
- **`ultrawidelock_fusion_may_predict()`** stays out of the new path. Its
  fail-open-on-UNKNOWN polarity is documented and intentional for the
  legacy ANCHOR=1 availability goal, and is exactly wrong for this one.
  VERIFIED: it is unreachable in a SIDE=1 build (`main.c` guards it with
  `!IS_ENABLED(CONFIG_ULTRAWIDELOCK_SIDE_GATE)`).

The existing `ultrawidelock_side` classifier, temporal filter, decision log
and `ultrawidelock_satellite` staleness rule are all kept and reused as-is.

---

## 10. New configuration surface

All default n; the default build stays byte-for-byte unchanged. Follows the
SIDE=1 overlay pattern: `overlay-latch.conf`, wired as `LATCH=1` in
`mk/cdk.mk`'s `CDK_CONF` chain.

```
ULTRAWIDELOCK_INSIDE_LATCH        bool, default n; the latch + veto call site
ULTRAWIDELOCK_WITNESS_LINK_OT     bool, default n; WV2-over-Thread ingest
ULTRAWIDELOCK_WITNESS_ENROLL      bool, default n; lock-side BLE enrollment
ULTRAWIDELOCK_LATCH_ENTRY_DWELL_MS   int, default 60000
ULTRAWIDELOCK_LATCH_CLEAR_WINDOWS    int, default 3
ULTRAWIDELOCK_LATCH_CLEAR_MIN_MM     int, default 3000
ULTRAWIDELOCK_WITNESS_STALE_MS       int, default 1500
```

`overlay-latch.conf` sets `ULTRAWIDELOCK_ANCHOR=y` and
`ULTRAWIDELOCK_SIDE_GATE=y` (the latch consumes the side filter's
decisions) plus the three new bools; `SIDE_FEED_RTT` and `SIDE_PEER_EMIT`
stay off -- no probe, no address logging.

Size budget, estimate until built (against the shipping config's measured
54,332 B flash / 20,060 B RAM free, `size-baseline.json`):

| piece | flash est. | RAM est. |
|---|---|---|
| latch module + settings glue | ~1.5 KB | ~200 B |
| WV2 codec (shared, host-tested) | ~1 KB | ~0 |
| witness link (socket, PSA CCM glue, pairing) | ~2.5 KB | ~600 B |
| enrollment (separable Kconfig) | ~2 KB | ~300 B |
| total | **~7 KB** | **~1.1 KB** |

Both fit with wide margin. A measured report is stage P9's deliverable;
treat a large overshoot as a design problem per the brief.

---

## 11. Staged implementation plan

Order: plan-invalidators first, then platform-free code, then integration,
then hardware. P0 needs the bench; P2-P4 do not depend on its numbers and
can proceed in parallel with it. Each stage carries its pass/fail check; a
stage failing twice after fixes stops downstream work per the working
rules.

**P0 -- multiprotocol spike (plan invalidator, hardware).**
Build the existing ble-witness with an added OpenThread SED overlay on an
nRF52840 dongle; attach to a bench Thread network; measure filtered adv
packets per 2 s window at 2 m from an iPhone, while attached.
Pass: >= 3 packets/window sustained. Fail: the witness link moves to a
design review (BLE transport reconsidered) before any integration work.
Also re-verifies the section 8 advertising assumption on current iOS.

**P1 -- this document.** Done when it answers every question in the task
brief and the safety table enumerates every loss case. (This stage is what
you are reading.)

**P2 -- WV2 codec, platform-free.**
`modules/ultrawidelock_anchor/{include/ultrawidelock_witness_msg.h,src/ultrawidelock_witness_msg.c}`:
encode/decode + validation, no crypto (sealing stays with PSA at the call
site), following the `ultrawidelock_uwb_msg` builder/parser split. Host
tests `tests/host/test_ultrawidelock_witness_msg.c`, registered in
`sources.sh` and `test_main.c`.
Pass: `make check` green; round-trip, truncation, and malformed-input
cases covered.

**P3 -- latch module, platform-free.**
`modules/ultrawidelock_anchor/{include/ultrawidelock_latch.h,src/ultrawidelock_latch.c}`:
the section 3 state machine over caller-owned structs, integer-only,
serialize/deserialize with CRC. Host tests
`tests/host/test_ultrawidelock_latch.c` covering, at minimum, safety rows
1-7: corrupt record, reboot restore, opportunity semantics, entry dwell,
multi-credential independence, nonce-stale clears rejected, counter
regression rejected, silence never clears, grant re-latches.
Pass: `make check` green; every safety-table row that is host-testable has
a named test.

**P4 -- side-filter replay evidence.**
Extend `test_ultrawidelock_side_replay.c`-style traces with a synthetic
walk-up and a through-door misread trace; assert the latch clears on the
first and refuses the second. Pass: `make check` green.

**P5 -- lock integration behind LATCH=1.**
`apps/dwm3001cdk-lock/src/witness_link.c` (otUdpSocket, nonce epochs, CCM
via PSA, pairing, feeds `ultrawidelock_side_filter_feed`), latch persistence
via settings, peer-address capture at CoC open without logging, the veto at
the existing call site, `overlay-latch.conf`, `LATCH=1` in `mk/cdk.mk`.
Pass: default `make build` byte-identical to baseline;
`make build LATCH=1 CDK_BUILD=build/cdk-latch` links;
`make cdk-size CDK_SIZE_REPORTS=0 CDK_BUILD=build/cdk-latch` reports the
delta against the section 10 budget.

**P6 -- witness firmware v2.**
Single image: role/dataset/key from NVS, WV2 reports with CCM, boot HELLO,
challenge echo, LED states, USB `ENROLL` bench fallback. Keep the P0
overlay as its Thread base.
Pass: two dongles, flashed identically, enrolled with different roles,
each cold-boots to "attached and reporting" with no host attached.

**P7 -- wireless enrollment + optional accel opportunity source.**
Lock-side `ULTRAWIDELOCK_WITNESS_ENROLL` (button window, central + CoC,
ECDH, sealed record) and witness-side enrollment mode. Separately, a bench
capture deciding whether LIS2DH12 door-swing transients are detectable at
low mg (UNMEASURED; the SLAM Kconfig proves the IRQ wiring exists). If
they are not, the accel opportunity source is dropped and the
NFC-after-mechanical-exit tax in section 3.4 stands.
Pass (enrollment): a factory-reset witness reaches "reporting" purely over
the air. Pass (accel): 20 normal door swings all detected, 0 false events
overnight; otherwise record the negative result and drop the feature.

**P8 -- bench soak and margin sizing.**
Scripted walk-ups from outside; resident-phone weekend soak inside with
daily door traffic by a second credential. Size the outside margin and N
from the recorded traces; write the measured numbers back into the module
headers with dates, matching the tree's convention.
Pass: zero passive unlocks with the phone inside across the soak; walk-up
grant rate reported honestly, whatever it is (a low rate is a tuning item,
not a safety failure).

**P9 -- size report + doc closure.**
Measured flash/RAM delta for LATCH=1 against the committed baseline; update
this document's section 10 with measured numbers; note the RTT feed's
demotion in its Kconfig help. Updating `SIDE_GATE.md`'s operator flow is a
repo-stance change and is routed to the main session rather than done
unilaterally here.

Validation gates for the tree work (P2-P5), unchanged from the brief:

```
make check
make build
make cdk-size CDK_SIZE_REPORTS=0
```

---

## 12. Open measurements, ranked

1. **P0**: BLE observer + OT SED coexistence and per-window packet counts
   on nRF52840 (LIKELY per Nordic multiprotocol support; unverified here).
2. **Section 8**: phone advertises with the session AdvA during approach
   (MEASURED 2026-08-11; re-verify on current iOS).
3. **P7**: LIS2DH12 door-swing detectability at low mg (UNMEASURED).
4. **P8**: window-pairing smear vs the outside margin (UNMEASURED).
5. **P9**: real flash/RAM delta vs the ~7 KB / ~1.1 KB estimate.
