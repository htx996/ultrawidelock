# Unlocking another lock

The DWM3001CDK is a Matter door lock. This is how it also becomes a Matter
*client*: when the UWB gate opens the local lock, it sends `UnlockDoor` straight
to another Matter lock on the same Thread network. No hub, no automation, no
cloud round trip. The two locks talk to each other.

Off by default. Everything below needs `CONFIG_ULTRAWIDELOCK_MATTER_CLIENT=y`.

## The idea in ten lines

Matter calls this the **Binding** pattern, and it has exactly three moving
parts:

1. **A shared fabric.** Both locks must be commissioned onto the same fabric.
   Apple Home will not let you write bindings, so you add a *second*
   administrator with `chip-tool`. Both fabrics coexist: Apple Home keeps
   working, and the second fabric is what carries the binding.
2. **A binding on this lock.** The Binding cluster (0x001E) holds a list of
   targets. An administrator writes "node X, endpoint 1, cluster 0x0101" onto
   the UWB lock, and that is the entire configuration.
3. **An ACL entry on the target.** The other lock has no reason to obey a
   stranger. Its Access Control cluster needs an entry granting *Operate* on
   DoorLock to the UWB lock's node id. Without it, every unlock comes back
   `UNSUPPORTED_ACCESS` and nothing opens.

Miss any one and it fails silently from the user's point of view, which is why
the helper below does all three and the log names which one broke.

## The helper

```sh
python3 scripts/bind-helper.py
```

It is interactive, idempotent, and keeps no secrets on disk. It walks you
through:

- checking `chip-tool` is installed (and printing how to get it if not),
- putting each lock into pairing mode and commissioning it onto the helper's
  fabric,
- reading the target's existing ACL, **merging** in the new entry, and writing
  it back,
- writing the binding onto the UWB lock,
- optionally test-firing one real unlock, behind an explicit `y/N`.

Re-running it is safe. It re-reads state before every write, so a half-finished
run is finished rather than duplicated.

Every command it runs is printed before it runs. `chip-tool`'s argument
spelling has drifted between releases, so if one is rejected by your build, the
printed line is the thing to adjust.

## Any administrator will do, not just chip-tool

The helper uses `chip-tool` because it is the reference implementation and it
runs anywhere. Nothing in the design requires it. Both writes in the list above
are ordinary Matter attribute writes, so **any** administrator on the fabric
that can write an attribute can do them: the ACL entry on the target lock, and
the binding list on this one.

Home Assistant's Matter integration is the obvious second option, and it is a
better one for most people: it is probably already commissioned onto the target
lock, and it does not need a terminal. Note what role it is playing here. It is
the *administrator*, used once at setup time to write two attributes. It is
**not** in the unlock path afterwards, and it can be switched off, rebooted or
thrown away without the front door noticing. That is a different arrangement
from the automation described at the bottom of this page, and the difference is
the whole point of the binding.

Whichever administrator you use, the sequence and the values are the ones below.

## Doing it by hand

The helper is a wrapper, not magic. The four commands, with `$UWB` the UWB
lock's node id and `$PEER` the target lock's:

```sh
# 1. both locks onto your fabric (get the setup code off each device)
chip-tool pairing code $UWB  <setup-code-of-uwb-lock>
chip-tool pairing code $PEER <setup-code-of-target-lock>

# 2. read the target's ACL, so you can add to it rather than replace it
chip-tool accesscontrol read acl $PEER 0

# 3. write it back with one more entry: Operate (3) over CASE (2) for $UWB
#    KEEP the administer entry that is already there, or you lock yourself out.
chip-tool accesscontrol write acl \
  '[{"privilege":5,"authMode":2,"subjects":[112233],"targets":null},
    {"privilege":3,"authMode":2,"subjects":['"$UWB"'],
     "targets":[{"cluster":257,"endpoint":1,"deviceType":null}]}]' $PEER 0

# 4. bind the UWB lock to the target
chip-tool binding write binding \
  '[{"node":'"$PEER"',"endpoint":1,"cluster":257}]' $UWB 1
```

`257` is `0x0101`, the DoorLock cluster. `112233` is chip-tool's own default
node id; if yours differs, use the subject that is already in the ACL you read
in step 2.

## Testing it without buying a lock

You do not need a Nuki or an Aqara to prove the whole path. This repository
already contains a second Matter door lock: `apps/esp32-matter-lock` is a full
DoorLock server. Bind the CDK to it and the entire chain is in-repo firmware.

```sh
# build and flash the ESP32 lock (see docs/esp32-bringup.md for the board setup)
make esp-build APP=matter-lock TARGET=esp32c6
make esp-flash APP=matter-lock TARGET=esp32c6

# build and flash the CDK with the client on
make build RELEASE=1 SMP=1 CDK_CONF="overlay-thread.conf;overlay-release.conf;overlay-smp.conf;overlay-lto.conf;overlay-client.conf"
make flash
```

Then run the helper, pointing it at both. Watch the CDK's RTT console
(`make monitor`) while you walk up to it: one line per attempt and one per
outcome.

The ESP32 lock requires no PIN, which makes it the right first target. Add the
PIN only once the no-PIN path works.

## The PIN caveat

