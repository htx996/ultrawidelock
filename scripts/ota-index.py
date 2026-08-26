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

        # An update from an image to ITSELF. ultrawidelock_patch.py refuses to
        # build one now, but this is the file that decides what gets PUBLISHED,
        # and an older delta could still be sitting in the directory. Left in,
        # the page would offer a board an update to the image it is already
        # running -- and then "verify" it by checking the hash still matches,
        # which it trivially does. Success, reported for nothing.
        if from_sha == to_sha:
            die(
                f"{entry['file']} applies to the image it produces ({from_sha[:16]}...).\n"
                f"  That is an update to the image the board already runs. It usually\n"
                f"  means the build did not change between the two releases."
            )

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

    def recovery_files(directory, sha, size=4096, version="0.3.1", claim=None):
        """A recovery .bin and its sidecar, as `ultrawidelock_patch.py recovery`
        would have written them. `claim` lets a test lie about the size."""
        name = f"ultrawidelock-cdk-{sha[:16]}.bin"
        (Path(directory) / name).write_bytes(b"\x00" * size)
        (Path(directory) / f"ultrawidelock-cdk-{sha[:16]}.recovery.json").write_text(
            json.dumps({
                "file": name,
                "size": claim if claim is not None else size,
                "sha256": sha,
                "version": version,
                "board": "decawave_dwm3001cdk",
                "kind": "recovery",
            })
        )

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

    # 9. A recovery image beside the deltas: the whole-image path, for a board
    #    whose application does not run and therefore cannot be asked what it is.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, c, size=406524)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("a recovery image indexes cleanly", rc.returncode == 0, rc.stderr.strip())
        if rc.returncode == 0:
            t = json.loads(Path(out).read_text())["targets"]["dwm3001cdk"]
            check("recovery is published", "recovery" in t)
            check("recovery carries the size", t["recovery"]["size"] == 406524)
            check("recovery is the image the deltas produce",
                  t["recovery"]["sha256"] == c)
            check("the deltas are still there beside it", set(t["updates"]) == {a})

    # 10. A build with no recovery image is still a valid index. The page just
    #     does not offer the option, which is the whole of the degradation.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("no recovery image is not an error", rc.returncode == 0)
        if rc.returncode == 0:
            t = json.loads(Path(out).read_text())["targets"]["dwm3001cdk"]
            check("and no recovery key is emitted", "recovery" not in t)

    # 11. THE ONE THAT MATTERS. A recovery image that is not what the deltas
    #     produce would leave a rescued board on a build nothing updates.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, b)
        rc = run(d, str(Path(d) / "index.json"))
        check("a recovery image that is not the latest is refused",
              rc.returncode != 0)
        check("and says what it would have left behind",
              "no update applies to" in rc.stderr, rc.stderr.strip())

    # 12. Two recovery images: one of them is stale, and nothing here can tell
    #     which, so neither gets published.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, c)
        recovery_files(d, b)
        rc = run(d, str(Path(d) / "index.json"))
        check("two recovery images are refused", rc.returncode != 0)
        check("and both are named", "more than one recovery image" in rc.stderr)

    # 13. A sidecar whose size does not match the file beside it. Same class of
    #     bug as the delta size check, and the same reason to refuse.
    with tempfile.TemporaryDirectory() as d:
        wrap_zip(d, a, c)
        recovery_files(d, c, size=4096, claim=9999)
        rc = run(d, str(Path(d) / "index.json"))
        check("a recovery size mismatch is refused", rc.returncode != 0)
        check("and says both numbers",
              "4,096" in rc.stderr and "9,999" in rc.stderr, rc.stderr.strip())

    # 14. THE FIRST RELEASE. No board is running an older image, so there is
    #     nothing to compute a delta against -- but the whole image is still
    #     installable through serial recovery, and it is the only over-the-air
    #     artifact this release can have. Refusing it would mean a brand new
    #     release ships no update path at all.
    with tempfile.TemporaryDirectory() as d:
        recovery_files(d, c, size=406524)
        out = str(Path(d) / "index.json")
        rc = run(d, out)
        check("a recovery image with no deltas is a valid index",
              rc.returncode == 0, rc.stderr.strip())
        if rc.returncode == 0:
            t = json.loads(Path(out).read_text())["targets"]["dwm3001cdk"]
            check("first release: the whole image defines the latest",
                  t["latest"]["sha256"] == c)
            check("first release: the fan is empty rather than absent",
                  t["updates"] == {})
            check("first release: recovery is published", t["recovery"]["sha256"] == c)

    # 15. Neither deltas nor a whole image is still nothing to publish.
    with tempfile.TemporaryDirectory() as d:
        rc = run(d, str(Path(d) / "index.json"))
        check("an empty directory is still refused", rc.returncode != 0)
        check("and says both things are missing",
              "no recovery image" in rc.stderr, rc.stderr.strip())

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


