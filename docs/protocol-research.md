# Protocol research

## Abstract

This report documents the on-air behavior of a phone-driven ultra-wideband (UWB)
proximity unlock: how a phone opens a fixed reader on approach. The phone conducts the
protocol exchange over Bluetooth LE, then the two run a UWB secure ranging exchange that
lets the reader measure distance before it unlocks. The scope here is deliberately
narrow: what reaches the air, and what must hold before ranging can begin.

The work began from a specific failure: NFC tap-to-unlock worked, but the door would not
open on approach. Tap working is a useful signal: it means the BLE transport,
provisioning, and credentials are healthy, so the fault is UWB-specific. The fallback to
NFC when UWB is disturbed is by design, which is why tap continued to work throughout the
period when ranging was dead.

> **Educational, research, and study use only.** These notes exist for learning,
> independent research, and personal study of how BLE + UWB secure ranging systems behave
> on the air. They are not an invitation to bypass, defeat, or gain unauthorized access
> to any access control system. Only work with hardware you own or have explicit, written
> authorization to test.
>
> This report is not affiliated with, endorsed by, or speaking for any company, standards
> body, or specification owner mentioned or alluded to. All protocol specifications,
> standards, trademarks, product names, and other intellectual property referenced or
> implied remain the property of their respective owners, along with every right,
> license, and disclaimer attached to them.

## 1. System under study

- The **phone** is the UWB initiator (the side sending poll packets). It carries a U-class
  UWB chip and runs iOS 26 or later.
- The **reader** is the fixed lock, acting as the UWB responder with a single anchor. The
  radio studied here is a bare DW3000-family part (a DW3110) on an nRF5340, with no
  dedicated UWB coprocessor, so the MAC, PHY, and STS are all implemented in host
  firmware.
- All protocol-level traffic rides on BLE. UWB carries no application data; it is purely
  the distance measurement. The unlock policy is entirely the reader's decision: in the
  configuration studied, it unlocks at 150 cm or closer and relocks past 180 cm, giving
  30 cm of hysteresis.

## 2. The full transaction, top to bottom

The complete exchange in order. Everything above the line is in the clear or
authentication-protected; everything below is encrypted under BleSK.

```
 1     Initiate Access Protocol                          phone → reader (clear)
 2-9   Access/authentication exchange (AUTH0 [± cert, AUTH1])
10-11  EXCHANGE (carries the zero-length "URSK-ready" trigger, tag 0x98)
------ everything below encrypted under BleSK ---------------------------------
12     Reader: "Access Protocol Completed"               reader → phone
13     Time Sync                                         phone → reader   (section 5)
14     (optional) Initiate Ranging Session               phone → reader
15-18  Ranging Setup M1 / M2 / M3 / M4                   (section 6)
       ... secure UWB ranging; distance computed at the reader ...   (section 7)
19-22  Suspend / Resume (either side)                    (section 8)
23     Reader Status Changed → unlock/lock decision      reader → phone
```

Once "Access Protocol Completed" passes, both sides drop the authentication session keys.
BleSK persists, and it guards all ranging control traffic from that point on (time sync,
M1-M4, suspend/resume).

## 3. Discovery and connection (all sniffable)

The reader beacons `ADV_IND` on LE 1M (and optionally coded S=2). The payload is service
data under 16-bit UUID `0xFFF2`. The fields relevant to reverse engineering:

- Byte 7, bit 7 is the "BLE + UWB flow supported" flag. This is the one bit a UWB-capable
  build sets and a control-only build leaves at 0; if it is 0, the phone will not attempt
  to range. For reference, the rest of that byte: bit 6 is the BLE-only flow, bits 4:3 are
  notification/error, bits 2:0 are the advertisement version.
- Byte 8 is TX power. Bytes 9-18 are a truncated reader group identifier plus a
  sub-identifier.
- Bytes 19-22 are the dynamic tag expiry as Unix time, or `0xFFFFFFFF` if the reader has
  no clock. Byte 23 is reserved.
- Bytes 24-30 are the dynamic tag itself: the first 7 octets of
  `AES-128(GroupResolvingKey, pad ‖ AdvA ‖ expiry)`.

The phone scans passively and identifies its own readers by recomputing the dynamic tag
with each group resolving key it holds. On a match, it sends `CONNECT_IND`. Conversely, a
reader that has gone stale or been reprovisioned is effectively invisible to the phone
even while advertising, because the tag no longer resolves.

The GATT and L2CAP handshake proceeds as follows. The phone reads a characteristic that
returns the dynamic SPSM (in `0x0080-0x00FF`), the supported protocol versions
(`v1.0 = 0x0100`), and a feature bitmap (bit 0 is time sync procedure 0, bit 1 is
procedure 1, bit 2 is LE coded PHY). It writes back the version and bitmap it selected.
Both version values are folded into the secure channel KDF, so a version downgrade makes
the two sides derive mismatched keys and the exchange fails. From there everything runs
over an L2CAP connection-oriented channel on that SPSM. The reader is the GAP peripheral
and GATT server, the phone is central and client, and BLE pairing is not required.

For initial sniffing, the fastest sanity check is `0xFFF2` service data with byte 7 bit 7
set: it confirms the reader side of discovery is alive within seconds.

## 4. Authentication, and the origin of the ranging key

The phone sends *Initiate Access Protocol* in the clear, and the reader runs one of two
paths:

- The **fast path** is symmetric. It works from a cached long-term key, `Kpersistent`,
  from an earlier transaction, and produces a 160-byte block of derived keys. The target
  key is URSK, at offset 128, 32 bytes long.
