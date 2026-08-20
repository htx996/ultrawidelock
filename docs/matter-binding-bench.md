# Bringing the binding up on hardware

The client half of `docs/matter-binding.md` has never exchanged a packet with a
real device. Every gate it passes is the code agreeing with itself: the host
suites prove the encoders, the key schedule and the exchange choreography are
internally consistent, and a loopback test runs this node's initiator against
this node's own responder. None of that is evidence that a Nuki, an Aqara or an
ESP32 accepts what goes out on the air.

This is the procedure for finding out. It is written to be run once, badly, and
to produce a diagnosis rather than a verdict.

**Expect it to fail the first time.** The value of the run is which quarter of
the path it stops in, not whether a door opens.

## What you need to physically have

| | | Why |
|---|---|---|
| 1x DWM3001CDK | the initiator | the thing under test |
| 1x peer lock | any Matter DoorLock server | a Nuki, an Aqara U200, or `apps/esp32-matter-lock` |
| 1x Thread Border Router | an Apple TV, a HomePod, or an OTBR | **mandatory.** This node runs no SRP server, so nothing resolves without one |
| a second controller | Home Assistant, or `chip-tool` | Apple Home alone cannot do this |

No nRF5340DK is involved. Use the in-repo ESP32 peer before a commercial lock:
a Nuki gives you a silent drop and no way to tell a rejection from a lost
packet, while the ESP32 can be instrumented at both ends at once, which is the
entire point of a first run.

## Stage 0: build and flash

```
make build CLIENT=1 RELEASE=1 SMP=1
make flash
make monitor
```

`CLIENT=1` is the part that matters. Without it none of the client code is
compiled and the lock behaves exactly as it does today. For a signed image,
`make release RELEASE_KEY=<path> CLIENT=1`.

Leave `make monitor` running for the whole session. The RTT log is the only
diagnostic there is.

## Stage 1: get both devices onto one fabric

This is the stage that eats the afternoon, and the one most likely to defeat you
before you reach any of the client code.

1. Commission the CDK into Apple Home as normal.
2. Commission the peer lock into the same home.
3. Add **Home Assistant as a second administrator** to both, through Apple
   Home's "Turn On Pairing Mode".

Step 3 is the historically fragile one. If it fails, check in this order:

- Is the firmware newer than 2026-08-07? Older builds cannot be added to any
  non-Apple controller at all, and fail exactly as "pairing failed". See
  `docs/troubleshooting.md`.
- Are both devices on **one** Thread network rather than two? This is the most
  common cause of a lock that pairs and then goes missing.
- Is the border router actually reachable?

Home Assistant is needed specifically because **Apple Home will not write a
binding**. It is used once, at setup. It is not in the unlock path afterwards
and can be switched off, rebooted or thrown away without the door noticing.

## Stage 2: prove the peer before involving this node

**Do not skip this.** Debugging two unknowns at once is how a day disappears.

1. From Home Assistant or `chip-tool`, invoke `UnlockDoor` on the peer lock
   directly. If the door does not open, stop: nothing after this point would
   mean anything.
2. Note the CDK's **operational node id on the Home Assistant fabric**. It is
   needed in the next stage and it is the value most often got wrong.

When this passes, every later failure belongs to us. That is what makes the
capture worth taking.

## Stage 3: write the two attributes

1. **On the peer lock:** an ACL entry granting the CDK's node id `Operate`
   privilege on the DoorLock cluster.
2. **On the CDK:** the Binding attribute, naming the peer's node id, its
   endpoint (usually 1), and the DoorLock cluster.
3. **If the peer wants a PIN:** write it to the vendor PIN attribute on the CDK.
   It is write-only and never reads back.

`scripts/bind-helper.py` does both ends through `chip-tool`; the exact commands
are in `docs/matter-binding.md`. Read the binding back before continuing, to
confirm it stuck.

## Stage 4: the run

Walk up with an enrolled phone. **Once.** A second attempt overlaps the first
one's backoff and makes the log ambiguous.

The CDK prints a line at each step it completes, so where the log stops is the
diagnosis:

