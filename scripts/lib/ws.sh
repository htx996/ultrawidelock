#!/usr/bin/env bash
#
# ws.sh — where the NCS workspace lives, and what makes one workspace different
# from another.
#
# The workspace is 5.5 GB and takes an hour to fetch, and every worktree needed
# its own because the tree holds one patch state at a time: `git apply` writes
# into the fetched repos, so a checkout that shares a workspace with another
# branch builds that branch's patches. ws-seed.sh answered that with an APFS
# copy-on-write clone per worktree, which is cheap in disk and still a copy to
# make, a bootstrap to re-run, and one more 5.5 GB tree to forget about.
#
# The answer here is to name the workspace after its contents instead. Two
# things decide what a bootstrapped tree holds:
#
#   the fetch key   the add-on pin and the NCS version -- what `west update`
#                   pulls, before anything of ours touches it
#   the patch key   the integration patch set -- what we then apply on top
#                   (scripts/integration-patch-id.py)
#
# An entry in the store is named for both, so every checkout that agrees on both
# shares one tree and links to it in milliseconds, and a branch that edits a
# patch gets a tree of its own without anyone deciding that it should. The
# guard in nrf5340dk-build.sh that used to catch a stale workspace after the
# fact now has almost nothing left to catch, which is the point: the states it
# was built to detect can no longer be reached.
#
# Sharing is what makes the lock in setup.sh matter here, and what makes
# ws_apply_patches live in this file rather than in bootstrap.sh: ws-link.sh
# reaches for it too, and two implementations of "apply the patches" that drift
# apart would put two different trees under the same name.
#
#   . "$TREE/scripts/lib/ws.sh"

# ---- identity ----------------------------------------------------------------
# sha256 of stdin, hex, no filename. shasum ships with macOS, sha256sum with
# coreutils; a host with neither cannot name an entry, so say so here rather
# than silently naming them all the same.
ws_sha() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then sha256sum | awk '{print $1}'
  else echo "ERROR: neither shasum nor sha256sum found" >&2; return 1
  fi
}

# The store holds every bootstrapped workspace on this machine, one directory
# per (fetch, patch) pair. Not under ~/Library/Caches or ~/.cache: macOS purges
# the first under disk pressure and cleaners empty both, and an hour of fetching
# is not something to hand to a cleaner. ULTRAWIDELOCK_WS_STORE moves it, which
# is also the way out when $HOME is on a different volume from the checkout --
# the clone below needs both on one volume to cost nothing.
ws_store_root() { printf '%s\n' "${ULTRAWIDELOCK_WS_STORE:-$HOME/.ultrawidelock/ws}"; }

# $1 = add-on pin, $2 = NCS version. What a fetch produces, and therefore what
# can be cloned instead of fetched again.
ws_fetch_key() { printf 'ultrawidelock-ws-fetch-v1\0%s\0%s\0' "$1" "$2" | ws_sha; }

# $1 = fetch key, $2 = patch ID. The entry name carries both halves, truncated
# to keep it readable: the fetch half is a prefix so `ls` groups every tree that
# a clone could be taken from, which is exactly the question ws-link.sh asks.
ws_entry_name() {
  printf '%s-%s\n' "$(printf '%s' "$1" | cut -c1-12)" "$(printf '%s' "$2" | cut -c1-12)"
}

# ---- the store ---------------------------------------------------------------
# An entry is finished when it has a .west and the stamp below. Both, because a
# clone interrupted halfway has the first and a tree still being patched has
# neither, and adopting either as finished is how a build ends up compiling half
# a patch set.
ws_entry_ready() { [ -d "$1/.west" ] && [ -f "$1/.ultrawidelock-ws.id" ]; }

