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
WS="${ULTRAWIDELOCK_WS:-$TREE/workspace}"
NCS_VER="${NCS_VER:-v3.3.0}"
PIN="a5ad7fde1041d81690710a949c98eda1985fee0b"     # ncs-door-lock-and-access-control (public)
ADDON_URL="https://github.com/nrfconnect/ncs-door-lock-and-access-control"
ADDON="$WS/ncs-door-lock-and-access-control"
P="$TREE/integrations/nrfconnect-door-lock/patches"
PATCH_STATE="$WS/.ultrawidelock-patches.sha256"

# Where Nordic publishes the nrfutil binary, one directory per host triple.
NRFUTIL_URL="https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables"
NRFUTIL_PAGE="https://www.nordicsemi.com/Products/Development-tools/nrf-util"

# ---- how this script talks --------------------------------------------------
# Phases announce themselves with ==>, their detail lines indent four spaces,
# and anything fatal goes to stderr as ERROR: with aligned continuation lines.
#
# `die` exists so every stop looks the same and none of them ends without a next
# command: this script's whole job is getting a stranger's machine from a clone
# to a build, and a stop that only says what is wrong leaves them exactly as
# stuck as no message at all.
step() { printf '==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
# HANDLED is set by anything that has already explained itself — die(), the
# interrupt handler, and the successful end of the script. The exit trap prints
# only when it is still 0, i.e. when the run stopped somewhere that said nothing.
HANDLED=0
die() {
  HANDLED=1
  printf 'ERROR: %s\n' "$1" >&2; shift
  local line
  for line in "$@"; do
    if [ -z "$line" ]; then printf '\n' >&2; else printf '       %s\n' "$line" >&2; fi
  done
  exit 1
}

# Every phase below is resumable — the fetch sentinel and the pre-patch reset
# both exist so a second run picks up where an interrupted one stopped. So say
# that, rather than leaving a bare ^C over a half-populated multi-GB directory
# that anyone would reasonably read as damage.
PHASE="starting up"
on_signal() {
  HANDLED=1
  printf '\n'
  info "interrupted — nothing is broken; re-run 'make bootstrap' and it resumes"
  exit 130
}

# The exit status has to be forced here. On a `set -e`/`set -u` abort bash runs
# this trap with $? already reset to 0, and then adopts the trap's own status as
# the script's — so a handler that just prints and returns turns every one of
# those aborts into a silent success. That is the exact failure mode this file
# is meant to remove from someone else's afternoon, so: name the phase, and exit
# nonzero on purpose.
on_exit() {
  local rc=$?
  [ "$HANDLED" -eq 1 ] && exit "$rc"
  [ "$rc" -eq 0 ] && rc=1
  printf '\nERROR: bootstrap failed while %s (exit %d)\n' "$PHASE" "$rc" >&2
  printf '       re-run '"'"'make bootstrap'"'"' — it resumes rather than starting over.\n' >&2
  printf '       still stuck? docs/troubleshooting.md, section "Build and flash"\n' >&2
  exit "$rc"
}
trap on_signal INT TERM
trap on_exit EXIT

# Yes/no for the one thing this script offers to install for you. Mirrors
# DOCS_AUTO in mk/web.mk, including the rule that no terminal means no.
#
#   SETUP_AUTO=1   yes, no prompt   (CI, containers, scripted first-run setup)
#   SETUP_AUTO=0   no, no prompt    (locked-down machines, or just quiet)
SETUP_AUTO="${SETUP_AUTO:-}"
ask() {
  case "$SETUP_AUTO" in
    1) return 0 ;;
    0) info "declined by SETUP_AUTO=0: $1"; return 1 ;;
  esac
  if [ ! -t 0 ]; then
    info "skipped: $1  (no terminal to ask; SETUP_AUTO=1 to allow)"
    return 1
  fi
  printf '    %s [y/N] ' "$1"
  read -r reply </dev/tty || return 1
  case "$reply" in
    y|Y|yes|YES) return 0 ;;
    *) info "declined"; return 1 ;;
  esac
}

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

