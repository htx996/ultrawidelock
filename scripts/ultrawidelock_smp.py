#!/usr/bin/env python3
"""Push a delta patch to the board over SMP, the way a phone would.

WHY THIS EXISTS BESIDE ultrawidelock_push.py. ultrawidelock_push speaks the native framed protocol
over an L2CAP CoC, which no phone app can open. This one speaks mcumgr over
GATT -- byte for byte what nRF Device Manager sends -- so the device half can be
proved from a Mac before anyone starts tapping at a phone. When this works and
the app does not, the fault is in the app or the file it is given, not in the
firmware.

    scripts/ultrawidelock_smp.py build/cdk.wdfu          push a patch
    scripts/ultrawidelock_smp.py --list                 read the image list and stop

Requires the board to be built with SMP=1 (apps/dwm3001cdk-lock/overlay-smp.conf).

CBOR IS HAND-ROLLED HERE, deliberately. The maps mcumgr exchanges are half a
dozen keys of ints and byte strings, and vendoring a CBOR library into the OTA
venv to encode that would be more moving parts than the encoder itself.
"""

import argparse
import asyncio
import struct
import sys
from pathlib import Path

# Zephyr's SMP service, subsys/mgmt/mcumgr/transport/src/smp_bt.c.
SMP_SVC_UUID = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"

# HOW A BOARD IS RECOGNISED, and every line of this is measured rather than
# assumed, because two earlier assumptions here were both wrong.
#
# MEASURED 2026-08-27 on a provisioned SMP=1 board at -53 dBm:
#
#   name           'ultrawidelo'                             <- TRUNCATED
#   service_uuids  ['8d53dc1d-1db7-4cd3-868b-8a527460aa84']  <- SMP, advertised
#   service_data   {'0000fff2-...': 24 bytes}
#
# Two corrections to what this file used to say.
#
# The SMP service IS advertised on an SMP=1 build -- the comment here claimed it
# never was, and the scan therefore ignored the one UUID that identifies exactly
# the service this tool needs. It is now the primary match.
#
# And the local name is SHORTENED. An advertisement is 31 bytes; 24 of them are
# the credential service data, so the name does not fit and the controller emits
# a shortened-local-name AD instead. `"ultrawidelock" in "ultrawidelo"` is
# false, which is why a board sitting at -53 dBm was reported as "not
# advertising -- is it powered?". The name test is now a prefix match in the
# direction that survives truncation, and it is a fallback rather than the
# primary.
#
# Service DATA is checked as well as the UUID list, for the reason
# ultrawidelock_push.py documents at length: a commissionable Matter
# advertisement carries 0xFFF6 as service data and puts something else in the
# UUID list, so matching only the list misses a board that is advertising
# perfectly well.
SMP_SVC_SHORT = "8d53dc1d"
SCAN_NAME = "ultrawidelock"
# The shortest prefix that still cannot collide with anything else nearby. The
# controller decides how much of the name fits, so nothing may depend on the
# exact truncation point.
SCAN_NAME_MIN = "ultrawide"
SCAN_UUIDS = ("0000fff2-0000-1000-8000-00805f9b34fb", "0000fff6-0000-1000-8000-00805f9b34fb")

OP_READ_REQ, OP_READ_RSP, OP_WRITE_REQ, OP_WRITE_RSP = 0, 1, 2, 3
GRP_OS, GRP_IMG = 0, 1
OS_ID_RESET = 5
IMG_ID_STATE, IMG_ID_UPLOAD = 0, 1

# mgmt_defines.h. Only the ones this tool can actually provoke are named.
MGMT_ERR = {
    0: "ok",
    2: "out of memory",
    3: "malformed request",
    6: "bad state",
    8: "not supported",
    11: "access denied -- no update window is open (press SW2)",
}


def die(msg):
    """Exit the process with the formatted error message prefixed by ultrawidelock_smp."""
    sys.exit(f"ultrawidelock_smp: {msg}")


