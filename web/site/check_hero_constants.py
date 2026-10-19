#!/usr/bin/env python3
# Copyright (c) 2026 UltraWideLock contributors
# SPDX-License-Identifier: ISC
#
# Drift gate for the landing page's hero instrument.
#
# The hero is not decoration: it computes a real distance from a real tick
# rate and compares it against the real unlock bound, so a reader who checks
# the arithmetic against the firmware should find it agrees. That only stays
# true if the constants stay true. Every value in hero.js's FW table carries a
# `// path:line` citation; this re-reads each cited line from the C tree and
# fails if the value is no longer on it.
#
# Same contract and same format as web/twin/check_constants.py, deliberately:
# one citation convention for the whole site is one thing to learn.
#
# Usage: python3 web/site/check_hero_constants.py   (from anywhere)

"""Verify the landing hero's firmware constants still match the source tree."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERO = ROOT / "web" / "assets" / "design" / "js" / "hero.js"

ENTRY = re.compile(r"^\s*([A-Z][A-Z0-9_]*):\s*(-?\d+),\s*// ([\w./-]+):(\d+)\s*$")


def value_on_line(value: str, line: str) -> bool:
    """True if `value` appears on `line` as a whole numeric literal.

    Tolerates what C actually writes: an integer suffix (499200000ULL) and a
    redundant fraction (128.0). Rejects the value being a fragment of a longer
    number or an identifier, which is the whole point -- 128 must not match
    1280, and 100 must not match a line that only mentions 1000.
    """
    pattern = r"(?<![\w.])" + re.escape(value) + r"(?:\.0+)?[uUlL]*(?![\w.\d])"
    return re.search(pattern, line) is not None


def main() -> int:
    if not HERO.is_file():
        print(f"FAIL: {HERO} not found")
        return 1
    text = HERO.read_text(encoding="utf-8")

    m = re.search(r"var FW = \{(.*?)\n  \};", text, re.S)
    if not m:
        print("FAIL: no FW constant table found in hero.js — format drifted?")
        return 1

    entries = [e for e in (ENTRY.match(ln) for ln in m.group(1).splitlines()) if e]
    if len(entries) < 3:
        print(f"FAIL: only {len(entries)} parsable FW entries — "
              "citation format drifted?")
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
        print(f"  ok {name} = {value}  ({path}:{lineno})")

    if bad:
        print(f"\n{bad} constant(s) drifted from the firmware — re-cite hero.js")
        return 1
    print(f"\nall {len(entries)} hero constants match the firmware tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
