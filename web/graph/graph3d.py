#!/usr/bin/env python3
"""Build the flyable 3D graph of the code surface: dist/graph/index.html.

This is v0.3.0's graph3d page, restored. The renderer, the camera arrival, the
cluster and containment forces, the parallax dust and the detail panel are that
page's; what changed is where the data comes from. The original mined
documate's per-file markdown and its search index, and both are gone, so the
same payload is built from graphify's AST graph instead:

    graphify . --update --code-only

Node and link shape is unchanged, so the template is untouched apart from its
palette and its links:

    nodes  {id, name, grp, slug, blurb}
    links  {source, target}
    slots  group -> colour slot        colors  the slot palette

Blurbs come from each file's own header comment, which is better than what the
markdown carried: it is the text the author wrote next to the code.

The payload build() produces is committed as files.json, so this page is what
the site ships from a fresh clone and from CI, not only from a machine that has
run graphify. See PAYLOAD.

3d-force-graph (MIT) is vendored under web/vendor/, which is gitignored. With
no vendor copy the build falls back to the flat SVG graph, the same way the
twin degrades without emscripten.
"""

from __future__ import annotations

import json
import re
from collections import Counter
from pathlib import Path

CORE_TOP = ("modules", "ports", "apps")
VENDORED = ("dwt_uwb_driver", "detools")
# graphify extracts prose as well as code, one node per Markdown heading, typed
# "document". A subsystem graph has no use for them, and they break it: group_of
# takes the first two path components, so apps/README.md is its own group rather
# than a file inside one, and three of those pushed the group count past the
# palette. Filtered here rather than in group_of, because a README is not a node
# on this page under any grouping.
NOT_CODE = (".md", ".txt", ".rst", ".json", ".yml", ".yaml", ".csv")
LIB = "3d-force-graph.min.js"
REPO = "https://github.com/ultrawidelock/ultrawidelock/blob/main/"

# The committed payload: build()'s output, ~600 KB for 393 files, their links
# and their symbols. graphify's own graph.json is 11 MB and gitignored, so a
# fresh clone and CI have nothing node-level to render from and the graph page
# fell back to the flat SVG there -- which is what the published website was.
# Carrying the reduction is the same bargain web/graph/subsystems.json already
# makes, one size up: written only by `make docs-graph-refresh`, never as a
# side effect of a build, and diffable because it is indented and key-sorted.
PAYLOAD = Path(__file__).with_name("files.json")

# Slot palette: one colour per subsystem, in sorted group order.
#
# This replaced a 14-entry cool-only set that the page indexed with `% len`.
# There are 20 subsystems, so six of them rendered in a colour already spoken
# for -- ports/zephyr was modules/README.md's blue exactly -- and the fourteen
# that were distinct were all mints, teals and blues, chosen to sit quietly on
# a mint-on-teal stage. Quiet was the problem: the point of colouring by group
# is telling groups apart.
#
# Built by maximising the weakest adjacent pair under normal vision and under
# simulated deuteranopia, protanopia and tritanopia at once, inside OKLCH
# lightness 0.48-0.67 and a 0.19 chroma ceiling, then ordered so neighbouring
# slots are the pair that separates best. Measured against the near-black
# stage this is drawn on:
#
#   adjacent, normal vision   dE 26.9   (floor 15)
#   adjacent, worst CVD       dE 14.4   (target 8)
#   contrast vs #04100e       all 20 >= 3:1
#
# Twenty mutually distinguishable colours do not exist -- the all-pairs worst
# case is dE 8.8, and no palette of this size beats it. Colour is a first
# guess here, not the answer: every node carries its filename, the legend
# names every subsystem, and tapping one isolates it.
#
# Re-check after any edit:
#   node <dataviz-skill>/scripts/validate_palette.js "$(python3 -c '...')" \
#        --mode dark --surface "#04100e"
COLORS = ["#f25761", "#668bff", "#976705", "#236ade", "#c82c4a",
          "#017ca7", "#c38608", "#7e50d1", "#57ac17", "#bb3285",
          "#8c9006", "#dc5cb8", "#288802", "#ad4ec2", "#07ac8b",
          "#ba4900", "#0ba2d1", "#e86904", "#ae6ff0", "#03856a"]

