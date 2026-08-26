#!/usr/bin/env bash
#
# bootstrap.sh — build a self-contained west workspace, PRISTINE from upstream.
#
# Fetches everything the build needs from public GitHub into ./workspace
# (git-ignored), then applies our integration patches on top. It never reads from
# any other local checkout — a clean upstream fetch every time.
#
# Fetches (all public):
#   - Nordic add-on  ncs-door-lock-and-access-control @ the pin below
#   - NCS v3.3.0 + Zephyr + every module (via the add-on's own west manifest)
#
# The NCS v3.3.0 toolchain it needs is installed here too, once per machine, so
# a clone reaches a build in one command instead of three.
#
# Usage:  scripts/bootstrap.sh                       # workspace in ./workspace
#         ULTRAWIDELOCK_WS=/big/disk/ws scripts/bootstrap.sh # put the multi-GB workspace elsewhere
set -euo pipefail

TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NCS_VER="${NCS_VER:-v3.3.0}"
PIN="a5ad7fde1041d81690710a949c98eda1985fee0b"     # ncs-door-lock-and-access-control (public)
ADDON_URL="https://github.com/nrfconnect/ncs-door-lock-and-access-control"
P="$TREE/integrations/nrfconnect-door-lock/patches"
# WS, ADDON, PATCH_STATE and FETCHED are resolved in preflight, once the store
# entry this checkout belongs in is known. ULTRAWIDELOCK_WS still names a
# workspace directly and skips the store entirely.

# Where Nordic publishes the nrfutil binary, one directory per host triple.
NRFUTIL_URL="https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables"
NRFUTIL_PAGE="https://www.nordicsemi.com/Products/Development-tools/nrf-util"

# The dialect both bootstraps speak: step/info/die, the resumable-interrupt and
# forced-exit-status traps, ask/SETUP_AUTO, the package hints and the disk and
# network checks. scripts/esp-bootstrap.sh sources the same file, so a machine
# that stops in one of them stops the same way in the other.
# shellcheck source=scripts/lib/setup.sh
. "$TREE/scripts/lib/setup.sh"
# The store: what names a workspace, where the machine keeps them, and the patch
# application shared with ws-link.sh.
# shellcheck source=scripts/lib/ws.sh
. "$TREE/scripts/lib/ws.sh"
setup_init "bootstrap" "make bootstrap"

# Launch the nRF Util SDK manager toolchain with the configured NCS version, passing through all remaining arguments.
# ULTRAWIDELOCK_TOOLCHAIN=env skips that wrapper and runs the command directly — for
# environments with the toolchain already on PATH (the NCS toolchain container
# in CI, where nrfutil's toolchain index is not reachable).
if [ "${ULTRAWIDELOCK_TOOLCHAIN:-}" = env ]; then
  launch() { "$@"; }
else
  # Execute a command inside the nRF Connect SDK toolchain environment for NCS_VER, forwarding all arguments.
  # Wrapper around `nrfutil sdk-manager toolchain launch`.
  launch() { nrfutil sdk-manager toolchain launch --ncs-version "$NCS_VER" -- "$@"; }
fi

# =============================================================================
# 0. Preflight — everything this run needs, checked before anything is fetched.
#
#    Two rules, both learned from the shape of the old failures:
#
#      * Report every miss in one pass. Stopping at the first one turns a single
#        setup session into as many round trips as the machine has gaps.
#      * Never report a miss without the command that closes it on THIS host.
#        A tool name is a search; `brew install git` is a fix.
#
#    Everything here is cheap — `command -v`, one `df`, one HEAD request. The
#    expensive phases start below, and by then nothing is left that can stop
#    them for a reason we could have seen from here.
# =============================================================================
PHASE="checking this machine"
step "preflight"

# The two the fetch and patch phases cannot work around. python3 is here because
# scripts/integration-patch-id.py stamps the workspace at the end — a miss there
# wastes the entire fetch, which is the same mistake the toolchain check below
# was written to stop making. curl is deliberately not required: only the
# optional download below needs it, and a machine that already has nrfutil
# should not be stopped over a tool this run will never call.
require_tools git python3

# ---- which workspace this checkout belongs to --------------------------------
# Named for its contents, so every checkout that agrees on the pin, the NCS
# version and the patch set shares one 5.5 GB tree and the rest is a symlink.
# scripts/lib/ws.sh carries the reasoning.
PATCH_ID="$("$TREE/scripts/integration-patch-id.py" "$P")"
FETCH_KEY="$(ws_fetch_key "$PIN" "$NCS_VER")"
ENTRY="$(ws_entry_name "$FETCH_KEY" "$PATCH_ID")"
STORE="$(ws_store_root)"

