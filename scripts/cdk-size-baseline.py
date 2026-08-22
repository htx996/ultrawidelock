#!/usr/bin/env python3
"""cdk-size-baseline.py — turn a size report into the committed baseline.

    scripts/cdk-size-baseline.py --from build/cdk-matter/size-report.json \
                                 --out apps/dwm3001cdk-lock/size-baseline.json
    make cdk-size-baseline

Two things happen here and both matter.

VOLATILE FIELDS ARE DROPPED. A timestamp and a build directory change on every
run, so carrying them would make the committed file churn on every refresh and
bury the numbers that actually moved in a diff nobody reads. The commit is kept:
it is what makes the record auditable.

THE GATE SETTINGS SURVIVE A REFRESH. Floor and cap are a decision about how much
headroom this board must keep, not a measurement, so re-recording the numbers
must not quietly reset them -- which is exactly how a floor ratchets down to
meet whatever the image happens to weigh today. They are only ever changed by
editing the file or passing them here explicitly.
"""

import argparse
import importlib.util
import json
import os
import sys

VOLATILE = ("generated", "build_dir")

# THE CONFIGURATION THE GATE TRACKS is the one that ships: SMP=1 RELEASE=1 with
# LTO on, which is what `make release` builds (mk/cdk.mk:564) and what `make
# fota` pushes over Bluetooth. That image is the one that has to fit a board in
# somebody's door, so it is the one whose headroom is worth blocking a merge
# over. Bare `make build` -- the debug image, 8 KB of RTT ring and no mcumgr --
# is recorded too, because it is what a contributor measures locally, but it is
# a different build with a different budget and is never compared to this one.
PRIMARY = "thread+release+smp+lto"

# Shared with the comparator rather than duplicated: the two must agree on what
# names a configuration, or a baseline gets written under a key nothing reads.
_spec = importlib.util.spec_from_file_location(
    "cdk_size_compare",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "cdk-size-compare.py"),
)
_cmp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_cmp)
config_key = _cmp.config_key

