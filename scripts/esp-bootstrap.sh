#!/usr/bin/env bash
#
# esp-bootstrap.sh — ESP-IDF, and on request esp-matter, for the ESP32 ports.
#
# The sibling of scripts/bootstrap.sh, which does the same job for the two
# Zephyr ports. They share scripts/lib/setup.sh, so both stop, ask and resume
# the same way; what differs is only what gets installed.
#
# Two stages, because the disk costs differ by an order of magnitude and
# docs/porting-esp32.md already tells people to stage it this way:
#
#   1. ESP-IDF          about 5 GB   enough for APP=reader, satellite, initiator
#   2. esp-matter       about 15 GB  additionally needed by APP=matter-lock
#
# Nothing is downloaded without being asked first (SETUP_AUTO=1 answers yes,
# SETUP_AUTO=0 answers no), and both stages are idempotent: an existing install
# is re-pinned and reused rather than refetched.
#
# Usage:  make esp-bootstrap                    # stage 2 too, because APP defaults to matter-lock
#         make esp-bootstrap APP=reader         # stage 1 only
#         make esp-bootstrap ESP_MATTER=1       # force stage 2 for any APP
#
# Everything below is overridable, and the two paths deliberately come from the
# same variables mk/esp32.mk builds with — so what this installs is what the
# build looks for, and there is no third place for them to disagree.
set -euo pipefail

TREE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/setup.sh
. "$TREE/scripts/lib/setup.sh"

IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
IDF_DIR="$(dirname "$IDF_EXPORT")"
ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"

# The versions the bench builds against, recorded in mk/esp32.mk and now also
# the default this script installs. Not a pin the build enforces: mk/esp32.mk
# fixes paths and nothing else, so a machine that already carries another pair
# keeps working exactly as before. It is a reproducible starting point for a
# machine that carries none, which is the only thing this script is for.
#
# v5.5.4 is not our choice either — it is the version esp-matter asks for at the
# revision below (docs/en/developing.rst there names it outright).
IDF_VER="${IDF_VER:-v5.5.4}"
ESP_MATTER_REV="${ESP_MATTER_REV:-93b168027bf95f4e467230345ad1f2d147eb1f6b}"

# Which chip toolchains to fetch. One target is a couple of GB; "all" is many,
# and installing every architecture for a bench that has one board on it is the
# single easiest way to spend 10 GB by accident.
ESP_TARGETS="${ESP_TARGETS:-esp32s3}"

# esp-matter's install.sh builds chip-tool and chip-cert by default, which is
# tens of minutes of C++ that a firmware build never touches. Off here; set
# ESP_HOST_TOOLS=1 if you want chip-tool on the bench for commissioning.
ESP_HOST_TOOLS="${ESP_HOST_TOOLS:-0}"
ESP_MATTER_CREATED=0
ESP_MATTER_FETCHED=0

# What this script would install, for anything that wants to display it rather
# than restate it. mk/esp32.mk's `esp-env` reads this, so the versions live in
# exactly one file and a reader is never shown a second, staler copy.
if [ "${1:-}" = --print-pins ]; then
  printf 'IDF_VER=%s\nESP_MATTER_REV=%s\n' "$IDF_VER" "$ESP_MATTER_REV"
  exit 0
fi

# Past the query: from here this is a run, and a run gets the traps.
setup_init "esp-bootstrap" "make esp-bootstrap"

# Stage 2 is implied by the app that needs it. APP comes from make, and defaults
# there to matter-lock.
if [ -z "${ESP_MATTER:-}" ]; then
  case "${APP:-matter-lock}" in
    matter-lock) ESP_MATTER=1 ;;
    *)           ESP_MATTER=0 ;;
  esac
fi

# =============================================================================
# 0. Preflight — before anything is fetched. Same rules as the NCS bootstrap:
#    every gap in one pass, and never a gap without the command that closes it.
# =============================================================================
PHASE="checking this machine"
step "preflight"

require_tools git python3

# $HOME/esp is one tree shared by every worktree on this machine, unlike the NCS
# workspace which ws-seed.sh gives each worktree its own copy of. So take a lock:
# two of these running at once would fetch submodules over each other.
setup_lock "$IDF_DIR"

# ESP-IDF's own installer brings its cmake and ninja along, so these are a note
# rather than a stop — but a machine missing them is a machine that will fail
# later at a less obvious moment, and the note costs nothing.
suggest_tools cmake ninja dfu-util

if [ ! -d "$IDF_DIR/.git" ]; then
  need_disk "$IDF_DIR" 5 \
      "ESP-IDF plus one chip toolchain is about 5 GB." \
      "install it elsewhere with:  make esp-bootstrap IDF_EXPORT=/big/disk/esp-idf/export.sh"
