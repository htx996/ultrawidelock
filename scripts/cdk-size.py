#!/usr/bin/env python3
"""cdk-size.py — what the DWM3001CDK image costs, as a machine-readable record.

The CDK image is the constrained one: reader, DW3110 ranging, a hand-written
Matter node and an OpenThread MTD on a 128 KB-RAM nRF52833. RAM is the scarcest
resource in the project and the one most easily spent by accident, so this reads
an already-built tree and says how much of each region is left.

    scripts/cdk-size.py --build build/cdk-matter
    scripts/cdk-size.py --build build/cdk-matter --json out.json
    make cdk-size

FREE BYTES, NOT PERCENTAGES. At 97% of 128 KB a 644 B regression moves the
percentage by half a point and reads as noise; "3,891 B free" does not.

NO TOOLCHAIN REQUIRED for the headline numbers. The ELF is parsed here rather
than shelled out to arm-none-eabi-size/nm, for the same reason objcopy is
avoided: those binaries live inside the NCS toolchain, so requiring
them turns "the gate ran" into "the gate ran if you had bootstrapped". Zephyr's
own ram_report/rom_report DO need the toolchain and are folded in when they are
available (--reports), as a cross-check rather than as the only source.

REGION SIZES COME FROM THE BUILD. Origin and length are read out of the linker's
own Memory Configuration block in zephyr.map, and the partition layout out of
partitions.yml, so a pm_static change moves these numbers instead of silently
invalidating a datasheet constant hardcoded here.

Exit 0 on a report, 2 when there is no build tree to measure. Measuring is all
this does: scripts/cdk-size-compare.py is what fails a build.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
from datetime import datetime, timezone

# ---- ELF ---------------------------------------------------------------------
# Little-endian ELF32 (ARM Cortex-M). Only the four tables this needs are
# decoded: program headers for region accounting, section headers for the
# per-section table, and the symbol table for attribution.

PT_LOAD = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHF_ALLOC = 0x2

STT_OBJECT = 1
STT_FUNC = 2


class Elf:
    """Just enough ELF32 to account for what the linker placed where."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            self.blob = fh.read()
        if self.blob[:4] != b"\x7fELF":
            raise ValueError(f"{path} is not an ELF file")
        if self.blob[4] != 1 or self.blob[5] != 1:
            raise ValueError(f"{path} is not little-endian ELF32 (Cortex-M expected)")

        (phoff, shoff) = struct.unpack_from("<II", self.blob, 0x1C)
        (phentsize, phnum, shentsize, shnum, shstrndx) = struct.unpack_from(
            "<HHHHH", self.blob, 0x2A
        )
        self.segments = [
            dict(
                zip(
                    ("type", "offset", "vaddr", "paddr", "filesz", "memsz", "flags", "align"),
                    struct.unpack_from("<8I", self.blob, phoff + i * phentsize),
                )
            )
            for i in range(phnum)
        ]

        raw = [
            dict(
                zip(
                    ("name", "type", "flags", "addr", "offset", "size",
                     "link", "info", "addralign", "entsize"),
                    struct.unpack_from("<10I", self.blob, shoff + i * shentsize),
                )
            )
            for i in range(shnum)
        ]
        strtab = raw[shstrndx]
        for sec in raw:
            sec["sname"] = self._cstr(strtab["offset"] + sec["name"])
        self.sections = raw

    def _cstr(self, off):
        end = self.blob.index(b"\x00", off)
        return self.blob[off:end].decode("utf-8", "replace")

    def alloc_sections(self):
        """Every section the loader gives address space to, with its size."""
        return [s for s in self.sections if (s["flags"] & SHF_ALLOC) and s["size"]]

    def symbols(self):
        """(name, addr, size) for sized code and data symbols."""
        out = []
        for sec in self.sections:
            if sec["type"] != SHT_SYMTAB:
                continue
            strtab = self.sections[sec["link"]]
            count = sec["size"] // 16
            for i in range(count):
                (st_name, st_value, st_size, st_info, _st_other, _shndx) = struct.unpack_from(
                    "<IIIBBH", self.blob, sec["offset"] + i * 16
                )
                if not st_size:
                    continue
                if (st_info & 0xF) not in (STT_OBJECT, STT_FUNC):
                    continue
                name = self._cstr(strtab["offset"] + st_name)
                if name:
                    out.append((name, st_value, st_size))
        return out


