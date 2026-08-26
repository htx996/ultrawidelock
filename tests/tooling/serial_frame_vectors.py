#!/usr/bin/env python3
"""Expected mcumgr serial frames, built from the standard library.

WHY THIS IS NOT JUST A SECOND COPY OF serial.js. The rest of the fotawire suite
compares the browser against the Python the board was proved against; there is
no Python for the serial transport, so there is nothing to diff against in the
same way. What there IS, and what makes this worth writing, is that the two
pieces most likely to be wrong have independent implementations in the standard
library:

    binascii.crc_hqx(data, 0)   IS CRC-16/XMODEM -- poly 0x1021, init 0x0000,
                                not reflected. Exactly what
                                zephyr/subsys/mgmt/mcumgr/transport/src/serial_util.c:30
                                calls crc16_itu_t(0x0000, ...). Written by
                                somebody else, decades ago, for another purpose.
    base64.b64encode            likewise.

So a CRC that is subtly the wrong variant, a byte order that is backwards, or a
base64 that pads differently cannot pass this file. What it does NOT prove is
the slicing -- the choice of 93 and 90 bytes per frame is transcribed from the C
in both places, so a misreading of the C would agree with itself. The frame
length assertions below are the guard against that, because they come from the
one number the C states outright: MCUMGR_SERIAL_MAX_FRAME = 127
(zephyr/include/zephyr/mgmt/mcumgr/transport/serial.h:28).

    tests/tooling/serial_frame_vectors.py <out.json>
"""

import base64
import binascii
import json
import sys

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

    vectors = []
    for name, body in BODIES:
        wire = encode(body)

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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
