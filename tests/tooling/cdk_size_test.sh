#!/usr/bin/env bash
#
# cdk_size_test.sh — the DWM3001CDK size gate, checked without a build tree.
#
# CI builds no firmware (see ci.yml's header), so this cannot measure a real
# image. What it can do is pin the two things that were actually got wrong while
# the gate was written, and the refusals that make its numbers trustworthy:
#
#   1. THE ACCOUNTING RULE. A first version summed PT_LOAD segment sizes and
#      reported 131,240 B of RAM used against a 131,072 B part -- a negative
#      free figure -- because Zephyr's ELF nests a covering RAM segment around
#      the .data and .ramfunc ones. ld reports the SPAN from the region origin
#      to the highest byte placed, padding included. The segment tables below
#      are the real ones from a decawave_dwm3001cdk build, and the expected
#      figures are what that build's linker printed. If account_segments stops
#      reproducing ld, this fails.
#
#   2. LTO SYMBOL NORMALISATION, without which the top-mover list is noise:
#      GCC renumbers .lto_priv/.constprop/.isra clones between builds.
#
#   3. THE COMPARATOR'S REFUSALS. A delta measured across a toolchain bump, an
#      overlay change or an LTO flip is not a delta, and reporting one is worse
#      than reporting nothing -- LTO alone is worth 41,084 B on this image. It
#      must exit non-zero rather than produce a number.
#
# Exit 0 clean, 1 on a failure.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT" || exit 1

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0
ok() { printf '  ok    %s\n' "$1"; }
bad() {
	printf '  FAIL  %s\n' "$1"
	fails=$((fails + 1))
}

# Check an exit code, naming what was expected. `set -e` is deliberately off:
# every case runs so one failure does not hide the next.
expect_exit() {
	local want="$1" got="$2" what="$3"
	if [ "$got" = "$want" ]; then ok "$what (exit $got)"; else
		bad "$what: expected exit $want, got $got"
	fi
}

printf '\n── cdk size · region accounting reproduces the linker\n'

# The real PT_LOAD tables and memory configurations of one build, with the
# figures that build's own linker printed beside them.
python3 - <<'PY'
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("cs", "scripts/cdk-size.py")
cs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cs)


def seg(vaddr, paddr, filesz, memsz):
    return {"type": 1, "vaddr": vaddr, "paddr": paddr,
            "filesz": filesz, "memsz": memsz}


class Fake:
    def __init__(self, segments):
        self.segments = segments


CASES = [
    # application image: FLASH 410,640 / RAM 124,564, per its link
    ("application",
     {"FLASH": {"origin": 0xA200, "size": 433664},
      "RAM": {"origin": 0x20000000, "size": 131072}},
     [seg(0x0000A200, 0x0000A200, 403960, 403960),
      # .ramfunc: SHT_NOBITS, filesz 0, and ld still charges its 8 B to FLASH
      seg(0x200020B8, 0x0006CBF8, 0, 8),
      seg(0x200020C0, 0x0006CC00, 6668, 6668),
      seg(0x0006E60C, 0x0006E60C, 4, 4),
      # the covering RAM segment the naive sum double-counted
      seg(0x20000000, 0x20000000, 0, 124564)],
     {"FLASH": 410640, "RAM": 124564}),
    # MCUboot image: FLASH 32,928 / RAM 19,968. RAM here is 4 B MORE than the
    # sum of its sections -- alignment padding between device_states and bss --
    # which is the case that proves the figure is a span and not a sum.
    ("mcuboot",
     {"FLASH": {"origin": 0x0, "size": 40960},
      "RAM": {"origin": 0x20000000, "size": 131072}},
     [seg(0x00000000, 0x00000000, 32680, 32680),
      seg(0x20000000, 0x00007FA8, 244, 244),
      seg(0x0000809C, 0x0000809C, 4, 4),
      seg(0x200000F8, 0x200000F8, 0, 19720)],
     {"FLASH": 32928, "RAM": 19968}),
]

rc = 0
for name, regions, segments, expected in CASES:
    got = cs.account_segments(Fake(segments), regions)
    for region, want in expected.items():
        if got[region] == want:
            print(f"  ok    {name} {region} = {want:,} B (matches the linker)")
        else:
            print(f"  FAIL  {name} {region}: linker says {want:,} B, got {got[region]:,} B")
            rc = 1

