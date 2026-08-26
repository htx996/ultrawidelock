#!/usr/bin/env bash
#
# ws-link.sh — point this checkout at the workspace its branch actually needs.
#
# The one command a new worktree runs. Usually it makes a symlink and exits in
# milliseconds, because some other checkout on the machine has already fetched
# and patched exactly this tree and the store knows it under a name computed
# from the pin, the NCS version and the patch set. When the patch set is this
# branch's own, it clones the nearest entry -- copy-on-write and ~0 disk where
# the filesystem has block cloning, a real copy where it does not -- and
# re-applies the patches, which is minutes at worst rather than the hour a fetch
# costs. Only a machine with nothing in the store at all is sent to bootstrap.
#
# It replaces ws-seed.sh, which gave every worktree a private clone. The clone
# was cheap; having N of them, each needing its own bootstrap and its own
# cleanup, was not. See scripts/lib/ws.sh for what names an entry.
#
# Usage:  scripts/ws-link.sh            # link this checkout
#         scripts/ws-link.sh <path>     # link a worktree that has no copy of this script
#         scripts/ws-link.sh --adopt    # move a pre-store ./workspace into the store first
#         scripts/ws-link.sh --print    # say what this checkout resolves to, change nothing
set -euo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODE=link
TREE="$SELF"
for arg in "$@"; do
  case "$arg" in
    --adopt) MODE=adopt ;;
    --print) MODE=print ;;
    -*) echo "usage: $0 [--adopt|--print] [worktree]" >&2; exit 2 ;;
    *)  TREE="$arg" ;;
  esac
done
[ -d "$TREE" ] || { echo "ERROR: no such worktree: $TREE" >&2; exit 1; }
TREE="$(cd "$TREE" && pwd)"

# shellcheck source=scripts/lib/setup.sh
. "$SELF/scripts/lib/setup.sh"
# shellcheck source=scripts/lib/ws.sh
. "$SELF/scripts/lib/ws.sh"
setup_init "ws-link" "make ws-link"

# The patches and the manifest belong to the TARGET checkout, since they are
# what its build will use; only the scripts come from this one. That is what
# lets a checkout with this script link a worktree whose branch predates it.
P="$TREE/integrations/nrfconnect-door-lock/patches"
[ -d "$P" ] || die "no integration patches in $TREE" \
    "that does not look like an UltraWideLock checkout."

# bootstrap.sh is the one authority on the pin (west.yml documents the same
# value, and tests/tooling/patch_drift_check.sh fails when the two disagree).
# Read it rather than keeping a second copy here.
PIN="$(sed -n 's/^PIN="\([0-9a-f]\{40\}\)".*/\1/p' "$TREE/scripts/bootstrap.sh" 2>/dev/null || true)"
[ -n "$PIN" ] || die "could not read the add-on pin from $TREE/scripts/bootstrap.sh"
NCS_VER="${NCS_VER:-v3.3.0}"

PATCH_ID="$("$SELF/scripts/integration-patch-id.py" "$P")"
FETCH_KEY="$(ws_fetch_key "$PIN" "$NCS_VER")"
ENTRY_NAME="$(ws_entry_name "$FETCH_KEY" "$PATCH_ID")"
STORE="$(ws_store_root)"
ENTRY="$STORE/$ENTRY_NAME"

# Ahead of the ULTRAWIDELOCK_WS check below, because --print changes nothing and
# the name it reports does not depend on the store existing or being used: it is
# a function of the pin, the NCS version and the patch files, all of which a bare
# checkout has. .github/workflows/ci.yml wants exactly that -- the entry name as
# a cache key -- while also setting ULTRAWIDELOCK_WS, because a job that saves
# `path: workspace` through actions/cache cannot have a symlink there. Refusing
# to answer a question because of a variable that only affects where a build
# would put its files made those two mutually exclusive for no reason.
if [ "$MODE" = print ]; then
  printf 'checkout   %s\n' "$TREE"
  printf 'entry      %s\n' "$ENTRY_NAME"
  printf 'store      %s\n' "$STORE"
  printf 'state      %s\n' "$(ws_entry_ready "$ENTRY" && echo present || echo 'not fetched')"
  if [ -n "${ULTRAWIDELOCK_WS:-}" ]; then
    printf 'workspace  %s   (ULTRAWIDELOCK_WS -- this checkout does not use the store)\n' \
        "$ULTRAWIDELOCK_WS"
  else
    printf 'workspace  %s\n' "$(ws_resolved "$TREE" || echo 'not linked')"
  fi
  # setup.sh's exit trap turns any unexplained exit into a nonzero one with a
  # "failed while starting up" on it, which is right for every path that can
  # stop halfway and wrong for a report that finished.
  HANDLED=1
  exit 0