# ---- LTO symbol names --------------------------------------------------------
# GCC renames under -flto: a static that survived inlining becomes
# `foo.lto_priv.0`, a specialised clone `foo.constprop.3`, an argument-reduced
# one `foo.isra.0`. The NUMBERS ARE NOT STABLE -- they are allocated in
# whole-program order, so an unrelated edit elsewhere renumbers them and a raw
# name diff then reports hundreds of symbols that did not change. Stripping the
# suffix merges the clones back onto the symbol they came from, which is why
# attribution here is reported as indicative rather than exact.
_LTO_SUFFIX = re.compile(
    r"(\.(lto_priv|constprop|isra|part|cold|localalias|llvm)(\.\d+)?)+$|(\.\d+)+$"
)


def normalise_symbol(name):
    prev = None
    while prev != name:
        prev = name
        name = _LTO_SUFFIX.sub("", name)
    return name


# ---- linker memory configuration ---------------------------------------------
# The authority on how big each region is. Read from the map rather than from
# the nRF52833 datasheet, because on this board the FLASH region is the
# mcuboot_primary_app PARTITION (0x69e00), not the part's 512 KB -- so a
# pm_static.yml change has to move this number, and a hardcoded one would not.
_REGION_RE = re.compile(r"^(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s*(\S*)\s*$")

# Not a real memory region: the linker uses IDT_LIST as scratch address space at
# a sentinel address and discards it, so it has no capacity to run out of.
PSEUDO_REGIONS = {"*default*", "IDT_LIST"}


def read_memory_config(map_path):
    regions = {}
    if not os.path.isfile(map_path):
        return regions
    with open(map_path, "r", errors="replace") as fh:
        in_block = False
        for line in fh:
            line = line.rstrip("\n")
            if line.startswith("Memory Configuration"):
                in_block = True
                continue
            if not in_block:
                continue
            if line.startswith("Linker script and memory map"):
                break
            if not line.strip() or line.startswith("Name "):
                continue
            m = _REGION_RE.match(line)
            if not m:
                continue
            name, origin, length, attrs = m.groups()
            if name in PSEUDO_REGIONS:
                continue
            regions[name] = {
                "origin": int(origin, 16),
                "size": int(length, 16),
                "attributes": attrs,
            }
    return regions


# ---- region accounting -------------------------------------------------------
# TWO INDEPENDENT METHODS, on purpose, because a size gate that measures the
# wrong thing is worse than no gate.
#
#   segments  PT_LOAD program headers, reproducing what GNU ld charges to each
#             region for --print-memory-usage -- the "Memory region  Used Size"
#             line at the end of a build. This is the headline.
#   sections  SHF_ALLOC section headers bucketed by address, which is what
#             `arm-none-eabi-size -A` shows.
#
# SUMMING SECTION SIZES IS THE WRONG ANSWER, and establishing that took checking
# against two real links rather than reasoning about it. ld reports
# `region->current - region->origin`: how far the placement pointer advanced,
# which INCLUDES the alignment padding between sections. So the figure is the
# span from the region origin to the highest byte placed, not the sum of what
# was placed. On this build's MCUboot image the two differ by 4 B (19,968
# against 19,964), which is padding between `device_states` and `bss`; on the
# application image by 6,676 B, which is the .data load image plus padding.
#
# max(filesz, memsz) FOR THE LOAD VIEW is the other half of the agreement.
# .ramfunc is SHT_NOBITS on the application image -- an empty reserved 8 B
# window with filesz 0 -- and ld still charges those 8 B to FLASH, because the
# section is placed `> RAM AT> FLASH`. Using filesz alone came out 8 B under.
#
# A segment's run address and its load address are charged separately, which is
# how .data is counted against RAM where it lives AND against FLASH where its
# initialiser image is stored. Both are real occupancy.
#
# VERIFIED AGAINST THE LINKER, all four regions of both images of one build,
# exact to the byte:
#   application  FLASH 410,640   RAM 124,564
#   MCUboot      FLASH  32,928   RAM  19,968
# tests/tooling/cdk_size_test.sh re-checks this against a recorded fixture, so a
# change here that stops reproducing ld is a test failure rather than a quietly
# wrong gate.