if [ -n "${ULTRAWIDELOCK_WS:-}" ]; then
  # An explicit path is still a path: no store, no link, no sharing. This is the
  # way to put the workspace on another volume, and CI's way to keep it inside a
  # runner's own directory.
  WS="$ULTRAWIDELOCK_WS"
  info "workspace: $WS   (ULTRAWIDELOCK_WS — outside the store)"
else
  WS="$STORE/$ENTRY"
  # A checkout from before the store keeps its fetch: adopted rather than
  # repeated. ./workspace becomes a link to it at the end of this run.
  ws_adopt "$TREE" "$WS" "$PIN" "$STORE" || true
  info "workspace: $STORE/$ENTRY"
fi

ADDON="$WS/ncs-door-lock-and-access-control"
PATCH_STATE="$WS/.ultrawidelock-patches.sha256"

# The store is shared by every checkout on the machine, so two bootstraps racing
# into one entry would fetch and patch over each other. Per entry, not per store:
# two checkouts on two different patch sets have no reason to wait for one
# another. An explicit ULTRAWIDELOCK_WS is one caller's own directory, and takes
# the lock too -- two runs into the same explicit path race just as badly.
setup_lock "$WS"

# Only for what this run will actually pull: a re-run over a populated workspace
# needs almost nothing.
if [ ! -f "$WS/.ultrawidelock-fetch-done" ]; then
  need_disk "$WS" 8 \
      "free some space, or put the workspace on another disk:" \
      "  ULTRAWIDELOCK_WS=/big/disk/ultrawidelock-ws make bootstrap" \
      "  ULTRAWIDELOCK_WS_STORE=/big/disk/ws make bootstrap   (moves the whole store)"
  warn_offline
fi

# The toolchain unpacks under nrfutil's install-dir, which is somewhere in $HOME
# by default — a different volume from the workspace on plenty of machines, so
# the number above says nothing about it. A warning rather than a stop: this run
# may well find the toolchain already installed and pull nothing at all.
if [ "${ULTRAWIDELOCK_TOOLCHAIN:-}" != env ] && [ -z "${NO_TOOLCHAIN:-}" ]; then
  home_gb="$(free_gb "$HOME")"
  if [ -n "$home_gb" ] && [ "$home_gb" -lt 3 ]; then
    info "warning: ${home_gb} GB free on the volume holding \$HOME — the NCS toolchain needs about 2 GB"
    info "         'nrfutil sdk-manager config set --install-dir <path>' moves where it lands"
  fi
fi

# ---- nrfutil ----------------------------------------------------------------
# It is what installs the toolchain, so under the default it is not optional.
# What follows is the only thing this script offers to install for you: a single
# signed-over-HTTPS binary from Nordic, into your own ~/.local/bin, after asking.
nrfutil_triple() {
  case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)          echo aarch64-apple-darwin ;;
    Darwin-x86_64)         echo x86_64-apple-darwin ;;
    Linux-x86_64)          echo x86_64-unknown-linux-gnu ;;
    Linux-aarch64|Linux-arm64) echo aarch64-unknown-linux-gnu ;;
    *) return 1 ;;
  esac
}

# Fetch the nrfutil binary for this host into $BIN and make it runnable.
install_nrfutil() {
  local triple bin tmp
  triple="$(nrfutil_triple)" || return 1
  command -v curl >/dev/null 2>&1 || { info "no curl here to download it with"; return 1; }
  bin="${ULTRAWIDELOCK_BIN:-$HOME/.local/bin}"
  mkdir -p "$bin" || return 1
  tmp="$bin/.nrfutil.$$"
  info "downloading nrfutil ($triple, about 5 MB)"
  if ! curl -sSfL --retry 3 --retry-delay 2 -o "$tmp" "$NRFUTIL_URL/$triple/nrfutil"; then
    rm -f "$tmp"; return 1
  fi
  chmod +x "$tmp" && mv -f "$tmp" "$bin/nrfutil" || { rm -f "$tmp"; return 1; }
  # Usable from this run whatever the caller's PATH says; the shell rc line is
  # the reader's to add, and saying so beats a working bootstrap followed by a
  # 'make build' that cannot find the binary this one just installed.
  case ":$PATH:" in
    *":$bin:"*) : ;;
    *) PATH="$bin:$PATH"; export PATH
       info "installed to $bin, which is not on your PATH — add this to your shell rc:"
       info "  export PATH=\"$bin:\$PATH\"" ;;
  esac
  command -v nrfutil >/dev/null 2>&1
}