- The **standard path** is ECDH: AUTH0/AUTH1 with certificates, deriving a fresh 160-byte
  block (URSK still at offset 128) and caching a new `Kpersistent`. If the fast-path check
  fails, it falls through to the standard path.

Two properties here are load-bearing for the whole effort:

1. The URSK never goes out on the wire. Both sides derive it independently from the
   authentication, which cryptographically ties UWB ranging to a credential check that
   actually passed. It cannot be lifted from sniffed BLE traffic and injected or replayed.
2. After authentication, the reader sends an EXCHANGE command with a zero-length trigger
   (observed tagged `0x98`) that instructs the phone to load the URSK into its UWB chip.
   If the phone never sees that trigger, it responds with `URSK_Unavailable`, leaving
   tap-style behavior and no ranging. A build that authenticates cleanly but never emits
   `0x98` is the classic silent failure, and is worth learning to recognize on sight.

Glossary of the labels used: `Kpersistent` is the long-term symmetric seed, `BleSK`
guards the ranging control messages, `URSK` is the 32-byte ranging root above, and
`transaction_identifier` is a per-transaction value both sides feed into the crypto.

## 5. Time sync (using a BLE event to align the UWB clocks)

A battery-powered reader cannot leave its UWB receiver on continuously, so it must know
when the phone will transmit. The mechanism takes one BLE event both sides can observe and
maps it into each side's own UWB clock.

The reference instant is the anchor point of a BLE connection event: the on-air boundary
where the phone (central) transmits and the reader catches the first packet of the event,
so both radios observe the same moment. There are two procedures for selecting the event:

- **Procedure 0** happens at connection. The phone timestamps the very first connection
  event, sets `DeviceEventCount = 0`, and sends the time sync unprompted. This lands
  before any session key exists, so it is readable in the clear. It is the only time sync
  that can be snooped.
- **Procedure 1** is a resync. The reader initiates an `LE Set PHY` (`LL_PHY_REQ`), and
  the event carrying the phone's `LL_PHY_UPDATE_IND` becomes the reference. The reader
  uses this when it judges the phone's clock has drifted more than about 1 ms (for
  example, the last sync is older than ~150 s, or there has been no valid measurement
  within 10 s of it).

Each side reads its own UWB clock at that instant. The phone reports its value as
`UWB_Device_Time`; the reader keeps its own as `UWBVehicleTime` and never transmits it.
How each side maps the BLE anchor into its UWB clock is implementation-defined, which is
the difficult part covered in section 9.

The time sync message (phone to reader) carries: device event count (8 B), UWB device time
(8 B, µs resolution, 64-bit, scoped to the session and free to start at any value), UWB
device time uncertainty (1 B, log-encoded from 1 µs up to about an hour), a clock-skew
available flag, device max PPM (2 B), a success field (0/1/2), and a retry delay (2 B).

The math:

```
same event mapped both sides:  SyncOffset = UWB_Device_Time − UWBVehicleTime
different events:              SyncOffset = UWB_Device_Time
                                           + (VehicleEventCount − DeviceEventCount) × ConnectionInterval
                                           − UWBVehicleTime

reader opens block-i RX window at:
   local_listen(i) = UWB_Time0 + i × T_Block − SyncOffset            [µs, reader UWB clock]
                     ± ( 2^(Uncertainty/8) µs + skew·elapsed + reader mapping error )
```

`UWB_Time0` is in µs (the same 64-bit counter as `UWB_Device_Time`), and
`T_Block = N_RAN × 96 ms`. The phone's clock becomes undefined if the session suspends or
dies, or after 30 s with neither UWB nor BLE present. At least one successful time sync is
required before M1-M4. Once the first UWB packet lands, everything resyncs in-band far
tighter than BLE could manage. The BLE time sync only has to be good enough to place the
listen window in roughly the right spot, and the bar there is about 1 ms.

## 6. Ranging setup (M1-M4)

These four messages ride the BleSK channel as ranging service messages (message IDs:
M1=0, M2=1, M3=2, M4=3, suspend=4/5, resume=6/7). Either side can start the negotiation,
but M1 must not be sent while a session is already live.

| Msg | Dir | Carries |
|-----|-----|---------|
| **M1** | R→P | UWB config id(s), pulse shape combination, channel bitmask, **UWB Session Id** |
| **M2** | P→R | selected config/pulse/channel + SYNC code bitmask, **RAN Multiplier**, slot bitmask, hopping bitmask |
| **M3** | R→P | selected RAN Multiplier, **Chaps per Slot**, Number Responder Nodes, **Slots per Round** (≥ responders+4), SYNC code subset, hopping, **MAC Mode** (1 or 2 rounds/block + round offset) |
| **M4** | P→R | **STS Index0, UWB Time0**, Hop Mode Key, selected SYNC code index |

The key point: every negotiated parameter is load-bearing for the crypto, not just for the
radio config. This set is hashed into a value referred to here as the *SaltedHash*:

`ProtocolVersion ‖ ConfigId ‖ SessionId ‖ STS_Index0 ‖ ResponderNodes ‖ RAN_Multiplier ‖
SlotsPerRound ‖ ChapsPerSlot ‖ PulseShape`

and that hash feeds the ranging key KDF. So if the two sides disagree on any of it, they
derive different STS keys and never hear each other, even though the setup handshake
reported "success." That is the exact failure revisited in section 7 and section 10: the
negotiation "worked," and the radio is silent. (Aside: MAC mode set to 2 rounds per block
is the mechanism used to distinguish front-of-door from back-of-door.)

