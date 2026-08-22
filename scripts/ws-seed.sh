#!/usr/bin/env bash
# ws-seed.sh — give this git worktree its own NCS workspace, cheaply.
#
# Frequent branch-bouncing over a single shared workspace is a trap: the tree
# holds one patch state at a time (last bootstrap wins), so a build from the
# wrong worktree silently compiles another branch's patches. This seeds a
# per-worktree workspace at the default path ($TREE/workspace) so build.sh picks
# it up with no env var, and each worktree stays self-contained.
#
# Cheap because it uses an APFS copy-on-write clone (cp -c): the clone shares
# every block with the primary and costs ~0 extra disk until a patched file
# diverges. Cleanup is automatic — the workspace lives inside the worktree, so
# deleting the worktree deletes it (see `make ws-clean`).
set -euo pipefail

# The worktree to seed. Defaults to the one holding this script, so the plain
# `make ws-seed` path is unchanged. Passing a path lets ONE checkout that has
# this script seed a worktree whose branch predates it, without copying files
# into that worktree or committing to its branch.
TREE="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
[ -d "$TREE" ] || { echo "ERROR: no such worktree: $TREE" >&2; exit 1; }
TREE="$(cd "$TREE" && pwd)"
# bootstrap.sh is re-run INSIDE the target to normalize patches to its branch,
# so a target without one cannot be seeded correctly -- say so here rather than
# failing three steps later with the clone already made.
[ -x "$TREE/scripts/bootstrap.sh" ] || {
  echo "ERROR: $TREE/scripts/bootstrap.sh missing or not executable" >&2
  exit 1
}
WS="$TREE/workspace"

if [ -d "$WS/.west" ]; then
  echo "==> $WS already seeded — nothing to do"
  exit 0
fi

# A $WS that exists but has no .west is an interrupted bootstrap, not a blank
# slate. It matters because the cleanup trap below removes $WS wholesale when a
# run fails, and that trap cannot tell a directory this script created from one
# that was already there holding a part-fetched 6.5 GB. Refuse instead of
# adopting it, and let the person who knows what is in there decide.
if [ -e "$WS" ]; then
  echo "ERROR: $WS exists but has no .west" >&2
  echo "       That is a part-finished bootstrap, and seeding over it would let a" >&2
  echo "       failure here delete it. Look first, then clear it deliberately:" >&2
  echo "         rm -rf '$WS'   # then re-run make ws-seed" >&2
  exit 1
fi

# Resolve the primary checkout's workspace (same logic build.sh uses to fall back).
common="$(git -C "$TREE" rev-parse --git-common-dir 2>/dev/null || true)"
[ -n "$common" ] || { echo "ERROR: not a git repo"; exit 1; }
case "$common" in /*) ;; *) common="$TREE/$common" ;; esac
primary="$(cd "$(dirname "$common")" && pwd)"

if [ "$primary" = "$TREE" ]; then
  echo "ERROR: this IS the primary checkout — run make bootstrap here, don't seed onto self"
  exit 1
fi
[ -d "$primary/workspace/.west" ] || {
  echo "ERROR: primary workspace not bootstrapped ($primary/workspace) — run make bootstrap there first"
  exit 1
}

# If we bail before the workspace is fully seeded, remove the partial clone so a
# later run (or build.sh's auto-seed) retries cleanly instead of wedging on a
# half-copied dir with no .west. cleanup preserves the real exit code — a bare
# short-circuit as the trap's last command would otherwise mask success as 1.
created="" done=""
# Cleanup handler for the workspace-seeding script's exit trap.
# Captures the last command's exit status, and if a workspace was created but the run did not
# reach completion (created set, done unset), removes WS recursively before re-exiting with the
# captured status.
cleanup() { local rc=$?; [ -n "$created" ] && [ -z "$done" ] && rm -rf "$WS"; exit "$rc"; }
trap cleanup EXIT

echo "==> COW-cloning $primary/workspace -> $WS"
created=1                             # arm cleanup before the clone so a failed cp also tidies up.
                                      # Only honest because the guard above proved $WS did not
                                      # exist: reaching here means cp is what creates it, so the
                                      # trap can only ever delete this script's own partial work.
cp -c -R "$primary/workspace" "$WS"   # cp -c = APFS clonefile; fails loudly off APFS

echo "==> normalizing patches to this worktree's branch"
( cd "$TREE" && ./scripts/bootstrap.sh )   # reuses the clone, re-applies THIS branch's patches

done=1
echo "    ✓ isolated workspace ready — build.sh will use it automatically"
