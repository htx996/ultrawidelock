#!/usr/bin/env python3
# Copyright (c) 2026 UltraWideLock contributors
# SPDX-License-Identifier: ISC
#
# Drift gate for the walk-up digital twin web page: every constant that is
# still JavaScript in web-twin/index.html carries a firmware citation
# (`NAME: value, // path:line`); this script re-reads each cited line from the
# C tree and fails if the value is no longer on it (or, for #define lines, if
# the name moved too). The range store / trust gate need no gate at all — they
# ARE the firmware, compiled to WASM (twin.js) — so what remains here is the
# ESP32 walk-up controller port and the world pacing.
#
# Usage: python3 web-twin/check_constants.py   (from anywhere; exits nonzero on drift)

"""Verify that the web-twin's hardcoded firmware constants in index.html stay synchronized with their source definitions. Parses the FW table, reads the cited source lines, and reports any mismatches or missing citations."""

import re
import sys
from pathlib import Path

# The repository root, which is what the citations are relative to. This read
# parent.parent -- web/ -- for as long as the gate has existed, so every cited
# path resolved to web/apps/... or web/modules/..., nothing could be read, and
# the check could only ever have failed. It is wired into `make docs-check`
# now, so it would no longer fail quietly.
ROOT = Path(__file__).resolve().parents[2]
HTML = Path(__file__).resolve().parent / "index.html"

ENTRY = re.compile(r"^\s*([A-Z][A-Z0-9_]*):\s*(-?\d+),\s*// ([\w./-]+):(\d+)\s*$")


def value_on_line(value: str, line: str) -> bool:
    """Return true if the given value appears on the line as a standalone literal (not embedded in a longer number or identifier), allowing C integer suffixes like u."""
    # Match the literal with C integer suffixes allowed (192u) but reject it
    # embedded in a longer number or identifier (300, x30, 1.30).
    return re.search(r"(?<![\w.])" + re.escape(value) + r"(?![0-9.])", line) is not None


def main() -> int:
    """Parse the FW constant table from the web-twin index.html and verify that each cited constant matches the exact line and value in the source tree. Reports which constants drifted and returns 1 if any do; 0 if all are in sync."""
    text = HTML.read_text(encoding="utf-8")
    m = re.search(r"const FW = \{(.*?)\n\};", text, re.S)
    if not m:
        print("FAIL: no FW constant table found in index.html")
        return 1

    entries = [e for e in (ENTRY.match(ln) for ln in m.group(1).splitlines()) if e]
    if len(entries) < 8:
        print(f"FAIL: only {len(entries)} parsable FW entries — citation format drifted?")
        return 1

    bad = 0
    for e in entries:
        name, value, path, lineno = e.group(1), e.group(2), e.group(3), int(e.group(4))
        src = ROOT / path
        try:
            line = src.read_text(encoding="utf-8").splitlines()[lineno - 1]
        except (OSError, IndexError):
            print(f"FAIL {name}: cannot read {path}:{lineno}")
            bad += 1
            continue
        if not value_on_line(value, line):
            print(f"FAIL {name}: {value} not on {path}:{lineno}: {line.strip()!r}")
            bad += 1
            continue
        if "#define" in line and not re.search(r"\b" + re.escape(name) + r"\b", line):
            print(f"FAIL {name}: name missing from {path}:{lineno}: {line.strip()!r}")
            bad += 1
            continue
        print(f"  ok {name} = {value}  ({path}:{lineno})")

    if bad:
        print(f"\n{bad} constant(s) drifted from the firmware — re-cite index.html")
        return 1
    print(f"\nall {len(entries)} twin constants match the firmware tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
