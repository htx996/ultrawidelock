#!/usr/bin/env python3
# Copyright (c) 2026 UltraWideLock contributors
# SPDX-License-Identifier: ISC
#
# Drift gate for the commissioning codes printed on web-flasher/index.html.
#
# The page shows the Matter setup QR code and the 11-digit manual pairing code
# for the image it flashes. Both are STATIC: the matter-lock app builds with
# CONFIG_ENABLE_TEST_SETUP_PARAMS=y and no factory-data provider, so every board
# flashed from this page carries the same passcode and discriminator. That is
# what makes printing them possible at all, and it is also the trap: the strings
# are hand-pasted HTML, derived from constants that live in fetched upstream
# (esp-matter/CHIP), which nothing in this repository compiles or version-pins
# beyond a SHA in a workflow. Nobody would notice them going stale, and a wrong
# pairing code is not a cosmetic bug: it is a page confidently telling people to
# type digits that will never commission anything.
#
# So this recomputes both strings from the constants, re-encodes the QR payload
# the way CHIP does (Matter 1.x section 5.1.3 / 5.1.4), and fails if the page
# disagrees. Three layers, cheapest first:
#
#   1. Always: the page's two strings match what the constants produce, and the
#      committed QR image is the one that was generated from that payload (its
#      provenance comment records the payload and a sha256 of the <svg>).
#   2. Always: the in-tree half of the constants -- the BLE rendezvous flag the
#      app passes to GetQRCode() -- is still what app_main.cpp passes.
#   3. When ESP_MATTER_PATH points at a checkout: the four upstream constants
#      (passcode, discriminator, vendor id, product id) still hold there.
#
# Layer 3 is the one that catches an esp-matter bump changing a default, and it
# is the one CI cannot run, because CI has no esp-matter checkout. It runs on a
# contributor's machine and in any job that has one. Same shape as the approtect
# gate's two layers.
#
# Usage: python3 web-flasher/check_codes.py          (exits nonzero on drift)
#        python3 web-flasher/check_codes.py --svg    (regenerate the QR block)

"""Verify that the Matter commissioning codes shown on the browser flasher page still match the firmware it flashes. Recomputes the QR payload and manual pairing code from the CHIP test-setup constants, checks the page against them, and can regenerate the inline QR image."""

from __future__ import annotations

import hashlib
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# The repository root. This read HERE.parent -- web/ -- so every source path
# below resolved to web/ports/... and the gate died on a FileNotFoundError
# rather than reporting drift. Same off-by-one directory as the twin's
# check_constants.py had, and invisible for the same reason: nothing ran it.
ROOT = HERE.parents[1]
HTML = HERE / "index.html"

# --- the constants the payload is built from --------------------------------
# Upstream values, checked against their source by layer 3 below. The citations
# are paths inside an esp-matter checkout ($ESP_MATTER_PATH), not this repo.
CHIP_CFG = "connectedhomeip/connectedhomeip/src/include/platform/CHIPDeviceConfig.h"
CHIP_KCONFIG = "connectedhomeip/connectedhomeip/config/esp32/components/chip/Kconfig"

PASSCODE = 20202021  # CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE
DISCRIMINATOR = 0xF00  # CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR
VENDOR_ID = 0xFFF1  # CONFIG_DEVICE_VENDOR_ID default
PRODUCT_ID = 0x8000  # CONFIG_DEVICE_PRODUCT_ID default
VERSION = 0
COMMISSIONING_FLOW = 0  # kStandard: the code is on the device, no vendor step
RENDEZVOUS_BLE = 2  # RendezvousInformationFlag::kBLE, what the app passes

# The in-tree half: app_print_onboarding_codes() decides the transport bit.
APP_MAIN = ROOT / "apps/esp32-matter-lock/main/app_main.cpp"
APP_RENDEZVOUS = "RendezvousInformationFlags rendezvous(chip::RendezvousInformationFlag::kBLE)"

# The assumption the whole page rests on: the image has no per-device identity.
# Turning the factory-data provider on is one line in an sdkconfig.defaults, and
# it would make every printed code wrong on every board without changing a
# single constant this script reads -- the one drift the layers above cannot
# see. The defaults files never mention these symbols today (the values come
# from esp-matter's Kconfig), so the check is for the line appearing at all.
APP_DEFAULTS = sorted((ROOT / "apps/esp32-matter-lock").glob("sdkconfig.defaults*"))
FIXED_IDENTITY_BREAKERS = (
    "CONFIG_ENABLE_ESP32_FACTORY_DATA_PROVIDER=y",
    "CONFIG_SEC_CERT_COMMISSIONABLE_DATA_PROVIDER=y",
    "CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER=y",
    "CONFIG_ENABLE_TEST_SETUP_PARAMS=n",
)

BASE38 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."

