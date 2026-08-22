#!/usr/bin/env python3
"""cdk-size-compare.py — head against the recorded baseline, as a gate.

    scripts/cdk-size-compare.py --baseline apps/dwm3001cdk-lock/size-baseline.json \
                                --current build/cdk-matter/size-report.json
    make cdk-size-check

Exit 0 when the image still fits with room to spare, 1 on a floor or cap
violation, 2 when there is nothing to compare, and 3 when the two reports were
not built the same way.

THREE IS NOT A SOFTER ONE. A size delta measured across a toolchain bump, an
overlay change or an LTO flip is not a delta: LTO alone is worth 41,084 B of
flash on this image (mk/cdk.mk), which would swamp every real signal in either
direction. So a configuration difference REFUSES TO PRODUCE A NUMBER rather
than producing a misleading one, and the fix is to refresh the baseline, not to
widen the cap.

THE FLOOR IS THE GATE, and it is expressed in free bytes. The CDK image runs at
roughly 95% of a 128 KB part, where a 644 B regression moves the percentage by
half a point and reads as rounding. Percentages are printed for orientation and
nothing is decided on them.

TOP MOVERS ARE A DIAGNOSTIC AND NEVER FAIL A BUILD. Under LTO the symbol names
are not stable across builds (see normalise_symbol in cdk-size.py), so the
attribution below is indicative: it tells you where to look, not what happened.
"""

import argparse
import importlib.util
import json
import os
import sys

# ---- which recorded configuration this build is ------------------------------
# THE BASELINE HOLDS MORE THAN ONE, because more than one is a real build. The
# shipping image is SMP=1 RELEASE=1 with LTO on -- what `make release` and
# `make fota` produce (mk/cdk.mk:564) -- and it is the one that has to fit a
# customer's board, so it is the one CI gates. But bare `make build` is what a
# contributor runs all day, and if the only record were the shipping one their
# local check would refuse to say anything at all. Both are recorded, keyed by
# the overlay set that distinguishes them, and each is only ever compared
# against itself.
#
# RELEASE and SMP are not small: RELEASE gives up the 8 KB RTT ring for 7,168 B
# of RAM and SMP costs 3,712 B for mcumgr, so the two images differ by thousands
# of bytes of headroom in opposite directions. One record covering both would be
# a number that describes neither.


# Imported rather than re-implemented: cdk-size.py writes the reports, so it
# owns what names a configuration. Two copies of this would drift into a
# baseline recorded under one key and looked up under another, which reads as
# "no baseline for this configuration" and is very hard to see.
_spec = importlib.util.spec_from_file_location(
    "cdk_size",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "cdk-size.py"),
)
_sz = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_sz)
config_key = _sz.config_key


def baselines_of(doc):
    """Every recorded configuration, as {key: baseline}."""
    if isinstance(doc.get("baselines"), dict):
        return doc["baselines"]
    # A file written before the baseline held more than one configuration. Read
    # rather than rejected: an old record is still a true measurement.
    return {config_key(doc.get("config")): doc}

# Everything that changes the numbers without any source file changing. Compared
# exactly, and any difference stops the comparison. Read out of the build by
# cdk-size.py, not out of the make variables someone typed, because a build
# directory reused with different -D flags keeps the configuration it was
# configured with (`-p auto` does not re-run CMake on a flag change).
CONFIG_FIELDS = (
    "board",
    "image",
    "extra_conf_file",
    "ncs_version",
    "zephyr_version",
    "toolchain",
)


def load(path, what):
    if not path or not os.path.isfile(path):
        sys.stderr.write(
            f"\n  no {what} report at {path}\n"
            "      This gate does not pass without both sides. \"nothing to compare\" and\n"
            "      \"compared, unchanged\" are different answers.\n\n"
        )
        return None
    with open(path, "r", errors="replace") as fh:
        return json.load(fh)