def image_sha(path):
    """The SHA-256 MCUboot recorded in a signed image's TLVs.

    The same hash the board reports in its image list, so comparing the two
    answers "did the update actually land" with the image's own bytes rather
    than with anyone's assumption. Used after a phone push, where nothing else
    on this machine witnessed the transfer.
    """
    d = Path(path).read_bytes()
    magic, _, hdr_sz, _, img_sz, _ = struct.unpack_from("<IIHHII", d, 0)
    if magic != 0x96F3B83D:
        die(f"{path} is not an MCUboot image")

    base = hdr_sz + img_sz
    tlv_magic, tlv_tot = struct.unpack_from("<HH", d, base)
    if tlv_magic == 0x6908:  # protected block first, unprotected after it
        base += tlv_tot
        tlv_magic, tlv_tot = struct.unpack_from("<HH", d, base)
    if tlv_magic != 0x6907:
        die(f"{path} has no TLV block where its header says it should")

    p, end = base + 4, base + tlv_tot
    while p + 4 <= end:
        kind, _, ln = struct.unpack_from("<BBH", d, p)
        p += 4
        if kind == 0x10 and ln == 32:
            return d[p : p + 32]
        p += ln
    die(f"{path} has no SHA-256 TLV")


# ---- the smallest CBOR that carries an mcumgr request ------------------------


def _head(major, n):
    """Encode a CBOR unsigned integer length prefix for the given value n and major type. Returns 1, 2, 3, or 5 bytes depending on the magnitude."""
    if n < 24:
        return bytes([major | n])
    if n < 0x100:
        return bytes([major | 24, n])
    if n < 0x10000:
        return bytes([major | 25]) + struct.pack(">H", n)
    return bytes([major | 26]) + struct.pack(">I", n)


def cbor_encode(obj):
    if isinstance(obj, bool):
        return bytes([0xF5 if obj else 0xF4])
    if isinstance(obj, int):
        return _head(0x00, obj)
    if isinstance(obj, bytes):
        return _head(0x40, len(obj)) + obj
    if isinstance(obj, str):
        b = obj.encode()
        return _head(0x60, len(b)) + b
    if isinstance(obj, dict):
        out = _head(0xA0, len(obj))
        for k, v in obj.items():
            out += cbor_encode(k) + cbor_encode(v)
        return out
    if isinstance(obj, list):
        out = _head(0x80, len(obj))
        for v in obj:
            out += cbor_encode(v)
        return out
    raise TypeError(f"cannot encode {type(obj)}")


def cbor_decode(buf, i=0):
    """Return (value, next_index). Enough of CBOR to read mcumgr's replies."""
    b = buf[i]
    major, extra = b & 0xE0, b & 0x1F
    i += 1

    # Major 7 carries the simple values, and its argument is NOT a length --
    # false/true/null are 20/21/22, all below 24, so they have to be taken here
    # before the length decoding below claims them.
    if major == 0xE0:
        if extra in (20, 21):
            return extra == 21, i
        if extra == 22:
            return None, i
        raise ValueError(f"unsupported CBOR simple value {extra}")

    # Indefinite length: items run until a 0xFF break. zcbor emits maps and
    # lists this way unless ZCBOR_CANONICAL is defined, which Zephyr does not
    # define, so EVERY map the board sends arrives in this form.
    if extra == 31:
        if major == 0x80:
            out = []
            while buf[i] != 0xFF:
                v, i = cbor_decode(buf, i)
                out.append(v)
            return out, i + 1
        if major == 0xA0:
            out = {}
            while buf[i] != 0xFF:
                k, i = cbor_decode(buf, i)
                v, i = cbor_decode(buf, i)
                out[k] = v
            return out, i + 1
        raise ValueError(f"indefinite length is not supported for major type {major:#x}")

    if extra < 24:
        n = extra
    elif extra == 24:
        n, i = buf[i], i + 1
    elif extra == 25:
        n, i = struct.unpack_from(">H", buf, i)[0], i + 2
    elif extra == 26:
        n, i = struct.unpack_from(">I", buf, i)[0], i + 4
    else:
        raise ValueError(f"unsupported CBOR additional info {extra}")

    if major == 0x00:
        return n, i
    if major == 0x20:
        return -1 - n, i
    if major == 0x40:
        return buf[i : i + n], i + n
    if major == 0x60:
        return buf[i : i + n].decode(), i + n
    if major == 0x80:
        out = []
        for _ in range(n):
            v, i = cbor_decode(buf, i)
            out.append(v)
        return out, i
    if major == 0xA0:
        out = {}
        for _ in range(n):
            k, i = cbor_decode(buf, i)
            v, i = cbor_decode(buf, i)
            out[k] = v
        return out, i
    raise ValueError(f"unsupported CBOR major type {major:#x}")


