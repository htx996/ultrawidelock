#!/usr/bin/env python3
"""bind-helper.py — point the UWB lock at another Matter lock.

Three things have to be true before a walk-up at the DWM3001CDK opens a second
lock, and only one of them lives in this repository:

  1. both locks are on ONE fabric        (this script commissions them)
  2. the UWB lock holds a binding        (this script writes it)
  3. the target's ACL lets it in         (this script merges the entry)

Miss the third and every unlock comes back UNSUPPORTED_ACCESS, which from the
outside looks exactly like nothing happening. That asymmetry -- easy to get
wrong, invisible when wrong -- is the reason this script exists rather than a
paragraph telling you to run four chip-tool commands.

Everything it does is printed before it does it. chip-tool's argument spelling
has drifted between releases, so a rejected command is a line you can copy,
adjust and run yourself. Nothing here is required; it is a wrapper.

  python3 scripts/bind-helper.py

Idempotent: state is re-read before each write, so a run that stopped halfway
is finished rather than duplicated. No secret is written to disk by this
script. chip-tool keeps its own fabric keys wherever it was built to keep them,
which is its business and worth knowing about.

See docs/matter-binding.md.
"""

import argparse
import json
import re
import shutil
import subprocess
import sys

# DoorLock, and the endpoint every lock in this repository puts it on.
DOOR_LOCK_CLUSTER = 0x0101
DEFAULT_ENDPOINT = 1

# The Binding cluster, and the manufacturer-specific attribute this node reads
# an unlock PIN out of. The vendor prefix must match the image's
# CONFIG_ULTRAWIDELOCK_MATTER_VENDOR_ID or the write lands on nothing.
BINDING_CLUSTER = 0x001E
DEFAULT_VENDOR_ID = 0xFFF1

# chip-tool's own node id on the fabric it creates. Only a default: the real
# one is whatever the target's ACL already names, which is why it is read.
CHIP_TOOL_NODE_ID = 112233

# Privilege 3 is Operate and AuthMode 2 is CASE (Access Control cluster,
# 9.10.4). Operate rather than Manage: the UWB lock needs to send UnlockDoor
# and has no business changing the target's configuration.
PRIVILEGE_OPERATE = 3
PRIVILEGE_ADMINISTER = 5
AUTH_MODE_CASE = 2


class Failed(Exception):
    """A step that cannot sensibly be continued past."""


def say(msg=""):
    print(msg, flush=True)


def head(msg):
    say()
    say(f"== {msg}")


def ask(prompt, default=None):
    suffix = f" [{default}]" if default is not None else ""
    try:
        got = input(f"   {prompt}{suffix}: ").strip()
    except EOFError:
        raise Failed("no terminal to ask on")
    return got or (default if default is not None else "")


def confirm(prompt, default_yes=False):
    hint = "Y/n" if default_yes else "y/N"
    got = ask(f"{prompt} ({hint})", "")
    if not got:
        return default_yes
    return got.lower().startswith("y")