| Last line you see | It got as far as | Look at |
|---|---|---|
| nothing | the gate never fired | the UWB grant, not Matter. `matter_client_want()` is called only on a granted unlock |
| `resolving <instance>` | DNS-SD query sent, no answer | the border router, the peer's SRP registration, and whether both are on ONE Thread network |
| `Sigma1 out to node ...` | the handshake started, no Sigma2 came back | **the most likely stop.** See below |
| `Sigma2 REJECTED (-6)` | the peer does not hold this fabric's keys | the binding names the wrong node, or the peer is on another fabric |
| `Sigma2 REJECTED (-9)` | right fabric, wrong identity | the binding's node id |
| `Sigma3 out: session ...` | we accepted the peer and answered | the peer is verifying our signature. No StatusReport means it refused |
| `CASE ESTABLISHED as initiator` | the handshake is done | the invoke, below |
| `the bound lock refused the timed window` | the peer would not open a timed window | rare; a peer that does this refuses the invoke too |
| `UnlockDoor out: endpoint n` | the command is on the air | the peer's ACL. `UNSUPPORTED_ACCESS` is the expected answer to a missing entry |
| `the bound lock UNLOCKED` | it worked | stop reading |

## The two things most likely to be wrong

Ranked, and both are deliberate deviations rather than oversights. Two further
faults were found by reading the driver against this list and have since been
fixed; they are described at the end so that a capture showing their signature
is recognised rather than re-diagnosed.

### 1. The Sigma1 source node id

This node puts its **operational** node id in the message header's source field
rather than a random ephemeral one. That is deliberate and documented in
`matter_client.c`, and it is the single place the implementation knowingly
differs from what CHIP does.

**Signature:** `Sigma1 out` repeating on the backoff schedule with no `Sigma2`
ever. A peer that validates the source against its fabric table before checking
the destination identifier drops it without answering, so it looks exactly like
the packet never arrived.

**How to tell it apart from a lost packet:** the peer's console. If the ESP32
logs a received datagram and no Sigma2, it is a rejection. If it logs nothing,
it is routing.

### 2. No retransmit on the outbound path

MRP is not wired on anything this node originates. A dropped Sigma1 or
TimedRequest is not resent; the whole attempt times out after
`MATTER_CLIENT_STEP_MS` and starts again.

**Signature:** intermittent success that correlates with mesh quality, and
retries that always restart from `resolving` rather than resuming.

## Two faults already fixed, and their signatures

Listed so that a capture showing one of these is read as a regression rather
than diagnosed from scratch.

### A retransmitted StatusReport with nowhere to go

The peer sets R on the StatusReport that ends CASE and retransmits until
acknowledged. This node acknowledges once and never repeats it, so a lost
acknowledgement used to leave the retransmission unroutable: the handshake flag
had already cleared, and a Secure Channel message that is neither Sigma1 nor
Sigma3 falls into the unsecured drop.

The handshake exchange now lingers for `CLIENT_HS_LINGER_MS` after it succeeds
and answers a repeat with another acknowledgement and no change of state.

**Signature if it returns:** `CASE ESTABLISHED` here, the peer still
retransmitting, and the invoke failing against a session the peer is tearing
down. Intermittent, so it presents as "works sometimes".

### A fabric pointer outliving its fabric

`s_fabric` points into the fabric table, and a RemoveFabric that zeroes the slot
left it addressing valid memory describing nothing. The Sigma1 path tested it
for NULL, which a cleared slot is not.

It is tested for liveness now, by comparing the pointer against a fresh lookup
by index, and an attempt whose administrator has gone is dropped rather than
signed with a zeroed key.

**Signature if it returns:** only after removing an administrator without
rebooting. A Sigma1 refused by everything, with the log naming a fabric that is
no longer there.

## What to capture

`make monitor` from the first walk-up through two full retries. Two, because
the backoff doubling is visible in the second one and confirms the state machine
is running rather than wedged.

From the peer, whatever it prints on datagram receipt. If the ESP32 shows
nothing at all, the problem is Thread routing and neither Matter implementation
is involved yet.

Record the result in `docs/hardware-validation.md` as a new row when it passes.
Until then this document is the record.