# How this host installs things, for the hints below. Unknown is fine: the
# fallback names the tool and lets the reader's package manager take it.
if command -v brew >/dev/null 2>&1;       then PKG="brew install"
elif command -v apt-get >/dev/null 2>&1;  then PKG="sudo apt-get install -y"
elif command -v dnf >/dev/null 2>&1;      then PKG="sudo dnf install -y"
elif command -v pacman >/dev/null 2>&1;   then PKG="sudo pacman -S"
elif command -v zypper >/dev/null 2>&1;   then PKG="sudo zypper install"
else                                           PKG=""
fi
pkg_hint() {   # $1 = package name
  if [ -n "$PKG" ]; then printf '%s %s' "$PKG" "$1"
  else printf 'install %s with this system'\''s package manager' "$1"
  fi
}

# The two the fetch and patch phases cannot work around. python3 is here because
# scripts/integration-patch-id.py stamps the workspace at the end — a miss there
# wastes the entire fetch, which is the same mistake the toolchain check below
# was written to stop making. curl is deliberately NOT in this list: only the
# optional download below needs it, and a machine that already has nrfutil
# should not be stopped over a tool this run will never call.
missing=""
for t in git python3; do
  command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
if [ -n "$missing" ]; then
  hints=()
  for t in $missing; do
    hints+=("$(printf '%-8s %s' "$t" "$(pkg_hint "$t")")")
  done
  die "this machine is missing:$missing" "" "${hints[@]}" "" "then re-run: make bootstrap"
fi

# Disk. The workspace is about 6.5 GB fetched and the toolchain about 2 GB
# unpacked, and a short disk surfaces as a git or nrfutil failure thousands of
# lines deep. Ask df instead, and only for what this run will actually pull:
# a re-run over a populated workspace needs almost nothing.
need_gb=0
[ -f "$WS/.ultrawidelock-fetch-done" ] || need_gb=$((need_gb + 8))
free_gb() {   # $1 = path; echoes GiB free on the filesystem holding it, or nothing
  local probe="$1" kb
  while [ ! -d "$probe" ] && [ "$probe" != "/" ]; do probe="$(dirname "$probe")"; done
  kb="$(df -Pk "$probe" 2>/dev/null | awk 'NR==2 {print $4}')" || return 0
  case "$kb" in ''|*[!0-9]*) return 0 ;; esac
  printf '%d' $((kb / 1024 / 1024))
}
if [ "$need_gb" -gt 0 ]; then
  have_gb="$(free_gb "$WS")"
  if [ -n "$have_gb" ] && [ "$have_gb" -lt "$need_gb" ]; then
    die "not enough free disk for the workspace" \
        "need about ${need_gb} GB, ${have_gb} GB free on the volume holding $WS" \
        "" \
        "free some space, or put the workspace on another disk:" \
        "  ULTRAWIDELOCK_WS=/big/disk/ultrawidelock-ws make bootstrap" \
        "in a linked worktree, 'make ws-seed' clones the primary's workspace for ~0 disk"
  fi
  info "disk  ·  ${have_gb:-?} GB free, about ${need_gb} GB needed"
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

