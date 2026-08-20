#!/usr/bin/env python3
"""Push a signed delta patch to a DWM3001CDK over Bluetooth.

    scripts/ultrawidelock_push.py update.wdfu

The board accepts nothing until an update window is open, so this connects and
then WAITS, asking once a second and prompting you to press SW2. The window
lasts CONFIG_ULTRAWIDELOCK_DFU_WINDOW_MS, five minutes by default. Start the push first
or press the button first; either order works.

On success the board reboots into MCUboot, which applies the patch -- about
30 seconds during which it is not on the air. The Bluetooth connection dropping
right after COMMIT is the expected ending, not a failure.

Needs bleak:

    python3 -m pip install bleak

WHY GATT AND NOT THE L2CAP CoC the firmware also offers: no Python Bluetooth
library can open an L2CAP connection-oriented channel. CoreBluetooth and BlueZ
both can, bleak wraps neither, and bleak is the only cross-platform option. The
firmware carries both transports for exactly this reason; an iPhone app would
use the CoC and get better throughput.
"""

import argparse
import asyncio
import secrets
import struct
import sys
from pathlib import Path

DFU_SVC_UUID = "d3b5a140-9e23-4b3a-8be4-6b1ee5f980a3"
DFU_CHR_UUID = "d3b5a141-9e23-4b3a-8be4-6b1ee5f980a3"

OP_BEGIN, OP_DATA, OP_COMMIT, OP_ABORT = 0x11, 0x12, 0x13, 0x14
RSP_OK, RSP_ERR = 0x81, 0x82

ERRORS = {
    1: "no update window is open -- press SW2 on the board, then retry",
    2: "out of sequence",
    3: "too large for patch_staging",
    4: "header signature did not verify (wrong signing key?)",
    5: "length or CRC disagreed at commit",
    6: "a flash write or erase failed",
    7: "malformed frame",
    8: "another update transport owns the receiver",
}


def die(msg):
    """Exit with a prefixed error message."""
    sys.exit(f"ultrawidelock_push: {msg}")


class Session:
    """One update conversation: write a frame, wait for its reply."""

    def __init__(self, client):
        """Store the BLE client and replies queue for a GATT notification session."""
        self.client = client
        self.replies = asyncio.Queue()
        self.transfer_id = secrets.randbits(32) or 1

    def on_notify(self, _sender, data):
        """Queue a received GATT notification payload for retrieval by wait_for_window."""
        self.replies.put_nowait(bytes(data))

    async def call(self, frame, timeout=20.0, tolerate=(), retries=3, expected_offset=None):
        """Send a frame, return the board's byte count.

        Errors listed in `tolerate` come back as a negative code instead of
        ending the run; everything else is fatal, because there is nothing
        useful to do with a board that has refused the transfer.
        """
        rsp = None
        for attempt in range(retries):
            await self.client.write_gatt_char(DFU_CHR_UUID, frame, response=True)
            deadline = asyncio.get_event_loop().time() + timeout
            while True:
                try:
                    candidate = await asyncio.wait_for(
                        self.replies.get(), deadline - asyncio.get_event_loop().time()
                    )
                except asyncio.TimeoutError:
                    if attempt + 1 == retries:
                        die("the board stopped answering after idempotent retries")
                    break

                # A notification whose first delivery timed out can arrive
                # behind its retry and poison the following DATA call. The
                # transfer id rejects another uploader; the expected cursor
                # rejects this transfer's late reply without retransmitting.
                if candidate and candidate[0] == RSP_OK and len(candidate) == 9:
                    transfer_id, next_offset = struct.unpack("<II", candidate[1:9])
                    if transfer_id != self.transfer_id:
                        continue
                    if expected_offset is not None and next_offset != expected_offset:
                        continue
                rsp = candidate
                break
            if rsp is not None:
                break

        if not rsp:
            die("empty reply")
        if rsp[0] == RSP_ERR:
            code = rsp[1] if len(rsp) > 1 else 0
            if code in tolerate:
                return -code
            die(f"refused: {ERRORS.get(code, f'unknown error {code}')}")
        if rsp[0] != RSP_OK:
            die(f"unexpected reply {rsp[0]:#04x}")
        if len(rsp) != 9:
            die(f"malformed OK reply ({len(rsp)} bytes)")
        transfer_id, next_offset = struct.unpack("<II", rsp[1:9])
        if transfer_id != self.transfer_id:
            die(f"reply belongs to transfer {transfer_id:#010x}, expected {self.transfer_id:#010x}")
        return next_offset

    async def wait_for_window(self, total, deadline):
        """Retry BEGIN until someone opens the update window.

        The board refuses everything until SW2 is pressed, so a push that
        started first would otherwise just fail. Asking repeatedly costs the
        board a comparison and a two-byte notification: no flash, no state.
        """
        begin = struct.pack("<BII", OP_BEGIN, self.transfer_id, total)
        loop = asyncio.get_event_loop()
        prompted = False

        while True:
            if await self.call(begin, tolerate=(1,), expected_offset=0) >= 0:
                return
            if not prompted:
                print("\n  >>> PRESS SW2 ON THE BOARD to open the update window <<<\n")
                prompted = True
            if loop.time() > deadline:
                die("no update window opened in time")
            await asyncio.sleep(1.5)


