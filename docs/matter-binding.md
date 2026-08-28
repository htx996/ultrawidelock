# Unlocking another lock

The DWM3001CDK is a Matter door lock. This is how it also becomes a Matter
*client*: when the UWB gate opens the local lock, it sends `UnlockDoor` straight
to another Matter lock on the same Thread network. No hub, no automation, no
cloud round trip.

Off by default. Everything below needs `CONFIG_ULTRAWIDELOCK_MATTER_CLIENT=y`.

> **Working on hardware since 2026-08-22.** A walk-up opens the bound lock and
> stepping away closes it, against `apps/nrf5340dk-lock` on a Home Assistant
> fabric. What follows describes something that runs, not something that
> should. `docs/matter-binding-bench.md` has the bring-up procedure and the
> five interoperability faults that stood between "every test passes" and
> "the door opens".

## The idea in ten lines

Matter calls this the **Binding** pattern. Three moving parts:

1. **A shared fabric.** Both locks must be commissioned onto the same fabric.
   Apple Home will not let you write bindings, so add a *second* administrator,
   Home Assistant or `chip-tool`; both fabrics coexist and Apple Home keeps
   working. A binding is fabric-scoped, so the fabric that writes it is where the
   CASE session runs and where the target's ACL entry names this node, and it is
   not a setup-time convenience you can discard afterwards.
2. **A binding on this lock.** The Binding cluster (0x001E) holds a list of
   targets. An administrator writes "node X, endpoint 1, cluster 0x0101" onto
   the UWB lock, and that is the entire configuration.
3. **An ACL entry on the target.** The other lock has no reason to obey a
   stranger. Its Access Control cluster needs an entry granting *Operate* on
   DoorLock to the UWB lock's node id. Without it, every unlock comes back
   `UNSUPPORTED_ACCESS` and nothing opens.

Miss any one and it fails silently; the helper below does all three and the log
names which one broke.

## The helper

```sh
python3 scripts/bind-helper.py
```

Interactive, idempotent, and keeps no secrets on disk. It walks through:

- checking `chip-tool` is installed (and printing how to get it if not),
- putting each lock into pairing mode and commissioning it onto the helper's
  fabric,
- reading the target's existing ACL, **merging** in the new entry, and writing
  it back,
- writing the binding onto the UWB lock,
- optionally test-firing one real unlock, behind an explicit `y/N`.

Re-running is safe: it re-reads state before every write, so a half-finished run
is finished rather than duplicated.

Every command is printed before it runs. `chip-tool`'s argument spelling has
drifted between releases, so a line your build rejects is the thing to adjust.

## Any administrator will do, not just chip-tool

The helper uses `chip-tool` because it is the reference implementation. Both
writes above are ordinary Matter attribute writes, so **any** administrator on
the fabric can do them: the ACL entry on the target lock, and the binding list
on this one.

Home Assistant's Matter integration is the better option for most people: it is
probably already commissioned onto the target lock, and it needs no terminal.
**This is the route the 2026-08-22 bring-up used**, without `chip-tool`
installed.

It is the *administrator*, used once at setup to write two attributes. It is
**not** in the unlock path afterwards and can be switched off, rebooted or
thrown away without the front door noticing -- but its FABRIC has to stay,
because that is the one the binding is scoped to.

### Driving it without the UI

Home Assistant's Matter server exposes its own websocket, and it is what the
bring-up used because the integration's UI writes neither of these attributes:

```
ws://<ha-host>:5580/ws          # the Matter Server app, not HA's API; no token
```

Send `{"message_id":..., "command":"start_listening"}` first -- the reply is
every node with its cached attributes -- then `read_attribute` and
`write_attribute` with `{node_id, attribute_path}`, where the path is
`"endpoint/cluster/attribute"` in decimal: `1/30/0` is Binding, `0/31/0` the
ACL, `1/257/0` LockState. Structures come back keyed by their wire tags, and go
back the same way: a binding target is `{1:node, 3:endpoint, 4:cluster}`.

**Read the ACL, merge, write the whole set.** An ACL write is fabric-scoped and
REPLACES that fabric's entries, and entries belonging to other fabrics read
back as a bare `{254:index}` with no fields -- invisible, not absent. Drop the
administrator's own entry and it locks itself out with no way back but a
factory reset. `scripts/bind-helper.py` refuses to write without finding one;
anything hand-rolled should do the same.

## Doing it by hand

The four commands, with `$UWB` the UWB lock's node id and `$PEER` the target
lock's:

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

`apps/esp32-matter-lock` is a full DoorLock server. Bind the CDK to it and the
entire chain is in-repo firmware.

```sh
# build and flash the ESP32 lock (see docs/esp32-bringup.md for the board setup)
make esp-build APP=matter-lock TARGET=esp32c6
make esp-flash APP=matter-lock TARGET=esp32c6

# build and flash the CDK with the client on
make build RELEASE=1 SMP=1 CDK_CONF="overlay-thread.conf;overlay-release.conf;overlay-smp.conf;overlay-lto.conf;overlay-client.conf"
make flash
```