if [ "${ULTRAWIDELOCK_TOOLCHAIN:-}" != env ] && [ -z "${NO_TOOLCHAIN:-}" ] &&
   ! command -v nrfutil >/dev/null 2>&1; then
  if nrfutil_triple >/dev/null 2>&1 && ask "install nrfutil? one 5 MB binary from Nordic into ~/.local/bin"; then
    install_nrfutil || die "could not install nrfutil" \
        "download it by hand instead: $NRFUTIL_PAGE" \
        "then re-run: make bootstrap"
    info "nrfutil  ·  $(nrfutil --version 2>/dev/null | head -1)"
  else
    manual="$NRFUTIL_PAGE"
    nrfutil_triple >/dev/null 2>&1 && manual="curl -sSfL -o ~/.local/bin/nrfutil $NRFUTIL_URL/$(nrfutil_triple)/nrfutil && chmod +x ~/.local/bin/nrfutil"
    die "nrfutil is not on PATH — it is what installs the NCS toolchain" \
        "" \
        "let this script do it:  SETUP_AUTO=1 make bootstrap" \
        "or install it yourself:" \
        "  $manual" \
        "  ($NRFUTIL_PAGE)" \
        "" \
        "already have a Zephyr toolchain on PATH? ULTRAWIDELOCK_TOOLCHAIN=env make bootstrap"
  fi
fi

# ULTRAWIDELOCK_TOOLCHAIN=env is a promise that a Zephyr toolchain is already on PATH,
# and nothing below verifies it until `west init` fails several minutes in with
# "command not found" — which reads as a broken script rather than an unmet
# promise. It costs one lookup to say so here instead.
if [ "${ULTRAWIDELOCK_TOOLCHAIN:-}" = env ] && ! command -v west >/dev/null 2>&1; then
  die "ULTRAWIDELOCK_TOOLCHAIN=env is set, but there is no 'west' on PATH" \
      "that setting means 'the Zephyr toolchain is already here, do not install one'," \
      "so this run has nothing to build with." \
      "" \
      "activate the toolchain environment first (the NCS container does this for you)," \
      "or drop the setting and let bootstrap install NCS $NCS_VER itself:" \
      "  make bootstrap"
fi