def in_region(addr, region):
    return region["origin"] <= addr < region["origin"] + region["size"]


def account_segments(elf, regions):
    high = {name: None for name in regions}

    def claim(name, end):
        if high[name] is None or end > high[name]:
            high[name] = end

    for seg in elf.segments:
        if seg["type"] != PT_LOAD:
            continue
        # Where it is stored. NOBITS output sections placed `AT>` a load region
        # reserve space there with no file content, hence max() and not filesz.
        load_size = max(seg["filesz"], seg["memsz"])
        for name, reg in regions.items():
            if load_size and in_region(seg["paddr"], reg):
                claim(name, seg["paddr"] + load_size)
            # Where it runs.
            if seg["memsz"] and in_region(seg["vaddr"], reg):
                claim(name, seg["vaddr"] + seg["memsz"])

    return {
        name: (high[name] - reg["origin"]) if high[name] is not None else 0
        for name, reg in regions.items()
    }


def load_address(elf, vaddr):
    """Where a run-time address is stored, following the segment that maps it."""
    for seg in elf.segments:
        if seg["type"] != PT_LOAD or not seg["memsz"]:
            continue
        if seg["vaddr"] <= vaddr < seg["vaddr"] + seg["memsz"]:
            return seg["paddr"] + (vaddr - seg["vaddr"])
    return vaddr


def account_sections(elf, regions):
    """The `arm-none-eabi-size -A` view: sum of allocated sections, by address.

    Returned alongside the load-image total, so the gap against the linker's own
    figure is ACCOUNTED FOR rather than merely reported. A section placed
    `> RAM AT> FLASH` shows up in RAM here and its initialiser image is charged
    to FLASH; whatever is left over after both is alignment padding.
    """
    used = {name: 0 for name in regions}
    stored = {name: 0 for name in regions}
    detail = {}
    for sec in elf.alloc_sections():
        detail[sec["sname"]] = detail.get(sec["sname"], 0) + sec["size"]
        home = None
        for name, reg in regions.items():
            if in_region(sec["addr"], reg):
                home = name
                used[name] += sec["size"]
                break
        lma = load_address(elf, sec["addr"])
        for name, reg in regions.items():
            if name != home and in_region(lma, reg):
                stored[name] += sec["size"]
                break
    return used, stored, detail


# ---- partitions --------------------------------------------------------------
# partitions.yml is a flat map of name -> {address, size, ...} with anchors that
# only ever alias lists. A three-line reader beats a PyYAML dependency that the
# NCS container has but a contributor's bare python3 may not.
_PART_KEY = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):\s*$")
_PART_VAL = re.compile(r"^  (address|size|end_address|region):\s*(\S+)\s*$")


def read_partitions(path):
    parts = {}
    if not os.path.isfile(path):
        return parts
    cur = None
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = _PART_KEY.match(line)
            if m:
                cur = m.group(1)
                parts[cur] = {}
                continue
            m = _PART_VAL.match(line)
            if m and cur:
                key, val = m.groups()
                if key == "region":
                    parts[cur][key] = val
                else:
                    try:
                        parts[cur][key] = int(val, 0)
                    except ValueError:
                        pass
    return {k: v for k, v in parts.items() if v}