Some locks set `RequirePINforRemoteOperation`. They will refuse an `UnlockDoor`
that carries no `PINCode`, and the refusal looks like any other failure.

The Matter binding entry has nowhere to put a credential. A binding says *who*
to talk to and never *how* to authenticate. So this node carries the PIN in a
manufacturer-specific attribute beside the binding list, and that is a real
tradeoff rather than a feature:

- It is **one PIN for the node**, not one per target. Two locks demanding two
  different PINs are not supported.
- It is a **shared secret stored in flash**. Anyone who can read this board's
  flash can read the PIN of the lock it opens.
- It **never reads back**. The attribute always returns empty, so an
  administrator on the fabric cannot harvest it.

If your target lock can be configured not to require a PIN for remote
operation, that is the better configuration. Use the PIN only when the lock
gives you no choice.

## What it costs, and what it will not do

Measured on the `thread+release+smp+lto` image, client off against client on:

| | off | on | delta | free after |
|---|---:|---:|---:|---:|
| flash | 400,332 | 414,628 | **+14,296** | 19,036 |
| RAM | 114,792 | 117,352 | **+2,560** | 13,720 |

About 4 KB of the flash is OpenThread's DNS client, which a default build does
not carry; the rest is the CASE initiator, the Interaction Model's outbound
direction, the binding table and the driver. Off, the image is byte for byte
the one that existed before any of it: the sources are not compiled and the
two places that could not move into a source file of their own
(`matter_clusters.c`, `matter_exchange.c`) preprocess away.

Four limits, stated because none of them is visible from the outside:

- **One target per unlock.** The table holds four and the first bound DoorLock
  target that resolves is the one that gets the command. Two locks bound means
  one of them opens.
- **No MRP retransmission on the client's own messages.** A dropped Sigma1 or
  invoke is caught by a five-second step timeout and retried as a whole
  attempt, not by the reliable-messaging layer. The peer's messages *are*
  acknowledged.
- **A granted unlock expires after 8 seconds.** It is not retried until it
  succeeds; see the last row of the table below for why.
- **The binding survives a reboot, and dies with its fabric.** It is stored in
  the same record as the operational identity, so removing the administrator
  that wrote a binding removes the binding too.

## Before any of it: getting a second administrator on

Everything on this page assumes this lock already carries a second
administrator, because Apple Home will not write a binding and no ecosystem
lets a device bind itself. Adding one is the AdministratorCommissioning path:
open a window from the first ecosystem, then commission from the second.

**If that ends in "pairing failed", check the build date before anything
else.** Adding a non-Apple administrator needs two things that this firmware
did not always have, and without either one the attempt fails exactly that way:

| Landed | What it added |
|---|---|
| 2026-08-06 | `_matterc._udp` published while a window is open, with the `_S`/`_L` discriminator subtypes. Every non-Apple controller browses DNS-SD and gives up when nothing answers. |
| 2026-08-07 | PASE answered over IP as well as over BLE. Before this a controller could resolve the node, match the discriminator, open an exchange, and then time out waiting for a `PBKDFParamResponse` the node had decided not to send. |

Apple Home needs neither, because it commissions over BLE, which this node has
always advertised. So a build from before 2026-08-07 pairs perfectly with Apple
Home and cannot be added to Home Assistant at all. That is the single most
likely explanation for a "pairing failed" report, and it is a re-flash rather
than a bug.

If you are on a build newer than that and it still fails, that is worth filing,
and the thing that identifies it is a capture of the RTT console
(`make monitor`) across one failed attempt: where it stops (PASE, AddNOC, or
operational discovery and CASE) picks out which third of the path is broken.

## When it does not work

| What you see | What it means | Fix |
|---|---|---|
| `UNSUPPORTED_ACCESS` in the RTT log | the target's ACL has no entry for this node | step 3 above; check the subject is the UWB lock's node id, not the target's |
| `NEEDS_TIMED_INTERACTION` | the invoke arrived without its TimedRequest | a firmware bug, not a configuration one; the client always sends one |
| the resolve times out | DNS-SD found nothing for the bound node | the target is not on this Thread network, or its SRP registration lapsed. Check it responds to `chip-tool doorlock read lock-state $PEER 1` |
| nothing at all in the log | no binding is set for the DoorLock cluster | step 4 above; re-read it with `chip-tool binding read binding $UWB 1` |
| it works once, then stops | the session was lost and the backoff is running | it recovers by itself; the backoff doubles to a minute |
| the unlock lands seconds late, or not at all | the target is a sleepy device polling slowly | expected. A granted unlock is dropped after 8 seconds rather than delivered late, deliberately: a lock that opens by itself half a minute later is a surprise |

## The Home Assistant alternative

If you would rather not run `chip-tool`, Home Assistant's Matter integration can
do the same job from an automation: trigger on the UWB lock's `LockOperation`
event and call `lock.unlock` on the other lock.

It is easier to set up and it is not the same thing. The hub becomes a
dependency of your front door: if it is rebooting, updating, or off the network
when you walk up, nothing opens. The binding path has no such dependency, which
is the whole reason it exists. Use Home Assistant to try the idea, and the
binding for the installation you actually live with.