# Verhoeff dihedral tables, for the manual code's check digit.
_D = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9],
    [1, 2, 3, 4, 0, 6, 7, 8, 9, 5],
    [2, 3, 4, 0, 1, 7, 8, 9, 5, 6],
    [3, 4, 0, 1, 2, 8, 9, 5, 6, 7],
    [4, 0, 1, 2, 3, 9, 5, 6, 7, 8],
    [5, 9, 8, 7, 6, 0, 4, 3, 2, 1],
    [6, 5, 9, 8, 7, 1, 0, 4, 3, 2],
    [7, 6, 5, 9, 8, 2, 1, 0, 4, 3],
    [8, 7, 6, 5, 9, 3, 2, 1, 0, 4],
    [9, 8, 7, 6, 5, 4, 3, 2, 1, 0],
]
_P = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9],
    [1, 5, 7, 6, 2, 8, 3, 0, 9, 4],
    [5, 8, 0, 3, 7, 9, 6, 1, 4, 2],
    [8, 9, 1, 6, 0, 4, 3, 5, 2, 7],
    [9, 4, 5, 3, 1, 2, 6, 8, 7, 0],
    [4, 2, 8, 6, 5, 7, 3, 9, 0, 1],
    [2, 7, 9, 3, 8, 0, 6, 4, 1, 5],
    [7, 0, 4, 6, 9, 1, 3, 2, 5, 8],
]
_INV = [0, 4, 3, 2, 1, 5, 6, 7, 8, 9]


def check_digit(digits: str) -> str:
    """Return the Verhoeff check digit for a decimal string, the last digit of a Matter manual pairing code."""
    c = 0
    for i, ch in enumerate(reversed(digits)):
        c = _D[c][_P[(i + 1) % 8][int(ch)]]
    return str(_INV[c])


def base38(data: bytes) -> str:
    """Encode bytes in Matter's base-38 alphabet: three bytes become five characters, a two-byte tail four, a one-byte tail two."""
    out = []
    for i in range(0, len(data), 3):
        chunk = data[i : i + 3]
        value = int.from_bytes(chunk, "little")
        for _ in range({3: 5, 2: 4, 1: 2}[len(chunk)]):
            out.append(BASE38[value % 38])
            value //= 38
    return "".join(out)