Then run the helper, pointing it at both, and watch the CDK's RTT console
(`make monitor`) while you walk up: one line per attempt and one per outcome.

The ESP32 lock requires no PIN, which makes it the right first target.

## The PIN caveat

Some locks set `RequirePINforRemoteOperation`. They refuse an `UnlockDoor` that
carries no `PINCode`, and the refusal looks like any other failure.

A binding says *who* to talk to and never *how* to authenticate, so this node
carries the PIN in a manufacturer-specific attribute beside the binding list.
That is a tradeoff, not a feature:

- It is **one PIN for the node**, not one per target. Two locks demanding two
  different PINs are not supported.
- It is a **shared secret stored in flash**. Anyone who can read this board's
  flash can read the PIN of the lock it opens.
- It **never reads back**. The attribute always returns empty, so an
  administrator on the fabric cannot harvest it.

If the target lock can be configured not to require a PIN for remote operation,
do that. Use the PIN only when the lock gives you no choice.

## What it costs, and what it will not do

Measured on the `thread+release+smp+lto` image, client off against client on:

| | off | on | delta | free after |
|---|---:|---:|---:|---:|
| flash | 406,712 | 423,640 | **+16,928** | 8,288 |
| RAM | 117,928 | 122,856 | **+4,928** | 8,216 |

Flash free is measured against the 431,928 B slot MCUboot will sign, not the
433,664 B linker region, which reports 10,024 B and would pass an image that
cannot ship. 8,288 B clears the size gate's 8,192 B floor by 96 B, which is why
the client build is the one to watch when anything grows.

About 4 KB of the flash is OpenThread's DNS client, which a default build does
not carry; the rest is the CASE initiator, the Interaction Model's outbound
direction, the binding table and the driver. Off, the sources are not compiled
and `matter_clusters.c` and `matter_exchange.c` preprocess away, with one
exception: `matter_exchange_ack_initiator()` is deliberately outside the
`MATTER_FEATURE_CLIENT` guard, because the server's own subscription reports
ride exchanges this node opened and every image needs that ack. So the image is
no longer byte for byte the one that existed before any of it.

Four limits, none of them visible from the outside:

- **One target per unlock.** The table holds four and the first bound DoorLock
  target that resolves is the one that gets the command. Two locks bound means
  one of them opens.
- **The handshake retransmits, the interaction does not.** A dropped Sigma1 or
  Sigma3 is resent on an MRP timer. Past `CASE ESTABLISHED` it is not: those
  messages are sealed by `matter_exchange`, whose counters the client does not
  own, so a lost invoke costs the whole attempt and the retry restarts from the
  resolve. The peer's messages *are* acknowledged throughout.
- **A granted unlock expires after 8 seconds.** It is not retried until it
  succeeds; see the last row of the table below for why.
- **The binding survives a reboot, and dies with its fabric.** It is stored in
  the same record as the operational identity, so removing the administrator
  that wrote a binding removes the binding too.

## Before any of it: getting a second administrator on

This page assumes the lock already carries a second administrator, because Apple
Home will not write a binding and no ecosystem lets a device bind itself. Adding
one is the AdministratorCommissioning path: open a window from the first
ecosystem, then commission from the second.

**If that ends in "pairing failed", check the build date before anything
else.** Adding a non-Apple administrator needs two things that this firmware
did not always have, and without either one the attempt fails exactly that way:

| Landed | What it added |
|---|---|
| 2026-08-06 | `_matterc._udp` published while a window is open, with the `_S`/`_L` discriminator subtypes. Every non-Apple controller browses DNS-SD and gives up when nothing answers. |
| 2026-08-07 | PASE answered over IP as well as over BLE. Before this a controller could resolve the node, match the discriminator, open an exchange, and then time out waiting for a `PBKDFParamResponse` the node had decided not to send. |

Apple Home needs neither, because it commissions over BLE, which this node has
always advertised. A build from before 2026-08-07 pairs perfectly with Apple
Home and cannot be added to Home Assistant at all: the most likely explanation
for a "pairing failed" report, and a re-flash rather than a bug.

On a newer build it is worth filing, with a capture of the RTT console
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

## Before you trust any of it

`docs/matter-binding-bench.md` is the bring-up procedure: what to prove before
involving this node, which line the log stops on, and what is most likely wrong.

## The Home Assistant alternative

Home Assistant's Matter integration can do the same job from an automation:
trigger on the UWB lock's `LockOperation` event and call `lock.unlock` on the
other lock.

Easier to set up, and not the same thing: the hub becomes a dependency of your
front door, and if it is rebooting, updating or off the network when you walk
up, nothing opens. The binding path has no such dependency.
