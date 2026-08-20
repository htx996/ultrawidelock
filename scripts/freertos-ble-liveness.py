#!/usr/bin/env python3
"""freertos-ble-liveness.py — judge a running FreeRTOS lock over the air.

Usage:
    scripts/freertos-ble-liveness.py            # all three checks
    scripts/freertos-ble-liveness.py --scan     # advertisement only

Needs bleak (pip install bleak) and a host BLE radio. Exit: 0 pass, 1 a check
failed, 2 the board was never found.

WHY THIS EXISTS, and why a boot log is not enough.

This port has twice printed a complete, healthy boot log from a board that was
already dead: once when the kernel tick armed an RTC compare it could never
match, and once when a %lld reached newlib-nano's printf, bus-faulted on the
%s after it, and spun default_handler with the tick stopped. Both times every
line up to the fault was correct and every line after it simply never came.
RTT makes that worse rather than better -- the up-buffer skips writes when it
is full, so the boot lines survive and the periodic ones vanish, which looks
identical to a board that stopped.

So these checks are all things a stopped board cannot fake. Advertising needs
the controller scheduling events; a connection needs the host task servicing
them; a GATT reply needs the ATT server running on the work queue. None of it
survives a hung tick.

The DFU check is the one worth keeping honest about scope. It proves the
window gate refuses -- not that an update works. Driving a real update needs
the L2CAP CoC, and no cross-platform Python library opens one.
"""
import asyncio
import struct
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.stderr.write("needs bleak: pip install bleak\n")
    raise SystemExit(2)

CRED_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"
DFU_SVC = "d3b5a140-9e23-4b3a-8be4-6b1ee5f980a3"
DFU_CHR = "d3b5a141-9e23-4b3a-8be4-6b1ee5f980a3"

RSP_ERR = 0x82
ERR_CLOSED = 1

# Every opcode the receiver knows. All four must be refused identically: a gate
# that only guards BEGIN leaves the rest of the state machine reachable.
CASES = [
    ("BEGIN", bytes([0x11]) + struct.pack("<II", 1, 1024)),
    ("DATA", bytes([0x12]) + struct.pack("<II", 1, 0)),
    ("COMMIT", bytes([0x13]) + struct.pack("<I", 1)),
    ("ABORT", bytes([0x14]) + struct.pack("<I", 1)),
]


def ok(msg):
    print(f"  ok   {msg}")


def bad(msg):
    print(f"  FAIL {msg}")


async def find():
    return await BleakScanner.find_device_by_filter(
        lambda d, a: CRED_UUID in [u.lower() for u in (a.service_uuids or [])],
        timeout=15.0,
    )


async def main(scan_only):
    dev = await find()
    if dev is None:
        bad("no advertisement carrying the credential service UUID 0xFFF2")
        return 2
    ok(f"advertising as {dev.name!r}")
    if scan_only:
        return 0

    fails = 0
    async with BleakClient(dev, timeout=20.0) as client:
        uuids = [s.uuid.lower() for s in client.services]
        if CRED_UUID in uuids:
            ok("credential GATT service is discoverable")
        else:
            bad("credential GATT service missing from the table")
            fails += 1
        if DFU_SVC in uuids:
            ok("update channel is discoverable")
        else:
            bad("update channel missing from the table")
            return fails + 1

        replies = asyncio.Queue()
        await client.start_notify(DFU_CHR, lambda _h, d: replies.put_nowait(bytes(d)))
        for name, frame in CASES:
            await client.write_gatt_char(DFU_CHR, frame, response=True)
            try:
                r = await asyncio.wait_for(replies.get(), timeout=5.0)
            except asyncio.TimeoutError:
                bad(f"{name} got no reply; the ATT server is not answering")
                fails += 1
                continue
            if r[0] == RSP_ERR and len(r) > 1 and r[1] == ERR_CLOSED:
                ok(f"{name} refused with CLOSED")
            else:
                bad(f"{name} was not refused: {r.hex()}")
                fails += 1
        await client.stop_notify(DFU_CHR)

    print()
    print("PASS" if fails == 0 else f"FAIL ({fails})")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main("--scan" in sys.argv)))