# ---- build configuration -----------------------------------------------------
# THE CONFIGURATION IS PART OF THE MEASUREMENT. A delta taken across an LTO flip
# is worth 41,084 B of flash here and is not a delta at all; reporting it as one
# is worse than reporting nothing. So everything that could move the numbers
# without any source changing is recorded beside them, read out of the build
# rather than out of the make variables that were typed -- a build directory
# reused with different -D flags keeps the configuration it was configured with
# (`-p auto` does not re-run CMake on a flag change; see mk/cdk.mk).

def read_cmake_cache(path, keys):
    out = {}
    if not os.path.isfile(path):
        return out
    want = {k: None for k in keys}
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("//") or line.startswith("#"):
                continue
            name, sep, value = line.partition("=")
            if not sep:
                continue
            name = name.split(":", 1)[0]
            if name in want:
                out[name] = value
    return out


def read_kconfig(path, keys):
    out = {}
    if not os.path.isfile(path):
        return out
    want = set(keys)
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, sep, value = line.partition("=")
            if sep and name in want:
                out[name] = value.strip('"')
    return out


def read_version_header(path, macro):
    if not os.path.isfile(path):
        return None
    pat = re.compile(r'^#define\s+' + re.escape(macro) + r'\s+"?([^"\s]+)"?')
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = pat.match(line.strip())
            if m:
                return m.group(1)
    return None


# Symbols that decide how big the image is without any source file changing.
# Each is read back from the built .config, so an overlay that failed to apply
# is visible as a configuration difference rather than as an unexplained delta.
CONFIG_AXIS = [
    "CONFIG_LTO",
    "CONFIG_ISR_TABLES_LOCAL_DECLARATION",
    "CONFIG_SEGGER_RTT_BUFFER_SIZE_UP",
    "CONFIG_INIT_STACKS",
    "CONFIG_MCUMGR",
    "CONFIG_DEBUG",
    "CONFIG_LOG",
    "CONFIG_LOG_DEFAULT_LEVEL",
    "CONFIG_SPEED_OPTIMIZATIONS",
    "CONFIG_SIZE_OPTIMIZATIONS",
    "CONFIG_NO_OPTIMIZATIONS",
    "CONFIG_OPENTHREAD_DEBUG",
    "CONFIG_ULTRAWIDELOCK_MATTER_BLE",
]


def collect_config(build, image):
    kconfig = os.path.join(build, image, "zephyr", ".config")
    cache = read_cmake_cache(
        os.path.join(build, "CMakeCache.txt"), ("BOARD", "EXTRA_CONF_FILE")
    )
    axis = read_kconfig(kconfig, CONFIG_AXIS)
    gen = os.path.join(build, image, "zephyr", "include", "generated")
    cfg = {
        "board": cache.get("BOARD"),
        "image": image,
        "extra_conf_file": cache.get("EXTRA_CONF_FILE"),
        "kconfig": axis,
        "zephyr_version": read_version_header(
            os.path.join(gen, "zephyr", "version.h"), "KERNEL_VERSION_STRING"
        ),
        "ncs_version": read_version_header(
            os.path.join(gen, "ncs_version.h"), "NCS_VERSION_STRING"
        ),
        # Only CI knows which container ran, and it is the single largest
        # invisible source of drift: a toolchain bump moves every number here.
        # Recorded from the environment so a local report says "local" honestly
        # instead of claiming a digest it cannot know.
        "toolchain": os.environ.get("CDK_SIZE_TOOLCHAIN", "local"),
    }
    return cfg


def relative_to_root(path, root):
    """A build path that can be committed: never above the repo, never absolute."""
    try:
        rel = os.path.relpath(path, os.path.abspath(root))
    except ValueError:  # different drive on Windows
        return os.path.basename(path)
    # A build directory outside the checkout (ULTRAWIDELOCK_BUILD elsewhere) would come
    # back as ../../somewhere, which is still a path off this machine. The
    # basename is all the report needs to say which tree it measured.
    return os.path.basename(path) if rel.startswith("..") else rel