# Network. A warning, never a stop: this is one HEAD request against one host,
# and a proxy that refuses it can still be a proxy the fetch works through.
# Its value is in the failure that follows — an unreachable github.com explains
# a 'west update' error far better than the error does.
if [ ! -f "$WS/.ultrawidelock-fetch-done" ] && command -v curl >/dev/null 2>&1 &&
   ! curl -sSf -m 10 -o /dev/null -I https://github.com 2>/dev/null; then
  info "warning: cannot reach github.com right now — the fetch below needs it"
  info "         behind a proxy? export https_proxy=… and re-run"
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
  if [ "$installed" -eq 0 ] && nrfutil sdk-manager toolchain list --styling never 2>/dev/null \
     | grep -q "^${NCS_VER}[[:space:]]"; then
    installed=1
  fi

  if [ "$installed" -eq 1 ]; then
    info "already installed — nothing to fetch"
  else
    # A version Nordic does not publish fails several hundred MB in with one
    # line of Rust error. Ask the index first, and if the pin is not there,
    # answer the question the reader is about to have: what IS there.
    if avail="$(nrfutil sdk-manager search --styling never 2>/dev/null)" &&
       [ -n "$avail" ] && ! printf '%s\n' "$avail" | grep -q "[[:space:]]${NCS_VER}[[:space:]]"; then
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
PHASE="applying the integration patches"
step "applying integration patches"
# Apply patch files to a repository, resetting it to its pinned HEAD first.
#
# That reset is what makes bootstrap idempotent -- the previous run's patches have
# to come off before this run's go on -- but hand-editing $WS is the normal way
# upstream gets debugged here, and those edits look identical to it. So say what is
# about to go, and keep a copy: a run that silently eats an afternoon of debugging
# is the worst thing this script can do. ULTRAWIDELOCK_KEEP_WS_EDITS=1 stops instead, for
# when the edits are the point and re-patching is not.
apply_to() {   # $1 = repo, remaining args = patch files
  local repo="$1"; shift
  local dirty saved
  dirty="$(git -C "$repo" status --porcelain --untracked-files=no)"
  if [ -n "$dirty" ]; then
    if [ "${ULTRAWIDELOCK_KEEP_WS_EDITS:-0}" = 1 ]; then
      echo "ERROR: $repo has local changes and ULTRAWIDELOCK_KEEP_WS_EDITS=1 — stopping" >&2
      printf '%s\n' "$dirty" | sed 's/^/      /' >&2
      exit 1
    fi
    mkdir -p "$WS/.ultrawidelock-discarded"
    saved="$WS/.ultrawidelock-discarded/$(basename "$repo")-$(date -u +%Y%m%dT%H%M%SZ).patch"
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
# HA=1 also applies the Home Assistant data-model patches: the DoorLock
# LockOperation event and the UWB-proximity occupancy endpoint. Off by default
# because they change the Matter data model of an already-commissioned lock, and
# that has not been validated on hardware. They are a pair, not independent: the
# occupancy patch is cut against a tree with the LockOperation one applied, so
# applying it alone will not apply cleanly. Pair with `make build HA=1`.
ha_patches=()
if [ "${HA:-0}" = 1 ]; then
  ha_patches=("$P/ha-lockoperation-event.patch" "$P/ha-occupancy-endpoint.patch")
fi

apply_to "$ADDON"                 "$P/custom_impl-uwb.patch" "$P/crypto-timesync-tap.patch" "$P/pretty-shell.patch" "$P/cred-shell-factoryreset.patch" "$P/console-quiet-flood.patch" "$P/kpersistent-orphan-selfheal.patch" "$P/cred-doc-time-ratchet.patch" "$P/cred-time-persist.patch" "$P/extnvs-rollback-mirror-id.patch" "$P/approach-direction-cluster.patch" "$P/nfc-transport-seam.patch" ${ha_patches[@]+"${ha_patches[@]}"}
apply_to "$WS/nrf"                "$P/nrf-flashfit-dfu-guards.patch"
apply_to "$WS/modules/lib/matter" "$P/matter-ble-multi-identity.patch"

# A dirty repository proves that some patch exists, not that it is today's
# patch set. Record the exact patch contents and optional HA mode so build.sh
# rejects a workspace left behind by an older checkout.
"$TREE/scripts/integration-patch-id.py" "$P" "${HA:-0}" >"$PATCH_STATE"

# Calculate number of patches applied to $ADDON (always 11 base patches plus the HA patches if HA=1)
addon_patch_count=$((11 + ${#ha_patches[@]}))
total_patch_count=$((13 + ${#ha_patches[@]}))

echo "    ✓ pristine upstream + $total_patch_count patches (add-on ×$addon_patch_count, nrf, matter)"

HANDLED=1
step "ready. Build with:  make build"