# A file's header comment. Sources here open with an SPDX line, then the real
# description in a // run or a /** @file */ block, so the FIRST comment is
# almost never the one wanted: taking it yielded 17 blurbs out of 393 and every
# one of them was licence boilerplate. Collect every candidate, drop the ones
# that are only a licence, and take the first that says something.
COMMENT = re.compile(r"/\*[*!]?(.*?)\*/|((?:^[ \t]*//[^\n]*\n)+)", re.S | re.M)
LICENCE = re.compile(
    r"SPDX-License-Identifier|Copyright|Public Domain|Licensed under|"
    r"CC0|Apache License|WITHOUT WARRANTIES", re.I)
NOISE = re.compile(r"@file\s+\S+\s*[-—–]?\s*|@brief\s*")


def blurb_for(path: Path) -> str:
    """One sentence from the file's header comment, or an empty string."""
    try:
        head = path.read_text(errors="replace")[:3000]
    except OSError:
        return ""

    for match in COMMENT.finditer(head):
        raw = match.group(1) or match.group(2) or ""
        text = re.sub(r"^\s*(?://|\*)\s?", "", raw, flags=re.M)
        text = NOISE.sub("", text)
        text = " ".join(re.sub(r"[`*]", "", text).split())
        if len(text) < 24 or LICENCE.search(text):
            continue
        # First sentence: the panel gives this one line.
        out = re.split(r"(?<=[.!?])\s+", text)[0]
        return (out[:240].rstrip() + "…") if len(out) > 240 else out
    return ""


def keep(source_file: str) -> bool:
    if not source_file or source_file.split("/")[0] not in CORE_TOP:
        return False
    if source_file.endswith(NOT_CODE):
        return False
    return not any(v in source_file.split("/") for v in VENDORED)


def group_of(source_file: str) -> str:
    parts = source_file.split("/")
    return "/".join(parts[:2]) if len(parts) > 1 else parts[0]


def build(graph: dict, root: Path) -> dict:
    owner = {n["id"]: n.get("source_file", "") for n in graph["nodes"]}

    files = sorted({sf for sf in owner.values() if keep(sf)})
    nodes = [{
        "id": sf,
        "name": sf.rsplit("/", 1)[-1],
        "grp": group_of(sf),
        "slug": REPO + sf,
        "blurb": blurb_for(root / sf),
    } for sf in files]

    weight: Counter = Counter()
    for link in graph["links"]:
        a, b = owner.get(link["source"], ""), owner.get(link["target"], "")
        if keep(a) and keep(b) and a != b:
            weight[(a, b) if a < b else (b, a)] += 1
    links = [{"source": a, "target": b} for (a, b) in sorted(weight)]

    # Slots in sorted group order, so a rebuild does not reshuffle the colours.
    groups = sorted({n["grp"] for n in nodes})
    slots = {g: i for i, g in enumerate(groups)}
    check_palette(slots)
    return {"nodes": nodes, "links": links, "syms": symbols(graph, set(files)),
            "slots": slots, "colors": COLORS}


def check_palette(slots: dict) -> None:
    """Fail the build when a subsystem would have to borrow another's colour.

    The page indexes COLORS with `slots[g] % colors.length`, so a palette
    shorter than the group list does not error -- it silently paints two
    subsystems the same, which is how six of them came to share a colour with
    another. Adding a directory under apps/, modules/ or ports/ is the way
    this recurs, and it should stop the build rather than the reader.
    """
    if len(slots) > len(COLORS):
        raise SystemExit(
            f"graph3d: {len(slots)} subsystems but only {len(COLORS)} colours; "
            f"{len(slots) - len(COLORS)} would reuse another subsystem's colour. "
            "Extend COLORS and re-run the palette validator (see its comment).")


# What the page shows as a file's API surface: one row per symbol, badged by
# kind, linking to the exact line on the source browser.
#
# This was `"syms": {}` -- an empty dict, shipped -- which made the whole symbol
# half of the template dead code: the API-surface caption, the .sym rows, the
# F/T/# badge map, kdist() and the close symbol orbit in layerData all had
# nothing to render. The comment explaining it said the data was gone with the
# generator that used to mine it. It is not gone; graphify emits it.
#
# graphify gives one node per file (label is the filename, at L1) and one per
# symbol below it (label like `grant_init()`, with _callable set on functions).
# The file node is skipped by matching source_location, not by name: two
# symbols in a header can share a filename-shaped label.
SYM_CAP = 60


