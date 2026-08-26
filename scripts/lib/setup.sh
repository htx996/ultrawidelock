#!/usr/bin/env bash
#
# setup.sh — the shared half of the two bootstraps: scripts/bootstrap.sh (NCS,
# for both Zephyr ports) and scripts/esp-bootstrap.sh (ESP-IDF and esp-matter).
#
# It carries no knowledge of either SDK. What it owns is the dialect both of
# them speak, because the thing a setup script is really for is telling a
# stranger's machine what it is missing — and two scripts that answer that
# question in two different voices are two scripts to learn instead of one.
#
#   . "$ROOT/scripts/lib/setup.sh"
#   setup_init "esp-bootstrap" "make esp-bootstrap"
#   PHASE="checking this machine"
#   step "preflight"
#   require_tools git python3
#   ask "install X? 5 MB" && ...
#   setup_done "ready. Build with:  make esp-build"
#
# The rules it enforces, both learned from the shape of the old failures:
#
#   * Report every miss in one pass. Stopping at the first one turns a single
#     setup session into as many round trips as the machine has gaps.
#   * Never report a miss without the command that closes it on THIS host.
#     A tool name is a search; `brew install git` is a fix.

# ---- output -----------------------------------------------------------------
# Phases announce themselves with ==>, their detail lines indent four spaces,
# and anything fatal goes to stderr as ERROR: with aligned continuation lines.
step() { printf '==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }

# HANDLED is set by anything that has already explained itself — die(), the
# interrupt handler, and setup_done(). The exit trap prints only when it is
# still 0, i.e. when the run stopped somewhere that said nothing.
HANDLED=0
PHASE="starting up"
SETUP_NAME="setup"
SETUP_RESUME="re-run it"

# `die` exists so every stop looks the same and none of them ends without a next
# command: these scripts' whole job is getting a stranger's machine from a clone
# to a build, and a stop that only says what is wrong leaves them exactly as
# stuck as no message at all.
die() {
  HANDLED=1
  setup_unlock
  printf 'ERROR: %s\n' "$1" >&2; shift
  local line
  for line in "$@"; do
    if [ -z "$line" ]; then printf '\n' >&2; else printf '       %s\n' "$line" >&2; fi
  done
  exit 1
}

# Every phase of both scripts is resumable. So say that, rather than leaving a
# bare ^C over a half-populated multi-GB directory that anyone would reasonably
# read as damage.
_setup_on_signal() {
  HANDLED=1
  setup_unlock
  printf '\n'
  info "interrupted — nothing is broken; run '$SETUP_RESUME' and it resumes"
  exit 130
}

# The exit status has to be forced here. On a `set -e`/`set -u` abort bash runs
# this trap with $? already reset to 0, and then adopts the trap's own status as
# the script's — so a handler that just prints and returns turns every one of
# those aborts into a silent success. That is the exact failure mode these files
# are meant to remove from someone else's afternoon, so: name the phase, and
# exit nonzero on purpose.
_setup_on_exit() {
  local rc=$?
  setup_unlock
  [ "$HANDLED" -eq 1 ] && exit "$rc"
  [ "$rc" -eq 0 ] && rc=1
  printf '\nERROR: %s failed while %s (exit %d)\n' "$SETUP_NAME" "$PHASE" "$rc" >&2
  printf "       run '%s' — it resumes rather than starting over.\n" "$SETUP_RESUME" >&2
  printf '       still stuck? docs/troubleshooting.md\n' >&2
  exit "$rc"
}

# $1 = what this run is called in messages, $2 = the command that resumes it.
setup_init() {
  SETUP_NAME="$1"
  SETUP_RESUME="$2"
  trap _setup_on_signal INT TERM
  trap _setup_on_exit EXIT
}

# The run finished. Marks the exit trap quiet and prints the closing line.
setup_done() {
  HANDLED=1
  step "$*"
}

# ---- asking ------------------------------------------------------------------
# Yes/no for the things these scripts offer to install for you. Mirrors
# DOCS_AUTO in mk/web.mk, including the rule that no terminal means no.
#
#   SETUP_AUTO=1   yes, no prompt   (CI, containers, scripted first-run setup)
#   SETUP_AUTO=0   no, no prompt    (locked-down machines, or just quiet)
SETUP_LOCK=""
SETUP_AUTO="${SETUP_AUTO:-}"
ask() {
  case "$SETUP_AUTO" in
    1) info "$1  ·  yes (SETUP_AUTO=1)"; return 0 ;;
    0) info "declined by SETUP_AUTO=0: $1"; return 1 ;;
  esac
  if [ ! -t 0 ]; then
    info "skipped: $1  (no terminal to ask; SETUP_AUTO=1 to allow)"
    return 1
  fi
  printf '    %s [y/N] ' "$1"
  local reply
  read -r reply </dev/tty || return 1
  case "$reply" in
    y|Y|yes|YES) return 0 ;;
    *) info "declined"; return 1 ;;
  esac
}

# ---- host tools --------------------------------------------------------------
# How this host installs things, for the hints below. Unknown is fine: the
# fallback names the tool and lets the reader's package manager take it.
if command -v brew >/dev/null 2>&1;       then PKG="brew install"
elif command -v apt-get >/dev/null 2>&1;  then PKG="sudo apt-get install -y"
elif command -v dnf >/dev/null 2>&1;      then PKG="sudo dnf install -y"
elif command -v pacman >/dev/null 2>&1;   then PKG="sudo pacman -S"
elif command -v zypper >/dev/null 2>&1;   then PKG="sudo zypper install"
else                                           PKG=""
fi

