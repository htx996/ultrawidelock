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

## What you need

| | |
|---|---|
| the initiator | a DWM3001CDK flashed with `make release RELEASE_KEY=<path> CLIENT=1`, or `make build CLIENT=1 RELEASE=1 SMP=1` for a bench image |
| the peer | anything that is a Matter DoorLock server on the same fabric. `apps/esp32-matter-lock` is the one to start with |
| a border router | an Apple TV or HomePod is enough. Required: this node runs no SRP server, so nothing resolves without one |
| two consoles | `make monitor` on the CDK, and the peer's own serial |

Use the in-repo ESP32 peer before a commercial lock. A Nuki gives you a silent
drop and no way to tell a rejection from a lost packet. The ESP32 can be
instrumented on both ends at once, which is the entire point of a first run.

## Prove the peer before involving this node

Do not debug two unknowns at once. Before the CDK sends anything:

1. Commission the peer onto the fabric.
2. Have a known-good administrator invoke `UnlockDoor` on it. `chip-tool
   doorlock unlock-door $PEER 1`, or Home Assistant, or Apple Home.
3. Write the ACL entry granting **this node's** operational node id `Operate` on
   the DoorLock cluster, and confirm the same invoke still works.

If step 2 or 3 fails, nothing after this point means anything. When they pass,
every later failure belongs to us, which is what makes the capture worth taking.

Then set up the binding itself per `docs/matter-binding.md`, and confirm it
reads back before walking up to the lock.

## The run

Start both consoles, then trigger one walk-up. One. A second attempt overlaps
the first one's backoff and makes the log ambiguous.

The CDK prints a line at each step it completes, so where the log stops is the
diagnosis:

| Last line you see | It got as far as | Look at |
|---|---|---|
| nothing | the gate never fired | the UWB grant, not Matter. `matter_client_want()` is called only on a granted unlock |
| `resolving <instance>` | DNS-SD query sent, no answer | the border router, the peer's SRP registration, and whether both are on ONE Thread network |
| `Sigma1 out to node ...` | the handshake started, no Sigma2 came back | **the most likely stop.** See below |
| `Sigma2 REJECTED (n)` | the peer answered and we refused it | `-6` means it does not hold this fabric's keys, `-9` means it does and is not who we asked for |
| `Sigma3 out: session ...` | we accepted the peer and answered | the peer is verifying our signature. No StatusReport means it refused |
| `CASE ESTABLISHED as initiator` | the handshake is done | the invoke, below |
| `the bound lock refused the timed window` | the peer would not open a timed window | rare; a peer that does this refuses the invoke too |
| `UnlockDoor out: endpoint n` | the command is on the air | the peer's ACL. `UNSUPPORTED_ACCESS` is the expected answer to a missing entry |
| `the bound lock UNLOCKED` | it worked | stop reading |

## The four things most likely to be wrong

Ranked. The first two are known deviations, the last two were found by reading
the driver adversarially and have never been observed.

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

### 3. A retransmitted StatusReport cannot be answered

`s_handshake` is cleared the moment the peer's StatusReport is handled, and
`matter_client_owns_exchange()` is gated on it. So if **our acknowledgement of
that StatusReport is lost**, the peer's retransmission has nowhere to route: it
is a Secure Channel message that is neither Sigma1 nor Sigma3, so it falls into
the unsecured drop.

**Signature:** `CASE ESTABLISHED` on our side, the peer still retransmitting,
and the invoke failing against a session the peer is tearing down. Intermittent
by nature, so it presents as "works sometimes", which is the hardest shape to
diagnose. If you see the peer complain about an unacknowledged StatusReport
while we claim success, this is it.

### 4. A stale fabric pointer

`s_fabric` points into the fabric table and is not cleared when a session is
dropped. The Sigma1 path only re-chooses a target `if (s_fabric == NULL)`, so
after a RemoveFabric the zeroed slot is reused rather than re-validated.

**Signature:** only after removing an administrator without rebooting. A Sigma1
built from a zeroed key, refused by everything, with the log naming a fabric
that is no longer there.

## What to capture

`make monitor` from the first walk-up through two full retries. Two, because
the backoff doubling is visible in the second one and confirms the state machine
is running rather than wedged.

From the peer, whatever it prints on datagram receipt. If the ESP32 shows
nothing at all, the problem is Thread routing and neither Matter implementation
is involved yet.

Record the result in `docs/hardware-validation.md` as a new row when it passes.
Until then this document is the record.