def symbols(graph: dict, files: set[str]) -> dict:
    """slug -> [[kind, name, url], ...] for each kept file."""
    per_file: dict[str, list] = {}
    for node in graph["nodes"]:
        src = node.get("source_file", "")
        if src not in files or node.get("source_location") == "L1":
            continue
        line = str(node.get("source_location") or "").lstrip("L")
        # Line-anchored nodes only. graphify's AST pass gives every symbol a
        # line; its semantic pass adds prose nodes with source_location None,
        # written by an LLM in an earlier run and never refreshed by `graphify
        # update`. Twenty-four of them still spelled the project's old name,
        # and check-purity's brand ratchet caught them the moment this payload
        # was first committed. They also cannot link to a line, which is the
        # one thing this panel promises.
        if not line.isdigit():
            continue
        label = (node.get("label") or "").strip()
        if not label:
            continue
        kind = "f" if node.get("_callable") else "c"
        per_file.setdefault(src, []).append(
            (line.zfill(6), [kind, label.rstrip("()"), REPO + src + f"#L{line}"]))

    out = {}
    for src, rows in per_file.items():
        # Source order, which is the order someone reading the file would meet
        # them, and truncated: a 300-symbol header is a scroll, not a summary.
        rows.sort(key=lambda r: r[0])
        out[REPO + src] = [r[1] for r in rows[:SYM_CAP]]
    return out


def load(source: Path, root: Path) -> dict:
    """Accept either a full graphify graph or the committed payload.

    The payload is already this page's data, so it is passed straight through;
    a graphify graph is reduced to it. "slots" tells them apart -- build() adds
    it and graphify never emits it.
    """
    data = json.loads(source.read_text())
    if "slots" not in data:
        return build(data, root)
    # The palette is code, not data: a colour edit here should show on the next
    # build rather than waiting for someone to re-extract the graph.
    check_palette(data["slots"])
    return data | {"colors": COLORS}


def payload_is_current(data: dict) -> bool:
    """Whether the committed payload already says what this data says."""
    return (PAYLOAD.is_file()
            and PAYLOAD.read_text(encoding="utf-8") == _payload_text(data))


def write_payload(data: dict) -> bool:
    """Write the payload, returning whether it changed."""
    if payload_is_current(data):
        return False
    PAYLOAD.write_text(_payload_text(data), encoding="utf-8")
    return True


def _payload_text(data: dict) -> str:
    """The payload's exact on-disk bytes. One definition, so that the staleness
    check and the write can never disagree about formatting. Indented and
    key-sorted: 600 KB is only reviewable one field per line, and sorting keeps
    an unrelated re-extraction from reordering the whole file. COLORS is left
    out because load() supplies it from this module."""
    keep_keys = {k: v for k, v in data.items() if k != "colors"}
    return json.dumps(keep_keys, indent=1, sort_keys=True) + "\n"


def render(source: Path, root: Path) -> str:
    data = load(source, root)
    tpl = Path(__file__).with_name("graph3d.tpl.html").read_text()
    return (tpl.replace("@@DATA@@", _embed(data))
               .replace("@@LIB@@", LIB))


def _embed(data: dict) -> str:
    """JSON for a <script type="application/json"> block.

    An HTML parser ends that block at the first literal `</script`, wherever it
    appears -- inside a JSON string included. The blurbs here are harvested from
    arbitrary source-file header comments by blurb_for(), so the contents are
    whatever a contributor wrote in a comment. One comment containing the
    characters `</script>` would truncate the data, break the page, and leave
    the remainder of the blurb parsed as HTML.

    Escaping the slash is the standard fix and is invisible to JSON.parse:
    "<\\/script>" and "</script>" are the same string. `<!--` gets the same
    treatment for the same reason.
    """
    raw = json.dumps(data, separators=(",", ":"))
    return raw.replace("</", "<\\/").replace("<!--", "<\\!--")


if __name__ == "__main__":
    import sys
    here = Path(__file__).resolve().parents[2]
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else here / "graphify-out/graph.json"
    sys.stdout.write(render(src, here))
