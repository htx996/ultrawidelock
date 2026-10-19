#!/usr/bin/env python3
# Copyright (c) 2026 UltraWideLock contributors
# SPDX-License-Identifier: ISC
#
# Does graphify need to run again? Prints "absent", "stale", or nothing.
#
# `make docs` asks this before offering to re-extract, because extraction walks
# ~900 files and is worth neither the time nor the prompt when nothing it reads
# has changed.
#
# Staleness is decided by modification time against the trees graphify's output
# is actually derived from, not by comparing HEAD to the graph's built_at_commit.
# The commit comparison looked tidier and was wrong: it re-extracted after every
# commit, including the ones that only touched a guide, the website, or the
# Makefile -- none of which can change a single node or edge.

"""Report whether graphify-out/graph.json is missing or older than its inputs."""

import os
import sys
from pathlib import Path

# The same trees web/graph/graph.py keeps: core only. A change anywhere else
# cannot alter the graph, so it must not trigger a rebuild of it.
TREES = ("modules", "ports", "apps")
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".cc"}


def newest_source(root: Path) -> float:
    newest = 0.0
    for tree in TREES:
        base = root / tree
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            # Build output under a source tree is not a source.
            dirnames[:] = [d for d in dirnames
                           if d not in {"build", ".git", "__pycache__"}]
            for name in filenames:
                if Path(name).suffix not in SOURCE_SUFFIXES:
                    continue
                try:
                    newest = max(newest, os.path.getmtime(Path(dirpath) / name))
                except OSError:
                    pass                      # vanished mid-walk; not our problem
    return newest


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    graph = root / "graphify-out" / "graph.json"
    if not graph.is_file():
        print("absent")
        return 0
    if newest_source(root) > graph.stat().st_mtime:
        print("stale")
    return 0


if __name__ == "__main__":
    sys.exit(main())
