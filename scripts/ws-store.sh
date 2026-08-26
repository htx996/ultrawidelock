#!/usr/bin/env bash
#
# ws-store.sh — what the machine is holding, and who is still using it.
#
# The store keeps a whole workspace per (pin, NCS version, patch set, HA mode).
# Sharing means a checkout no longer owns its tree, so deleting a worktree no
# longer reclaims one either: an entry outlives every checkout that ever linked
# to it, silently, at 5.5 GB. This is the page that makes that visible.
#
# It removes nothing on its own. An entry is either linked from somewhere or it
# is not, and only the person reading knows whether the branch it belongs to is
# finished -- so the last column is the command to remove it, not the removal.
#
# Usage:  scripts/ws-store.sh
set -euo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/ws.sh
. "$SELF/scripts/lib/ws.sh"

STORE="$(ws_store_root)"
[ -d "$STORE" ] || { echo "no workspace store at $STORE — run: make bootstrap"; exit 0; }

# Every checkout of this repo on the machine: the primary plus its worktrees.
# `git worktree list` names them all, from any one of them.
checkouts=()
while read -r path _; do
  [ -n "$path" ] && checkouts+=("$path")
done < <(git -C "$SELF" worktree list --porcelain 2>/dev/null | sed -n 's/^worktree //p')

printf '  store  %s\n\n' "$STORE"
printf '  %-26s %7s  %s\n' ENTRY SIZE 'LINKED FROM'

found=0
for entry in "$STORE"/*; do
  [ -d "$entry" ] || continue
  found=1
  name="$(basename "$entry")"
  size="$(du -sh "$entry" 2>/dev/null | awk '{print $1}')"
  ws_entry_ready "$entry" || name="$name (unfinished)"

  linked=()
  for tree in ${checkouts[@]+"${checkouts[@]}"}; do
    [ "$(ws_resolved "$tree" 2>/dev/null || true)" = "$entry" ] && linked+=("$(basename "$tree")")
  done

  if [ ${#linked[@]} -eq 0 ]; then
    printf '  %-26s %7s  %s\n' "$name" "${size:-?}" '— nobody'
    printf '  %-26s %7s  rm -rf %s\n' '' '' "$entry"
  else
    printf '  %-26s %7s  %s\n' "$name" "${size:-?}" "$(IFS=', '; printf '%s' "${linked[*]}")"
  fi
done

[ "$found" = 1 ] || printf '  (empty)\n'
printf '\n  an entry is named <fetch>-<patches>: the first half is the upstream it holds,\n'
printf '  the second what we applied. Same first half = clonable in seconds.\n'