def config_diff(base, cur):
    """Every way the two builds were not the same build."""
    out = []
    b, c = base.get("config", {}), cur.get("config", {})
    for field in CONFIG_FIELDS:
        if b.get(field) != c.get(field):
            out.append((field, b.get(field), c.get(field)))
    bk, ck = b.get("kconfig", {}), c.get("kconfig", {})
    for key in sorted(set(bk) | set(ck)):
        if bk.get(key) != ck.get(key):
            # Absence is a value: an overlay that failed to apply shows up here
            # as "y" -> None, which is exactly the case worth stopping for.
            out.append((key, bk.get(key), ck.get(key)))
    return out


def fmt(n):
    if n is None:
        return "n/a"
    return f"{n:,}"


def signed(n):
    if n is None:
        return "n/a"
    return f"{n:+,}"


def movers(base, cur, limit):
    """Symbols whose size changed, largest absolute change first."""
    bs, cs = base.get("symbols", {}), cur.get("symbols", {})
    rows = []
    for name in set(bs) | set(cs):
        before, after = bs.get(name, 0), cs.get(name, 0)
        if before != after:
            rows.append((after - before, name, before, after))
    rows.sort(key=lambda r: -abs(r[0]))
    return rows[:limit]


def gate_region(name, base, cur, floor, cap, allow_growth):
    """Compare one region. Returns (row, failures)."""
    b = base["regions"].get(name)
    c = cur["regions"].get(name)
    if not b or not c:
        return None, [f"{name}: absent from one of the two reports"]

    delta = c["used"] - b["used"]

    # THE FLOOR IS CHECKED AGAINST WHAT MCUBOOT ACCEPTS, not against the linker
    # region, whenever the report carries that figure. The region is the `app`
    # partition; the slot also has to hold the image header, the signature TLVs
    # and the boot trailer, and imgtool refuses at sign time when the total does
    # not fit. On this board that gap is 1,735 B, so gating on the region passed
    # images that could not ship -- MEASURED 2026-08-21, when the debug client
    # image linked with 456 B "free" and then would not sign.
    #
    # Optional on purpose: a report produced without signed artifacts, and every
    # synthetic report in tests/tooling/cdk_size_test.sh, has no `signing` block
    # and falls back to the region. The gate gets stricter when it can, never
    # unavailable when it cannot.
    c_sign = c.get("signing") or {}
    b_sign = b.get("signing") or {}
    free_now = c_sign.get("free", c["free"])
    free_was = b_sign.get("free", b["free"])
    free_basis = "against what MCUboot accepts" if c_sign else "against the linker region"

    row = {
        "region": name,
        "size": c["size"],
        "base_used": b["used"],
        "used": c["used"],
        "delta": delta,
        "base_free": b["free"],
        "free": c["free"],
        "pct": c["pct"],
        "gated_free": free_now,
        "base_gated_free": free_was,
    }
    fails = []
    if c["size"] != b["size"]:
        fails.append(
            f"{name}: the region itself changed size, {fmt(b['size'])} -> {fmt(c['size'])} B. "
            "The partition layout moved, so used/free are not comparable across it."
        )
    if floor is not None and free_now < floor:
        fails.append(
            f"{name}: {fmt(free_now)} B free {free_basis}, under the "
            f"{fmt(floor)} B floor (was {fmt(free_was)} B). This is the gate."
        )
    if cap is not None and delta > cap:
        if allow_growth:
            row["waived"] = True
        else:
            fails.append(
                f"{name}: grew {fmt(delta)} B against a {fmt(cap)} B cap. "
                "Justify it and refresh the baseline in the same pull request, or set "
                "CDK_SIZE_ALLOW_GROWTH=1 if the growth is understood and intended."
            )
    return row, fails


