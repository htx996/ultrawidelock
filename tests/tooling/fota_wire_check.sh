#!/usr/bin/env bash
#
# fota_wire_check.sh — the browser's copy of the update protocols still agrees
#                      with the Python the board was proved against.
#
# web/flasher/smp.js and web/flasher/uwldfu.js are hand ports of
# scripts/ultrawidelock_smp.py and scripts/ultrawidelock_push.py. Both talk to
# firmware that cannot be reached from a test machine, so nothing here can prove
# they WORK -- but the failure that actually threatens them is drift, not logic:
# someone fixes a CBOR corner or an offset in one file and the other keeps the
# old behaviour, and it is found by a board refusing an update in someone's hall.
#
# So this compares the two implementations byte for byte on vectors they both
# encode and decode, with no radio involved:
#
#   1. CBOR round trips, including the indefinite-length maps zcbor emits
#      because Zephyr does not define ZCBOR_CANONICAL -- the shape EVERY reply
#      from the board actually arrives in, and the one a naive decoder drops.
#   2. The 8-byte SMP header, big-endian, against struct.pack(">BBHHBB").
#   3. The native DFU frames, little-endian, against struct.pack("<BII").
#
# The Python side IMPORTS the shipping scripts rather than reimplementing them.
# Reimplementing would compare two copies of the same mistake.
#
# Skipped loudly, not failed, when node is missing -- same rule as twin_suite.sh.
# A skip that looked like a pass would be worse than no suite at all.
#
#   tests/tooling/fota_wire_check.sh   # run as the `fotawire` suite
#
# Exit 0 clean or skipped, 1 on any disagreement.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if ! command -v node >/dev/null 2>&1; then
	printf '\n── fota wire · SKIPPED\n'
	printf '  node is not installed, so the browser copies of the update\n'
	printf '  protocols were not compared against the Python. This suite\n'
	printf '  proved nothing on this machine.\n\n'
	exit 0
fi

printf '\n── fota wire · vectors\n'
python3 "$ROOT/tests/tooling/fota_wire_vectors.py" "$WORK/vectors.json" "$ROOT"

printf '\n── fota wire · compare\n'
node "$ROOT/tests/tooling/fota_wire_check.mjs" "$WORK/vectors.json" "$ROOT"

# The serial transport, three ways. The vectors are built from the STANDARD
# LIBRARY -- binascii.crc_hqx IS CRC-16/XMODEM, b64encode is base64 -- and the
# generator checks ultrawidelock_smp.py against them in-process before writing
# them out for the browser. So the CLI client and the page are compared to each
# other through something neither of them wrote, and a CRC of the wrong variant
# or a byte order the wrong way round cannot agree with any of it.
printf '\n── fota serial · framing\n'
python3 "$ROOT/tests/tooling/serial_frame_vectors.py" "$WORK/serial.json"
node "$ROOT/tests/tooling/serial_frame_check.mjs" "$WORK/serial.json" "$ROOT"

# The bytes being right is not the same as the ORDER being right, and the order
# is what decides whether a real update works: identify before prompting, wait
# the update window out rather than failing on it, and re-read the board's hash
# after the reset instead of assuming. That last one is why `make fota-done`
# exists as a manual step on the command line.
printf '\n── fota flow · fake board\n'
node "$ROOT/tests/tooling/fota_flow_check.mjs" "$ROOT"
