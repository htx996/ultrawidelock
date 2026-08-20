# Satellite responder (stage B)

The second UWB anchor of docs/second-anchor.md: the CRED-tier CCC ranging
engine, minus BLE credential auth, plus key injection. It joins the phone's
own ranging session as responder 1 in the slot the phone reserves when the
lock advertises a 2-responder round, and measures its own DS-TWR distance to
the phone. Developed on the nRF5340 DK + DWM3000EVB; lands on a second
DWM3001CDK later by changing `SAT_BOARD` (stage E).

## Build and flash

    make sat-build
    make sat-flash CDK_PROBE=<VID:PID:Serial of the DK probe>

## Use

1. Flash the lock with the stage-B bench image
   (`CDK_CONF="overlay-thread.conf;overlays/bench-2resp.conf;overlay-lto.conf"`).
   Both images must agree on `ULTRAWIDELOCK_NUM_RESPONDERS` — it is baked into the
   session's key derivation.
2. Open the satellite's shell on the DK's UART VCOM.
3. Walk up with the phone. The lock prints a `SAT-HANDOFF: sat join …` line at
   credential start; paste it into the satellite shell. Pre-poll recovery
   means a late paste costs one ranging block, not the session.
4. The satellite prints `SAT range <n> cm` per latched range.

Stage B ran 2026-08-21 and its pass criterion is now known unreachable: the
satellite joins, decodes and transmits correctly, but a stock iPhone reports
`nresp=1` no matter who answers in slot 3. See docs/second-anchor.md stage B
for the receipts and stage B' for where the app goes next (same engine, same
key handoff, alternating `Response_0` by block parity instead of a second
slot).

The shell handoff is first light only; the sealed lock→satellite link over
Thread (witness_link pattern) replaces it in stage C.
