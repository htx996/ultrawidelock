# Satellite — the second UWB anchor

One half of the two-anchor inside/outside product; the DWM3001CDK lock
(`apps/dwm3001cdk-lock`, built with `make anchorlink`) is the other. This board
runs the CRED-tier CCC ranging engine without BLE credential auth, joins the
phone's own ranging session as responder 1 in the slot the phone reserves when
the lock advertises a 2-responder round, and measures its own DS-TWR distance
to the phone.

Two distances to one phone from the same ranging block is what makes a side
decision possible: the lock pairs its own measurement with this one and asks
which side of the anchor pair's frontier the phone is on. Design and receipts
live in `docs/second-anchor.md`.

Developed on the nRF5340 DK + DWM3000EVB. `SAT_BOARD=decawave_dwm3001cdk`
builds this same image for a second DWM3001CDK (stage E): 272,160 B flash
(52.73%) and 108,032 B RAM (82.42%) with Thread, recorded in
`size-baseline-decawave_dwm3001cdk-thread.json` beside this file. It has not
been flown -- the second board is not on the bench yet.

## The link to the lock

Ranges reach the lock as **sealed datagrams over Thread UDP** — AES-CCM under a
shared 16-byte key, 13-byte nonce, 8-byte tag, on
`CONFIG_ULTRAWIDELOCK_WITNESS_PORT`. `src/anchor_link.c` holds it and is
compiled only under `CONFIG_OPENTHREAD`, which `overlay-thread.conf` supplies
and `mk/satellite.mk` layers only for `SAT_THREAD=1`.

There is no cable in the steady state, and no per-session step: the sealed link
carries the session handoff itself. USB is used once, at install, to type the
key and the Thread dataset in.

## Build and flash

    make sat-build SAT_THREAD=1
    make sat-flash

`SAT_THREAD` is opt-in rather than default so the ranging arm margin measured
without OpenThread on this core stays buildable as the baseline to judge the
Thread build against.

## Provision, once per board

Open the DK's UART VCOM (`make sat-term`) and type:

    sat key <32 hex chars>       # the same bytes as the lock's `ultrawidelock anckey`
    sat dataset <tlv hex>        # the lock's Thread dataset
    sat link                     # sealed-link status

The dataset comes off the lock: build it with
`overlays/thread-dataset-dump.conf` layered on and read the hex between the
`BEGIN/END THREAD DATASET` markers.

## Watch it

    make sat-monitor        # RTT, survives a dead shell thread
    make nrf-monitor-rtt    # RTT with a log copy under build/
    make sat-term           # UART: logs plus a shell you can type into

`SAT range <n> cm` per latched range.

## Shell

| command | what it does |
|---|---|
| `sat join <ursk> <rcfg> <channel> <sync-code>` | first-light manual handoff; the sealed link replaces it |
| `sat key <hex32>` | the sealed-link key |
| `sat dataset <tlv-hex>` | join the lock's Thread network |
| `sat link` | sealed-link status |
| `sat stop` | stop ranging, quiesce the radio |

## Known limit

A stock iPhone reports `nresp=1` no matter who answers in slot 3, which is why
the stage-B pass criterion is unreachable as written; the satellite itself
joins, decodes and transmits correctly. `docs/second-anchor.md` stage B has the
receipts and stage B' has where this goes next.