# The specific regression: summing instead of spanning overflowed the part.
regions = CASES[0][1]
used = cs.account_segments(Fake(CASES[0][2]), regions)["RAM"]
if used <= regions["RAM"]["size"]:
    print(f"  ok    RAM used ({used:,} B) does not exceed the region ({regions['RAM']['size']:,} B)")
else:
    print(f"  FAIL  RAM used {used:,} B exceeds the {regions['RAM']['size']:,} B region")
    rc = 1

for raw, want in [
    ("ot_thread_run.lto_priv.0", "ot_thread_run"),
    ("ultrawidelock_derive.constprop.12", "ultrawidelock_derive"),
    ("ccc_shim_rx.isra.3.lto_priv.9", "ccc_shim_rx"),
    ("bt_conn_cb_area.part.1", "bt_conn_cb_area"),
    ("plain_symbol", "plain_symbol"),
]:
    got = cs.normalise_symbol(raw)
    if got == want:
        print(f"  ok    {raw} -> {want}")
    else:
        print(f"  FAIL  {raw} -> {got}, expected {want}")
        rc = 1

sys.exit(rc)
PY
[ $? -eq 0 ] || fails=$((fails + 1))

printf '\n── cdk size · the comparator refuses what it cannot compare\n'

# A pair of minimal reports. Only the fields the comparator reads are present,
# which is also a check that it reads no more than it documents.
mkbase() {
	python3 - "$1" "$2" "$3" "$4" <<'PY'
import json, sys
out, ram_used, lto, toolchain = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
size = 131072
json.dump({
    "commit": "0" * 40,
    "config": {
        "board": "decawave_dwm3001cdk", "image": "firmware",
        "extra_conf_file": "overlay-thread.conf;overlay-lto.conf",
        "ncs_version": "3.3.0", "zephyr_version": "4.3.99",
        "toolchain": toolchain,
        "kconfig": {"CONFIG_LTO": lto},
    },
    "regions": {
        "RAM": {"origin": 0x20000000, "size": size, "used": ram_used,
                "free": size - ram_used, "pct": round(100 * ram_used / size, 2)},
        "FLASH": {"origin": 0xA200, "size": 433664, "used": 410640,
                  "free": 23024, "pct": 94.69},
    },
    "symbols": {"some_symbol": 128},
    "gate": {"ram_free_floor": 4096, "flash_free_floor": 8192,
             "ram_delta_cap": 2048, "flash_delta_cap": 8192},
}, open(out, "w"))
PY
}

CMP="python3 $ROOT/scripts/cdk-size-compare.py"

mkbase "$TMP/base.json" 124564 y sha256:aaaa

# unchanged
mkbase "$TMP/same.json" 124564 y sha256:aaaa
$CMP --baseline "$TMP/base.json" --current "$TMP/same.json" >/dev/null 2>&1
expect_exit 0 "$?" "a byte-identical image passes"

# under the floor: 131072 - 128000 = 3,072 B free, floor is 4,096
mkbase "$TMP/fat.json" 128000 y sha256:aaaa
$CMP --baseline "$TMP/base.json" --current "$TMP/fat.json" >/dev/null 2>&1
expect_exit 1 "$?" "free RAM under the floor is blocked"

# Over the cap but still above the floor, which is a narrow window here: the
# baseline leaves 6,508 B free, the floor takes 4,096 of it and the cap is 2,048,
# so this must land between 126,612 and 126,976 B used to isolate the cap.
# 126,800 gives +2,236 B of growth with 4,272 B still free.
mkbase "$TMP/grown.json" 126800 y sha256:aaaa
$CMP --baseline "$TMP/base.json" --current "$TMP/grown.json" >/dev/null 2>&1
expect_exit 1 "$?" "growth past the cap is blocked"

CDK_SIZE_ALLOW_GROWTH=1 $CMP --baseline "$TMP/base.json" --current "$TMP/grown.json" >/dev/null 2>&1
expect_exit 0 "$?" "CDK_SIZE_ALLOW_GROWTH waives the cap"

# ...but never the floor. That distinction is the whole point of having both.
CDK_SIZE_ALLOW_GROWTH=1 $CMP --baseline "$TMP/base.json" --current "$TMP/fat.json" >/dev/null 2>&1
expect_exit 1 "$?" "CDK_SIZE_ALLOW_GROWTH does NOT waive the floor"

