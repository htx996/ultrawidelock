#!/usr/bin/env python3
"""gatt_table_check.py — no service a browser cannot walk may be published
unconditionally.

WHY THIS EXISTS. On 2026-08-27 the DWM3001CDK advertised as a commissioned
credential reader and published the Matter commissioning service anyway. Nothing
on the board cared. macOS did: it reserves the Matter UUIDs for the system, so
CoreBluetooth refused descriptor discovery on 0xFFF6's C1 characteristic with
CBError 8, and Chromium's macOS backend never completes GATT discovery after
that error (crbug.com/609844). getPrimaryService() therefore never settled and
EVERY Web Bluetooth client lost the board -- firmware updates included. The SMP
service was fine the whole time; it was unreachable because of a neighbour.

That is the property this gate defends, and it is not obvious from any one file:

    A BROWSER SEES A DEVICE ALL OR NOTHING. Web Bluetooth walks the entire GATT
    table and has no API to limit discovery, so one unreachable service takes
    down every other service on the device.

A native client hides this. ultrawidelock_smp.py passes services=[SMP] and never
touches the rest, which is why the CLI worked for weeks against a board no
browser could reach.

WHAT IT CHECKS, by reading the sources rather than a device:

  1. No statically defined service carries a UUID a browser cannot reach.
  2. 0xFFF6 in particular is registered dynamically, never statically.
  3. The dynamic registration is actually gated on the advertised state.
  4. The CDK build enables the dynamic database that requires.

WHAT IT CANNOT CHECK. This is a source gate, not a runtime one. It cannot prove
the gating predicate is CORRECT -- only that a gate exists and that the service
is not nailed into the table. A board that published 0xFFF6 permanently through
a bug in the predicate would pass this and fail on a Mac. CDK-37 and CDK-38 in
docs/hardware-validation.md are the rows that cover that, and they need hardware.

  tests/tooling/gatt_table_check.py [--self-test]

Exit 0 clean, 1 on any violation.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

# ---- what a browser cannot reach --------------------------------------------
#
# Two DIFFERENT gatekeepers, and they fail the same way from the page, so they
# are listed together and labelled apart:
#
#   os        the operating system refuses to enumerate it for a third party.
#             Discovery dies and takes the whole table with it.
#   blocklist the BROWSER refuses to expose it, whatever the OS thinks. Chrome
#             ships https://github.com/WebBluetoothCG/registries gatt_blocklist.txt;
#             the service is simply invisible to the page.
#
# The distinction matters for the fix: an OS refusal is survivable by withdrawing
# the service, a blocklist entry means that service can never be the transport.
UNREACHABLE = {
    "0000fff6": ("os", "Matter commissioning. macOS reserves it; MEASURED CBError 8 "
                       "at C1 on 2026-08-27, which is what this gate was written for"),
    "00001530": ("blocklist", "Nordic legacy DFU. On Chrome's GATT blocklist because "
                              "unsigned firmware update from a web page is a bad idea. "
                              "This project uses mcumgr/SMP, which is not blocked, and "
                              "that is the only reason browser updates are possible"),
    "00001812": ("blocklist", "HID. Blocked so a page cannot keylog"),
    "0000fffd": ("blocklist", "FIDO. Blocked so a page cannot impersonate an authenticator"),
}

PORTS = "ports/zephyr"
CDK_CONF = "apps/dwm3001cdk-lock/prj.conf"

# BT_GATT_SERVICE_DEFINE(name, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xFFF6)), ...
STATIC_16 = re.compile(
    r"BT_GATT_SERVICE_DEFINE\s*\(\s*\w+\s*,\s*"
    r"BT_GATT_PRIMARY_SERVICE\s*\(\s*BT_UUID_DECLARE_16\s*\(\s*(0[xX][0-9a-fA-F]{1,4})\s*\)",
    re.S)
# ...BT_GATT_PRIMARY_SERVICE(&k_some_uuid) -- a 128-bit service, resolved below.
STATIC_128 = re.compile(
    r"BT_GATT_SERVICE_DEFINE\s*\(\s*\w+\s*,\s*"
    r"BT_GATT_PRIMARY_SERVICE\s*\(\s*&\s*(\w+)", re.S)
UUID_128 = re.compile(
    r"(\w+)\s*=\s*BT_UUID_INIT_128\s*\(\s*BT_UUID_128_ENCODE\s*\(\s*(0[xX][0-9a-fA-F]+)")


def norm16(text: str) -> str:
    """0xFFF6 -> the first word of its full 128-bit form, lowercased."""
    return f"{int(text, 16):08x}"


class Checker:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, ok: bool, what: str, detail: str = "") -> bool:
        if ok:
            self.passed += 1
            print(f"  ok   {what}")
        else:
            self.failed += 1
            print(f"  FAIL {what}")
            if detail:
                for line in detail.splitlines():
                    print(f"         {line}")
        return ok


def static_services(root: pathlib.Path) -> list[tuple[str, str]]:
    """Every (uuid-word, file) statically registered under ports/zephyr."""
    found = []
    for path in sorted((root / PORTS).rglob("*.c")):
        src = path.read_text()
        rel = str(path.relative_to(root))
        for m in STATIC_16.finditer(src):
            found.append((norm16(m.group(1)), rel))
        names = dict(UUID_128.findall(src))
        for m in STATIC_128.finditer(src):
            raw = names.get(m.group(1))
            if raw:
                found.append((norm16(raw), rel))
    return found


def run(root: pathlib.Path) -> int:
    c = Checker()

    print("\n-- gatt table . nothing unreachable is published unconditionally")
    statics = static_services(root)
    c.check(bool(statics), "found statically defined services to check",
            "The patterns matched nothing, which means this gate is asleep, not clean.")
    for uuid, rel in statics:
        entry = UNREACHABLE.get(uuid)
        # 16-bit UUIDs live in the Bluetooth base, so print the short form for
        # those and the full first word for a vendor 128-bit one. Printing
        # uuid[4:] for both turns d3b5a140 into a nonexistent "0xA140".
        label = f"0x{uuid[4:].upper()}" if uuid.startswith("0000") else f"0x{uuid.upper()}..."
        c.check(entry is None,
                f"{label} static in {rel}",
                "" if entry is None else
                f"{entry[0]}: {entry[1]}\n"
                f"A browser cannot walk a table containing this. Register it\n"
                f"dynamically and withdraw it when it is not needed, or move the\n"
                f"function it serves to a UUID a browser can reach.")

    print("\n-- gatt table . the Matter service is dynamic and gated")
    matter = root / PORTS / "matter/matter_ble_zephyr.c"
    src = matter.read_text() if matter.exists() else ""
    c.check(bool(src), f"{PORTS}/matter/matter_ble_zephyr.c is readable")
    if src:
        c.check("BT_GATT_SERVICE_DEFINE" not in src,
                "0xFFF6 is not statically defined",
                "BT_GATT_SERVICE_DEFINE nails a service into the table for the\n"
                "life of the image. This one must come and go.")
        c.check("bt_gatt_service_register" in src and "bt_gatt_service_unregister" in src,
                "0xFFF6 both registers and unregisters",
                "A service that can be registered and never withdrawn is a static\n"
                "service with extra steps.")
        c.check("int matter_ble_publish(bool on)" in src,
                "matter_ble_publish() is the single entry point")

    ble = root / PORTS / "ble/ultrawidelock_ble_zephyr.c"
    adv = ble.read_text() if ble.exists() else ""
    c.check("matter_ble_publish(" in adv,
            "the advertiser drives it",
            "Nothing calls matter_ble_publish(), so the service is either never\n"
            "published or never withdrawn. It must follow the same predicate that\n"
            "decides which advert the board carries: a board saying 'I am a\n"
            "commissioned reader' must not also offer to be commissioned.")
    c.check(re.search(r"matter_ble_publish\(\s*!\s*commissioned\s*\)", adv) is not None,
            "it is gated on the advertised state, not on something else",
            "The call exists but is not keyed to `commissioned`. That is the\n"
            "predicate the advert already uses; any other gate lets the table and\n"
            "the advert disagree again, which is the original bug.")

    print("\n-- gatt table . the build allows a dynamic database")
    conf = (root / CDK_CONF).read_text()
    c.check("CONFIG_BT_GATT_DYNAMIC_DB=y" in conf,
            f"{CDK_CONF} enables CONFIG_BT_GATT_DYNAMIC_DB",
            "Without it bt_gatt_service_register() is not compiled in and the\n"
            "gating above cannot work.")

    print(f"\n  gatt table: {'PASS' if c.failed == 0 else 'FAIL'} "
          f"({c.passed} checks, {c.failed} failing)\n")
    return 1 if c.failed else 0


# ---- the gate's own logic ----------------------------------------------------
# A gate that cannot fail is decoration. These feed it sources that BREAK each
# rule and require it to notice, which is the only way to know the patterns
# match anything at all.
SELF_TESTS = [
    ("a static 0xFFF6 is caught",
     "BT_GATT_SERVICE_DEFINE(matter_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xFFF6)),",
     STATIC_16, "0000fff6", True),
    ("a static 0xFFF2 is allowed",
     "BT_GATT_SERVICE_DEFINE(uwl_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xFFF2)),",
     STATIC_16, "0000fff2", False),
    ("Nordic legacy DFU would be caught",
     "BT_GATT_SERVICE_DEFINE(dfu, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0x1530)),",
     STATIC_16, "00001530", True),
    ("a 128-bit static service resolves through its variable",
     "static const struct bt_uuid_128 k_dfu = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xd3b5a140,\n"
     "BT_GATT_SERVICE_DEFINE(s_dfu, BT_GATT_PRIMARY_SERVICE(&k_dfu),",
     STATIC_128, "d3b5a140", False),
]


def self_test() -> int:
    c = Checker()
    print("\n-- gatt table . the gate's own logic")
    for name, src, pattern, want_uuid, want_flagged in SELF_TESTS:
        if pattern is STATIC_128:
            names = dict(UUID_128.findall(src))
            m = pattern.search(src)
            got = norm16(names[m.group(1)]) if m and m.group(1) in names else None
        else:
            m = pattern.search(src)
            got = norm16(m.group(1)) if m else None
        c.check(got == want_uuid, f"{name}: extracts {want_uuid}", f"got {got!r}")
        c.check((got in UNREACHABLE) == want_flagged,
                f"{name}: {'flagged' if want_flagged else 'permitted'}")

    c.check(all(v[0] in ("os", "blocklist") for v in UNREACHABLE.values()),
            "every unreachable UUID says which gatekeeper refuses it")
    c.check(all(len(k) == 8 for k in UNREACHABLE),
            "the table is keyed by normalised 32-bit words")

    print(f"\n  gatt gate self-test: {'PASS' if c.failed == 0 else 'FAIL'} "
          f"({c.passed} checks, {c.failed} failing)\n")
    return 1 if c.failed else 0


# ---- proof that the gate bites ----------------------------------------------
# The self-test above proves the PATTERNS match. It does not prove the gate
# rejects a real tree, and a gate that passes everything passes quietly forever.
# So put the bug back, four ways, in a throwaway copy, and require a non-zero
# exit each time. This is the check that would have caught the original: every
# mutation below is a state this repository was actually in before 2d4985ca.
MUTATIONS = [
    ("0xFFF6 back to a static service",
     "ports/zephyr/matter/matter_ble_zephyr.c",
     "static struct bt_gatt_attr matter_attrs[] = {",
     "BT_GATT_SERVICE_DEFINE(matter_svc,\nstatic struct bt_gatt_attr matter_attrs[] = {"),
    ("the advertiser stops driving it",
     "ports/zephyr/ble/ultrawidelock_ble_zephyr.c",
     "(void)matter_ble_publish(!commissioned);",
     "/* removed */"),
    ("gated on something other than the advert",
     "ports/zephyr/ble/ultrawidelock_ble_zephyr.c",
     "(void)matter_ble_publish(!commissioned);",
     "(void)matter_ble_publish(true);"),
    ("the dynamic database turned back off",
     "apps/dwm3001cdk-lock/prj.conf",
     "CONFIG_BT_GATT_DYNAMIC_DB=y",
     "CONFIG_BT_GATT_DYNAMIC_DB=n"),
]


def prove(root: pathlib.Path) -> int:
    import io
    import contextlib
    import shutil
    import tempfile

    c = Checker()
    print("\n-- gatt table . the gate rejects the bug when it is put back")
    for name, rel, old, new in MUTATIONS:
        with tempfile.TemporaryDirectory() as td:
            tmp = pathlib.Path(td) / "tree"
            for d in (PORTS, "apps/dwm3001cdk-lock"):
                shutil.copytree(root / d, tmp / d, dirs_exist_ok=True)
            path = tmp / rel
            src = path.read_text()
            if old not in src:
                c.check(False, f"rejects: {name}",
                        f"the anchor for this mutation is gone from {rel}, so the\n"
                        f"mutation was never applied and proved nothing. Re-point it.")
                continue
            path.write_text(src.replace(old, new, 1))
            # run() is noisy by design; a passing proof should say one line.
            with contextlib.redirect_stdout(io.StringIO()):
                rc = run(tmp)
            c.check(rc != 0, f"rejects: {name}",
                    "The gate accepted a tree with the bug in it. Whatever it is\n"
                    "checking, it is not this.")

    print(f"\n  gatt gate proof: {'PASS' if c.failed == 0 else 'FAIL'} "
          f"({c.passed} checks, {c.failed} failing)\n")
    return 1 if c.failed else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--prove", action="store_true",
                    help="put the bug back and require the gate to reject it")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.prove:
        return prove(pathlib.Path(args.root).resolve())
    return run(pathlib.Path(args.root).resolve())


if __name__ == "__main__":
    sys.exit(main())