def git_commit(root):
    try:
        out = subprocess.run(
            ["git", "-C", root, "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=10, check=False,
        )
        return out.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


# ---- Zephyr's own reports ----------------------------------------------------
# ram_report and rom_report are what the RAM work in this repo already quotes, so
# they are the headline when they can be produced. They need west, the toolchain
# python and pyelftools, and they are a ninja target with real dependencies --
# so on a stale tree they RELINK rather than just measure. That is why they are
# opt-in and why the ELF-derived numbers above never depend on them.

def run_reports(build, image, run_prefix, timeout):
    out = {"ram": None, "rom": None, "error": None}
    imgdir = os.path.join(build, image)
    for kind in ("ram", "rom"):
        cmd = list(run_prefix) + ["build", "-d", imgdir, "-t", f"{kind}_report"]
        try:
            proc = subprocess.run(
                cmd, capture_output=True, text=True, timeout=timeout, check=False
            )
        except (OSError, subprocess.SubprocessError) as exc:
            out["error"] = f"{kind}_report: {exc}"
            continue
        if proc.returncode != 0:
            tail = (proc.stderr or proc.stdout or "").strip().splitlines()[-3:]
            out["error"] = f"{kind}_report exited {proc.returncode}: {' / '.join(tail)}"
            continue
        jpath = os.path.join(imgdir, f"{kind}.json")
        if not os.path.isfile(jpath):
            out["error"] = f"{kind}_report produced no {kind}.json"
            continue
        with open(jpath, "r", errors="replace") as fh:
            out[kind] = json.load(fh).get("total_size")
    return out


# ---- report ------------------------------------------------------------------

def build_report(args):
    build = os.path.abspath(args.build)
    image = args.image
    imgdir = os.path.join(build, image, "zephyr")
    elf_path = os.path.abspath(args.elf) if args.elf else os.path.join(imgdir, "zephyr.elf")
    map_path = (
        os.path.abspath(args.map) if args.map
        else os.path.splitext(elf_path)[0] + ".map" if args.elf
        else os.path.join(imgdir, "zephyr.map")
    )

    if not os.path.isfile(elf_path):
        sys.stderr.write(
            f"\n  no ELF at {elf_path}\n"
            "      Nothing to measure. `make build` first, or point --build at a tree that\n"
            "      holds one. \"could not measure\" and \"measured, unchanged\" are different\n"
            "      answers and this will not pass one off as the other.\n\n"
        )
        return None

    elf = Elf(elf_path)
    regions = read_memory_config(map_path)
    if not regions:
        sys.stderr.write(
            f"\n  no Memory Configuration block in {map_path}\n"
            "      Region capacities are read from the linker rather than hardcoded, so\n"
            "      without the map there is no denominator and no free-bytes figure.\n\n"
        )
        return None

    seg_used = account_segments(elf, regions)
    sec_used, sec_stored, sec_detail = account_sections(elf, regions)

    out_regions = {}
    for name, reg in regions.items():
        used = seg_used[name]
        out_regions[name] = {
            "origin": reg["origin"],
            "size": reg["size"],
            "used": used,
            "free": reg["size"] - used,
            "pct": round(100.0 * used / reg["size"], 2) if reg["size"] else None,
            # The `size -A` cross-check and what accounts for its difference
            # from the linker's figure. padding is the residual: if it ever goes
            # negative or large, one of the two methods has stopped being right
            # and the report says so instead of averaging them.
            "used_by_sections": sec_used[name],
            "load_images": sec_stored[name],
            "padding": used - sec_used[name] - sec_stored[name],
        }

    parts = read_partitions(os.path.join(build, "partitions.yml"))
    merged = os.path.join(build, "merged.hex")
    artifact = {
        "merged_hex_bytes": os.path.getsize(merged) if os.path.isfile(merged) else None,
        "partitions": parts,
    }
    signed = os.path.join(imgdir, "zephyr.signed.hex")
    artifact["signed_hex_bytes"] = (
        os.path.getsize(signed) if os.path.isfile(signed) else None
    )

    symbols = {}
    for name, addr, size in elf.symbols():
        key = normalise_symbol(name)
        if size >= args.symbol_floor:
            symbols[key] = symbols.get(key, 0) + size

    report = {
        "schema": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commit": git_commit(args.repo_root),
        # RELATIVE, and never the absolute path. This report is committed as
        # apps/dwm3001cdk-lock/size-baseline.json, and an absolute build path names the
        # machine and the user that produced it.
        # It is also the reason the numbers are not reproducible by anyone else.
        "build_dir": relative_to_root(build, args.repo_root),
        "config": collect_config(build, image),
        "regions": out_regions,
        "sections": dict(sorted(sec_detail.items(), key=lambda kv: -kv[1])),
        "artifact": artifact,
        "symbol_floor": args.symbol_floor,
        "symbols": dict(sorted(symbols.items())),
        "reports": None,
    }

    if args.reports:
        run_prefix = args.run_prefix.split() if args.run_prefix else ["west"]
        rep = run_reports(build, image, run_prefix, args.reports_timeout)
        report["reports"] = rep
        # The cross-check the prompt for this gate asks for: Zephyr's own
        # accounting against the one computed here. They are not required to be
        # equal -- size_report ignores the load image of .data, for one -- so a
        # difference is REPORTED rather than reconciled by preferring either.
        cross = {}
        if rep.get("ram") is not None and "RAM" in out_regions:
            cross["ram"] = {
                "regions": out_regions["RAM"]["used"],
                "ram_report": rep["ram"],
                "delta": out_regions["RAM"]["used"] - rep["ram"],
            }
        if rep.get("rom") is not None and "FLASH" in out_regions:
            cross["rom"] = {
                "regions": out_regions["FLASH"]["used"],
                "rom_report": rep["rom"],
                "delta": out_regions["FLASH"]["used"] - rep["rom"],
            }
        report["crosscheck"] = cross

    return report


# ---- human output ------------------------------------------------------------

def fmt(n):
    return f"{n:,}" if isinstance(n, int) else str(n)


def print_table(report, stream):
    cfg = report["config"]
    stream.write("\n  DWM3001CDK image size\n")
    stream.write(f"    board      {cfg.get('board')}\n")
    stream.write(f"    image      {cfg.get('image')}\n")
    stream.write(f"    overlays   {cfg.get('extra_conf_file')}\n")
    stream.write(
        f"    ncs        {cfg.get('ncs_version')}   zephyr {cfg.get('zephyr_version')}"
        f"   toolchain {cfg.get('toolchain')}\n"
    )
    if report.get("commit"):
        stream.write(f"    commit     {report['commit']}\n")
    stream.write("\n    region        size        used        free     used%\n")
    for name, reg in report["regions"].items():
        stream.write(
            f"    {name:<10} {fmt(reg['size']):>10}  {fmt(reg['used']):>10}"
            f"  {fmt(reg['free']):>10}   {reg['pct']:>6.2f}%\n"
        )
    stream.write("\n    cross-check against the `size -A` view (sum of allocated sections):\n")
    for name, reg in report["regions"].items():
        stream.write(
            f"    {name:<10} sections {fmt(reg['used_by_sections']):>10}"
            f"  + load images {fmt(reg['load_images']):>7}"
            f"  + padding {fmt(reg['padding']):>5}  = {fmt(reg['used']):>10}\n"
        )
        if reg["padding"] < 0 or reg["padding"] > 4096:
            stream.write(
                f"    WARNING: {name} leaves {fmt(reg['padding'])} B unaccounted between the two\n"
                "             methods. That is not alignment padding, so one of them has stopped\n"
                "             being right. Treat both numbers as suspect until this is explained.\n"
            )
    cross = report.get("crosscheck") or {}
    for kind, c in cross.items():
        key = "ram_report" if kind == "ram" else "rom_report"
        stream.write(
            f"\n    {kind}: linker regions {fmt(c['regions'])} B vs Zephyr {key}"
            f" {fmt(c[key])} B  (delta {fmt(c['delta'])} B)\n"
        )
    if report.get("reports") and report["reports"].get("error"):
        stream.write(f"\n    ram/rom_report unavailable: {report['reports']['error']}\n")
    stream.write("\n")


def config_key(config):
    """A readable name for an overlay set: the thing that makes builds differ.

    Defined here, next to the measurement, because it names a property OF a
    report; the comparator and the baseline writer both import it from here so
    a build cannot be recorded under one name and looked up under another.
    """
    conf = (config or {}).get("extra_conf_file") or ""
    parts = []
    for item in conf.split(";"):
        item = item.strip()
        if not item:
            continue
        name = os.path.basename(item)
        if name.startswith("overlay-"):
            name = name[len("overlay-"):]
        if name.endswith(".conf"):
            name = name[: -len(".conf")]
        parts.append(name)
    return "+".join(parts) if parts else "default"


def markdown(report):
    """A standalone table, for a build that measures but does not compare."""
    cfg = report["config"]
    # The configuration is the HEADING, not a detail in the subtitle: a dispatch
    # summary carries the shipping and the debug image one after the other, and
    # they differ by thousands of bytes in opposite directions on each region.
    # A reader who cannot tell at a glance which table is which is worse off
    # than one with no table.
    out = [f"## DWM3001CDK image size · `{config_key(cfg)}`\n\n"]
    out.append(
        f"`{cfg.get('board')}` · image `{cfg.get('image')}` · overlays "
        f"`{cfg.get('extra_conf_file')}` · NCS {cfg.get('ncs_version')}\n\n"
    )
    out.append("| region | size | used | **free** | used% |\n|---|---:|---:|---:|---:|\n")
    for name, reg in report["regions"].items():
        out.append(
            f"| {name} | {fmt(reg['size'])} | {fmt(reg['used'])} | "
            f"**{fmt(reg['free'])}** | {reg['pct']}% |\n"
        )
    out.append(
        "\nFree bytes, not percentages: this image runs above 92% of a 128 KB part, "
        "where a few hundred bytes moves the percentage by less than a point.\n"
    )
    return "".join(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", required=True, help="sysbuild build directory")
    ap.add_argument("--image", default="firmware", help="sysbuild image name")
    ap.add_argument("--elf", help="ELF path when the tree is not sysbuild-shaped "
                    "(default <build>/<image>/zephyr/zephyr.elf)")
    ap.add_argument("--map", help="linker map path (default beside the ELF)")
    ap.add_argument("--json", help="write the report here (default: stdout)")
    ap.add_argument("--summary", help="append a markdown table here ($GITHUB_STEP_SUMMARY)")
    ap.add_argument("--quiet", action="store_true", help="no human table")
    ap.add_argument("--repo-root", default=root)
    ap.add_argument(
        "--reports", action="store_true",
        help="also run Zephyr's ram_report/rom_report (needs the toolchain; "
             "relinks a stale tree)",
    )
    ap.add_argument("--run-prefix", default="west",
                    help="how to invoke west (e.g. the nrfutil launcher)")
    ap.add_argument("--reports-timeout", type=int, default=1800)
    ap.add_argument(
        "--symbol-floor", type=int, default=64,
        help="smallest symbol recorded for attribution, in bytes",
    )
    args = ap.parse_args()

    report = build_report(args)
    if report is None:
        return 2

    text = json.dumps(report, indent=2, sort_keys=False)
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)) or ".", exist_ok=True)
        with open(args.json, "w") as fh:
            fh.write(text + "\n")
    else:
        sys.stdout.write(text + "\n")
    if args.summary:
        with open(args.summary, "a") as fh:
            fh.write(markdown(report))
    if not args.quiet:
        print_table(report, sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