def collect_cdk_recovery(cdk_dir, latest):
    """The whole-image entry, if one was published beside the deltas.

    WHY THIS IS SEPARATE FROM THE DELTAS. Every entry in `updates` answers "the
    board is running X, what applies to it"; this one answers "the board is
    running nothing I recognise, or nothing at all". It is reached through
    MCUboot's serial recovery rather than through the application, so it has no
    starting hash, no window gate and no delta -- and it is the only path on
    this board that can rescue an image that does not boot.

    Optional on purpose. A build that did not publish one still produces a valid
    index; the page simply does not offer the recovery option.
    """
    sidecars = sorted(Path(cdk_dir).glob("*.recovery.json"))
    if not sidecars:
        return None

    if len(sidecars) > 1:
        die(
            "more than one recovery image in "
            f"{cdk_dir}:\n  " + "\n  ".join(x.name for x in sidecars)
            + "\n  A stale file from an earlier build is the usual cause."
        )

    meta = json.loads(sidecars[0].read_text())
    for key in ("file", "size", "sha256", "version"):
        if key not in meta:
            die(f"{sidecars[0].name} has no {key}. Built by an older recovery step?")

    blob = Path(cdk_dir) / meta["file"]
    if not blob.is_file():
        die(f"{sidecars[0].name} names {meta['file']}, which is not beside it")

    actual = blob.stat().st_size
    if actual != meta["size"]:
        die(f"{meta['file']} is {actual:,} B, but its sidecar says {meta['size']:,} B")

    # THE RECOVERY IMAGE MUST BE THE IMAGE THE DELTAS CONVERGE ON, and this is
    # the guard in this file that matters most.
    #
    # A recovery image is what someone reaches for when a board is already in
    # trouble. If it installs a DIFFERENT build than the over-the-air path
    # produces, the board comes back running something no published delta
    # applies to -- so the next update finds no path, the page correctly says
    # so, and the owner is left with a board that cannot be updated at all by
    # the route that was supposed to rescue it. Refusing here costs a release;
    # not refusing costs a field visit.
    # With no deltas there is nothing to be consistent WITH, and the whole image
    # is then the definition of the current release rather than a claim about it.
    if latest is not None and meta["sha256"].lower() != latest.lower():
        die(
            f"the recovery image is not the image the deltas produce:\n"
            f"  recovery  {meta['sha256'][:16]}...  ({meta['file']})\n"
            f"  deltas    {latest[:16]}...\n"
            f"  Recovering a board would leave it on a build no update applies to."
        )

    return {
        "file": meta["file"],
        "size": meta["size"],
        "sha256": meta["sha256"].lower(),
        "version": meta["version"],
    }


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
        rec = collect_cdk_recovery(args.cdk_dir, latest)

        # A FAN OF NO DELTAS IS VALID WHEN A WHOLE IMAGE IS PUBLISHED, and the
        # first release is exactly that case: no board in the world is running
        # an older image, so there is nothing to compute a delta against -- but
        # the whole image is still installable, over MCUboot serial recovery,
        # and it is the only over-the-air artifact that release can have. The
        # page then offers the reinstall option and no others, which is honest
        # rather than degraded.
        if not updates and not rec:
            die(f"no *.zip deltas and no recovery image in {args.cdk_dir}")

        targets["dwm3001cdk"] = {
            "transport": "smp",
            "name": "DWM3001CDK",
            "dir": "dwm3001cdk",
            "latest": {
                "version": args.version or "unknown",
                # With no deltas there is no shared destination to read it from,
                # so the whole image says what the current release is.
                "sha256": latest or rec["sha256"],
            },
            "updates": updates,
        }
        if rec:
            targets["dwm3001cdk"]["recovery"] = rec

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
            if "recovery" in target:
                print(f"  {'':<14} + whole image, {target['recovery']['size']:,} B "
                      f"for serial recovery")
    print(f"  wrote     {out}")


if __name__ == "__main__":
    main()