# $1 = command name. Echoes the line that installs it here. The package is not
# always the command: apt calls ninja ninja-build, and a hint that does not
# paste is barely better than no hint.
pkg_hint() {
  local pkg="$1"
  case "$PKG:$1" in
    "sudo apt-get install -y:ninja") pkg="ninja-build" ;;
    "sudo dnf install -y:ninja")     pkg="ninja-build" ;;
    "sudo apt-get install -y:python3") pkg="python3 python3-pip python3-venv" ;;
  esac
  if [ -n "$PKG" ]; then printf '%s %s' "$PKG" "$pkg"
  else printf 'install %s with this system'\''s package manager' "$pkg"
  fi
}

# Stop unless every named command is on PATH, reporting all of the misses at
# once with the command that closes each one.
require_tools() {
  local missing="" t
  for t in "$@"; do
    command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
  done
  [ -n "$missing" ] || return 0
  local hints=()
  for t in $missing; do
    hints+=("$(printf '%-8s %s' "$t" "$(pkg_hint "$t")")")
  done
  die "this machine is missing:$missing" "" "${hints[@]}" "" "then re-run: $SETUP_RESUME"
}

# Report the named commands that are absent, without stopping. For tools a
# stage recommends rather than requires.
suggest_tools() {
  local t
  for t in "$@"; do
    command -v "$t" >/dev/null 2>&1 && continue
    info "note: no '$t' here — $(pkg_hint "$t")"
  done
}

# ---- text ---------------------------------------------------------------------
# `cmd | grep -q PAT` is wrong in every file that sets `-o pipefail`, which is
# all of them. -q exits the moment it matches, the writer upstream gets SIGPIPE
# and dies with 141, pipefail adopts that as the pipeline's status — so the case
# where the pattern IS found reports failure. The guard inverts, silently, and
# only when the input is long enough for the writer to still be writing.
#
# Capture first, then match, and use -c so grep always drains its input.
text_has() {   # $1 = pattern, $2 = text
  local n
  n="$(printf '%s\n' "$2" | grep -c -- "$1" || true)"
  [ "${n:-0}" -gt 0 ]
}

# ---- disk --------------------------------------------------------------------
# GiB free on the filesystem holding $1, or nothing when df cannot say. Walks up
# to the nearest existing parent, because the directory being sized usually does
# not exist yet.
free_gb() {
  local probe="$1" kb
  while [ ! -d "$probe" ] && [ "$probe" != "/" ]; do probe="$(dirname "$probe")"; done
  kb="$(df -Pk "$probe" 2>/dev/null | awk 'NR==2 {print $4}')" || return 0
  case "$kb" in ''|*[!0-9]*) return 0 ;; esac
  printf '%d' $((kb / 1024 / 1024))
}

# $1 = path, $2 = GiB needed, rest = extra lines for the stop message. A short
# disk otherwise surfaces as a git or installer failure thousands of lines deep.
need_disk() {
  local path="$1" need="$2"; shift 2
  local have; have="$(free_gb "$path")"
  [ -n "$have" ] || return 0
  if [ "$have" -lt "$need" ]; then
    die "not enough free disk" \
        "need about ${need} GB, ${have} GB free on the volume holding $path" \
        "" "$@"
  fi
  # Named, because a run that checks two volumes prints two of these and an
  # unlabelled pair is a puzzle rather than a report.
  info "disk  ·  ${have} GB free, about ${need} GB needed for $(basename "$path")"
}

# ---- one run at a time -------------------------------------------------------
# Only needed where an install is SHARED, which both of these now are: the NCS
# workspace store holds one tree per patch set for the whole machine (see
# scripts/lib/ws.sh) and $HOME/esp is one tree every worktree installs into, so
# two runs started in two checkouts would fetch over each other either way.
#
# mkdir is the lock: it is atomic on every filesystem this runs on, unlike a
# test-then-create on a file. The holder's PID goes inside, so a lock left by a
# killed run is recognised as dead and taken over rather than blocking the
# machine forever.
setup_lock() {   # $1 = a path the lock should be named after
  local key lock owner
  key="$(printf '%s' "$1" | cksum | awk '{print $1}')"
  lock="${TMPDIR:-/tmp}/ultrawidelock-setup-$key.lock"
  if ! mkdir "$lock" 2>/dev/null; then
    owner="$(cat "$lock/pid" 2>/dev/null || echo '')"
    if [ -n "$owner" ] && kill -0 "$owner" 2>/dev/null; then
      die "another setup run (pid $owner) is already installing into $1" \
          "wait for it to finish, or stop it and re-run this one." \
          "if you are sure nothing is running:  rm -rf '$lock'"
    fi
    info "clearing a lock left by a run that did not finish (pid ${owner:-unknown})"
    rm -rf "$lock"
    mkdir "$lock" 2>/dev/null || die "cannot take the setup lock at $lock" \
        "remove it by hand if nothing is running:  rm -rf '$lock'"
  fi
  printf '%s\n' "$$" >"$lock/pid"
  SETUP_LOCK="$lock"
}
# Released from the exit path so a stop, an interrupt and a success all drop it.
setup_unlock() { [ -n "${SETUP_LOCK:-}" ] && rm -rf "$SETUP_LOCK"; SETUP_LOCK=""; }

# ---- network -----------------------------------------------------------------
# A warning, never a stop: this is one HEAD request against one host, and a
# proxy that refuses it can still be a proxy the fetch works through. Its value
# is in the failure that follows — an unreachable github.com explains a fetch
# error far better than the error does.
warn_offline() {
  command -v curl >/dev/null 2>&1 || return 0
  curl -sSf -m 10 -o /dev/null -I "${1:-https://github.com}" 2>/dev/null && return 0
  info "warning: cannot reach ${1:-https://github.com} right now — the fetch below needs it"
  info "         behind a proxy? export https_proxy=… and re-run"
}