fi
if [ "$ESP_MATTER" = 1 ] && [ ! -d "$ESP_MATTER_PATH/.git" ]; then
  need_disk "$ESP_MATTER_PATH" 15 \
      "esp-matter carries connectedhomeip, which is most of that." \
      "install it elsewhere with:  make esp-bootstrap ESP_MATTER_PATH=/big/disk/esp-matter" \
      "or skip it — APP=reader, satellite and initiator do not need it:" \
      "  make esp-bootstrap APP=reader"
fi
warn_offline

info "esp-idf      $IDF_VER  ->  $IDF_DIR"
if [ "$ESP_MATTER" = 1 ]; then
  info "esp-matter   ${ESP_MATTER_REV:0:10}…  ->  $ESP_MATTER_PATH"
else
  info "esp-matter   skipped  ·  APP=${APP:-matter-lock} does not need it (ESP_MATTER=1 to install anyway)"
fi

# =============================================================================
# 1. ESP-IDF.
#
#    The clone is shallow at the tag and its submodules are shallow with it:
#    same tree state, a fraction of the size and the time. install.sh then
#    fetches the compiler for $ESP_TARGETS into ~/.espressif (or $IDF_TOOLS_PATH).
#    Re-running it is cheap — it reports what is already there and fetches only
#    what is not — which is what makes this whole stage safe to repeat.
# =============================================================================
PHASE="installing ESP-IDF $IDF_VER"
step "ESP-IDF $IDF_VER"

if [ ! -d "$IDF_DIR/.git" ]; then
  ask "clone ESP-IDF $IDF_VER into $IDF_DIR? about 1.5 GB" || die \
      "ESP-IDF is what the ESP32 ports build with" \
      "install it yourself and point the build at it:" \
      "  make esp-build IDF_EXPORT=/path/to/esp-idf/export.sh" \
      "or accept without the prompt:  SETUP_AUTO=1 make esp-bootstrap"
  mkdir -p "$(dirname "$IDF_DIR")" || die "cannot create $(dirname "$IDF_DIR")" \
      "choose another location: make esp-bootstrap IDF_EXPORT=/somewhere/writable/esp-idf/export.sh"
  info "cloning (shallow at the tag, with submodules)"
  git clone -q --branch "$IDF_VER" --depth 1 --shallow-submodules --recursive \
      https://github.com/espressif/esp-idf.git "$IDF_DIR" || die \
      "the ESP-IDF clone failed" \
      "a partial clone is not reused, so it is safe to delete and retry:" \
      "  rm -rf '$IDF_DIR' && make esp-bootstrap"
else
  # An existing checkout is re-pinned rather than refetched. `describe` rather
  # than `rev-parse HEAD`, because a shallow clone at a tag has the tag and not
  # much else to compare against.
  have="$(git -C "$IDF_DIR" describe --tags --always 2>/dev/null || echo unknown)"
  # An IDF tree is not ours the way the west workspace is. bootstrap.sh resets
  # that one because the patches in it are this repo's and can be reapplied;
  # nothing here can put back somebody's local fix to an ESP-IDF component. So
  # this stops rather than resets, and the way past it is theirs to choose.
  # --ignore-submodules=all, because an installed IDF is never clean without it:
  # a shallow submodule checkout leaves several gitlinks reading as modified, so
  # the plain form calls every working install dirty and would refuse every
  # version bump anybody ever attempts. Real edits to real files still show.
  dirty="$(git -C "$IDF_DIR" status --porcelain --untracked-files=no --ignore-submodules=all 2>/dev/null || true)"
  if [ -n "$dirty" ] && [ "$have" != "$IDF_VER" ]; then
    die "$IDF_DIR has local changes, and is at $have rather than $IDF_VER" \
        "" \
        "$(printf '%s\n' "$dirty" | head -10 | sed 's/^/  /')" \
        "" \
        "nothing here will discard those. Either keep this tree and build with it:" \
        "  make esp-bootstrap IDF_VER=$have" \
        "or put the changes somewhere safe first:" \
        "  git -C '$IDF_DIR' stash   # then re-run"
  fi
  if [ "$have" != "$IDF_VER" ]; then
    info "found $have, wanted $IDF_VER"
    if ask "check out $IDF_VER in $IDF_DIR?"; then
      git -C "$IDF_DIR" fetch -q --depth 1 origin "refs/tags/$IDF_VER:refs/tags/$IDF_VER" 2>/dev/null || true
      git -C "$IDF_DIR" checkout -q "$IDF_VER" || die \
          "could not check out $IDF_VER in $IDF_DIR" \
          "that checkout may be shallow at another revision; the simplest fix is a fresh one:" \
          "  rm -rf '$IDF_DIR' && make esp-bootstrap" \
          "or keep what is there and build with it:  make esp-bootstrap IDF_VER=$have"
      git -C "$IDF_DIR" submodule update --init --depth 1 --recursive
    else
      info "keeping $have — the build will use it"
    fi
  else
    info "already at $IDF_VER"
  fi

  # Reconcile the submodules whatever the version turned out to be. This is the
  # hole the first real install fell into: a tree at the right tag whose
  # submodules are empty or at other commits passes every check above, and
  # install.sh does not care either — it is building a Python environment, not
  # compiling components. The failure surfaces a build later as
  #
  #     CMake Error: Include directory '…/components/unity/unity/src' is not a
  #     directory
  #
  # which names neither git nor a submodule and is nobody's idea of a clue. A
  # clone made without --recursive is enough to produce it, and this repo cannot
  # assume it made the clone: $HOME/esp is shared, and people arrive with a tree
  # already in it. So ask the tree what it should contain, and fetch whatever it
  # is missing. Costs one status walk when everything is already right.
  sub_state="$(git -C "$IDF_DIR" submodule status --recursive 2>/dev/null || true)"
  if text_has '^[-+]' "$sub_state"; then
    info "submodules are missing or off their recorded commits — fetching them"
    git -C "$IDF_DIR" submodule update --init --depth 1 --recursive || die \
        "could not fetch the ESP-IDF submodules in $IDF_DIR" \
        "the build needs them: a component whose submodule is empty fails cmake" \
        "with an include directory that 'is not a directory'." \
        "" \
        "run it by hand to see why:" \
        "  git -C '$IDF_DIR' submodule update --init --depth 1 --recursive"
  fi