# ---- SMP over GATT -----------------------------------------------------------


class Smp:
    """One mcumgr conversation. Reassembles responses, matches them by seq."""

    def __init__(self, client):
        """Initialize an SMP protocol instance bound to the given BLE client. Tracks the outgoing sequence number, buffers incoming notification chunks, and queues complete frames."""
        self.client = client
        self.seq = 0
        self.rx = bytearray()
        self.frames = asyncio.Queue()

    def on_notify(self, _sender, data):
        """Reassemble SMP response frames from BLE notifications. Buffers data and enqueues complete frames once the length declared in the 8-byte header is satisfied."""
        # A response longer than one notification arrives in pieces with a
        # single 8-byte header at the front, so buffer until the header's
        # declared length is complete.
        self.rx += data
        while len(self.rx) >= 8:
            length = struct.unpack_from(">H", self.rx, 2)[0]
            if len(self.rx) < 8 + length:
                return
            self.frames.put_nowait(bytes(self.rx[: 8 + length]))
            del self.rx[: 8 + length]

    async def call(self, op, group, cmd_id, payload, timeout=20.0):
        body = cbor_encode(payload)
        self.seq = (self.seq + 1) & 0xFF
        hdr = struct.pack(">BBHHBB", op, 0, len(body), group, self.seq, cmd_id)

        await self.client.write_gatt_char(SMP_CHR_UUID, hdr + body, response=False)

        while True:
            try:
                frame = await asyncio.wait_for(self.frames.get(), timeout)
            except asyncio.TimeoutError:
                die("the board stopped answering")
            if frame[6] == self.seq:
                break

        rsp, _ = cbor_decode(frame, 8) if len(frame) > 8 else ({}, 0)
        rc = rsp.get("rc", 0)
        if rc:
            die(f"refused: rc={rc} ({MGMT_ERR.get(rc, 'unknown')})")
        return rsp