# LTO flipped: worth 41,084 B on this image, so any delta reported across it
# would be a fiction. Must refuse rather than answer.
mkbase "$TMP/nolto.json" 124564 n sha256:aaaa
$CMP --baseline "$TMP/base.json" --current "$TMP/nolto.json" >/dev/null 2>&1
expect_exit 3 "$?" "an LTO difference refuses to produce a delta"

# toolchain bump, everything else equal
mkbase "$TMP/newtc.json" 124564 y sha256:bbbb
$CMP --baseline "$TMP/base.json" --current "$TMP/newtc.json" >/dev/null 2>&1
expect_exit 3 "$?" "a toolchain difference refuses to produce a delta"

# A config mismatch must outrank a floor violation: reporting "you are under the
# floor" from an incomparable pair would send someone hunting a regression that
# is really a configuration change.
mkbase "$TMP/both.json" 128000 n sha256:bbbb
$CMP --baseline "$TMP/base.json" --current "$TMP/both.json" >/dev/null 2>&1
expect_exit 3 "$?" "a config mismatch outranks a floor violation"

# Missing input is neither pass nor fail: "nothing to compare" is its own answer.
$CMP --baseline "$TMP/base.json" --current "$TMP/nope.json" >/dev/null 2>&1
expect_exit 2 "$?" "a missing report is refused, not passed"

printf '\n── cdk size · one baseline file, one entry per configuration\n'

# The shipping image (SMP=1 RELEASE=1, what `make release` builds) and the debug
# image that bare `make build` produces differ by thousands of bytes of headroom
# in opposite directions: RELEASE frees 7,168 B and SMP costs 3,712 B. Comparing
# either against the other's record would be worse than not comparing at all.
python3 - "$TMP" <<'PY'
import importlib.util
import json
import os
import subprocess
import sys

root = os.getcwd()
tmp = sys.argv[1]
spec = importlib.util.spec_from_file_location("cmp", "scripts/cdk-size-compare.py")
cmp_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cmp_mod)

rc = 0
for conf, want in [
    ("overlay-thread.conf;overlay-release.conf;overlay-smp.conf;overlay-lto.conf",
     "thread+release+smp+lto"),
    ("overlay-thread.conf;overlay-lto.conf", "thread+lto"),
    ("", "default"),
]:
    got = cmp_mod.config_key({"extra_conf_file": conf})
    if got == want:
        print(f"  ok    {conf or '(none)'} -> {want}")
    else:
        print(f"  FAIL  {conf or '(none)'} -> {got}, expected {want}")
        rc = 1


def report(path, conf, ram_used):
    size = 131072
    json.dump({
        "generated": "now", "build_dir": "cdk-x", "commit": "0" * 40,
        "config": {"board": "decawave_dwm3001cdk", "image": "firmware",
                   "extra_conf_file": conf, "ncs_version": "3.3.0",
                   "zephyr_version": "4.3.99", "toolchain": "t", "kconfig": {}},
        "regions": {
            "RAM": {"origin": 0x20000000, "size": size, "used": ram_used,
                    "free": size - ram_used,
                    "pct": round(100 * ram_used / size, 2)},
            # The gate reads both regions, so a fixture with only one makes
            # every comparison fail on "absent from one of the two reports".
            "FLASH": {"origin": 0xA200, "size": 433664, "used": 410640,
                      "free": 23024, "pct": 94.69},
        },
        "symbols": {},
    }, open(path, "w"))


SHIP = "overlay-thread.conf;overlay-release.conf;overlay-smp.conf;overlay-lto.conf"
DEBUG = "overlay-thread.conf;overlay-lto.conf"
out = os.path.join(tmp, "multi.json")
if os.path.exists(out):
    os.remove(out)

report(os.path.join(tmp, "ship.json"), SHIP, 121000)
report(os.path.join(tmp, "debug.json"), DEBUG, 124564)

rec = [sys.executable, "scripts/cdk-size-baseline.py", "--out", out, "--from"]
subprocess.run(rec + [os.path.join(tmp, "ship.json")], capture_output=True, check=False)

# An unrecorded configuration must refuse rather than borrow another's numbers.
p = subprocess.run(
    [sys.executable, "scripts/cdk-size-compare.py", "--baseline", out,
     "--current", os.path.join(tmp, "debug.json")],
    capture_output=True, text=True, check=False)
if p.returncode == 3 and "no baseline recorded" in p.stderr:
    print("  ok    an unrecorded configuration is refused, not compared (exit 3)")
else:
    print(f"  FAIL  unrecorded configuration: exit {p.returncode}")
    rc = 1

