#!/usr/bin/env python3
"""Write the index the browser FOTA page looks a board up in.

    scripts/ota-index.py --out build/release/ota/ota-index.json \
        --cdk-dir build/release/ota/dwm3001cdk \
        --esp-dir build/release/ota/esp32

TWO SHAPES, because the two chips are not alike, and the index is where that
difference is absorbed so the page does not have to carry it.

  dwm3001cdk   "updates", a map KEYED BY THE SHA-256 THE BOARD REPORTS.
               One MCUboot slot (apps/dwm3001cdk-lock/sysbuild.conf) and 32 KB
               of patch_staging mean what travels is a delta against the exact
               bytes already on the part, and a delta is valid for exactly one
               starting image. So shipping an update means shipping one file per
               release anyone might still be running -- the fan -- and the page
               picks from it by reading one 32-byte value off the board. No
               version parsing, no ordering, no "is 0.3.1 newer than 0.3.10".

               A board whose hash is not in the map has NO over-the-air path.
               That is the single-slot design, not a bug to route around, and
               the page says so and points at the J-Link.

  esp32*       "image", one file. Two whole OTA slots, so the update is the
               image itself and applies to a board in any state. Nothing to look
               up, nothing to key by, no fan.

Nothing here is recomputed. The CDK half reads the manifest
`ultrawidelock_patch.py wrap` writes into each .zip, and the ESP32 half reads
the sidecar `make esp-ota` writes beside each .wdfu -- in both cases the run
that signed the file is the only thing that knows what went into it.
"""

import argparse
import datetime
import json
import sys
import zipfile
from pathlib import Path

SCHEMA = 1


def die(msg):
    sys.exit(f"ota-index: {msg}")


def read_wrap_manifest(zip_path):
    """Pull the one file entry out of a `wrap`-built .zip.

    The schema mirrors nrf/scripts/bootloader/generate_zip.py; from_sha256 and
    to_sha256 are ultrawidelock's own additions to it.
    """
    try:
        with zipfile.ZipFile(zip_path) as z:
            manifest = json.loads(z.read("manifest.json"))
    except (OSError, KeyError, zipfile.BadZipFile, json.JSONDecodeError) as err:
        die(f"{zip_path}: not a wrap-built archive ({err})")

    files = manifest.get("files") or []
    if len(files) != 1:
        die(f"{zip_path}: expected exactly one file entry, found {len(files)}")
    entry = files[0]

    for key in ("from_sha256", "to_sha256", "size", "file"):
        if key not in entry:
            die(f"{zip_path}: manifest has no {key}. Built by an older wrap?")
    return entry


def collect_cdk(cdk_dir):
    """Every delta in a directory, keyed by the image each one applies to."""
    updates = {}
    tos = set()

    for zip_path in sorted(Path(cdk_dir).glob("*.zip")):
        entry = read_wrap_manifest(zip_path)
        from_sha = entry["from_sha256"].lower()
        to_sha = entry["to_sha256"].lower()

        bin_path = Path(cdk_dir) / entry["file"]
        if not bin_path.is_file():
            die(f"{zip_path} names {entry['file']}, which is not beside it")

        # Two deltas claiming the same starting image cannot both be right, and
        # picking one silently would mean the page installs a different build
        # than the directory listing suggests. Refuse instead.
        if from_sha in updates:
            die(
                f"two updates apply to {from_sha[:16]}...:\n"
                f"  {updates[from_sha]['file']}\n  {entry['file']}\n"
                f"  A stale file from an earlier build is the usual cause."
            )

        updates[from_sha] = {
            "file": entry["file"],
            "size": entry["size"],
            "to": to_sha,
            "version": entry.get("version_MCUBOOT", "unknown"),
        }
        tos.add(to_sha)

    if len(tos) > 1:
        die(
            "the deltas do not all produce the same image:\n  "
            + "\n  ".join(sorted(tos))
            + "\n  A fan has to converge, or the page cannot say what it is installing."
        )

    return updates, (tos.pop() if tos else None)