fi

# link and adopt both write, and neither has anything to write to when the
# workspace is named directly.
if [ -n "${ULTRAWIDELOCK_WS:-}" ]; then
  die "ULTRAWIDELOCK_WS is set, so this checkout does not use the store" \
      "it already names a workspace directly: $ULTRAWIDELOCK_WS" \
      "unset it to link into the store instead."
fi

if [ "$MODE" = adopt ]; then
  ws_adopt "$TREE" "$ENTRY" "$PIN" "$STORE" \
    || die "nothing to adopt: $TREE/workspace is not a fetched workspace of its own" \
           "if it is already a link, there is nothing to do."
fi

# Already pointing at the right tree. The common case by a wide margin, and the
# reason this is safe to run from a hook on every session: it reads four files
# and one symlink.
if [ "$(ws_resolved "$TREE" || true)" = "$ENTRY" ] && ws_entry_ready "$ENTRY"; then
  step "./workspace -> $ENTRY"
  info "already linked (entry $ENTRY_NAME)"
  setup_done "nothing to do."
  exit 0
fi

setup_lock "$ENTRY"

if ws_entry_ready "$ENTRY"; then
  ws_link "$TREE" "$ENTRY"
  step "./workspace -> $ENTRY"
  setup_done "linked. Build with:  make build"
  exit 0
fi

# Nothing under this name yet. An entry with the same fetch key holds the same
# upstream at the same revisions and differs only in what was applied on top, so
# clone it and re-patch rather than fetch 5.5 GB again.
PHASE="cloning a workspace for this patch set"
src="$(ws_clone_source "$STORE" "$FETCH_KEY" "$ENTRY")" || die \
    "no workspace in the store for add-on ${PIN:0:10}… / NCS $NCS_VER" \
    "nothing here can be cloned or re-patched into one. Fetch it once:" \
    "  cd '$TREE' && make bootstrap" \
    "" \
    "store: $STORE"

ws_same_volume "$src" "$STORE" || die \
    "$(basename "$src") is on a different volume from $STORE" \
    "a clone across volumes copies 5.5 GB for real. Either fetch:" \
    "  cd '$TREE' && make bootstrap" \
    "or put the store on that volume:  ULTRAWIDELOCK_WS_STORE=<path>"

step "cloning $(basename "$src") -> $ENTRY_NAME"
if [ -n "$(ws_cow_flag "$STORE")" ]; then
  info "same upstream, a different patch set — copy-on-write, then re-patch"
else
  info "same upstream, a different patch set — re-patched after the copy"
  info "no block cloning on this filesystem: 5.5 GB copied for real, minutes"
fi
mkdir -p "$STORE"
# Into a .partial and renamed, so an interrupt leaves something visibly
# unfinished rather than a directory the next run would read as an entry. The
# stamp is dropped with it: it names the tree we copied FROM.
rm -rf "$ENTRY.partial"
ws_cow_copy "$src" "$ENTRY.partial"   # block clone where the filesystem has one
rm -f "$ENTRY.partial/.ultrawidelock-ws.id"
mv "$ENTRY.partial" "$ENTRY"

PHASE="applying the integration patches"
step "applying integration patches"
ws_apply_patches "$ENTRY" "$P"
printf '%s\n' "$PATCH_ID" >"$ENTRY/.ultrawidelock-patches.sha256"
echo "    ✓ pristine upstream + $WS_PATCH_COUNT patches (add-on ×$WS_ADDON_PATCH_COUNT, nrf, matter)"

# Last, as in bootstrap.sh: the stamp is what tells every other checkout this
# entry is finished and theirs to link.
ws_stamp "$ENTRY" "$PIN" "$NCS_VER" "$PATCH_ID"
ws_link "$TREE" "$ENTRY"
step "./workspace -> $ENTRY"
setup_done "ready. Build with:  make build"
