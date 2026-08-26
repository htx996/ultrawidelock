#!/usr/bin/env bash
#
# app-upstream-diff.sh — how our copy of the door-lock application differs from
# the Nordic one it was taken from.
#
# The application used to be eleven patch files, and a patch answers this
# question by existing. Owned source does not: the changes are simply in the
# tree, indistinguishable from the upstream lines around them, and the only way
# back to "what did we change" is to go and look at what upstream says.
#
# So this fetches just that one directory at the pinned revision and diffs it.
# It reports rather than gates -- a difference is the point, not a failure --
# with one exception: a file we have that upstream never had, or a file upstream
# has that we dropped, is worth naming separately, because that is the shape a
# botched pin bump takes and a unified diff buries it among the edits.
#
# The reason to run it is a pin bump. Raising PIN in scripts/bootstrap.sh
# without reading this is merging on the assumption that upstream did not touch
# anything we did.
#
# Read-only network fetch from public GitHub; nothing in this repo is touched.
# Usage: scripts/app-upstream-diff.sh [--stat]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OURS="$ROOT/integrations/nrfconnect-door-lock/matter-aliro-door-lock-app"
UPSTREAM_PATH="applications/matter-aliro-door-lock-app"
URL="https://github.com/nrfconnect/ncs-door-lock-and-access-control"

[ -d "$OURS" ] || { echo "ERROR: no vendored application at $OURS" >&2; exit 1; }

# bootstrap.sh is the one authority on the revision, as it is for the patches.
# UPSTREAM.md states the same value for a reader; disagreeing with it here would
# mean diffing against a revision this application was never taken from.
PIN="$(sed -n 's/^PIN="\([0-9a-f]\{40\}\)".*/\1/p' "$ROOT/scripts/bootstrap.sh")"
[ -n "$PIN" ] || { echo "ERROR: could not read PIN from scripts/bootstrap.sh" >&2; exit 1; }
grep -q "$PIN" "$OURS/UPSTREAM.md" || {
  echo "ERROR: $OURS/UPSTREAM.md does not name the pin in bootstrap.sh ($PIN)" >&2
  echo "       One of the two moved without the other. Fix the record before" >&2
  echo "       trusting a diff taken against either." >&2
  exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "==> fetching $UPSTREAM_PATH at ${PIN:0:10}…"
# Blobless and sparse: this directory is 2.5 MB inside an 82 MB repository, and
# the rest of it has no bearing on the question.
git init -q "$WORK/upstream"
git -C "$WORK/upstream" remote add origin "$URL"
git -C "$WORK/upstream" config core.sparseCheckout true
git -C "$WORK/upstream" sparse-checkout set --no-cone "/$UPSTREAM_PATH/*"
git -C "$WORK/upstream" fetch -q --depth 1 --filter=blob:none origin "$PIN"
git -C "$WORK/upstream" checkout -q FETCH_HEAD

THEIRS="$WORK/upstream/$UPSTREAM_PATH"
[ -d "$THEIRS" ] || { echo "ERROR: $UPSTREAM_PATH is not in the add-on at ${PIN:0:10}…" >&2; exit 1; }

# Files that exist on one side only. UPSTREAM.md and the HA data model are ours
# by construction, so they are named rather than reported as surprises.
echo
echo "==> files only in this repository"
( cd "$OURS" && find . -type f | sort ) >"$WORK/ours.list"
( cd "$THEIRS" && find . -type f | sort ) >"$WORK/theirs.list"
comm -23 "$WORK/ours.list" "$WORK/theirs.list" | sed 's|^\./|    |' || true
echo
echo "==> files upstream has that we do not"
comm -13 "$WORK/ours.list" "$WORK/theirs.list" | sed 's|^\./|    |' || true

echo
echo "==> content"
if [ "${1:-}" = --stat ]; then
  diff -ru --exclude=UPSTREAM.md "$THEIRS" "$OURS" | diffstat 2>/dev/null \
    || diff -rq --exclude=UPSTREAM.md "$THEIRS" "$OURS" || true
else
  diff -ru --exclude=UPSTREAM.md "$THEIRS" "$OURS" || true
fi

echo
echo "    upstream ${PIN:0:10}…  ·  $UPSTREAM_PATH"
echo "    a difference here is expected: this application is ours to change."
