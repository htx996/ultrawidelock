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
make build CLIENT=1          # bench: the client at DBG, no DFU receiver
make flash
make monitor
```

`CLIENT=1` is the part that matters. Without it none of the client code is
compiled and the lock behaves exactly as it does today.

Two profiles carry the client, and for bringup you want the first:

| build | client log level | fits by | what it is for |
|---|---|---|---|
| `make build CLIENT=1` | DBG | 1,929 B | the bench. Reads back why a bound lock did or did not open. |
| `make build CLIENT=1 RELEASE=1 SMP=1` | ERR (global level 1) | 8,288 B | what ships. mcumgr, DFU, signed. |

The debug profile only fits because `overlay-client-debug.conf` applies
automatically to `CLIENT=1` without `RELEASE=1`: it silences the credential,
DFU and radio log modules and drops the DFU receiver, which a board on a desk
does not need. Read that file before adding to it -- it says which log symbols
exist, and setting one that does not aborts the CMake configure.

Neither profile has much room. If either stops linking or stops signing, that
is the size gate doing its job, not a broken tree: run `make cdk-size` and read
the `<- the one that ships` line, which is the only ceiling that counts.

For a signed image, `make release RELEASE_KEY=<path> CLIENT=1`.

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

## What is most likely to be wrong

Ranked. Both of the deviations this list used to lead with have since been
settled: one was checked against CHIP's own source and is not a blocker, the
other has been implemented. What remains is thinner than it was, which is the
point of having looked.

### 1. Nothing here has ever met a real peer

The honest top entry. Everything this node does from Stage 3 onwards has been
exercised only against a host double, and the CASE initiator has never had a
cryptographically valid Sigma2 put in front of it by another implementation. The parts that are tested
are tested well; they have simply never been wrong in company.

**Signature:** anything. This is the entry that says the first run is an
experiment, not a verification.

### 2. An outstanding DNS-SD query blocks the next attempt

`matter_thread_resolve()` refuses a second query while one is outstanding, and
nothing in the client can cancel one. So an attempt that times out after
`MATTER_CLIENT_STEP_MS` can leave a query behind that stops the NEXT attempt
from even starting. How long that lasts is OpenThread's business, not this
node's, which is why it cannot be pinned down off hardware.

**Signature:** the first walk-up produces `resolving`, and a second walk-up a
few seconds later produces nothing at all -- no log line, no datagram. It comes
back on its own once the query completes.

**Covered by a test** (`a query still outstanding blocks the next attempt`), so
the behaviour is known and bounded rather than surprising; what is unknown is
the duration on a real mesh.

### 3. The Sigma1 source node id -- checked, and NOT a blocker

This node puts its **operational** node id in the message header's source field
rather than a random ephemeral one, which is a real deviation from what CHIP
does. It was ranked first here until it was checked against CHIP's source, and
it does not survive that check as a failure mode:

- CHIP generates its ephemeral initiator node id as a random 64-bit value
  **constrained to the operational node id range**
  (`SessionManager::CreateUnauthenticatedSession`), so the value this node sends
  is indistinguishable in form from the value CHIP sends.
- The responder uses it as an opaque key to find or allocate an unauthenticated
  session (`SessionManager::OnMessageReceived` -> `FindOrAllocateResponder`) and
  validates nothing about it beyond its presence.

So a CHIP-based peer will not reject a Sigma1 over this. What the deviation
does cost is **privacy**: the value is stable rather than per-session, so any
passive Thread observer can link every handshake this node makes to one
identity. Worth fixing eventually; not worth suspecting on the bench.

### 4. Retransmission -- now implemented for the handshake

A dropped Sigma1 or Sigma3 is resent on an MRP timer rather than costing the
whole `MATTER_CLIENT_STEP_MS`. The first resend lands at roughly four times
`MATTER_MRP_IDLE_INTERVAL_MS`, because the deadline carries MRP's margin and
backoff multipliers.

The **interaction** past the session is still not covered: those messages are
sealed by `matter_exchange`, whose counters `matter_client.c` does not own.

**Signature of the remaining gap:** intermittent failure after
`CASE ESTABLISHED`, correlating with mesh quality, where the retry restarts
from `resolving` rather than resuming the invoke.

## Faults already fixed, and their signatures

Listed so that a capture showing one of these is read as a regression rather
than diagnosed from scratch. The first two were found by reading the driver;
the last three by putting it under test, which is the argument for having done
so -- none of them were reachable from a test of the modules underneath.

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

It is tested for liveness now -- a fresh lookup by index, the slot still
committed, AND the fabric id unchanged -- and an attempt whose administrator has
gone is dropped rather than signed with a zeroed key. The fabric id matters
because a slot is an array position: an administrator removed and another
commissioned into the same position gives back the very same pointer with the
same index, describing somebody else. The check is also made on the inbound
path, which runs from the receive callback and therefore ahead of the poll that
would otherwise notice.

**Signature if it returns:** only after removing an administrator without
rebooting. A Sigma1 refused by everything, with the log naming a fabric that is
no longer there. In the reused-slot form: an unlock sent on behalf of an
administrator that was removed, which is the worst outcome on this page.

### A handshake the schedule had given up on, still holding its exchange

`matter_client_sm_poll()` leaves the Sigma1 state on its own deadline and says
nothing to anybody, because it has no clock and no opinion about what its caller
is holding. Nothing cleared the client's handshake flag, so an abandoned attempt
kept its ephemeral private key and its transcript in RAM indefinitely, kept its
exchange id claimed against every inbound unsecured message, and would open a
Sigma2 that arrived long afterwards as though somebody were still waiting.

**Signature if it returns:** `CASE ESTABLISHED` appearing with no walk-up behind
it, minutes after a failed attempt. Also a Sigma1 addressed to THIS node being
silently dropped, because the client is still claiming an exchange id it should
have released.

### The retransmit timer dropped by any inbound message

Introduced and caught in the same sitting, and worth recording because the shape
recurs: the retransmission deadline was folded into the timer in the poll only,
while two other paths re-arm the same timer when a datagram arrives. Any inbound
message that did not acknowledge the outstanding one therefore re-armed from the
schedule alone and silently cancelled the pending resend.

**Signature if it returns:** resends that happen when the peer is silent and
stop the moment it says anything at all.

### `matter_client_init()` that did not initialise

Init set up the lock and the pointers and left the session, handshake and
handshake-linger state exactly as the previous run had them. Harmless on target,
where it runs once, and fatal to any attempt to reason about the file's starting
state.

## What to capture

`make monitor` from the first walk-up through two full retries. Two, because
the backoff doubling is visible in the second one and confirms the state machine
is running rather than wedged.

From the peer, whatever it prints on datagram receipt. If the ESP32 shows
nothing at all, the problem is Thread routing and neither Matter implementation
is involved yet.

Record the result in `docs/hardware-validation.md` as a new row when it passes.
Until then this document is the record.