## 7. The UWB session

### Time grid (block → round → slot)

Time is carved into blocks, rounds, and slots:

```
RSTU    = 416 / 499.2 MHz ≈ 833.33 ns
T_Chap  = 1/3 ms = 400 RSTU
T_Slot  = N_Chap_per_Slot  × T_Chap                (Chaps-per-Slot from M3)
T_Round = N_Slot_per_Round × T_Slot                (Slots-per-Round from M3)
T_Block = N_RAN × 288 × T_Chap = N_RAN × 96 ms     (RAN Multiplier from M2/M3)
N_Round = T_Block / T_Round                        (derived)
Ranging rate = 10.416667 / N_RAN Hz

block_start(i)      = UWB_Time0 + i × T_Block
round_start(i,s)    = block_start(i) + s × T_Round
slot_start(i,s,n)   = round_start(i,s) + n × T_Slot
reader_local(…)     = (any of the above) − SyncOffset      [reader UWB clock]
```

Of the `N_Round` rounds in a block, only one or two are active (that is the MAC mode), and
which one is selected by the hopping sequence below.

### Who transmits in which slot (DS-TWR, one to many)

`N_Resp` is the number of responder nodes. The first pre-poll of a session goes out at
`UWB_Time0`.

| Slot | Packet | Format | Sender |
|---|---|---|---|
| 0 | Pre-POLL | SP0 (data) | phone: session id, **encrypted `Poll_STS_Index`**, block `i`, `Hop_Flag(i)`, `Round_Idx(i)` |
| 1 | POLL | SP3 (RFRAME) | phone |
| 2 … N_Resp+1 | Response_l | SP3 | one reader anchor each |
| N_Resp+2 | Final | SP3 | phone |
| N_Resp+3 | Final_Data | SP0 | phone: encrypted Poll/Final timestamps, `Hop_Flag(i+1)`, `Round_Idx(i+1)` |

So Slots-per-Round must be at least `N_Resp + 4`. The SP3 frames carry only a scrambled
timestamp sequence (the STS): no readable payload, and the distance falls out of RMARKER
timing across poll, response, and final. The SP0 frames are the opposite: plain data
(vendor OUI `0x4A191B`) wrapped in AES-CCM*. SP0 and SP3 are structurally different
packets, so rebuilding a single round means switching the radio between no-STS data RX/TX
and STS-only RFRAME RX/TX, and reloading the STS IV on every SP3 slot. One easy mistake:
the STS index increments on every slot whether or not it is used, so a skipped slot must
skip its IV too; never reuse it.

### Bench check: the phone honors `N_Resp = 2` (2026-07-17)

The table above is written for general `N_Resp`, but everything this repo ships runs at
`N_Resp = 1`. To find out whether the phone actually builds the larger round, a firmware
build advertised `N_Resp = 2` with only one physical anchor present: slot 2 (`Response_0`)
ours as usual, slot 3 (`Response_1`) reserved and deliberately silent.

The count has to change in two places at once, because both feed the `RangingConfiguration`
SaltedHash the phone independently recomputes: the M3 `Number Responder Nodes` attribute and
byte 12 of the `RangingConfiguration` blob in §6. If they disagree, every derived STS, dURSK
and dUDSK diverges and nothing decodes at all. `ultrawidelock_round_config.h` now defines both from
one macro (`ULTRAWIDELOCK_NUM_RESPONDERS`, default 1) so they cannot desync.

Two frames move when `N_Resp` goes from 1 to 2, and they arrive on independent receive paths:

| Frame | Slot | Offset from POLL | `N_Resp=1` | `N_Resp=2` | Measured |
|---|---|---|---|---|---|
| Final | `N_Resp+2` | `N_Resp+1` | 2 | 3 | **3** (40 rounds) |
| Final_Data | `N_Resp+3` | `N_Resp+2` | 3 | 4 | **4** (38 rounds, none at 3) |

```
FINAL result st=… cper=0 ip=… d=…(…us) slots=3 stsq=…/… idx=…    × 40   (× 4 missed)
FINALDATA-2RESP blk=… nresp=1 fd_slots=4                          × 38   (× 0 at 3)
```

Both offsets are differences of real RX timestamps, not printed constants. The Final result
is the stronger of the two because it fails cleanly rather than quietly: the receiver is
armed at POLL+3 carrying the STS for index+3, so had the phone kept a 1-responder round its
Final at POLL+2 would have been missed altogether instead of reported at a wrong offset. 40
hits at exactly 3 slots with the STS chip-error flag clear (`cper=0`) means the phone
scrambled that frame with the key for index+3, which it derives only from a 2-responder
schedule.

**What this establishes.** The phone accepts `N_Resp = 2`, keeps its SaltedHash key
derivation in step, and grows the round by exactly one slot. Reserving a slot for a second
anchor is a solved problem on the phone side.

**What it does not.** No second anchor ever transmitted, so a real `Response_1` in slot 3
(POLL+2, STS index+2) has never been accepted or turned into a range. `Final_Data` reported
`nresp=1` throughout, which is expected rather than a failure: the phone emits a timestamp
record only for a responder that actually replied.

Source: commits `9106aed` (the probe) and `2da85c4` (collapsed onto the
`ULTRAWIDELOCK_NUM_RESPONDERS` knob), PR #8. The raw serial capture was never committed.

### Bench check: a real `Response_1`, and what CCC v4 requires (2026-08-21)