def self_test():
    """Prove the index means what the page assumes it means.

    The page trusts three things this file is the only guard for: that a hash
    maps to exactly one update, that every delta in a release produces the SAME
    image, and that a delta whose .bin is missing is caught here rather than as
    a 404 halfway through someone's update. All three are silent failures in
    the field, so they are pinned here.
    """
    import subprocess                                          # noqa: PLC0415
    import tempfile                                            # noqa: PLC0415

    fails = 0
    total = 0

    def wrap_zip(directory, from_sha, to_sha, version="0.3.1", write_bin=True):
        stem = f"ultrawidelock-{from_sha[:8]}-to-{to_sha[:8]}"
        payload = b"\x00" * 64
        if write_bin:
            (Path(directory) / f"{stem}.bin").write_bytes(payload)
        manifest = {
            "format-version": 1,
            "files": [{
                "file": f"{stem}.bin",
                "size": len(payload),
                "version_MCUBOOT": version,
                "from_sha256": from_sha,
                "to_sha256": to_sha,
            }],
        }
        with zipfile.ZipFile(Path(directory) / f"{stem}.zip", "w") as z:
            z.writestr("manifest.json", json.dumps(manifest))
        return stem

    def run(directory, out):
        return subprocess.run(
            [sys.executable, __file__, "--out", out, "--cdk-dir", directory,
             "--version", "v0.3.1"],
            capture_output=True, text=True, check=False)

    def check(name, ok, detail=""):
        nonlocal fails, total
        total += 1
        if ok:
            print(f"  ok   {name}")
        else:
            fails += 1
            print(f"  FAIL {name}  {detail}")

    a, b, c = "aa" * 32, "bb" * 32, "cc" * 32

    # 1. The happy path: two starting images, one destination.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        wrap_zip(d, b, c)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("two deltas index cleanly", rc.returncode == 0, rc.stderr.strip())
        if rc.returncode == 0:
            idx = json.loads(Path(out).read_text())
            t = idx["targets"]["dwm3001cdk"]
            check("keyed by the starting hash", set(t["updates"]) == {a, b})
            check("latest is the shared destination", t["latest"]["sha256"] == c)
            check("transport is smp", t["transport"] == "smp")
            check("schema is stamped", idx["schema"] == SCHEMA)
            check("version carries through", t["updates"][a]["version"] == "0.3.1")

    # 2. Two updates claiming the same board. Picking one silently would mean
    #    the page installs something other than what the directory implies.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, b)
        wrap_zip(d, a, c)
        rc = run(d, str(Path(d) / "index.json"))
        check("duplicate starting image is refused", rc.returncode != 0)
        check("and says which two files", "two updates apply to" in rc.stderr)

    # 3. A fan that does not converge. Two destinations means the page cannot
    #    say what it is about to install.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, b)
        wrap_zip(d, c, a)
        rc = run(d, str(Path(d) / "index.json"))
        check("divergent destinations refused", rc.returncode != 0)
        check("and says so", "do not all produce" in rc.stderr)

    # 4. A manifest whose .bin was never written.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c, write_bin=False)
        rc = run(d, str(Path(d) / "index.json"))
        check("missing payload refused", rc.returncode != 0)
        check("and names the file", "which is not beside it" in rc.stderr)

    # 5. An empty directory is an error, not an empty index: an empty index
    #    published to the site would read as "no updates exist" forever.
    with tempfile.TemporaryDirectory() as d:
        rc = run(d, str(Path(d) / "index.json"))
        check("empty directory refused", rc.returncode != 0)

    # ---- the ESP32 half: one image per chip, no fan, no starting hash -------

    def esp_image(directory, chip, size=4096, version="0.3.1", write_blob=True,
                  claim_size=None):
        name = f"ultrawidelock-{chip}-{version}.wdfu"
        if write_blob:
            (Path(directory) / name).write_bytes(b"\xe9" + b"\x00" * (size - 1))
        (Path(directory) / f"{chip}.json").write_text(json.dumps({
            "chip": chip, "file": name,
            "size": claim_size if claim_size is not None else size,
            "version": version,
        }))

    def run_esp(directory, out):
        return subprocess.run(
            [sys.executable, __file__, "--out", out, "--esp-dir", directory,
             "--version", "v0.3.1"],
            capture_output=True, text=True, check=False)

    # 6. Two chips, two targets, each carrying one whole image.
    with tempfile.TemporaryDirectory() as d:
        esp_image(d, "esp32s3")
        esp_image(d, "esp32c6")
        out = str(Path(d) / "index.json")
        rc = run_esp(d, out)
        check("two esp images index cleanly", rc.returncode == 0, rc.stderr.strip())
        if rc.returncode == 0:
            t = json.loads(Path(out).read_text())["targets"]
            check("one target per chip", set(t) == {"esp32s3", "esp32c6"})
            check("transport is uwldfu", t["esp32s3"]["transport"] == "uwldfu")
            check("carries an image, not a fan",
                  "image" in t["esp32s3"] and "updates" not in t["esp32s3"])
            check("name is readable", t["esp32s3"]["name"] == "ESP32-S3")

    # 7. A sidecar whose size disagrees with the file. The page checks the same
    #    thing before it sends anything, so catching it here is what stops a
    #    board being offered an image that fails that check mid-update.
    with tempfile.TemporaryDirectory() as d:
        esp_image(d, "esp32s3", size=4096, claim_size=9999)
        rc = run_esp(d, str(Path(d) / "index.json"))
        check("size mismatch refused", rc.returncode != 0)
        check("and says both numbers", "but its sidecar says" in rc.stderr)

    # 8. A sidecar naming a file nobody wrote.
    with tempfile.TemporaryDirectory() as d:
        esp_image(d, "esp32s3", write_blob=False)
        rc = run_esp(d, str(Path(d) / "index.json"))
        check("missing esp image refused", rc.returncode != 0)

    # The per-check rows above are what test-runner.sh counts; this line is for
    # a person reading the output directly. Counted rather than hardcoded, so
    # it cannot drift away from the number of checks actually run -- the first
    # draft of this file said 15 while running 13.
    if fails:
        print(f"\n  {fails} of {total} checks FAILED")
        return 1
    print(f"\nRESULT: PASS ({total} checks)")
    return 0