fi

PHASE="installing the ESP-IDF toolchain for $ESP_TARGETS"
info "toolchain for: $ESP_TARGETS  (ESP_TARGETS=all for every chip)"
( cd "$IDF_DIR" && ./install.sh "$ESP_TARGETS" ) || die \
    "ESP-IDF's install.sh failed" \
    "it names its own reason above; the command was:" \
    "  cd '$IDF_DIR' && ./install.sh $ESP_TARGETS" \
    "missing system packages are the usual cause — see Espressif's prerequisites:" \
    "  https://docs.espressif.com/projects/esp-idf/en/$IDF_VER/esp32s3/get-started/#installation"

[ -f "$IDF_EXPORT" ] || die \
    "ESP-IDF installed, but $IDF_EXPORT is not there" \
    "mk/esp32.mk sources exactly that path, so the build cannot find this install." \
    "point them at each other:  make esp-build IDF_EXPORT=<the real export.sh>"

# =============================================================================
# 2. esp-matter, when the app needs it.
#
#    The sequence is esp-matter's own, from docs/en/developing.rst at the
#    revision pinned above: IDF activated first, then a shallow clone, then its
#    submodules, then connectedhomeip's — filtered by platform, which is what
#    keeps this at 15 GB instead of very much more.
# =============================================================================
if [ "$ESP_MATTER" = 1 ]; then
  PHASE="installing esp-matter"
  step "esp-matter ${ESP_MATTER_REV:0:10}…"

  DONE_MARK="$ESP_MATTER_PATH/.ultrawidelock-install-done"
  if [ -f "$DONE_MARK" ] && [ "$(cat "$DONE_MARK" 2>/dev/null)" = "$ESP_MATTER_REV" ]; then
    info "already installed at this revision — nothing to do"
    info "(delete $DONE_MARK to force a reinstall)"
  else
    if [ ! -d "$ESP_MATTER_PATH/.git" ]; then
      ask "install esp-matter into $ESP_MATTER_PATH? about 15 GB and the better part of an hour" || die \
          "esp-matter is what APP=matter-lock builds against" \
          "the other three ESP32 apps do not need it:" \
          "  make esp-bootstrap APP=reader" \
          "already have it somewhere? point the build at it:" \
          "  make esp-build ESP_MATTER_PATH=/path/to/esp-matter"
      # A path that exists with something in it and no .git is not a blank
      # slate. `git init` over it would adopt those files and the checkout
      # below would start overwriting them, so refuse and let whoever put them
      # there decide -- the same rule ws-seed.sh applies to a part-finished
      # workspace, for the same reason.
      if [ -d "$ESP_MATTER_PATH" ] && [ -n "$(ls -A "$ESP_MATTER_PATH" 2>/dev/null)" ]; then
        die "$ESP_MATTER_PATH exists, has files in it, and is not a git checkout" \
            "installing into it would write over whatever that is." \
            "look first, then either clear it deliberately or install elsewhere:" \
            "  make esp-bootstrap ESP_MATTER_PATH=/somewhere/else"
      fi
      mkdir -p "$ESP_MATTER_PATH"
      git init -q "$ESP_MATTER_PATH"
      git -C "$ESP_MATTER_PATH" remote add origin https://github.com/espressif/esp-matter.git
      # Armed only after the guard above proved the directory was ours to make,
      # so this can never delete anything but this run's own partial work. It
      # covers the fetch alone: once submodules are down, a failed run has hours
      # of good bytes in it and re-running repairs the tree in place.
      ESP_MATTER_CREATED=1
    fi

    esp_matter_cleanup() {
      [ "${ESP_MATTER_CREATED:-0}" = 1 ] && [ "${ESP_MATTER_FETCHED:-0}" = 0 ] &&
        rm -rf "$ESP_MATTER_PATH"
      return 0
    }
    if [ "$(git -C "$ESP_MATTER_PATH" rev-parse HEAD 2>/dev/null)" != "$ESP_MATTER_REV" ]; then
      info "fetching ${ESP_MATTER_REV:0:10}… (shallow at the pinned revision)"
      git -C "$ESP_MATTER_PATH" fetch -q --depth 1 origin "$ESP_MATTER_REV" || { esp_matter_cleanup; die \
          "could not fetch esp-matter revision ${ESP_MATTER_REV:0:10}…" \
          "ESP_MATTER_REV must be a full 40-character SHA that still exists upstream." \
          "to take whatever is current instead:" \
          "  make esp-bootstrap ESP_MATTER_REV=\$(git ls-remote https://github.com/espressif/esp-matter HEAD | cut -f1)"; }
      git -C "$ESP_MATTER_PATH" checkout -q FETCH_HEAD
    fi
    ESP_MATTER_FETCHED=1

    # Everything below wants ESP-IDF active. Sourcing export.sh under `set -eu`
    # is not safe — it is not written for either flag — so the whole stage runs
    # in a subshell with them relaxed, and its exit status is checked here.
    PHASE="fetching esp-matter's submodules and installing it"
    info "submodules — connectedhomeip is large and this is the slow part"
    esp_matter_install() (
      set +eu
      # shellcheck disable=SC1090
      . "$IDF_EXPORT" >/dev/null 2>&1 || { echo "could not source $IDF_EXPORT" >&2; exit 1; }
      set -e
      cd "$ESP_MATTER_PATH"
      git submodule update --init --depth 1
      chip="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip"
      # The host platform's modules are only needed to BUILD the host tools, so
      # they are fetched only when those are wanted (esp-matter's own note).
      plats="esp32"
      if [ "$ESP_HOST_TOOLS" = 1 ]; then
        case "$(uname -s)" in
          Darwin) plats="esp32 darwin" ;;
          *)      plats="esp32 linux" ;;
        esac
      fi
      # shellcheck disable=SC2086  # deliberate: plats is a list of arguments
      ( cd "$chip" && ./scripts/checkout_submodules.py --platform $plats --shallow )
      if [ "$ESP_HOST_TOOLS" = 1 ]; then
        ./install.sh
      else
        ./install.sh --no-host-tool
      fi
    )
    esp_matter_install || die \
        "esp-matter's install did not finish" \
        "it names its own reason above. It is resumable — re-run:" \
        "  make esp-bootstrap" \
        "and if the tree itself is the problem, start it over:" \
        "  rm -rf '$ESP_MATTER_PATH' && make esp-bootstrap"

    printf '%s\n' "$ESP_MATTER_REV" >"$DONE_MARK"
  fi

  [ -f "$ESP_MATTER_PATH/export.sh" ] || die \
      "esp-matter installed, but $ESP_MATTER_PATH/export.sh is not there" \
      "mk/esp32.mk sources exactly that path." \
      "point them at each other:  make esp-build ESP_MATTER_PATH=<the real tree>"
fi

# =============================================================================
# 3. Say what to run, with the paths this run actually used — so a non-default
#    location arrives as a command that works rather than as homework.
# =============================================================================
over=""
[ "$IDF_EXPORT" = "$HOME/esp/esp-idf/export.sh" ] || over="$over IDF_EXPORT=$IDF_EXPORT"
[ "$ESP_MATTER_PATH" = "$HOME/esp/esp-matter" ] || over="$over ESP_MATTER_PATH=$ESP_MATTER_PATH"
if [ "$ESP_MATTER" = 1 ]; then
  setup_done "ready. Build with:  make esp-build APP=matter-lock TARGET=$ESP_TARGETS$over"
else
  setup_done "ready. Build with:  make esp-build APP=${APP:-reader} TARGET=$ESP_TARGETS$over"
fi
