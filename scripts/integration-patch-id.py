#!/usr/bin/env python3
"""Identify the exact integration patch set a workspace was built from."""

import hashlib
import sys
import tempfile
from pathlib import Path


def patch_id(patch_dir: Path) -> str:
    patches = sorted(patch_dir.glob("*.patch"))
    if not patches:
        raise ValueError(f"no patch files in {patch_dir}")

    # v2: HA is no longer part of this. It used to select two extra patches into
    # the fetched application; the application is ours now, and HA=1 picks a data
    # model inside it (CONFIG_ULTRAWIDELOCK_HA). Keeping it here would have split
    # the workspace store into two identical trees over a build-time flag.
    digest = hashlib.sha256(b"ultrawidelock-integration-patches-v2\0")
    for patch in patches:
        name = patch.name.encode()
        data = patch.read_bytes()
        digest.update(len(name).to_bytes(4, "big"))
        digest.update(name)
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)
    return digest.hexdigest()


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="ultrawidelock-patch-id-") as work:
        root = Path(work)
        (root / "b.patch").write_text("second\n", encoding="utf-8")
        (root / "a.patch").write_text("first\n", encoding="utf-8")
        initial = patch_id(root)
        assert initial == patch_id(root)
        print("  ok   patch ID is deterministic")

        (root / "a.patch").write_text("changed\n", encoding="utf-8")
        assert initial != patch_id(root)
        print("  ok   patch ID changes with content")

        (root / "c.patch").write_text("third\n", encoding="utf-8")
        assert initial != patch_id(root)
        print("  ok   patch ID changes when a patch is added")
    print("integration patch ID: PASS")


def main() -> None:
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        return
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} PATCH_DIR")
    print(patch_id(Path(sys.argv[1])))


if __name__ == "__main__":
    main()