def collect_esp(esp_dir):
    """Every ESP32 image in a directory, one target per chip.

    No fan and no starting hash: the ESP32 has two whole OTA slots, so an update
    is the image itself and applies to a board in any state. That is why these
    entries carry `image` where the CDK carries `updates` -- the page has
    nothing to look up, it just sends the file.

    Reads the sidecar `make esp-ota` writes beside each .wdfu, for the same
    reason the CDK half reads wrap's zip manifest: the run that signed the file
    is the only thing that knows what went into it.
    """
    targets = {}

    for sidecar in sorted(Path(esp_dir).glob("*.json")):
        try:
            meta = json.loads(sidecar.read_text())
        except (OSError, json.JSONDecodeError) as err:
            die(f"{sidecar}: unreadable sidecar ({err})")

        for key in ("chip", "file", "size", "version"):
            if key not in meta:
                die(f"{sidecar}: has no {key}")

        blob = Path(esp_dir) / meta["file"]
        if not blob.is_file():
            die(f"{sidecar} names {meta['file']}, which is not beside it")
        if blob.stat().st_size != meta["size"]:
            die(f"{meta['file']} is {blob.stat().st_size} B but its sidecar says {meta['size']} B")

        chip = meta["chip"]
        if chip in targets:
            die(f"two images for {chip}")

        targets[chip] = {
            "transport": "uwldfu",
            "name": chip.upper().replace("ESP32", "ESP32-"),
            "dir": Path(esp_dir).name,
            "latest": {"version": meta["version"], "sha256": meta.get("sha256", "")},
            "image": {
                "file": meta["file"],
                "size": meta["size"],
                "version": meta["version"],
            },
        }

    return targets


def main():
    if "--self-test" in sys.argv:
        sys.exit(self_test())

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true",
                    help="prove the index's own guarantees, then exit")
    ap.add_argument("--out", required=True, help="where to write ota-index.json")
    ap.add_argument("--cdk-dir",
                    help="directory of wrap-built DWM3001CDK deltas (.bin beside .zip)")
    ap.add_argument("--esp-dir",
                    help="directory of signed ESP32 images (.wdfu beside its .json sidecar)")
    ap.add_argument("--version", default="", help="release version these belong to")
    args = ap.parse_args()

    targets = {}

    if args.cdk_dir:
        updates, latest = collect_cdk(args.cdk_dir)
        if not updates:
            die(f"no *.zip deltas in {args.cdk_dir}")
        targets["dwm3001cdk"] = {
            "transport": "smp",
            "name": "DWM3001CDK",
            "dir": "dwm3001cdk",
            "latest": {"version": args.version or "unknown", "sha256": latest},
            "updates": updates,
        }

    if args.esp_dir:
        esp = collect_esp(args.esp_dir)
        if not esp:
            die(f"no *.json sidecars in {args.esp_dir}")
        targets.update(esp)

    if not targets:
        die("nothing to index. Pass --cdk-dir, --esp-dir, or both.")

    index = {
        "schema": SCHEMA,
        "generated": datetime.datetime.now(datetime.timezone.utc)
                     .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "targets": targets,
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(index, indent=2, sort_keys=True) + "\n")

    for name, target in sorted(targets.items()):
        if "image" in target:
            print(f"  {name:<14} whole image, {target['image']['size']:,} B "
                  f"-> {target['latest']['version']}")
        else:
            n = len(target["updates"])
            print(f"  {name:<14} {n} update{'' if n == 1 else 's'} "
                  f"-> {target['latest']['sha256'][:16]}...")
    print(f"  wrote     {out}")


if __name__ == "__main__":
    main()