async def run(args):
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        die("needs the 'bleak' module: python3 -m pip install bleak")

    blob = Path(args.patch).read_bytes()
    if len(blob) < 96:
        die(f"{args.patch} is too short to be a .wdfu")

    # The DFU service is NOT in the advertisement -- the advertising set is the
    # reader's and is already full -- so match on what the board does advertise:
    # credential 0xFFF2 once provisioned, Matter 0xFFF6 while commissionable.
    wanted = {
        "0000fff2-0000-1000-8000-00805f9b34fb",
        "0000fff6-0000-1000-8000-00805f9b34fb",
    }

    # SERVICE DATA AS WELL AS THE UUID LIST, and this is not belt-and-braces.
    # A Matter commissionable advertisement carries 0xFFF6 as service DATA --
    # the opcode, discriminator, vendor and product, which is the whole point of
    # it -- and puts something else in the 16-bit UUID list. Matching only the
    # list therefore misses a board that is advertising perfectly well, and says
    # "is it powered?" about a lock that is sitting there commissionable.
    # MEASURED 2026-08-15: an uncommissioned DWM3001CDK at -54 dBm advertising
    # service data 0000fff6=00000ff1ff018000 (discriminator 0x0F00, VID 0xFFF1,
    # PID 0x8001) with service_uuids = ['0000676e-...'] and no name at all.
    def advertises_ours(d, ad):
        if (d.name or "") == args.name:
            return True
        seen = {u.lower() for u in (ad.service_uuids or [])}
        seen |= {u.lower() for u in (ad.service_data or {})}
        return bool(wanted & seen)

    print("  scanning ...")
    device = await BleakScanner.find_device_by_filter(
        advertises_ours,
        timeout=args.scan_timeout,
    )
    if device is None:
        die("no board found advertising the credential service (0xFFF2) or Matter (0xFFF6). "
            "Is it powered and advertising?")
    print(f"  found {device.address}")

    # services=[...] is not an optimisation. Without it CoreBluetooth walks every
    # characteristic on the board and fails on one of the reader's with
    # "The specified UUID is not allowed for this operation" (CBError 8), which
    # aborts the connection before the DFU service is ever reached.
    async with BleakClient(device, services=[DFU_SVC_UUID]) as client:
        session = Session(client)
        await client.start_notify(DFU_CHR_UUID, session.on_notify)

        # One opcode byte plus ATT's three, out of whatever MTU was negotiated.
        mtu = getattr(client, "mtu_size", 23) or 23
        chunk = max(1, min(args.chunk, mtu - 12))
        print(f"  connected, MTU {mtu}, sending {len(blob):,} B in {chunk} B chunks")

        deadline = asyncio.get_event_loop().time() + args.window_timeout
        await session.wait_for_window(len(blob), deadline)

        sent = 0
        while sent < len(blob):
            piece = blob[sent:sent + chunk]
            expected = sent + len(piece)
            got = await session.call(
                struct.pack("<BII", OP_DATA, session.transfer_id, sent) + piece,
                expected_offset=expected,
            )
            if got != expected:
                die(f"board requested offset {got} after {expected} B were sent")
            sent = got
            pct = 100.0 * sent / len(blob)
            print(f"\r  {sent:>7,} / {len(blob):,} B  {pct:5.1f}%", end="", flush=True)
        print()

        await session.call(
            struct.pack("<BI", OP_COMMIT, session.transfer_id), expected_offset=len(blob)
        )
        print("  committed. The board is rebooting to apply it (~30 s).")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("patch", help="a .wdfu built by scripts/ultrawidelock_patch.py")
    p.add_argument("--name", default="ultrawidelock", help="advertised name to look for")
    p.add_argument("--chunk", type=int, default=180,
                   help="bytes of patch per frame, capped by the negotiated MTU")
    p.add_argument("--scan-timeout", type=float, default=15.0)
    p.add_argument("--window-timeout", type=float, default=120.0,
                   help="how long to keep asking while waiting for SW2")
    asyncio.run(run(p.parse_args()))


if __name__ == "__main__":
    main()