def run(cmd, check=True):
    """Run one chip-tool command, showing it first."""
    say()
    say("   $ " + " ".join(shlex_quote(c) for c in cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0 and check:
        tail = "\n".join((out or "").strip().splitlines()[-12:])
        raise Failed(f"chip-tool exited {proc.returncode}\n{tail}")
    return proc.returncode, out


def shlex_quote(s):
    """Quote for display only. Never used to build a shell command line."""
    return s if re.fullmatch(r"[\w@%+=:,./-]+", s) else "'" + s.replace("'", "'\\''") + "'"


def need_chip_tool(path):
    if shutil.which(path) or path.startswith("/") or path.startswith("./"):
        return path
    raise Failed(
        f"{path} is not on PATH.\n"
        "   chip-tool is part of connectedhomeip and is built, not packaged:\n"
        "     git clone --depth 1 https://github.com/project-chip/connectedhomeip\n"
        "     cd connectedhomeip && ./scripts/examples/gn_build_example.sh "
        "examples/chip-tool out/\n"
        "   Then re-run with --chip-tool out/chip-tool"
    )


def already_on_fabric(tool, node_id):
    """Does this node answer us? The cheapest possible proof of commissioning."""
    rc, _ = run([tool, "descriptor", "read", "server-list", str(node_id), "0"], check=False)
    return rc == 0


def commission(tool, node_id, what):
    head(f"{what}: node id {node_id}")

    if already_on_fabric(tool, node_id):
        say(f"   already on this fabric, and answering. Nothing to do.")
        return

    say("   Not on this fabric yet.")
    say()
    say("   Put it into pairing mode now:")
    say("     - the UWB lock advertises whenever it has no fabric of its own;")
    say("       if it is already in Apple Home, open Home > accessory settings")
    say("       > Add Matter Accessory to get a fresh setup code.")
    say("     - a commercial lock: follow its own 'add to another ecosystem'")
    say("       flow, which is where it prints a code for a SECOND fabric.")
    say()
    say("   Apple Home keeps working either way. A Matter device holds several")
    say("   fabrics at once, and this adds one rather than replacing it.")
    say()
    code = ask("Setup code (11 digits, or the 'MT:...' payload)")
    if not code:
        raise Failed("no setup code given")

    run([tool, "pairing", "code", str(node_id), code])
    say(f"   commissioned as node {node_id}.")


def read_acl(tool, node_id):
    """The target's ACL for OUR fabric, as chip-tool prints it.

    The attribute is fabric-scoped, so this only ever shows, and a write only
    ever replaces, the entries belonging to the fabric this script is on.
    Apple Home's entries live under its own fabric index and cannot be
    disturbed from here. That is what makes replacing the list safe.
    """
    rc, out = run([tool, "accesscontrol", "read", "acl", str(node_id), "0"], check=False)
    if rc != 0:
        raise Failed(
            "could not read the target's ACL.\n"
            "   That usually means it is not on this fabric, or not reachable."
        )
    return out


def admin_subjects(acl_text):
    """Whoever currently holds Administer on our fabric.

    Parsed out rather than assumed, because writing an ACL that does not
    contain the administrator is how you lock yourself out of a device
    permanently -- there is no path back except a factory reset.

    chip-tool's output shape is not a stable interface, so a failure to find
    anything falls back to its default node id and says so loudly.

    SEARCHED, not matched from the start of the line: chip-tool prefixes every
    line with a timestamp and a category, so nothing here is ever at column
    zero. Anchoring to the line start finds nothing at all, silently, and the
    fallback then looks like a device with no administrator.
    """
    subjects = []
    block = None
    for line in acl_text.splitlines():
        s = line.strip()
        m = re.search(r"\bPrivilege:\s*(\d+)", s)
        if m:
            block = int(m.group(1))
            continue
        # A subject is a bare number under its own index. The entry headers
        # ("[1]: {") and every named field ("AuthMode: 2") do not match.
        m = re.search(r"\[\d+\]:\s*(\d+)\s*$", s)
        if m and block == PRIVILEGE_ADMINISTER:
            subjects.append(int(m.group(1)))
    return subjects


# One real chip-tool ACL read, trimmed. Kept verbatim, prefixes and all,
# because the prefixes are exactly what the parser got wrong the first time.
SELF_TEST_ACL = """\
[1651181935.489] [4485:4485] CHIP: [TOO]   ACL: 2 entries
[1651181935.489] [4485:4485] CHIP: [TOO]     [1]: {
[1651181935.489] [4485:4485] CHIP: [TOO]       Privilege: 5
[1651181935.489] [4485:4485] CHIP: [TOO]       AuthMode: 2
[1651181935.489] [4485:4485] CHIP: [TOO]       Subjects: 1 entries
[1651181935.489] [4485:4485] CHIP: [TOO]         [1]: 112233
[1651181935.489] [4485:4485] CHIP: [TOO]       Targets: null
[1651181935.489] [4485:4485] CHIP: [TOO]       FabricIndex: 1
[1651181935.489] [4485:4485] CHIP: [TOO]      }
[1651181935.489] [4485:4485] CHIP: [TOO]     [2]: {
[1651181935.489] [4485:4485] CHIP: [TOO]       Privilege: 3
[1651181935.489] [4485:4485] CHIP: [TOO]       AuthMode: 2
[1651181935.489] [4485:4485] CHIP: [TOO]       Subjects: 1 entries
[1651181935.489] [4485:4485] CHIP: [TOO]         [1]: 1001
[1651181935.489] [4485:4485] CHIP: [TOO]       Targets: 1 entries
[1651181935.489] [4485:4485] CHIP: [TOO]         [1]: {
[1651181935.489] [4485:4485] CHIP: [TOO]           Cluster: 257
[1651181935.489] [4485:4485] CHIP: [TOO]           Endpoint: 1
[1651181935.489] [4485:4485] CHIP: [TOO]          }
[1651181935.489] [4485:4485] CHIP: [TOO]       FabricIndex: 1
[1651181935.489] [4485:4485] CHIP: [TOO]      }
"""


def self_test():
    """Check the one function here that reads somebody else's output format.

    Everything else in this script asks before it acts, so a mistake is
    visible. This one decides what goes into an ACL write, and an ACL written
    without its administrator cannot be undone without a factory reset.
    """
    checks = 0
    failed = 0

    def eq(what, got, want):
        nonlocal checks, failed
        checks += 1
        if got == want:
            print(f"  ok   {what}")
        else:
            failed += 1
            print(f"  FAIL {what}: got {got!r} want {want!r}")

    # The administrator is found through chip-tool's line prefixes.
    eq("the administrator is found", admin_subjects(SELF_TEST_ACL), [112233])
    # The operate entry's subject is NOT an administrator and must not be
    # promoted to one by being written back into the administer entry.
    eq("the operate subject is not mistaken for one",
       1001 in admin_subjects(SELF_TEST_ACL), False)
    eq("an empty read finds nobody", admin_subjects(""), [])
    eq("garbage finds nobody", admin_subjects("nothing to see here\n[1]: {\n"), [])
    # Two administrators are both kept: dropping either locks one out.
    eq("two administrators are both kept",
       admin_subjects(SELF_TEST_ACL.replace("        [1]: 112233",
                                            "        [1]: 112233\n"
                                            "[x] CHIP: [TOO]         [2]: 445566")),
       [112233, 445566])

    print(f"\nbind-helper: {checks - failed}/{checks} checks passed")
    return 0 if failed == 0 else 1


def write_acl(tool, peer, uwb, admins):
    head("the target's access control list")

    entries = [
        {
            "privilege": PRIVILEGE_ADMINISTER,
            "authMode": AUTH_MODE_CASE,
            "subjects": admins,
            "targets": None,
        },
        {
            # Operate on DoorLock only. The UWB lock can unlock this device and
            # cannot reconfigure it.
            "privilege": PRIVILEGE_OPERATE,
            "authMode": AUTH_MODE_CASE,
            "subjects": [uwb],
            "targets": [
                {"cluster": DOOR_LOCK_CLUSTER, "endpoint": DEFAULT_ENDPOINT, "deviceType": None}
            ],
        },
    ]

    say("   About to write these entries onto the target, for THIS fabric only:")
    say(f"     administer -> {admins}   (kept, so you are not locked out)")
    say(f"     operate    -> [{uwb}] on DoorLock endpoint {DEFAULT_ENDPOINT}")
    if not confirm("Write it?", default_yes=True):
        raise Failed("declined")

    run([tool, "accesscontrol", "write", "acl", json.dumps(entries), str(peer), "0"])
    say("   ACL written.")


def write_binding(tool, uwb, peer, endpoint):
    head("the binding on the UWB lock")

    entries = [{"node": peer, "endpoint": endpoint, "cluster": DOOR_LOCK_CLUSTER}]
    say(f"   Binding node {peer}, endpoint {endpoint}, DoorLock.")
    if not confirm("Write it?", default_yes=True):
        raise Failed("declined")

    run([tool, "binding", "write", "binding", json.dumps(entries), str(uwb),
         str(DEFAULT_ENDPOINT)])
    say("   binding written.")


def write_pin(tool, uwb, vendor_id, pin):
    head("the unlock PIN")

    attr = (vendor_id << 16) | 0x0000
    say("   Some locks set RequirePINforRemoteOperation and refuse an unlock")
    say("   that carries no PIN. A Matter binding has nowhere to put one, so")
    say("   this goes in a manufacturer attribute beside the binding list.")
    say()
    say("   It is one PIN for the whole node, it is stored in the board's")
    say("   flash, and it never reads back. If your lock can be told not to")
    say("   require a PIN remotely, that is the better configuration.")
    if not confirm("Write the PIN?", default_yes=False):
        say("   skipped.")
        return

    run([tool, "any", "write-by-id", hex(BINDING_CLUSTER), hex(attr),
         f'hex:{pin.encode("ascii").hex()}', str(uwb), str(DEFAULT_ENDPOINT)])
    say("   PIN written.")


def test_fire(tool, peer, endpoint):
    head("a supervised test unlock")

    say("   This sends a REAL UnlockDoor to the target lock, right now.")
    say("   Make sure that is safe where you are standing.")
    say()
    say("   What it proves: the target is reachable, is a DoorLock, and accepts")
    say("   a timed unlock. What it does NOT prove: the ACL entry written")
    say("   above, because this command arrives as the ADMINISTRATOR and not")
    say("   as the UWB lock. The end-to-end proof is walking up to the CDK")
    say("   with `make monitor` running.")
    if not confirm("Send it?", default_yes=False):
        say("   skipped.")
        return

    run([tool, "doorlock", "unlock-door", str(peer), str(endpoint),
         "--timedInteractionTimeoutMs", "2000"])
    say("   sent.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chip-tool", default="chip-tool", help="path to chip-tool")
    ap.add_argument("--uwb-node", type=int, default=1001, help="node id for the UWB lock")
    ap.add_argument("--peer-node", type=int, default=1002, help="node id for the target lock")
    ap.add_argument("--endpoint", type=int, default=DEFAULT_ENDPOINT,
                    help="the target's DoorLock endpoint")
    ap.add_argument("--vendor-id", type=lambda s: int(s, 0), default=DEFAULT_VENDOR_ID,
                    help="must match CONFIG_ULTRAWIDELOCK_MATTER_VENDOR_ID")
    ap.add_argument("--pin", default=None, help="unlock PIN for the target, if it needs one")
    ap.add_argument("--self-test", action="store_true",
                    help="check the ACL parser and exit; needs no hardware")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    say(__doc__.split("\n\n")[0])
    say()
    say("   Nothing is written without asking first.")

    try:
        tool = need_chip_tool(args.chip_tool)

        commission(tool, args.uwb_node, "The UWB lock (DWM3001CDK)")
        commission(tool, args.peer_node, "The lock it should open")

        acl = read_acl(tool, args.peer_node)
        admins = admin_subjects(acl)
        if not admins:
            say()
            say(f"   !! could not find an administrator in the target's ACL.")
            say(f"      Falling back to chip-tool's default, {CHIP_TOOL_NODE_ID}.")
            say(f"      If that is wrong, this write would lock you out of the")
            say(f"      target permanently. Check the read above before saying yes.")
            admins = [CHIP_TOOL_NODE_ID]

        write_acl(tool, args.peer_node, args.uwb_node, admins)
        write_binding(tool, args.uwb_node, args.peer_node, args.endpoint)

        if args.pin:
            write_pin(tool, args.uwb_node, args.vendor_id, args.pin)

        test_fire(tool, args.peer_node, args.endpoint)

        head("done")
        say(f"   UWB lock   node {args.uwb_node}")
        say(f"   target     node {args.peer_node}, endpoint {args.endpoint}, DoorLock")
        say(f"   ACL        operate granted to {args.uwb_node}")
        say(f"   binding    written")
        say()
        say("   Now walk up to the CDK with `make monitor` running. One line per")
        say("   attempt, one per outcome. UNSUPPORTED_ACCESS means the ACL did")
        say("   not take; see docs/matter-binding.md.")
        return 0

    except Failed as e:
        say()
        say(f"   stopped: {e}")
        say("   Nothing further was written. Re-running is safe.")
        return 1
    except KeyboardInterrupt:
        say()
        say("   interrupted.")
        return 130


if __name__ == "__main__":
    sys.exit(main())