# Defaults for a baseline that has none yet. Deliberately not derived from the
# measurement: a floor computed from the current image is not a floor, it is a
# restatement, and it would ratchet down every time someone spent headroom.
#
# 4,096 B of free RAM is one page and roughly two thread stacks' worth of slack
# on this image; below that a walk-up unlock is one unlucky allocation from
# failing. 2,048 B per pull request is about a quarter of the remaining RAM
# headroom, which is large enough not to trip on ordinary work and small enough
# that spending the rest takes a conversation.
#
# The caps are explicit review thresholds rather than values derived from the
# latest image. At the 2026-08-16 shipping measurement, FLASH has 36,304 B free
# and RAM has 10,332 B free. RELEASE gives up the 8 KB debug RTT ring, so the
# relative constraint changes between configurations. The fixed 4,096 B flash
# and 2,048 B RAM caps ensure a single change cannot silently consume a large
# share of either remaining margin.
DEFAULT_GATE = {
    "ram_free_floor": 4096,
    "flash_free_floor": 8192,
    "ram_delta_cap": 2048,
    "flash_delta_cap": 4096,
}


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--from", dest="src", required=True)
    ap.add_argument("--out", default=os.path.join(root, "firmware", "size-baseline.json"))
    ap.add_argument(
        "--name",
        help="which configuration this records (default: derived from the overlay set)",
    )
    ap.add_argument("--ram-free-floor", type=int)
    ap.add_argument("--flash-free-floor", type=int)
    ap.add_argument("--ram-delta-cap", type=int)
    ap.add_argument("--flash-delta-cap", type=int)
    args = ap.parse_args()

    if not os.path.isfile(args.src):
        sys.stderr.write(f"\n  no report at {args.src}  ·  run `make cdk-size` first\n\n")
        return 2
    with open(args.src, "r", errors="replace") as fh:
        report = json.load(fh)

    # One file, one entry per configuration. Recording the shipping image must
    # not delete the record of the debug one, and vice versa: they are different
    # builds with different budgets, and each is only ever compared to itself.
    doc = {"schema": 2, "primary": PRIMARY, "baselines": {}}
    if os.path.isfile(args.out):
        with open(args.out, "r", errors="replace") as fh:
            old = json.load(fh)
        if isinstance(old.get("baselines"), dict):
            doc = old
            doc.setdefault("primary", PRIMARY)
        elif old.get("config"):
            # Migrate a single-configuration file rather than dropping it.
            doc["baselines"][config_key(old.get("config"))] = old
    doc["schema"] = 2

    key = args.name or config_key(report.get("config"))

    # A Zephyr report names RAM and FLASH and takes the tuned defaults above.
    # An ESP-IDF report names linker segments (dram0_0_seg, iram0_0_seg, flash
    # cache windows); there the internal SRAM segments get the RAM-style
    # defaults and the multi-megabyte flash windows are left ungated -- flash
    # headroom on those parts is a partition-table question (`make esp-size`),
    # not a linker-region one.
    regions = report.get("regions", {})
    if not regions or any(r.upper() in ("RAM", "FLASH") for r in regions):
        gate = dict(DEFAULT_GATE)
    else:
        gate = {}
        for rname, reg in regions.items():
            if reg.get("size", 0) <= 4 * 1024 * 1024 and reg.get("used", 0) >= 4096:
                gate[f"{rname.lower()}_free_floor"] = DEFAULT_GATE["ram_free_floor"]
                gate[f"{rname.lower()}_delta_cap"] = DEFAULT_GATE["ram_delta_cap"]
    gate.update(doc["baselines"].get(key, {}).get("gate", {}))
    for field in DEFAULT_GATE:
        override = getattr(args, field, None)
        if override is not None:
            gate[field] = override

    for field in VOLATILE:
        report.pop(field, None)
    report["gate"] = gate

    ram = report.get("regions", {}).get("RAM", {})
    if ram and ram.get("free", 0) < gate["ram_free_floor"]:
        sys.stderr.write(
            f"\n  refusing to record a baseline that is already below its own floor:\n"
            f"      {ram['free']:,} B of RAM free against a {gate['ram_free_floor']:,} B floor.\n"
            "      Recording this would make the gate pass by lowering the bar to meet the\n"
            "      image, which is the one failure mode a baseline exists to prevent. Free\n"
            "      RAM first, or change the floor deliberately and say why in the commit.\n\n"
        )
        return 1

    flash = report.get("regions", {}).get("FLASH", {})
    flash_free = (flash.get("signing") or {}).get("free", flash.get("free"))
    if flash is not None and flash_free is not None and flash_free < gate["flash_free_floor"]:
        basis = (
            "against what MCUboot accepts"
            if flash.get("signing")
            else "against the linker region"
        )
        sys.stderr.write(
            f"\n  refusing to record a baseline that is already below its own floor:\n"
            f"      {flash_free:,} B of FLASH free {basis}, against a "
            f"{gate['flash_free_floor']:,} B floor.\n"
            "      Recording this would make the gate pass by lowering the bar to meet the\n"
            "      image, which is the one failure mode a baseline exists to prevent. Free\n"
            "      flash first, or pass --flash-free-floor deliberately and say why in the\n"
            "      commit -- a bench-only profile may legitimately run closer to the edge\n"
            "      than a shipping one, but that has to be a decision, not a side effect.\n\n"
        )
        return 1

    doc["baselines"][key] = report
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w") as fh:
        json.dump(doc, fh, indent=2, sort_keys=False)
        fh.write("\n")

    regions = report.get("regions", {})
    sys.stderr.write(f"\n  baseline written  ·  {args.out}\n")
    sys.stderr.write(f"      configuration  {key}")
    sys.stderr.write("  (the one CI gates)\n" if key == PRIMARY else "\n")
    others = sorted(k for k in doc["baselines"] if k != key)
    if others:
        sys.stderr.write(f"      also recorded  {', '.join(others)}\n")
    gated = sorted({k[: -len("_free_floor")] for k in gate if k.endswith("_free_floor")})
    by_lower = {k.lower(): k for k in regions}
    for low in gated:
        name = by_lower.get(low, low.upper())
        r = regions.get(name)
        if r:
            # The signing budget where there is one: printing the region's free
            # bytes next to a floor that is checked against a smaller number is
            # how the two got out of step in the first place.
            g = r.get("signing") or r
            note = "  (what MCUboot accepts)" if r.get("signing") else ""
            sys.stderr.write(
                f"      {name:<12} {g['used']:>9,} used   {g['free']:>8,} free   "
                f"{g['pct']}%{note}\n"
            )
    sys.stderr.write(
        "      floor  " + ", ".join(
            f"{by_lower.get(low, low.upper())} {gate[f'{low}_free_floor']:,} B free"
            for low in gated) + "\n"
        + "      cap    " + ", ".join(
            f"{by_lower.get(low, low.upper())} +{gate[f'{low}_delta_cap']:,} B"
            for low in gated if f"{low}_delta_cap" in gate) + "\n\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