# $1 = entry, then pin, NCS version, patch ID. Written last, so it can never
# describe a tree that was not finished.
ws_stamp() {
  local entry="$1"
  { printf 'pin=%s\n' "$2"
    printf 'ncs=%s\n' "$3"
    printf 'patches=%s\n' "$4"
    printf 'created=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } >"$entry/.ultrawidelock-ws.id"
}

# Any finished entry sharing $2's fetch key, or nothing. Such a tree already
# holds the right upstream at the right revisions and differs only in what we
# applied on top, so cloning it and re-patching costs a copy instead of an hour.
ws_clone_source() {   # $1 = store root, $2 = fetch key, $3 = entry to exclude
  local cand
  for cand in "$1/$(printf '%s' "$2" | cut -c1-12)"-*; do
    [ "$cand" != "$3" ] || continue
    ws_entry_ready "$cand" || continue
    printf '%s\n' "$cand"; return 0
  done
  return 1
}

# Same filesystem? A block clone works within one volume and copies for real
# across volumes, so a mismatch turns a free operation into 5.5 GB of I/O with
# nothing on screen to say so. Walks up to an existing parent, since the
# destination is usually the thing that does not exist yet.
ws_same_volume() {
  local a="$1" b="$2" da db
  while [ ! -e "$a" ] && [ "$a" != "/" ]; do a="$(dirname "$a")"; done
  while [ ! -e "$b" ] && [ "$b" != "/" ]; do b="$(dirname "$b")"; done
  # GNU FIRST, AND THE ORDER IS THE WHOLE BUG. `-f` means a format string to BSD
  # stat and --file-system to GNU stat, and `%d` is valid under both: the device
  # number to BSD, the count of FREE INODES to GNU. So `stat -f '%d'` does not
  # fail on Linux, it succeeds with the wrong number, the `||` never fires, and
  # two calls a moment apart on a busy filesystem disagree -- reporting one
  # volume as two. GNU's `-c` is rejected outright by BSD stat ("illegal option
  # -- c"), so asking for it first is unambiguous on both.
  da="$(stat -c '%d' "$a" 2>/dev/null || stat -f '%d' "$a" 2>/dev/null || echo x)"
  db="$(stat -c '%d' "$b" 2>/dev/null || stat -f '%d' "$b" 2>/dev/null || echo y)"
  [ "$da" = "$db" ]
}

# Which cp flag clones blocks instead of copying them? macOS spells it -c (APFS
# clonefile), GNU cp on btrfs or XFS spells it --reflink, and on ext4 -- which
# is what a Linux CI runner has -- there is no such thing. So this is a property
# of the filesystem the store sits on, not of the OS, and the only honest way to
# learn it is to try. Prints the flag, or nothing when the copy will be real.
#
# The probe asks for --reflink=always so ext4 says no; the copy asks for =auto so
# a subtree that cannot be cloned is copied rather than aborting halfway.
ws_cow_flag() {   # $1 = a directory on the target filesystem
  local dir="$1" a b flag=""
  a="$dir/.ws-cow-probe.$$"
  b="$a.copy"
  mkdir -p "$dir" 2>/dev/null || { printf '\n'; return 0; }
  : >"$a" 2>/dev/null || { printf '\n'; return 0; }
  if cp -c "$a" "$b" 2>/dev/null; then
    flag="-c"
  elif cp --reflink=always "$a" "$b" 2>/dev/null; then
    flag="--reflink=auto"
  fi
  rm -f "$a" "$b"
  printf '%s\n' "$flag"
}

# Copy a workspace the cheapest way this filesystem allows. Unquoted on purpose:
# $flag is one word or none, and an empty "" would be a filename to cp.
ws_cow_copy() {   # $1 = source, $2 = destination
  local flag
  flag="$(ws_cow_flag "$(dirname "$2")")"
  # shellcheck disable=SC2086
  cp $flag -R "$1" "$2"
}

# ---- attaching a checkout ----------------------------------------------------
# $1 = checkout, $2 = entry. Points $1/workspace at $2 and nothing else: a real
# directory there is somebody's 5.5 GB and is never removed to make room for a
# link. `ln -n -f -s` replaces an existing link in one step, so an interrupted
# run cannot leave a checkout with no workspace at all. -n and not -h: BSD ln
# takes either, GNU ln documents only -n, and this runs on both.
ws_link() {
  local tree="$1" entry="$2" link="$1/workspace"
  if [ -e "$link" ] && [ ! -L "$link" ]; then
    echo "ERROR: $link is a real directory, not a link into the store" >&2
    echo "       Nothing here will delete it. Adopt it into the store:" >&2
    echo "         $tree/scripts/ws-link.sh --adopt" >&2
    echo "       or move it aside and re-run." >&2
    return 1
  fi
  ln -n -f -s "$entry" "$link"
}

# A checkout from before the store has a real ./workspace holding a fetch that
# would otherwise be repeated. Move it into the store instead, which is a rename
# when $HOME and the checkout share a volume -- but only when it holds the
# revision we are pinned to: naming a tree after a pin it was not fetched at
# would hand every other checkout the wrong upstream under the right name.
#
# Uses die/step/info, so setup.sh has to be sourced first. Returns 1, quietly,
# when there is nothing to adopt.
ws_adopt() {   # $1 = checkout, $2 = destination entry, $3 = pin, $4 = store root
  local tree="$1" entry="$2" pin="$3" store="$4"
  [ -d "$tree/workspace/.west" ] && [ ! -L "$tree/workspace" ] || return 1
  [ "$(git -C "$tree/workspace/ncs-door-lock-and-access-control" rev-parse HEAD 2>/dev/null)" = "$pin" ] \
    || die "./workspace is not at the pinned add-on revision" \
           "it cannot be adopted into the store under a name that says it is." \
           "re-fetch into the store (the old tree is left alone):" \
           "  mv '$tree/workspace' '$tree/workspace.old' && make bootstrap"
  ws_same_volume "$tree/workspace" "$store" \
    || die "./workspace and the store are on different volumes" \
           "adopting it would copy 5.5 GB rather than rename it. Put the store on" \
           "the volume the checkout is on:" \
           "  ULTRAWIDELOCK_WS_STORE=<path on that volume> make bootstrap"
  step "adopting ./workspace into the store as $(basename "$entry")"
  mkdir -p "$store"
  [ ! -e "$entry" ] && [ ! -L "$entry" ] \
    || die "the store already holds $(basename "$entry")" \
           "two trees claim the same contents. Look at both, then remove one:" \
           "  ls -la '$entry' '$tree/workspace'"
  mv "$tree/workspace" "$entry"
  info "moved to $entry"
}

# What $1/workspace resolves to, or nothing when it is absent or dangling.
ws_resolved() {
  local link="$1/workspace"
  [ -d "$link/.west" ] || return 1
  ( cd "$link" && pwd -P )
}

# ---- patching ----------------------------------------------------------------
# What is left of the integration patches, in the order bootstrap.sh has always
# applied them. tests/tooling/patch_drift_check.sh keeps its own copy of these
# lists and fails when either disagrees with what is on disk, so a patch added
# to one and not the other is caught rather than quietly skipped.
#
# Four, where there were fifteen. Eleven of them changed the door-lock
# application, which is this repository's own source now
# (integrations/nrfconnect-door-lock/) -- including the two Home Assistant
# data-model patches, whose apply-in-this-order-or-not-at-all pairing went with
# them. These four remain because they change code that is not ours to move: two
# files in the add-on's Aliro subsystem, NCS's shared Matter sample sources, and
# connectedhomeip's Zephyr BLE platform.
ws_apply_patches() {   # $1 = workspace, $2 = patch dir
  local ws="$1" p="$2"

  _ws_apply_to "$ws" "$ws/ncs-door-lock-and-access-control" \
    "$p/custom_impl-uwb.patch" "$p/extnvs-rollback-mirror-id.patch"
  _ws_apply_to "$ws" "$ws/nrf"                "$p/nrf-flashfit-dfu-guards.patch"
  _ws_apply_to "$ws" "$ws/modules/lib/matter" "$p/matter-ble-multi-identity.patch"

  WS_PATCH_COUNT=4
  WS_ADDON_PATCH_COUNT=2
}

# Apply patch files to a repository, resetting it to its pinned HEAD first.
#
# That reset is what makes this idempotent -- the previous run's patches have to
# come off before this run's go on -- but hand-editing the workspace is the
# normal way upstream gets debugged here, and those edits look identical to it.
# So say what is about to go, and keep a copy: a run that silently eats an
# afternoon of debugging is the worst thing this can do.
# ULTRAWIDELOCK_KEEP_WS_EDITS=1 stops instead, for when the edits are the point
# and re-patching is not.
_ws_apply_to() {   # $1 = workspace, $2 = repo, remaining args = patch files
  local ws="$1" repo="$2"; shift 2
  local dirty saved
  dirty="$(git -C "$repo" status --porcelain --untracked-files=no)"
  if [ -n "$dirty" ]; then
    if [ "${ULTRAWIDELOCK_KEEP_WS_EDITS:-0}" = 1 ]; then
      echo "ERROR: $repo has local changes and ULTRAWIDELOCK_KEEP_WS_EDITS=1 — stopping" >&2
      printf '%s\n' "$dirty" | sed 's/^/      /' >&2
      exit 1
    fi
    mkdir -p "$ws/.ultrawidelock-discarded"
    saved="$ws/.ultrawidelock-discarded/$(basename "$repo")-$(date -u +%Y%m%dT%H%M%SZ).patch"
    # diff HEAD, not diff: staged work is just as easy to lose and `checkout -- .`
    # does not touch the index, so it would otherwise survive here and trip the
    # pristine assertion below with no record of what it was.
    git -C "$repo" diff HEAD >"$saved"
    echo "    discarding local changes in $repo:"
    printf '%s\n' "$dirty" | sed 's/^/      /'
    echo "    saved to $saved"
    echo "      restore with: git -C $repo apply '$saved'"
  fi
  git -C "$repo" checkout -q -- .
  # Reachable, despite the reset above: `checkout -- .` rewrites the working tree
  # from the index and leaves the index itself alone, so anything staged survives
  # to here. Staged work is deliberate work, so stop rather than clear it -- the
  # copy saved above is the way back if the stop is unwelcome.
  [ -z "$(git -C "$repo" status --porcelain --untracked-files=no)" ] || {
    echo "ERROR: $repo still not pristine — staged changes survive 'checkout -- .'" >&2
    echo "       Unstage them and re-run:  git -C $repo reset" >&2
    exit 1
  }
  git -C "$repo" apply --whitespace=nowarn "$@"
}