async def run(args):
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        die("bleak is not installed. Fix: make ota-deps")

    print("scanning...")
    device = None
    found = await BleakScanner.discover(timeout=args.scan, return_adv=True)
    want = (args.name or SCAN_NAME).lower()
    for dev, adv in found.values():
        name = (dev.name or "").lower()
        uuids = {u.lower() for u in (adv.service_uuids or [])}
        uuids |= {u.lower() for u in (adv.service_data or {})}

        # The SMP service itself, when the board advertises it. Unambiguous:
        # it is the exact service this tool is about to talk to.
        if any(SMP_SVC_SHORT in u for u in uuids):
            device = dev
            break
        # The name, matched in the direction that survives truncation. `want`
        # may be longer than what the controller actually emitted.
        if name and (name.startswith(want) or want.startswith(name)) and \
                name.startswith(SCAN_NAME_MIN[:len(name)]):
            device = dev
            break
        if not args.name and any(u in SCAN_UUIDS for u in uuids):
            device = dev
            break
    if device is None:
        die(f"no board advertising {want!r} found. Is it powered and out of a Matter session?")

    print(f"connecting to {device.name or device.address}")
    # services=[...] on purpose: CoreBluetooth aborts with CBError 8 while
    # enumerating one of the reader's own characteristics if it is allowed to
    # discover everything. Same trap as ultrawidelock_push.py.
    async with BleakClient(device, services=[SMP_SVC_UUID]) as client:
        smp = Smp(client)
        await client.start_notify(SMP_CHR_UUID, smp.on_notify)

        state = await smp.call(OP_READ_REQ, GRP_IMG, IMG_ID_STATE, {})
        for img in state.get("images", []):
            print(
                f"  slot {img.get('slot')}: v{img.get('version')} "
                f"sha={img.get('hash', b'')[:8].hex()} "
                f"active={img.get('active')} confirmed={img.get('confirmed')}"
            )
        if args.expect:
            want = image_sha(args.expect)
            got = next((i.get("hash", b"") for i in state.get("images", [])), b"")
            if got != want:
                die(
                    f"the board is NOT running that image.\n"
                    f"  board  {got.hex()}\n"
                    f"  wanted {want.hex()}\n"
                    f"  The update did not land, so the deployed record must not move:\n"
                    f"  a delta built from the wrong base is refused by the board."
                )
            print(f"  confirmed: the board is running {want[:8].hex()}...")
            return

        if args.list:
            return

        blob = Path(args.patch).read_bytes()
        if blob[:4] == b"WDFU":
            print("  raw .wdfu")
        elif struct.unpack_from("<I", blob, 0)[0] == 0x96F3B83D:
            # The phone-facing file. Sending it here is the point: it exercises
            # the board's wrapper-skipping on exactly the bytes the app sends.
            hdr_sz, img_sz = struct.unpack_from("<H", blob, 8)[0], struct.unpack_from("<I", blob, 12)[0]
            print(f"  mcuboot-wrapped: {hdr_sz} B header + {img_sz} B patch + {len(blob)-hdr_sz-img_sz} B TLV")
            if blob[hdr_sz : hdr_sz + 4] != b"WDFU":
                die("wrapped file has no WDFU magic at its payload offset")
        else:
            die(f"{args.patch} is neither a .wdfu nor an MCUboot image. Fix: make fota")

        # Leave room for the SMP header and the CBOR keys around the data. The
        # board's netbuf is 512 B, so this is bounded by the ATT MTU, not by it.
        mtu = getattr(client, "mtu_size", 0) or 185
        chunk = max(64, mtu - 80)

        print(f"pushing {len(blob)} B in {chunk} B chunks")
        off = 0
        while off < len(blob):
            body = {"image": 0, "off": off}
            if off == 0:
                body["len"] = len(blob)
            body["data"] = blob[off : off + chunk]

            rsp = await smp.call(OP_WRITE_REQ, GRP_IMG, IMG_ID_UPLOAD, body)
            nxt = rsp.get("off")
            if nxt is None:
                die("no off in the upload response")
            if nxt == off:
                die("the board is not advancing; it refused the chunk silently")
            off = nxt
            print(f"\r  {off}/{len(blob)} B", end="", flush=True)
        print("\n staged.")

        if args.no_reset:
            print(" not resetting (--no-reset). The patch applies at the next boot.")
            return

        print(" resetting; MCUboot will take 17-31 s to apply the patch")
        try:
            await smp.call(OP_WRITE_REQ, GRP_OS, OS_ID_RESET, {}, timeout=5.0)
        except SystemExit:
            # A board that reboots before answering is the expected case, not a
            # failure: the reset response races the reset itself.
            pass


def main():
    ap = argparse.ArgumentParser(description="Push a delta patch over SMP, as a phone would.")
    ap.add_argument("patch", nargs="?", help="the .wdfu patch from scripts/ultrawidelock_patch.py")
    ap.add_argument("--list", action="store_true", help="read the image list and stop")
    ap.add_argument(
        "--expect",
        metavar="SIGNED_BIN",
        help="check the board is running this image's SHA-256, then stop",
    )
    ap.add_argument("--name", default="", help="substring of the advertised name")
    # 12 s, not 8. MEASURED: two consecutive 8 s scans missed a board sitting at
    # -60 dBm that a 12 s scan then found immediately. CoreBluetooth filters
    # duplicates across back-to-back discovery sessions, so a board that is
    # plainly present can simply not be reported to a short window -- and the
    # failure reads as "the board is not advertising", which is the wrong
    # conclusion entirely.
    ap.add_argument("--scan", type=float, default=12.0, help="scan seconds")
    ap.add_argument("--no-reset", action="store_true", help="stage without rebooting")
    args = ap.parse_args()

    if not args.list and not args.expect and not args.patch:
        ap.error("give a patch file, or --list, or --expect")

    asyncio.run(run(args))


if __name__ == "__main__":
    main()