# Recording the second must not evict the first.
subprocess.run(rec + [os.path.join(tmp, "debug.json")], capture_output=True, check=False)
doc = json.load(open(out))
keys = sorted(doc.get("baselines", {}))
if keys == ["thread+lto", "thread+release+smp+lto"]:
    print("  ok    recording a second configuration keeps the first")
else:
    print(f"  FAIL  baselines are {keys}")
    rc = 1

for name, src in [("thread+release+smp+lto", "ship.json"), ("thread+lto", "debug.json")]:
    p = subprocess.run(
        [sys.executable, "scripts/cdk-size-compare.py", "--baseline", out,
         "--current", os.path.join(tmp, src)],
        capture_output=True, text=True, check=False)
    if p.returncode == 0:
        print(f"  ok    {name} still compares against its own record")
    else:
        print(f"  FAIL  {name}: exit {p.returncode}")
        rc = 1

if doc.get("primary") == "thread+release+smp+lto":
    print("  ok    the shipping configuration is marked as the one CI gates")
else:
    print(f"  FAIL  primary is {doc.get('primary')!r}")
    rc = 1

sys.exit(rc)
PY
[ $? -eq 0 ] || fails=$((fails + 1))

printf '\n── cdk size · the baseline will not lower its own bar\n'

# Recording an image that is already under the floor would make the gate pass by
# moving the bar to meet the image, which is the one failure a baseline exists
# to prevent.
cp "$TMP/base.json" "$TMP/out.json"
python3 - "$TMP/fatreport.json" <<'PY'
import json, sys
size = 131072
json.dump({
    "generated": "now", "build_dir": "/absolute/path/that/must/not/survive",
    "commit": "0" * 40,
    "config": {"board": "decawave_dwm3001cdk", "image": "firmware", "kconfig": {}},
    "regions": {"RAM": {"origin": 0x20000000, "size": size, "used": 128000,
                        "free": 3072, "pct": 97.66}},
    "symbols": {},
}, open(sys.argv[1], "w"))
PY
python3 "$ROOT/scripts/cdk-size-baseline.py" --from "$TMP/fatreport.json" --out "$TMP/out.json" >/dev/null 2>&1
expect_exit 1 "$?" "a baseline below its own floor is refused"

# The volatile fields must not reach a committed file. build_dir in particular
# is an absolute path that names the machine and the user who produced it --
# the same leak scripts/security-fw.sh blocks when it finds one in an image.
python3 - "$TMP/okreport.json" <<'PY'
import json, sys
size = 131072
json.dump({
    "generated": "now", "build_dir": "cdk-matter", "commit": "0" * 40,
    "config": {"board": "decawave_dwm3001cdk", "image": "firmware", "kconfig": {}},
    "regions": {"RAM": {"origin": 0x20000000, "size": size, "used": 124564,
                        "free": 6508, "pct": 95.03}},
    "symbols": {},
}, open(sys.argv[1], "w"))
PY
rm -f "$TMP/out2.json"
python3 "$ROOT/scripts/cdk-size-baseline.py" --from "$TMP/okreport.json" --out "$TMP/out2.json" >/dev/null 2>&1
if [ -f "$TMP/out2.json" ] && ! grep -q '"generated"\|"build_dir"' "$TMP/out2.json"; then
	ok "volatile fields are dropped from the committed baseline"
else
	bad "generated/build_dir survived into the baseline"
fi

# The committed baselines themselves: no absolute home paths, ever. Every one
# of them, discovered rather than listed -- this guarded only the DWM3001CDK
# until 2026-08-22, and the nRF5340 DK baseline carried four absolute overlay
# paths for a week because nothing looked at it. A new board must not be able
# to reintroduce that by being new.
_baselines="$(git ls-files 'apps/*size-baseline*.json' 2>/dev/null)"
if [ -z "$_baselines" ]; then
	bad "no committed size baseline found to check"
fi
for _b in $_baselines; do
	if grep -qE '/(Users|home)/[A-Za-z0-9._-]+|[A-Za-z]:\\\\Users' "$_b"; then
		bad "$_b contains an absolute home path"
	else
		ok "$_b carries no build-host path"
	fi
done

printf '\n'
if [ "$fails" -gt 0 ]; then
	printf '  cdk-size: %d failure(s)\n\n' "$fails"
	exit 1
fi
printf '  cdk-size: clean\n\n'