# =============================================================================
# 1. The NCS toolchain (compiler, west, ccache — about 2 GB), once per machine.
#
#    This was the one prerequisite the script documented and did not do, which
#    left a manual step in the middle of getting from a clone to a build. Its
#    cost when skipped was not the typing: it surfaced as a failure AFTER the
#    multi-GB fetch below, which is the worst place to learn about it.
#
#    Safe to run every time. `toolchain list` prints one row per installed
#    version, so an existing toolchain costs a query and nothing else.
#
#    It asks nrfutil rather than looking at a path, which is what makes a
#    toolchain installed somewhere unusual findable: `list` reports whatever is
#    in nrfutil's configured install-dir (`nrfutil sdk-manager config show`),
#    default or not. And it cannot disagree with the build, because build.sh
#    reaches the compiler the same way — `toolchain launch` resolves through the
#    same configuration. A toolchain nrfutil cannot see is one no build here
#    could have used either.
#
#    ULTRAWIDELOCK_TOOLCHAIN=env is the way out for a toolchain nrfutil does not manage
#    at all: it means one is already on PATH — the NCS container CI builds in,
#    where nrfutil's toolchain index is not even reachable — so there is nothing
#    to install and nothing to ask. NO_TOOLCHAIN=1 skips just this phase.
# =============================================================================
if [ "${ULTRAWIDELOCK_TOOLCHAIN:-}" != env ] && [ -z "${NO_TOOLCHAIN:-}" ]; then
  PHASE="installing the NCS $NCS_VER toolchain"
  step "NCS $NCS_VER toolchain"

  # nrfutil ships as a launcher with no commands in it: a machine that just
  # installed it has `nrfutil` on PATH and no `sdk-manager` behind it, and every
  # sdk-manager line below would fail with a subcommand error that reads like a
  # bug in this script. It is a few MB and a couple of seconds, so just add it.
  if ! nrfutil sdk-manager --version >/dev/null 2>&1; then
    info "adding nrfutil's sdk-manager command"
    nrfutil install sdk-manager >/dev/null 2>&1 || die \
        "nrfutil could not install its sdk-manager command" \
        "run 'nrfutil install sdk-manager' to see why" \
        "an old nrfutil is the usual cause: 'nrfutil self-upgrade' then re-run"
  fi

  # Ask in JSON first — a machine-readable answer cannot be broken by a change
  # to the table's column widths — and keep the table grep as the fallback for
  # an sdk-manager too old to offer it.
  installed=0
  if out="$(nrfutil --json sdk-manager toolchain list 2>/dev/null)"; then
    case "$out" in *"\"$NCS_VER\""*) installed=1 ;; esac
  fi
  if [ "$installed" -eq 0 ]; then
    table="$(nrfutil sdk-manager toolchain list --styling never 2>/dev/null || true)"
    text_has "^${NCS_VER}[[:space:]]" "$table" && installed=1
  fi

  if [ "$installed" -eq 1 ]; then
    info "already installed — nothing to fetch"
  else
    # A version Nordic does not publish fails several hundred MB in with one
    # line of Rust error. Ask the index first, and if the pin is not there,
    # answer the question the reader is about to have: what IS there.
    if avail="$(nrfutil sdk-manager search --styling never 2>/dev/null)" &&
       [ -n "$avail" ] && ! text_has "[[:space:]]${NCS_VER}[[:space:]]" "$avail"; then
      versions="$(printf '%s\n' "$avail" | awk 'NR>1 {print $2}' | head -8 | tr '\n' ' ')"
      die "NCS $NCS_VER is not one of the versions Nordic publishes a toolchain for" \
          "available: ${versions}…" \
          "the repo is pinned to v3.3.0; NCS_VER=<version> overrides it for a bench test"
    fi
    # If the match above ever goes stale, this is the cost: `install` without
    # --force does not replace an installation that is already there (that is
    # what --force is documented to do), so it is a no-op, not a repeat download.
    info "installing (~2 GB, once per machine — several minutes)"
    nrfutil sdk-manager toolchain install --ncs-version "$NCS_VER" || die \
        "the NCS $NCS_VER toolchain did not install" \
        "disk and network are the usual causes; the command was:" \
        "  nrfutil sdk-manager toolchain install --ncs-version $NCS_VER" \
        "re-run 'make bootstrap' once it is fixed — this phase is the only one that repeats"
  fi
fi

# 2. Fetch pristine upstream into $WS. A sentinel marks a completed fetch so an
#    interrupted `west update` resumes on the next run — without it, a partial
#    fetch would be skipped forever because the add-on clone already exists.
FETCHED="$WS/.ultrawidelock-fetch-done"
PHASE="fetching the west workspace into $WS"
step "workspace: $WS   (add-on pin ${PIN:0:10}…, NCS $NCS_VER)"

# An entry that already exists differs from this one only in what was applied on
# top: same pin, same NCS, same upstream revisions, because that is what its
# name says. So clone it and re-patch -- a copy-on-write clone and four `git
# apply`s -- rather than an hour of GitHub. This is what ws-seed.sh used to do
# per worktree, moved to where every checkout benefits from it.
if [ ! -d "$ADDON/.git" ] && [ -z "${ULTRAWIDELOCK_WS:-}" ] && [ "${NO_CLONE:-0}" != 1 ]; then
  if src="$(ws_clone_source "$STORE" "$FETCH_KEY" "$WS")"; then
    if ws_same_volume "$src" "$STORE"; then
      info "cloning $(basename "$src") — same upstream, a different patch set"
      # Into a .partial and renamed, rather than under an exit trap: the trap
      # setup.sh owns is what turns an interrupt into a resumable message, and a
      # second one here would replace it. A .partial left by a stop is visible,
      # is never mistaken for an entry, and this line is what clears it.
      mkdir -p "$STORE"
      rm -rf "$WS.partial"
      cp -c -R "$src" "$WS.partial"   # cp -c = APFS clonefile; fails loudly off APFS
      rm -f "$WS.partial/.ultrawidelock-ws.id"   # not this entry's stamp; earned below
      mv "$WS.partial" "$WS"
      info "cloned — the fetch below has nothing left to pull"
    else
      info "$(basename "$src") is on another volume — fetching rather than copying 5.5 GB"
    fi
  fi