def markdown(cur, base, rows, moved, fails, cfg_diff, allow_growth):
    out = []
    out.append("## DWM3001CDK image size\n")
    cfg = cur.get("config", {})
    out.append(
        f"`{cfg.get('board')}` · image `{cfg.get('image')}` · overlays "
        f"`{cfg.get('extra_conf_file')}` · NCS {cfg.get('ncs_version')}\n"
    )
    base_commit = (base.get("commit") or "unknown")[:12]
    cur_commit = (cur.get("commit") or "unknown")[:12]
    out.append(f"baseline `{base_commit}` → head `{cur_commit}`\n")

    if cfg_diff:
        out.append("\n### Not comparable\n")
        out.append(
            "\nThese two images were not built the same way, so no delta is reported. "
            "A size difference measured across a configuration change is not a size "
            "difference.\n\n"
        )
        out.append("| field | baseline | head |\n|---|---|---|\n")
        for field, b, c in cfg_diff:
            out.append(f"| `{field}` | `{b}` | `{c}` |\n")
        return "".join(out)

    out.append("\n| region | size | baseline used | head used | delta | free | used% |\n")
    out.append("|---|---:|---:|---:|---:|---:|---:|\n")
    for r in rows:
        flag = " ⚠️" if r.get("waived") else ""
        out.append(
            f"| {r['region']} | {fmt(r['size'])} | {fmt(r['base_used'])} | "
            f"{fmt(r['used'])} | **{signed(r['delta'])}**{flag} | "
            f"**{fmt(r['free'])}** | {r['pct']}% |\n"
        )

    cross = cur.get("crosscheck") or {}
    if cross:
        out.append("\n<details><summary>Cross-check against Zephyr's own reports</summary>\n\n")
        out.append("| | linker regions | Zephyr report | delta |\n|---|---:|---:|---:|\n")
        for kind, c in cross.items():
            key = "ram_report" if kind == "ram" else "rom_report"
            out.append(
                f"| {kind} | {fmt(c['regions'])} | {fmt(c[key])} | {signed(c['delta'])} |\n"
            )
        out.append(
            "\nThese measure slightly different things and are not expected to be equal; "
            "both are shown rather than one being picked.\n</details>\n"
        )

    if moved:
        out.append("\n<details><summary>Top movers (indicative — see note)</summary>\n\n")
        out.append("| symbol | baseline | head | delta |\n|---|---:|---:|---:|\n")
        for delta, name, before, after in moved:
            out.append(f"| `{name}` | {fmt(before)} | {fmt(after)} | {signed(delta)} |\n")
        out.append(
            "\nSymbol attribution under LTO is **indicative, not exact**. GCC emits "
            "`.lto_priv.N`, `.constprop.N` and `.isra.N` clones whose numbering shifts "
            "between builds for unrelated reasons; those suffixes are normalised away "
            "before diffing, which merges clones back onto one name and can move bytes "
            "between entries. Use this to decide where to look. Nothing here fails a "
            "build.\n</details>\n"
        )

    out.append("\n")
    if fails:
        out.append("### ❌ Blocked\n\n")
        for f in fails:
            out.append(f"- {f}\n")
    elif allow_growth and any(r.get("waived") for r in rows):
        out.append("### ⚠️ Passed with the growth cap waived\n")
    else:
        out.append("### ✅ Within budget\n")
    return "".join(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--baseline", default=os.path.join(root, "firmware", "size-baseline.json"))
    ap.add_argument("--current", required=True)
    ap.add_argument("--summary", help="append a markdown report here ($GITHUB_STEP_SUMMARY)")
    ap.add_argument("--movers", type=int, default=15)
    ap.add_argument(
        "--allow-growth", action="store_true",
        default=os.environ.get("CDK_SIZE_ALLOW_GROWTH", "") not in ("", "0", "n", "no"),
        help="waive the delta cap (never the free-bytes floor)",
    )
    args = ap.parse_args()

    doc = load(args.baseline, "baseline")
    cur = load(args.current, "current")
    if doc is None or cur is None:
        return 2

    recorded = baselines_of(doc)
    key = config_key(cur.get("config"))
    base = recorded.get(key)
    if base is None:
        sys.stderr.write(
            f"\n  no baseline recorded for the {key!r} configuration.\n"
            f"      recorded: {', '.join(sorted(recorded)) or '(none)'}\n"
            "      This is a build nobody has a reference for, so there is nothing to compare\n"
            "      it against. Record one with `make cdk-size-baseline` on a known-good tree,\n"
            "      or build the configuration the gate tracks:\n"
            "        make build SMP=1 RELEASE=1\n\n"
        )
        return 3

    cfg_diff = config_diff(base, cur)
    rows, fails, moved = [], [], []

    if cfg_diff:
        sys.stderr.write(
            "\n  these two images were not built the same way, so there is no delta to report.\n"
        )
        for field, b, c in cfg_diff:
            sys.stderr.write(f"      {field}: baseline {b!r} -> head {c!r}\n")
        sys.stderr.write(
            "      A number measured across a configuration change is worse than no number:\n"
            "      LTO alone moves this image by 41,084 B. Refresh the baseline against the\n"
            "      new configuration (the baseline-refresh dispatch) rather than widening a cap.\n"
        )
        # The one mismatch that is expected rather than a regression, and it
        # happens on the FIRST CI run of a freshly recorded baseline. A record
        # written on a contributor's machine says toolchain "local"; CI says the
        # container digest. They are genuinely not comparable -- a different host
        # toolchain is a different compiler -- so the refusal is right, but
        # without this line it reads as a bug in the gate rather than as a
        # baseline that has not been recorded in CI yet.
        if any(f == "toolchain" and b == "local" for f, b, _ in cfg_diff):
            sys.stderr.write(
                "\n      The baseline says toolchain 'local', so it was recorded on a developer\n"
                "      machine and never in CI. That is the expected state for a new baseline\n"
                "      and not a regression. Record CI's own numbers once:\n"
                "        Actions -> cdk-size -> Run workflow -> refresh_baseline\n"
                "      then download the cdk-size-baseline artifact it uploads, commit it over\n"
                "      apps/dwm3001cdk-lock/size-baseline.json, and every run after that compares.\n"
            )
        sys.stderr.write("\n")
    else:
        gate = base.get("gate", {})
        # Gate whatever regions the baseline names. A Zephyr baseline names
        # ram/flash; an ESP-IDF one names the internal SRAM segments it chose
        # to gate (cdk-size-baseline.py). No gate at all falls back to the
        # historical RAM/FLASH pair.
        wanted = sorted(
            {k[: -len("_free_floor")] for k in gate if k.endswith("_free_floor")}
            | {k[: -len("_delta_cap")] for k in gate if k.endswith("_delta_cap")}
        ) or ["ram", "flash"]
        by_lower = {k.lower(): k for k in base.get("regions", {})}
        for low in wanted:
            name = by_lower.get(low, low.upper())
            row, f = gate_region(
                name, base, cur,
                gate.get(f"{low}_free_floor"),
                gate.get(f"{low}_delta_cap"),
                args.allow_growth,
            )
            if row:
                rows.append(row)
            fails.extend(f)
        moved = movers(base, cur, args.movers)

        sys.stderr.write("\n  region     size        baseline         head        delta         free\n")
        for r in rows:
            sys.stderr.write(
                f"  {r['region']:<8} {fmt(r['size']):>9}  {fmt(r['base_used']):>12}"
                f"  {fmt(r['used']):>11}  {signed(r['delta']):>11}  {fmt(r['free']):>11}\n"
            )
        sys.stderr.write("\n")
        for f in fails:
            sys.stderr.write(f"  BLOCK {f}\n")

    md = markdown(cur, base, rows, moved, fails, cfg_diff, args.allow_growth)
    if args.summary:
        with open(args.summary, "a") as fh:
            fh.write(md)
    else:
        sys.stdout.write(md)

    if cfg_diff:
        return 3
    if fails:
        return 1
    sys.stderr.write("  within budget\n\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