The 2026-07-17 probe left one thing open: no second anchor had ever transmitted. On
2026-08-21 one did. A satellite (`examples/zephyr/satellite`, nRF5340 DK + DWM3000EVB)
holding the session URSK joined from the air alone, decoded Pre-POLL, POLL and Final at
`cper=0`, and transmitted `Response_1` in slot 3 every round with about 3 ms of arm margin
and no HPDWARN. The phone's `Final_Data` still reported `nresp=1`. A control run with the
LOCK's own proven radio rebuilt as `RESPONDER_INDEX=1` answered the same way: `tx123`
responses, zero records, session FAILED at the phase deadline.

**Our slot and STS arithmetic is spec-conformant, so it is not the cause.** Checked against
Table 20-2 (`ccc-v4.txt` l.53806-53874), the responder-slot formula (l.54244-54246) and the
STS increment list (l.55650-55668):

| Frame | CCC absolute | Relative to POLL | This tree | Site |
|---|---|---|---|---|
| `Response_l` STS | slot `2+l`, STS `+2+l` | `+1+l` | `widx + 1u + RESPONDER_INDEX` | `ccc_shim_rx.c:585` |
| `Response_l` RMARKER | slot `2+l` | `+1+l` slots | `poll_ip + RESP_SLOT + l*SLOT` | `ccc_shim_rx.c:1485` |
| Final | slot `N+2`, STS `+N+2` | `+N+1` | `FINAL_SLOT_OFFSET = N+1` | `cred_round_config.h:88` |
| Final_Data | slot `N+3` | `+N+2` | observed at POLL+4 for `N=2` | bench |

**What CCC actually requires of the report is narrower than "report every responder."** On
the `Final_Data` payload (l.54234-54252):

> "All the ranging measurement time stamps for all responders, up to N_Responder, whose
> valid ranging responses have been received at the initiator. The initiator may
> additionally add the time stamp data of responders where no valid response has been
> received."

Shall for validated responders, may for the rest. So `nresp=1` is compliant if and only if
the phone did not treat the slot-3 frame as a *valid* response, and the phone is never
obliged to pad. Nor is there any capability by which an initiator may declare itself
single-responder: the device's `Ranging_Capability_RS` carries only `Selected_UWB_Config_Id`
and `Selected_PulseShape_Combo` (l.44869-44913), and its `Ranging_Session_Setup_RS` carries
only `STS_Index0`, `UWB_Time0`, `HOP_Mode_Key`, `SYNC_Code_Index` (l.45361-45400).
`Number_Responder_Nodes` is set unilaterally by the vehicle in `Ranging_Session_Setup_RQ`
and cannot be negotiated down.

**There is no addressing to get wrong.** An SP3 RFRAME has no MAC header at all
(l.55249-55252):

> "the Auxiliary header is not relevant to SP3 packets [...] since SP3 packets do not get
> security processing due to the fact they do not carry MAC data frames (payload, MHR, and
> MFR)."

and identity is positional by design (l.55789-55792):

> "SP3 is intended for use cases where the participants in the secure ranging exchange are
> known to each other such that information about the source and/or the destination are
> implicit in the knowledge of what STS is used for transmission and reception."

So a responder is identified by *which slot it transmits in and which STS it uses*, and
nothing else. Both were verified conformant above. This kills the obvious benign
explanation -- there is no address field we could have failed to populate.

