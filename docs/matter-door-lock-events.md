# Door Lock events

A lock that only reports its bolt can say what it did, not what happened to it.
These are what the hand-written Matter node (`modules/ultrawidelock_matter`)
sends.

## The two events

Both are on the Door Lock cluster (0x0101) at endpoint 1, both priority
CRITICAL. That is not a judgement call: it is `priority="critical"` on the event
element in the cluster XML, the field a subscriber reads to decide what it may
drop.

| Event | Id | Fields | Raised when |
|---|---|---|---|
| `LockOperation` | 2 | type, source, user index, fabric index, source node | the reported bolt state CHANGED |
| `DoorLockAlarm` | 0 | alarm code | the door was forced, or left ajar while locked |

`LockOperation` exists because the CHIP-based lock builds serve it and this one
served none; Apple Home's "Manage Access" pane is the working hypothesis for
what that absence gates. It reports Remote for a controller command and the
credential source for a walk-up, with a null fabric and null source node,
because a walk-up belongs to no fabric and naming one would tell a controller
that IT opened the door.

`DoorLockAlarm` carries one field. Two of the nine `AlarmCodeEnum` values are
ever sent:

| Code | Value | What this board actually observes |
|---|---|---|
| `DoorForcedOpen` | 6 | the LIS2DH12 impact classifier latched a tamper while the bolt still reported LOCKED |
| `DoorAjar` | 7 | the frame-to-leaf swing angle stood away from CLOSED past the dwell, bolt still thrown |

The other six describe a motor, keypad or enclosure switch this board does not
have. `LockJammed` on a lock with no bolt to jam is a lie a controller cannot
check, so it is not sent.

## How one reaches a controller

No separate event channel: a recorded event goes into a ring of four in
`struct matter_device_info` and rides out on the next report.

- **Four slots, not an audit log.** A subscriber hears about each event as it
  happens, so the ring only has to survive the gap between an event and the
  report carrying it. A full ring drops its OLDEST: the newest event describes
  the state a controller can still see on the tile.
- **One ascending EventNumber sequence for the whole node**, starting at 1, and
  both event types share it. Zero is never a valid event number, which is what
  lets an EventFilter of 0 mean "everything you have" without also meaning
  "including one I already saw". Two rings would have to invent an ordering
  between them that the numbers already answer.
- **Each entry records which event it is**, so a subscriber watching
  `LockOperation` is never handed an alarm from the same cluster.

## What records them

| Source | Event | Where |
|---|---|---|
| `LockDoor` / `UnlockDoor` from a controller | `LockOperation`, source `MATTER_DL_OP_SOURCE_REMOTE` | `matter_clusters.c` |
| a credential walk-up | `LockOperation`, source `MATTER_DL_OP_SOURCE_ALIRO`, null fabric | `matter_commission.c`, the reader's lock-state listener |
| the impact/tamper latch | `DoorLockAlarm` / `DoorForcedOpen` | `door_alarm.c`, from the 250 ms loop in `main.c` |
| the door-angle dwell | `DoorLockAlarm` / `DoorAjar` | `door_alarm.c` |

Both alarms pass through `matter_commission_record_alarm()`, which applies the
one test the sensors cannot: the bolt has to report LOCKED. An alarm about a
door the owner deliberately left open is noise, and this node has one report
channel to spend.

## What it costs

The alarms compile only into the anchor build. `matter_clusters.[ch]` gates them
on `MATTER_FEATURE_DL_ALARMS`, a portable macro rather than a Kconfig symbol,
because those sources are platform-agnostic C11 the host suite compiles directly
and the flag has to mean the same thing to both. The Zephyr side defines it in
`modules/ultrawidelock_matter/CMakeLists.txt` under
`CONFIG_ULTRAWIDELOCK_ANCHOR`, the only build carrying a sensor that can witness
an alarm.

Off, every file preprocesses to what it was before the event existed. Verified,
not asserted: the default image built from HEAD and from the change, same build
directory, both `PRISTINE=1`, same signing key, and the only five differing
bytes are the embedded build-time string. On the anchor image the alarms cost
300 B of flash and no RAM.

`LockOperation` is not gated. It is in every image that has the Matter node.

## Not proven

- **No controller has been observed rendering either event.** Whether Apple Home
  or Home Assistant surfaces a `DoorLockAlarm` at all is the premise the feature
  rests on, and nobody has watched one arrive. Field verification is the next
  step, not a completed one.
- **The ajar alarm cannot fire yet.** Nothing feeds the door-angle state: the
  leaf tag's transport is the same missing stage that leaves
  `ultrawidelock_satellite_report()` uncalled. The seam is there so the
  transport has one place to deliver to.
- **A forced door is reported once per boot.** The impact classifier reports the
  transition and then latches, and nothing calls
  `ultrawidelock_slam_clear_tamper()`. Whoever wires the recovery path owns
  clearing it.
- **Every threshold behind the alarms is a placeholder.** The impact threshold,
  the hinge geometry and the ajar dwell are all Kconfig defaults nobody has
  measured on a real door. See [`configuring.md`](configuring.md) for the
  symbols and [`bodycal-falsification.md`](bodycal-falsification.md) for the
  shape a capture that settles such a number has to take.

## Where to read the code

`modules/ultrawidelock_matter/include/matter_clusters.h` carries the event ids,
the alarm codes and the field numbers, each cited to the line of the Door Lock
cluster XML it came from. `tests/host/test_matter_im.c`, suite
`matter_im_events`, is the record-to-report proof for both events, including
that an alarm does not answer a `LockOperation` path.
