# Range integrity: what a signed distance is worth

A presence assertion exists to be believed by someone who was not in the room.
Its whole content is a distance, so the question is not "what did the radio
report" but "why should a third party believe that number was measured rather
than chosen".

## The layers

`modules/ultrawidelock_uwb/include/fira_session.h` defines four, numbered with a gap.

| Layer | Check | Status |
|---|---|---|
| 1 | Plausibility band: below `-30 cm` is physically impossible, above 30 m is outside any proximity envelope | Always enforced |
| 2 | STS quality: the scrambled timestamp sequence correlated well enough to trust the timestamp | Recorded always; enforced for presence, optional for the lock |
| 3 | Ipatov first-path check | **Removed** — untunable on this hardware |
| 4 | Cross-block consensus: K consecutive blocks agreeing within 50 cm | Enforced; K=3 shipping, K=2 only under `ULTRAWIDELOCK_BENCH` |

Layer 2 is the one that matters against a distance-reduction attack: a spoofed
early first path cannot reproduce the scrambled sequence, so its STS quality
collapses. Layers 1 and 4 are statistical filters, raising the cost of an attack
without making it a cryptographic impossibility.

## What was wrong

`CONFIG_ULTRAWIDELOCK_RANGE_GATE_STRICT` gated layer 2 before it could reach the range
store. It defaulted to `n` ("shadow mode": log the verdict, latch the block
anyway), and the symbol was declared only in the Zephyr Kconfig tree. On ESP32
it was not merely off, it was **unreachable**, so `idf.py` could not produce it
and the `#if` in `ccc_shim_rx.c` could never be true.

That is the platform that signs presence assertions. Every frame the bench run
produced carried a distance that had passed plausibility and consensus but no
integrity check, and no field in which to say so.

## What changed

1. **The evidence travels with the range.** `fira_session_set_ccc_range_sts()`
   publishes the per-block verdict; the store accumulates it and
   `fira_session_last_range_integrity()` reports it. A latch with no evidence
   reads as a failed STS, never a passed one.
2. **The window matches layer 4.** The verdict is "the last K blocks all
   correlated", counted rather than AND-ed over the whole run, so one marginal
   block cannot poison a stationary phone indefinitely — it heals after K clean
   blocks.
3. **Presence fails closed; the lock does not.** The two consumers want
   opposite failure modes. A mis-tuned floor that refuses to open a door locks a
   human out of their house. A mis-tuned floor that refuses to sign an assertion
   costs one retry. Presence therefore requires a good STS regardless of the
   build flag; the door keeps shadow behaviour unless
   `CONFIG_ULTRAWIDELOCK_RANGE_GATE_STRICT` is set, which is now selectable on ESP32 too.

   **The DWM3001CDK now ships with that flag set** (`apps/dwm3001cdk-lock/prj.conf`),
   so on that board a block failing layer 2 is dropped rather than latched. It is
   the only board that does. The threshold it enforces is still the untuned
   default below, so today it rejects only what the driver itself calls bad; a
   real floor is still the release gate. First hardware evidence that the gate
   passes a normal walk-up: `sts_ok=1` at STS index 62, verdict 24, d=107 mm.
   That is one walk-up in line of sight, not a calibration.
4. **The evidence is signed.** Wire version 3 carries `range_flags`,
   `sts_quality` and `trust_level` inside the signed prefix. A verifier checks
   the claim instead of assuming the producer ran the check, and a frame that
   does not claim a good STS is rejected (`E_INTEGRITY`). v2 frames are refused
   rather than reinterpreted.
5. **Policy can tighten without reflashing.** `presence_verify.py
   --min-sts-quality N` demands better correlation than the firmware was built
   with.

## Still outstanding

**The STS-quality floor is 0**, which means "defer to the driver verdict". It is
now a build option, `CONFIG_ULTRAWIDELOCK_STS_QUALITY_MIN`, feeding
`FIRA_STS_QUALITY_MIN` (`modules/ultrawidelock_uwb/include/fira_session.h`), so
it can be raised without touching source -- but it has still never been sized
from real captures, and the Kconfig help has said to do that since before this
change. The flight recorder already logs `stsq_val` and `stsq_ret`
(`flight_recorder.h`), so the capture path exists: record a walk-up, histogram
the quality index, pick a floor above the noise. Until that happens, layer 2
rejects only what the DW3000 driver itself calls bad. This matters more now that
the DWM3001CDK enforces rather than shadows: an untuned floor that is enforced is
a door that can refuse to open.

**Relay resistance is still not measured.** The time-of-flight argument is
structural and depends on enforcing STS, but no one has attacked this stack.
Do not describe it as tested. Read the literature on distance-reduction attacks
against HRP UWB with STS first.

**A compromised producer can still assert anything.** The evidence is signed by
the same key that signs the distance, so it proves the frame came from an
enrolled device, not that the device was honest. This is the same limit the
identity-clone attack demonstrates.