fi

if [ ! -d "$ADDON/.git" ]; then
  # Clone the manifest repo, checkout the pinned SHA, then `west init -l`.
  # (`west init -m … --mr <SHA>` is wrong: it runs `git clone --branch <SHA>`, and
  #  --branch only accepts a tag or branch name, never a commit SHA.)
  mkdir -p "$WS" || die "cannot create the workspace directory $WS" \
      "check the permissions, or choose another location:" \
      "  ULTRAWIDELOCK_WS=/somewhere/writable make bootstrap"
  if [ "${ULTRAWIDELOCK_SHALLOW:-0}" = 1 ]; then
    # Pinned-SHA shallow fetch (`git clone --depth` would need PIN at a tip).
    git init -q "$ADDON"
    git -C "$ADDON" remote add origin "$ADDON_URL"
    git -C "$ADDON" fetch -q --depth 1 origin "$PIN"
    git -C "$ADDON" checkout -q FETCH_HEAD
  else
    git clone -q "$ADDON_URL" "$ADDON"
    git -C "$ADDON" checkout -q "$PIN"
  fi
fi

# Deliberately not folded into the clone above. Everything between the clone and
# `west init` used to be reachable only on the run that cloned, so an interrupt
# in that window left a directory with a .git in it and nothing else — and every
# later run skipped straight past it, because the only question asked was whether
# the clone existed. Ask instead whether the two things the fetch below needs are
# true, and this whole window becomes resumable.
if [ "$(git -C "$ADDON" rev-parse HEAD 2>/dev/null)" != "$PIN" ]; then
  info "add-on: checking out the pinned revision ${PIN:0:10}…"
  git -C "$ADDON" checkout -q "$PIN" 2>/dev/null ||
    { git -C "$ADDON" fetch -q origin "$PIN" && git -C "$ADDON" checkout -q FETCH_HEAD; } ||
    die "could not check out add-on revision ${PIN:0:10}… in $ADDON" \
        "the clone there is incomplete; delete it and re-run:" \
        "  rm -rf '$ADDON' && make bootstrap"
fi
if [ ! -d "$WS/.west" ]; then
  launch west init -l "$ADDON"
fi

if [ ! -f "$FETCHED" ]; then
  info "west update — fetching NCS + modules from GitHub (multi-GB on a first run)"
  # ULTRAWIDELOCK_SHALLOW=1 fetches exactly the pinned revisions with no history —
  # same tree state, a fraction of the size. For CI; the bench default keeps
  # full clones (git archaeology in the workspace stays possible).
  if [ "${ULTRAWIDELOCK_SHALLOW:-0}" = 1 ]; then
    ( cd "$WS" && launch west update --narrow -o=--depth=1 )
  else
    ( cd "$WS" && launch west update )
  fi
  touch "$FETCHED"
else
  info "already fetched — reusing (delete $WS for a clean re-fetch)"
fi

# 3. Apply our patches on top. Each target repo is reset to its pinned HEAD and
#    verified clean first, so a patch can never land on unexpected local state.
#    The lists and the reset live in scripts/lib/ws.sh, because ws-link.sh
#    re-patches a cloned entry with the same code -- two implementations would
#    put two different trees into the store under one name.
PHASE="applying the integration patches"
step "applying integration patches"
ws_apply_patches "$WS" "$P"

# A dirty repository proves that some patch exists, not that it is today's
# patch set. Record the exact patch contents and optional HA mode so build.sh
# rejects a workspace left behind by an older checkout.
printf '%s\n' "$PATCH_ID" >"$PATCH_STATE"

echo "    ✓ pristine upstream + $WS_PATCH_COUNT patches (add-on ×$WS_ADDON_PATCH_COUNT, nrf, matter)"

# 4. Name the finished tree, and point this checkout at it. The stamp goes last:
#    it is what ws_entry_ready and ws-link.sh read to decide an entry is somebody
#    else's to use, and a tree that stopped halfway through step 3 must never
#    look like one.
if [ -z "${ULTRAWIDELOCK_WS:-}" ]; then
  PHASE="linking ./workspace"
  ws_stamp "$WS" "$PIN" "$NCS_VER" "$PATCH_ID"
  ws_link "$TREE" "$WS"
  step "./workspace -> $WS"
  info "every checkout on this patch set links here: scripts/ws-link.sh, or make ws-link"
fi

setup_done "ready. Build with:  make build"
