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
  n_tuples   u8      up to K = 8
  tuples[]           per advertiser, loudest first:
    hash24   u24     truncated keyed hash of AdvA (per-witness key)
    mean_dbm i8
    n_pkts   u8
CCM: key = 128-bit per-witness link key, nonce = witness_id || boot_id || ctr,
     tag = 8 bytes. Header 21 B + 5 B per tuple = 61 B at K = 8, plus the
     seal: one 802.15.4 frame.
```

K is 8 rather than 4 because the binding case is the INSIDE witness, not the
outside one. During a walk-up the phone is nearly on top of the outside
witness and certainly ranks in the top few; the inside witness hears that same
phone through a door and ranks it below whatever else the house is running. If
it misses the cut there the pair has no inside reading, quorum fails, and no
clear is possible -- safe, and indistinguishable from a broken lock. A denser
RF environment than K = 8 covers wants the lock to hint the picked label back
to the witnesses so it is always included; that is a protocol addition on both
ends and is not built.

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

Identity: there is none, and that is a correction to an earlier revision of
this document rather than a refinement of it. The witness computes `hash24 =
trunc24(hash(group_key, AdvA))` for the loudest K advertisers, under a key the
WITNESSES share and the lock does not -- so the same advertiser carries the
same label at both witnesses, which is what lets inside be compared against
outside, while staying opaque to the lock and to the air.

The lock does not match that label against anything. It cannot. The phone is
the central, so the address the lock holds from the credential connection is
an InitA generated for the initiating role, while what the witnesses hear
comes from advertising sets with their own address state and their own
rotation timers. The Core Spec permits one RPA across roles and does not
require it; nothing Apple publishes promises it. Matching the two would fail
in the ordinary case, not in a corner.

Instead the lock picks the phone by TRAJECTORY: exactly one advertiser in the
room gets louder at the outside witness while the authenticated UWB range
falls. See `ultrawidelock_witness_pick.h`. No address is transmitted, none is
matched, and the lock learns nothing it could persist. A label whose RPA
rotates mid-approach retires and the scoring restarts, costing one approach.
24 bits keeps ambient collisions negligible at household scale; a collision at
worst contributes one wrong tuple to one window, which the N-consecutive-
windows rule and the paired second witness absorb.

---

## 6. Provisioning: once, before the dongle goes on the wall

Steady state has no operator action, no host and no probe. Getting there takes
one command per dongle, over its own USB CDC console, at install:

```
PROV <role> <link-key-hex32> <group-key-hex32> <dataset-hex>
SHOW | WIPE | HELP        (make witness-prov-help prints the full form)
```

- **role** — `inside`, `outside` or `threshold`. A mounting fact, declared
  once. It is no longer a build flag, so all dongles run one image and moving
  one from inside to outside is a re-provision, not a reflash.
- **link key** — 16 bytes, DIFFERENT per dongle. Seals that witness's reports;
  the lock holds the same bytes under `uwl/wit/k/<role>`.
- **group key** — 16 bytes, THE SAME on every dongle and deliberately NOT on
  the lock. It labels advertisers so inside can be compared against outside
  (section 5) while the labels stay opaque to the lock.
- **dataset** — the Thread active operational dataset TLVs, from
  `ot-ctl dataset active -x` on any node of the network.

All four persist in the witness's settings. It cold-boots into scanning,
joining and reporting, and holds that for weeks with nothing attached. The LED
is the only diagnostic once it is on the wall: fast blink unprovisioned, slow
blink provisioned but not attached, solid attached and reporting.

**Why this is not over the air, which was the earlier plan.** The lock builds
with `CONFIG_BT_OBSERVER=n` and `CONFIG_BT_CENTRAL=n`: it advertises and
accepts a connection, and that is all its BLE stack does. Wireless enrollment
needs the lock to either scan for a dongle or connect to one, so it means
adding a BLE role to the stack that carries the credential -- the single most
security-sensitive surface on the device -- to save a one-time step performed
while the dongle is already in your hand, plugged into the machine that just
flashed it. That trade is bad in the direction that matters. It is a deferral
with a stated reason, not an oversight, and it does not touch the requirement
that STEADY STATE be wireless, which it is.

The provisioning console prints no key material back, so a captured session
log does not compromise the link.

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

Size, MEASURED 2026-08-19 on this tree. The estimate this section used to
carry was ~7 KB flash / ~1.1 KB RAM, and it held.

| build | FLASH | RAM |
|---|---|---|
| `make build` (thread+lto) | 417,684 (96.32%) | 118,312 (90.26%) |
| `make build LATCH=1` | 424,544 (97.90%) | 119,464 (91.14%) |
| **delta** | **+6,860 B** | **+1,152 B** |
| `RELEASE=1 SMP=1 LATCH=1` | 406,536 (93.74%) | 115,944 (88.46%) |

The delta is not the interesting number; the residue is. On the dev config
LATCH=1 leaves 9,120 B free, which is not a budget anything else can be added
to. On the shipping configuration it leaves 27,128 B, which is workable. The
asymmetry is the logging the dev config carries, and it means the enrollment
path (stage P7, unbuilt) must be measured against the shipping config or it
will look affordable and not be.

Default build unchanged, VERIFIED 2026-08-19: commit 588459f5 and this
branch's HEAD both produce a 417,684 B loadable image differing in exactly 4
bytes, the `__TIME__` inside OpenThread's version string. Every allocated
section is identical in size and placement.

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

**P6 -- witness firmware v2. BUILT.**
`examples/zephyr/ble-witness/` is one image for every mounting position: it
labels every advertiser under the group key, ranks them with the shared
accumulator, seals a WV2 window under its link key and sends it over Thread as
a sleepy end device. `LEARN`, `ADDR`, the per-role build flag and the UART
summary line are all gone, and it shares the lock's codec rather than
reimplementing it. Builds at 285,576 B flash / 96,360 B RAM on the nRF52840,
27.56% and 36.76% of the part.
Pass: two dongles, flashed identically, provisioned with different roles, each
cold-boots to solid-LED and reports with no host attached. NOT YET RUN -- it
needs the hardware, and it is the same session as P0.

**P7 -- optional accel opportunity source.**
A bench capture deciding whether LIS2DH12 door-swing transients are detectable
at low mg (UNMEASURED; the SLAM Kconfig proves the IRQ wiring exists). If they
are not, the accel opportunity source is dropped and the NFC-after-mechanical-
exit tax in section 3.4 stands.
Wireless enrollment is NOT in this stage any more; see section 6 for what
replaced it and why.
Pass: 20 normal door swings all detected, 0 false events overnight; otherwise
record the negative result and drop the feature.

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