What the initiator is able to object to is enumerated in Table 20-7 (l.54536-54556):
`0x0` success, `0x1` transaction overflow ("RESPONSE SP3 frame from this responder cannot be
processed by the device"), `0x2` transaction expired ("No RESPONSE SP3 frame was received
from this responder"), `0x3` incorrect frame ("The RESPONSE SP3 frame received from this
responder was not correct"). Had the phone padded a record for responder 1, that byte would
have named the reason. It padded nothing, which the spec permits, so the silence carries no
diagnosis.

That leaves the conclusion the 2026-08-21 run reached, now on firmer ground: **the phone's
report path is single-responder in practice**, whatever the spec permits. The residual
alternative is only that it never armed a receiver in slot 3 -- which is the same statement
from the other side.

The discriminator still worth running: have the LOCK listen in slot 3 and verify the
satellite's `Response_1` STS using the session keys it already holds. A peer holding the same
keys validating that frame closes the last gap between "we transmitted something wrong" and
"the phone does not listen there."

**Roles, for the record** (l.52707-52716). The PHONE is initiator *and* controller; the lock
is the "responder-device"; each anchor is a "responder". The party that builds and transmits
the measurement report is therefore the phone, and a lock cannot compel what goes into it.

**Two clauses that matter for any two-anchor design.** First, silence in a slot is ordinary
(l.54128-54130):

> "If any of the responders does not receive the POLL message in the current ranging round
> from the initiator, that responder shall not transmit during its dedicated response slot."

No retry counter, no round abort; a skipped round is merely "mitigated by employing the
ranging round hopping strategy" (l.52862-52867). Second, per-round responder selection is
explicitly the responder-device's call (l.54484-54491):

> "It is up to the vehicle to determine which set of its responders it will involve in the
> ranging exchange in each ranging round."

Together those put block-parity alternation (second-anchor.md stage B') inside the
sanctioned model, where stage B's un-enrolled second responder sat outside it: l.52730-52736
makes coordinating "which logical responders transmit and in which order" the
responder-device's duty. Also noted there: one round is limited to 7 responders "due to the
maximum time stamp value for the Final message", with `N_Responder <= 10` fixed at setup.

Source: `ccc-v4.txt`, the CCC Digital Key Technical Specification v4.0.0 (CCC-TS-101), read
locally. Line numbers index that text extraction, not the PDF's pages. Quotes above were
re-read at those lines rather than copied from notes.

### The estimator's error budget: what can and cannot move a CCC distance (2026-08-21)

Block-parity alternation (`second-anchor.md` stage B') produced the first authenticated
DS-TWR distances a second anchor has ever computed from the phone's own round: 88 in one
run, min 46 mm, median 159 mm, mean 335 mm, max 1323 mm. Those figures are the peer
session's bench measurement, reported here rather than reproduced. What follows is the
arithmetic that constrains their explanation. Two of the constraints refute claims made in
this tree before they were checked; both refutations are marked below.

*Provenance, added after a later run.* Those four figures are contaminated and should not be
quoted as the CCC path's accuracy. Two causes, both identified on 2026-08-21: the run was
taken with the DK lacking clear line of sight at phone height, and some of the statistics
were read out of a session that was still in progress, so approach samples sat inside the
median. The arithmetic below was derived to explain that distribution and stands on its own
-- it is about what the estimator can and cannot do, not about those numbers -- but the
distribution it was aimed at turned out to be an artifact. The clean measurement is at the
end of this section.

**A constant cannot produce scatter.** A hard floor at 46 mm with a mean (335 mm) more than
twice the median (159 mm) is a right-skewed distribution, and no additive offset produces
skew: an offset slides a distribution and leaves its shape alone. So the run contains at
least two phenomena -- whatever sets the centre, and whatever makes the tail -- and any
single-cause explanation is wrong before its details are argued.

**The estimator is exactly invariant to the reply/round split.** Write the six timestamps as
`t1` poll TX, `t2` poll RX, `t3` response TX, `t4` response RX, `t5` final TX, `t6` final RX,
and the four intervals as `R1 = t4-t1`, `r1 = t3-t2`, `R2 = t6-t3`, `r2 = t5-t4`
(`ds_twr.h:34-37`). Now delay the responder's Response TX by `d`. The phone's Final TX is
scheduled off its own POLL, so `t5` does not move, and the four intervals go
`R1+d`, `r1+d`, `R2-d`, `r2-d`. The denominator is unchanged by inspection. The numerator
changes by `d[(R2-R1) + (r1-r2)]`, and because a true flight time `T` makes `R1 = r1+2T` and
`R2 = r2+2T`, that bracket is `(r2-r1) + (r1-r2) = 0`. The `d^2` terms cancel too, so the
result is exact rather than first-order:

    dtof/dd = 0, for every d and every reply asymmetry.

Checked numerically against `ds_twr_tof_signed()`'s own formula at `T` = 21, 213 and 640
ticks (0.1 m, 1 m, 3 m), at reply ratios 1:1, 1.66:1 and 41:1, and at `d` = 0, +-15872 and
+100000 ticks: the returned ToF is the input `T` in all 36 cases.

*Correction.* This session earlier told the peer session that the sensitivity was
`dtof = d * (r1-r2)/(r1+r2)`, and proposed logging the reply asymmetry as a diagnostic. That
came from transposing the two relations above (`R1 = r2+2T` instead of `R1 = r1+2T`). The
sensitivity is zero, so the diagnostic would measure nothing. The conclusion it was offered
in support of -- that `CCC_RESP_ANT_DLY_HI32` cannot be biasing our own distance -- survives,
and is now stronger than when it rested on a small-asymmetry argument.

**So `CCC_RESP_ANT_DLY_HI32` is exonerated by construction, and the code comment beside it is
right.** That constant (`ccc_shim_rx.c:657`, 62 hi32 = 15872 DTU ticks) moves the Response
RMARKER earlier so that a peer computing a *single-sided* ToF -- which assumes the responder
replied exactly one nominal slot after the POLL -- lands on the right answer. Its size is a
receipt for that reading: 15872 ticks x 4.6917 mm is 74.5 m of round trip, so 37.2 m
one-way, matching the "constant ~+37 m high" the comment records measuring. Our own DS-TWR
consumes the measured `t3`, not the scheduled one, so by the invariance above it does not
see the shift at all.

**Clock offset cannot explain the spread either.** Let the responder's clock run at `1+e`
relative to the initiator's, which scales the two intervals the responder measures (`r1`,
`R2`) and leaves the two the initiator reports (`R1`, `r2`) alone. Then

    tof = T * (2+2e)/(2+e) ~= T * (1 + e/2)

so the error is `T*e/2`: proportional to the distance, independent of reply asymmetry, and
tiny. At 1 m and 20 ppm it is 0.010 mm; at 3 m and 100 ppm, 0.150 mm; at 3 m and a
preposterous 1000 ppm, 1.50 mm. Numerics agree with the closed form to four decimals. This
immunity is the whole reason the asymmetric four-term estimator is the one CCC uses -- the
symmetric variant would need the reply times balanced, which a phone's schedule never
guarantees.

**Why the anchor bench needs a 19 m calibration and the CCC path does not.** The two paths
differ in one respect that fully accounts for it:

| | anchor bench | CCC responder |
|---|---|---|
| TX timestamps in the estimator | **predicted** `(dx << 8)`, `anchor_twr.c:409,547` | **measured** `dwt_readtxtimestamp()`, `ccc_shim_rx.c:1611,1855` |
| lumped correction | `- CONFIG_ANCHOR_ANT_DLY_DTU` = 4092 ticks = 19.2 m, `anchor_twr.c:636` | none |

The anchor initiator must put `t5` inside the FINAL it is still transmitting, so it writes a
predicted RMARKER; the prediction omits whatever the TX antenna-delay register contributes,
and nothing in this tree programs that register (`anchor_twr.c:377-382`). The omission is a
constant, and 4092 is that constant measured (2,229 samples, mean 1003.3 mm at a true 1000,
`examples/zephyr/anchor/Kconfig:56-73`). The CCC path reads its `t3` back from the chip after
the fact and the phone reports its own measured timestamps, so the same term is never
introduced and must not be subtracted.

*Correction.* This session earlier told the peer that the CCC path was "missing" the anchor
path's antenna-delay subtraction and that it was worth fixing. Applying 4092 there would
subtract 19.2 m from distances whose median is 159 mm. The empirical check refutes it
without any of the above: an uncompensated 19.2 m offset cannot coexist with a 159 mm
median. What remains genuinely uncompensated on the CCC path is the RF delay between the
chip's timestamp reference plane and the antenna, at both ends. The clean run below bounds
it: under 25 mm, and if anything negative rather than positive.

**The clean measurement: there is nothing to calibrate.** The experiment this section
originally proposed -- tape the phone at a known distance and read *medians only*, the median
being robust to the tail -- was run at a 2.0 m tape. Three completed sessions, stationary
samples only, read **1975 / 1989 / 1980 mm**, IQR 10-23 mm. That is a mean of 1981.3 mm, 18.7
mm short of the tape, or 4 ticks, or 0.93%. It is also tighter than the 20.5 mm sigma stage A
measured on the dedicated anchor bench, where both ends are our own boards. No fixed offset,
no scale error, and the 18.7 mm is smaller than the placement error of taping a phone to a
wall. The whole calibration thread is closed: the CCC responder path needs no correction term
at all, which is what the predicted-versus-measured argument above predicts.

**The tail was placement, not first-path physics.** This section previously argued that
leading-edge detection explained the right tail, on the grounds that first-path error is
one-sided positive and so produces exactly a hard floor with a long tail. The reasoning is
sound and the conclusion was wrong. The 28% of samples above 400 mm disappeared completely
once the DK had clear line of sight at phone height; later sessions span 136-272 mm with zero
outliers. The signature fit, but the cause was geometry. No CIR or `fp_index` investigation is
needed, and the run that would have "confirmed" it would have confirmed an artifact.

**A methodology rule came out of this, and it is the transferable part.** Never read a
session's statistics until a *later* session has started. Reading them in flight leaves
approach samples inside the median: one session read 1656 mm while running and 1975 mm from
its last-8 stationary samples once complete. That single mistake produced a ~320 mm apparent
offset that was briefly believed and is now retracted. Both this session and the peer session
published numbers derived from in-flight reads before the rule existed; treat any figure in
the repo dated before 2026-08-21 that came from a live session as suspect.

**Unresolved: occlusion sensitivity.** Late in the clean run the distance shifted tightly from
~1980 mm to 1576/1581 mm after a person sat down between the DK and the phone. This is *not*
recorded here as an occlusion penalty, because it is equally consistent with the phone simply
being 400 mm closer once they sat; the two explanations are confounded and the run cannot
separate them. It needs a controlled test -- same taped phone position, body moved in and out
of the line -- and it matters more than it looks, because a body between the phone and one of
two anchors is the *normal* case at a door, and stage B'' puts both anchors in the same block
on the assumption that what differs between them is geometry.

**Still unverified.** Whether two boards built at `NUM_RESPONDERS=1` / `RESPONDER_INDEX=0`
and sharing one session's keys derive byte-identical per-block and per-slot material has not
been checked in this tree. Block-parity alternation assumes they do, and the 88 authenticated
ranges are consistent with it, but consistency across one satellite is not a proof for two.

### STS index and the key ladder

```
STS_Index0 : random in [0 .. 2^30−1], pinned to block 0 / round 0 / slot 0
STS_Index(i, s) = STS_Index0 + (i·N_Round + s) · N_Slot_per_Round     s = Round_Idx(i)
   within a round:  Pre-POLL=base · POLL=base+1 · Response_l=base+2+l · Final=base+N_Resp+2 · Final_Data=base+N_Resp+3

URSK (32 B, from section 4)
 ├─ mUPSK1/mUPSK2   per session   → AES-CCM* ENC-MIC64 over the Pre-POLL payload (static all session)
 └─ mURSK           per session
     └─ URSK_KT     per active round   (keyed on that round's Pre-POLL STS index)
         ├─ dURSK   → generates the STS via the 802.15.4z DRBG
         └─ dUDSK   → encrypts the Final_Data timestamps
```

The finding that made responders tractable is *pre-poll recovery*. Because `mUPSK1` comes
straight off the URSK and stays static for the whole session, the responder can decrypt
the very first pre-poll with no ranging-round state set up yet. Decrypt it, read
`Poll_STS_Index`, subtract 1, derive `URSK_KT`, then `dURSK` and `dUDSK`, and the SP3 STS
is armed for the poll that lands one slot later. Between slot 0 and slot 1 that is about 4
AES-CMAC chains of work. This was the finding that mattered most: anchor every block on
the in-band pre-poll `Poll_STS_Index` rather than on the BLE time sync, and arm each slot
*before* the ~2 ms KDF/decrypt cost hits. That is the change that turned "setup is fine but
the radio is silent" into live distance reports.

### Hopping (which round is active)

Fully deterministic in the block index, so both sides can compute it with no radio
contact:

```
h_i = ( (((i + HOP_Key) & 0xFFFF)² mod 65521) × (N_Round − O_k) ) >> 16      (65521 = 2^16 − 15)
f_i = h_i + O_k                                                              (O_k = round offset, ≠ 0)
```

`HOP_Key` is the hop mode key from M4 (0 means no hopping). Block 0 is always unhopped, on
rounds 0 and `O_k`, and the sequence only takes over from block 1 onward. Test vector: with
`HOP_Key = 0xCC5DD79F` and `N_Round = 80`, blocks 1 and 2 land on rounds 62 and 37. Three
modes: none (fixed round), continuous (recompute `h_i` every block), and adaptive (stay put
while things work, hop on interference; the phone announces the next block's flag and round
inside Final_Data, and a missing Final_Data is assumed to be "hop"). Because the whole
thing is deterministic, losing a Final_Data costs nothing; both sides still land in the
same next round.

On the PHY side: HRP UWB high band, either channel 5 (6489.6 MHz) or channel 9
(7987.2 MHz), BPRF, with the preamble and pulse shape as negotiated in M2/M3/M4.

## 8. Session lifecycle and key lifetimes

Suspend and resume: either side can suspend to save power. In practice the phone usually
suspends while the door sits unlocked and resumes when the reader flips from unlocked back
to locked (or when the phone detects motion). A resume hands over a fresh `UWB_Time0` and
`STS_Index0`, giving a new grid without redoing M1-M4, and it only works if the reader
still holds a URSK that has not expired. A session paused while the door is unlocked is
normal, not a bug.

The URSK is dropped when any of these occur: the STS index reaches `2^31−1`, the STS index
is lost, a 12-hour TTL expires (counted from the first `dURSK` derivation at M4), or the
BLE link drops. After that, either side reports `URSK_Unavailable` and a fresh access
transaction is required, which is cheap because it can use the fast path. This is why
ranging reliably dies once the phone leaves BLE range, or after about 12 hours.

## 9. Open problem: pinning the reader's clock

This is the one genuinely implementation-defined seam from section 5: how the reader maps
the BLE connection-event anchor into its own UWB clock. On a split-core SoC (BLE
controller on the network core, host and UWB on the app core), this cannot be done in
software alone. The app core cannot read the controller's clock or the network-core timer,
and the ready-made radio-notification conversion path is fused off on this SoC family.

The approach adopted is a physical bridge rather than cross-core clock math:

```
controller "event-start task" ──▶ network-core GPIOTE pin (one edge / conn event) ──▶ DW3110 time-latch
BLE "anchor report" (event counter N, anchor_µs(N)) ──HCI/IPC──▶ app core
correlate by connection-event counter:  DW_time(edge_N) + L  ↔  anchor_µs(N)
```

`L` is a calibrated constant lead, covering radio ramp-up plus the latch offset. That pair
is exactly the `(UWBVehicleTime, anchor)` the section 5 formulas require, and because the
connection-event counter is a shared protocol counter, it also aligns with the phone's
`DeviceEventCount`. The DW3000 family provides `dwt_config_ostr_mode()` (one-shot timebase
reset over the sync pin) as the primitive that defines the epoch. Two items remain open:

- The anchor-report enable must live in the network-core image. Enabling it only from the
  app side is the prime suspect for a silent failure where the events never appear.
- The larger gap is obtaining the peer's time sync. The phone's `UWB_Device_Time` and
  device event count are consumed inside a closed protocol library and never surface. The
  procedure 0 time sync does arrive in the clear before any session keys exist, so it can
  be snooped on the receive path before the library consumes it; the later BleSK-encrypted
  syncs cannot. Without either a snoop hook or an upstream API addition, the reader has its
  `UWBVehicleTime` but nothing to subtract it from.

## 10. Field guide: observations and their explanations

| Observation | Reverse-engineered explanation |
|---|---|
| Phone never connects over BLE | Advert missing/wrong: `0xFFF2` absent, byte 7 bit 7 clear (no UWB build), or Dynamic Tag unresolvable (stale/changed reader identity). |
| BLE connects, tap-like unlock, no ranging | No common protocol version, or the `0x98` "URSK ready" trigger never sent → `URSK_Unavailable`. |
| M1 sent, phone answers "Setup Later" | Phone-side UWB busy/unavailable; wait for *Initiate Ranging Session* before resending M1. |
| M1-M4 complete, zero distance reports | Radio path: antenna, channel 5/9, missing time sync (wrong listen window), or a parameter mismatch → different SaltedHash → different STS (section 6). Watch for *Secure Ranging Over UWB Radio Failed*. |
| Ranging stops while unlocked, resumes at lock | Conformant suspend/resume for power saving (section 8), not a bug. |
| Everything dies after BLE drop / ~12 h | URSK lifetime rules (section 8); a fresh fast-path transaction is required. |

On instruments: a BLE sniffer alone yields everything through section 6 (discovery, the
versions, the message IDs, suspend/resume) with no UWB gear. The clear-text half of that is
decoded by a Wireshark dissector, which is no longer in this repository: the
`0xFFF2` advert of this section and the Procedure-0 time sync of
section 5. Resolving section 7 needs a
UWB capture, or the radio's own per-frame diagnostics, which is what separates "setup
negotiated but the STS is mismatched" from "STS lines up but there is no RF link." The
single most useful question when distances vanish: is BLE still trading setup and ranging
control messages while the UWB side has gone silent? If so, the problem is in the
radio/parameter/STS path, not the control stack.

## 11. Aliro 1.0: what it inherits, and the two-round block

Aliro v1.0 (dated 2026-02-18) is the access-control profile of this same machinery. It cites
CCC Digital Key **4.0.0 specifically** (`aliro-1.0.txt` l.750) and delegates nearly
everything: the MAC layer to CCC §20 (l.9555-9557), the ranging exchange sequence to CCC
§20.5 (l.9596-9597), the PHY to CCC §21 (l.10011-10012), and the key ladder and STS to CCC
§22.1/22.2 (l.10015-10016). Roles are preserved exactly: User Device is Initiator, Reader is
Responder-device (Figure 12-2, l.9762-9767).

What it actually changes:

| Area | Aliro |
|---|---|
| Vendor OUI in the SP0 MHR | `0x4A191B`, the CSA identifier (l.10006-10007) |
| Hopping | CCC's AES sequence is FORBIDDEN; only Aliro §17's default applies (l.7527-7528) |
| URSK root | not CCC's ladder -- offset 128 of `derived_keys_fast` / `_volatile` (l.3011-3013, 3055-3057) |
| URSK lifetime | 12 h TTL, and discarded when the BLE link drops (l.6272-6285) |
| Setup | four messages M1-M4, not CCC's two RQ/RS pairs (l.9748-10003) |
| UWB session id | low four octets of the Transaction Identifier (l.7509-7511) |
| New attribute | MAC Mode, attribute ID 15 (l.7589-7600) |

`Number_Responder_Nodes` is left alone: Aliro states no numeric constraint, so CCC's 1-255
range and 10-per-round cap govern. Its one arithmetic addition,
`N_Slot_per_Round >= N_Responder + 4` (l.9955-9959), independently corroborates the slot
layout above -- four overhead slots plus one per responder puts `Response_l` at slot `2+l`.

### The two-round block -- Aliro's own answer to inside/outside

This is the single architectural idea Aliro adds, and it addresses precisely the question
this repo exists to answer (§12.1.1, l.9571-9582):

> "One or two ranging rounds out of all the ranging rounds per ranging block of a ranging
> session are used for the UWB ranging procedure. [...] **Two ranging rounds per ranging
> block enable 'in front of' and 'behind the Reader' detection by the Reader.** [...] A
> Reader MAY optionally support two ranging rounds per ranging block while **a User Device
> SHALL support two ranging rounds per ranging block.** [...] Additionally, the
> responder-device SHALL be responsible for mapping responders (at the responder-device) to
> the appropriate ranging rounds."

Mechanics: the two rounds sit at a session-constant non-zero offset `O^k` in rounds, chosen
by the Reader at setup and signalled in the MAC Mode attribute. They run two hopping
sequences related by `f_i = h_i + O^k` (l.9585-9591). `N_Responder` is per round, so a
two-round block ranges up to `2N` nodes. Round indices start at `Round_Idx_1 = 0`,
`Round_Idx_2 = O^k`.

Three properties make this stronger than anything on the multi-responder path:

1. **Phone support is mandatory.** "A User Device SHALL support two ranging rounds per
   ranging block." By contrast `Number_Responder_Nodes = 2` only ever obliged the phone to
   reserve a slot, and the 2026-08-21 bench showed it will not report the occupant.
2. **Both anchors measure inside the same block**, separated by `O^k x T_Round` rather than
   a whole 192 ms block. `ultrawidelock_fusion_eval` demands a same-round pair; this gets far
   closer to one than block-parity alternation can.
3. **Neither anchor loses rate.** Alternation halves each anchor's sample rate; two rounds
   per block does not.

**Each of the two rounds is a complete exchange.** Figure 12-1 (p.157 of the PDF; the text
extraction drops it) draws the round as `PP | P | R1 | R2 | R3 | F | FD` and the block as six
ranging rounds with two of them shaded, their positions moving block to block under the two
hopping sequences. So a two-round block is not one exchange with extra responder slots -- it
is *two* full exchanges, each with its own Pre-POLL, POLL, responder slots, Final and
Final_Data. That is why §12.1.2 subscripts everything by `p` (`Hop_Flag_p`, `Round_Idx_p`,
`p = 1` and `p = 2`) and why l.9709-9736 speaks of Final_Data1 and Final_Data2.

The consequence for a second anchor is the whole argument for this route. In round 2 the
satellite is **responder 0 of its own round**, and the phone sends it an ordinary
single-responder `Final_Data` containing its record. It never depends on the phone reporting
a *second* responder in one round -- which is precisely the behaviour that failed on
2026-08-21. The route sidesteps the observed limitation instead of arguing with it.

The specific hazard (l.9709-9736): if *either* Final_Data goes unheard, the responder-device
unconditionally assumes a hop and sets both `Hop_Flag` to 1. Two physical units must
therefore share Final_Data reception state, not merely the URSK, or they will compute
different next-round indices and desynchronise.

Not addressed by Aliro at all: AoA and antennas (zero hits), any distance threshold or
proximity policy (`distance`, zero hits), relay attacks (zero hits). How a Reader turns a
range into an access decision is out of scope. The only key-sharing language is
non-normative §16.2.6 (l.11545-11551), which contemplates a URSK travelling "to an external
UWB chip" over a protected channel, and does not require that chip to sit in the same
enclosure.

Source: `aliro-1.0.txt`, Aliro Specification v1.0. Line numbers index that text extraction.

## Credits

- [@kormax](https://github.com/kormax/) for ideas on ECP and UWB.
- [@rednblkx](https://github.com/rednblkx/) for ideas on HomeKey.
- [@scottjg](https://github.com/scottjg/) for help with UWB chipset ideas.

## Questions

Open an issue on the repository for corrections or questions about this research.
