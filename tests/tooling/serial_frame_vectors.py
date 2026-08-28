#!/usr/bin/env python3
"""Expected mcumgr serial frames, and a three-way agreement about them.

THREE IMPLEMENTATIONS HAVE TO AGREE for this file to pass, and they are
independent in the ways that matter:

    1. this file          builds the frames from the STANDARD LIBRARY.
                          binascii.crc_hqx(data, 0) IS CRC-16/XMODEM -- poly
                          0x1021, init 0x0000, not reflected -- which is exactly
                          what serial_util.c:30 calls crc16_itu_t(0x0000, ...).
                          base64.b64encode likewise. Both were written by
                          somebody else, long ago, for another purpose, so
                          neither can agree with a mistake of ours.
    2. ultrawidelock_smp  the CLI client -- the one that gets pointed at a real
                          board. Checked here, in-process, against (1).
    3. serial.js          the browser's copy. Checked by
                          serial_frame_check.mjs against the vectors written
                          out below, which are (1).

That ordering is the project's usual rule applied to a new transport: if the
browser and the script ever disagree, the script is right, because it is the one
with a board attached to it. What (1) adds is that BOTH can be wrong together
and still be caught, as long as they are wrong about the CRC, the byte order or
the base64 -- the three things a stdlib has its own opinion about.

What none of it proves is the slicing. The choice of 93 and 90 bytes per frame
is transcribed from the C in all three places, so a misreading of the C would
agree with itself. The frame-length assertions are the guard, because they come
from the one number the C states outright: MCUMGR_SERIAL_MAX_FRAME = 127
(zephyr/include/zephyr/mgmt/mcumgr/transport/serial.h:28).

    tests/tooling/serial_frame_vectors.py <out.json>
"""

import base64
import binascii
import json
import sys
from pathlib import Path

HDR_PKT = 0x0609
HDR_FRAG = 0x0414
MAX_FRAME = 127
FIRST_INPUT = ((MAX_FRAME - 3) >> 2) * 3   # 93
NEXT_INPUT = FIRST_INPUT - 3               # 90


def encode(body: bytes) -> bytes:
    """The frames a conforming sender emits for one SMP packet."""
    crc = binascii.crc_hqx(body, 0)
    # The length word counts the CRC and not itself: serial_util.c:213 sends
    # be16(len + 2).
    pkt = (len(body) + 2).to_bytes(2, "big") + body + crc.to_bytes(2, "big")

    out = bytearray()
    off, first = 0, True
    while off < len(pkt):
        take = min(FIRST_INPUT if first else NEXT_INPUT, len(pkt) - off)
        marker = HDR_PKT if first else HDR_FRAG
        out += marker.to_bytes(2, "big")
        out += base64.b64encode(pkt[off:off + take])
        out += b"\n"
        off += take
        first = False
    return bytes(out)


# The bodies. Sizes chosen for the boundaries rather than for variety. A packet
# is the body plus four bytes -- two of length, two of CRC -- and the first
# frame carries 93 of them, so body 89 is the largest that fits in one frame and
# body 90 is the first that does not.
BODIES = [
    ("empty", b""),
    ("one", b"\x2a"),
    ("smp-header", bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00])),
    ("just-under-a-frame", bytes((i * 7 + 3) & 0xFF for i in range(88))),
    ("exactly-a-frame", bytes((i * 7 + 3) & 0xFF for i in range(89))),
    ("one-past-a-frame", bytes((i * 7 + 3) & 0xFF for i in range(90))),
    ("two-frames", bytes((i * 13 + 5) & 0xFF for i in range(200))),
    ("upload-sized", bytes((i * 31 + 17) & 0xFF for i in range(425))),
]


def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2

    # The CLI client, loaded by path because scripts/ is not a package.
    import importlib.util                                       # noqa: PLC0415
    here = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location(
        "uwlsmp", here / "scripts" / "ultrawidelock_smp.py")
    uwlsmp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(uwlsmp)

    # The constants have to match before the bytes are worth comparing: a client
    # that sliced at a different width would still round-trip against itself.
    assert uwlsmp.SERIAL_HDR_PKT == HDR_PKT, uwlsmp.SERIAL_HDR_PKT
    assert uwlsmp.SERIAL_HDR_FRAG == HDR_FRAG, uwlsmp.SERIAL_HDR_FRAG
    assert uwlsmp.SERIAL_MAX_FRAME == MAX_FRAME, uwlsmp.SERIAL_MAX_FRAME
    assert uwlsmp.SERIAL_FIRST_INPUT == FIRST_INPUT, uwlsmp.SERIAL_FIRST_INPUT
    assert uwlsmp.SERIAL_NEXT_INPUT == NEXT_INPUT, uwlsmp.SERIAL_NEXT_INPUT

    vectors = []
    for name, body in BODIES:
        wire = encode(body)

        # (2) against (1): the client the board meets, against the standard
        # library. Byte for byte, not "close enough".
        mine = uwlsmp.serial_encode(body)
        if mine != wire:
            raise SystemExit(
                f"ultrawidelock_smp.serial_encode disagrees on {name}:\n"
                f"  client {mine.hex()}\n"
                f"  stdlib {wire.hex()}"
            )

        # And its decoder reads the standard library's frames, one byte at a
        # time -- which is how they arrive on a real port.
        framer = uwlsmp.SerialFramer()
        got = []
        for byte in wire:
            got += framer.feed(bytes([byte]))
        if got != [body]:
            raise SystemExit(
                f"ultrawidelock_smp.SerialFramer did not round-trip {name}: "
                f"{len(got)} packet(s)"
            )

        # Every line, markers and newline included, is within the one limit the
        # C states as a number.
        for line in wire.split(b"\n")[:-1]:
            assert len(line) + 1 <= MAX_FRAME, (name, len(line) + 1)

        vectors.append({
            "name": name,
            "body": body.hex(),
            "crc": binascii.crc_hqx(body, 0),
            "wire": wire.hex(),
        })

    # The published check value for CRC-16/XMODEM. Pins the variant itself, so
    # that "crc_hqx is the right function" is asserted rather than assumed.
    vectors_out = {
        "crc_check_value": binascii.crc_hqx(b"123456789", 0),
        "first_input": FIRST_INPUT,
        "next_input": NEXT_INPUT,
        "max_frame": MAX_FRAME,
        "vectors": vectors,
    }
    assert vectors_out["crc_check_value"] == 0x31C3, vectors_out["crc_check_value"]

    with open(sys.argv[1], "w") as fh:
        json.dump(vectors_out, fh)

    print(f"  {len(vectors)} serial vectors, crc check value 0x31C3")
    print("  ultrawidelock_smp.py agrees with the standard library, byte for byte")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