def qr_payload(vid=None, pid=None, rendezvous=None, disc=None, passcode=None) -> str:
    """Return the MT: onboarding payload string, packing the fields least-significant-bit first the way CHIP's QRCodeSetupPayloadGenerator does. The arguments exist so self_test() can run CHIP's own vector through this; they resolve at call time, not at definition, so the module constants stay the single source of truth."""
    vid = VENDOR_ID if vid is None else vid
    pid = PRODUCT_ID if pid is None else pid
    rendezvous = RENDEZVOUS_BLE if rendezvous is None else rendezvous
    disc = DISCRIMINATOR if disc is None else disc
    passcode = PASSCODE if passcode is None else passcode
    bits, offset = 0, 0
    for value, width in (
        (VERSION, 3),
        (vid, 16),
        (pid, 16),
        (COMMISSIONING_FLOW, 2),
        (rendezvous, 8),
        (disc, 12),
        (passcode, 27),
        (0, 4),  # padding to a byte boundary
    ):
        bits |= (value & ((1 << width) - 1)) << offset
        offset += width
    return "MT:" + base38(bits.to_bytes(offset // 8, "little"))


def manual_code(disc=None, passcode=None) -> str:
    """Return the 11-digit manual pairing code: one digit of discriminator, five of passcode plus discriminator, four of passcode, and a check digit. Arguments resolve at call time; see qr_payload."""
    disc = DISCRIMINATOR if disc is None else disc
    passcode = PASSCODE if passcode is None else passcode
    short = (disc >> 8) & 0xF  # the short (4-bit) discriminator
    chunk1 = (short >> 2) & 0x3  # bit 2 would flag a custom flow; ours is standard
    chunk2 = (passcode & 0x3FFF) | ((short & 0x3) << 14)
    chunk3 = (passcode >> 14) & 0x1FFF
    body = f"{chunk1:01d}{chunk2:05d}{chunk3:04d}"
    return body + check_digit(body)


def self_test() -> list[str]:
    """Run CHIP's own default setup payload through the encoders above and report any mismatch. The expected strings are from connectedhomeip's src/setup_payload/tests (TestHelpers.h kDefaultPayloadQRCode, TestSetupPayload.cpp TestFromStringNumericCode), so this proves the encoding here, not just the constants."""
    checks = (
        ("QR encoder", qr_payload(vid=12, pid=1, rendezvous=1, disc=128, passcode=2048), "MT:M5L90MP500K64J00000"),
        ("manual encoder", manual_code(disc=128, passcode=2048), "00204800002"),
    )
    return [f"{what} fails CHIP's test vector: got {got}, expected {want}" for what, got, want in checks if got != want]


def grouped(code: str) -> str:
    """Return the manual pairing code in the 4-3-4 grouping Apple Home and the Matter spec print."""
    return f"{code[:4]}-{code[4:7]}-{code[7:]}"


def qr_svg(payload: str) -> str:
    """Render the payload as an inline SVG QR code, one path with a horizontal run per dark segment. Needs segno, so this runs only when regenerating."""
    try:
        import segno  # noqa: PLC0415  (optional: only the --svg path needs it)
    except ImportError:
        sys.exit("--svg needs segno: python3 -m pip install segno")
    matrix = [[1 if cell else 0 for cell in row] for row in segno.make(payload, error="m").matrix]
    size = len(matrix)
    runs = []
    for y, row in enumerate(matrix):
        x = 0
        while x < size:
            if not row[x]:
                x += 1
                continue
            width = 0
            while x + width < size and row[x + width]:
                width += 1
            runs.append(f"M{x} {y}h{width}v1h-{width}z")
            x += width
    return (
        f'<svg class="qr-img" viewBox="0 0 {size} {size}" shape-rendering="crispEdges" '
        f'role="img" aria-label="Matter setup QR code for the ultrawidelock ESP32 lock">'
        f'<path fill="#141413" d="{"".join(runs)}"/></svg>'
    )


def upstream_layer(payload_ok: list[str]) -> None:
    """Re-read the four upstream constants from an esp-matter checkout when ESP_MATTER_PATH names one, and record a pass, a skip or a failure."""
    path = os.environ.get("ESP_MATTER_PATH", "")
    if not path or not (Path(path) / CHIP_CFG).is_file():
        payload_ok.append("  skip: no ESP_MATTER_PATH checkout, upstream constants unchecked")
        return
    root = Path(path)
    cfg = (root / CHIP_CFG).read_text(encoding="utf-8", errors="replace")
    kcfg = (root / CHIP_KCONFIG).read_text(encoding="utf-8", errors="replace")
    for name, want, text, pattern in (
        ("passcode", PASSCODE, cfg, r"#define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_PIN_CODE\s+(\S+)"),
        ("discriminator", DISCRIMINATOR, cfg, r"#define CHIP_DEVICE_CONFIG_USE_TEST_SETUP_DISCRIMINATOR\s+(\S+)"),
        ("vendor id", VENDOR_ID, kcfg, r"config DEVICE_VENDOR_ID\b.*?\n\s+default\s+(\S+)"),
        ("product id", PRODUCT_ID, kcfg, r"config DEVICE_PRODUCT_ID\b.*?\n\s+default\s+(\S+)"),
    ):
        m = re.search(pattern, text, re.S)
        if not m:
            payload_ok.append(f"  FAIL: {name} not found upstream; did esp-matter move it?")
        elif int(m.group(1), 0) != want:
            payload_ok.append(f"  FAIL: {name} is {m.group(1)} upstream, this script says {want} ({want:#x})")
    payload_ok.append(f"  upstream constants re-read from {path}")


def main() -> int:
    """Check the flasher page's commissioning codes against the constants, or print a fresh QR block with --svg. Returns 1 on drift."""
    payload, manual = qr_payload(), manual_code()

    if "--svg" in sys.argv[1:]:
        svg = qr_svg(payload)
        digest = hashlib.sha256(svg.encode()).hexdigest()
        print(f"<!-- QR image: {payload} sha256:{digest}")
        print("     Generated by check_codes.py --svg; regenerate rather than hand-edit. -->")
        print(svg)
        return 0

    html = HTML.read_text(encoding="utf-8")
    problems, notes = self_test(), []

    if payload not in html:
        problems.append(f"page does not show the QR payload {payload}")
    if grouped(manual) not in html:
        problems.append(f"page does not show the manual pairing code {grouped(manual)}")

    # The image: its provenance comment says which payload it was drawn from,
    # and hashing the <svg> that follows proves it is still that drawing.
    m = re.search(r"<!-- QR image: (MT:\S+) sha256:([0-9a-f]{64})\b.*?-->\s*(<svg\b.*?</svg>)", html, re.S)
    if not m:
        problems.append("no QR image with a provenance comment found on the page")
    else:
        if m.group(1) != payload:
            problems.append(f"QR image was generated from {m.group(1)}, but the payload is now {payload}")
        if hashlib.sha256(m.group(3).encode()).hexdigest() != m.group(2):
            problems.append("QR image has been edited since it was generated (sha256 mismatch)")

    if APP_RENDEZVOUS not in APP_MAIN.read_text(encoding="utf-8"):
        problems.append(f"{APP_MAIN.relative_to(ROOT)} no longer passes the BLE rendezvous flag")

    for defaults in APP_DEFAULTS:
        lines = defaults.read_text(encoding="utf-8").splitlines()
        for breaker in FIXED_IDENTITY_BREAKERS:
            if breaker in [ln.strip() for ln in lines]:
                problems.append(
                    f"{defaults.relative_to(ROOT)} sets {breaker}: the image no longer "
                    "has one fixed setup code, so no single code can be printed"
                )

    upstream_layer(notes)
    problems += [n.strip() for n in notes if "FAIL:" in n]

    if problems:
        print("FAIL: web-flasher commissioning codes have drifted")
        for p in problems:
            print(f"  {p}")
        print("  regenerate: python3 web-flasher/check_codes.py --svg")
        return 1

    print(f"web-flasher codes OK: {grouped(manual)}  {payload}")
    for n in notes:
        print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
