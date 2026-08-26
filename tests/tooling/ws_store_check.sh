#!/usr/bin/env bash
#
# ws_store_check.sh — the workspace store decides which 5.5 GB tree a checkout
# builds against. Everything here is about the two ways that can go wrong.
#
#   Two trees that differ get one name.   Every checkout on one of them builds
#                                         the other one's patches, which is the
#                                         failure the store exists to end.
#   Two trees that agree get two names.   Harmless, expensive, and invisible:
#                                         a fetch nobody asked for.
#
# So the identity functions are checked from both sides -- what has to change
# the name, and what must not -- and the operations that hand a checkout a tree
# (link, adopt, ws-link.sh itself) are run for real against fixtures small
# enough to build in a temp directory.
#
# Whether the patches still APPLY is a different question, answered by
# tests/tooling/patch_drift_check.sh against real upstream over the network.
# What is checked here is that one implementation of applying them is all there
# is, since two would be how two different trees end up under one name.
#
# Usage: tests/tooling/ws_store_check.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PASS=0; FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL %s\n' "$1"; }
is()   { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1"; printf '       want %s\n       got  %s\n' "$3" "$2"; fi; }
isnt() { if [ "$2" != "$3" ]; then ok "$1"; else bad "$1"; printf '       both were %s\n' "$2"; fi; }
yes()  { if "${@:2}"; then ok "$1"; else bad "$1"; fi; }
no()   { if "${@:2}"; then bad "$1"; else ok "$1"; fi; }

# pwd -P, because ws_resolved reports a fully resolved path and $TMPDIR on macOS
# is reached through a symlink: without this every comparison here fails on
# /var versus /private/var and says nothing about the code.
WORK="$(cd "$(mktemp -d)" && pwd -P)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck source=scripts/lib/setup.sh
. "$ROOT/scripts/lib/setup.sh"
# shellcheck source=scripts/lib/ws.sh
. "$ROOT/scripts/lib/ws.sh"

PIN_A=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
PIN_B=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb

printf '\n== what names a workspace ==\n'

# A name that moves when nothing moved would split every checkout onto its own
# tree; one that stays put when the pin changes would put the wrong upstream
# under the right name. Both directions, for both halves of the name.
fa="$(ws_fetch_key "$PIN_A" v3.3.0)"
is   "fetch key is deterministic"          "$(ws_fetch_key "$PIN_A" v3.3.0)" "$fa"
isnt "fetch key follows the add-on pin"    "$(ws_fetch_key "$PIN_B" v3.3.0)" "$fa"
isnt "fetch key follows the NCS version"   "$(ws_fetch_key "$PIN_A" v3.4.0)" "$fa"

# The fetch half is a PREFIX, and ws_clone_source globs on it. If that ever
# stops being true, nothing breaks loudly: every clonable tree simply stops
# being found and every checkout refetches.
n1="$(ws_entry_name "$fa" 1111111111111111)"
n2="$(ws_entry_name "$fa" 2222222222222222)"
is   "entries share the fetch prefix"      "${n1%%-*}" "${n2%%-*}"
isnt "entries differ by patch set"         "$n1" "$n2"
is   "entry name is short enough to read"  "${#n1}" 25

printf '\n== an entry is finished, or it is not ==\n'

STORE="$WORK/store"; mkdir -p "$STORE"
half="$STORE/$(ws_entry_name "$fa" cccccccccccc)"   # a clone that was interrupted
full="$STORE/$(ws_entry_name "$fa" dddddddddddd)"   # a finished tree
other="$STORE/$(ws_entry_name "$(ws_fetch_key "$PIN_B" v3.3.0)" eeeeeeeeeeee)"
mkdir -p "$half/.west" "$full/.west" "$other/.west"
ws_stamp "$full" "$PIN_A" v3.3.0 dddddddddddd
ws_stamp "$other" "$PIN_B" v3.3.0 eeeeeeeeeeee

no  "a tree with no stamp is not an entry"     ws_entry_ready "$half"
yes "a stamped tree is"                        ws_entry_ready "$full"
no  "a stamp with no .west is not either"      ws_entry_ready "$STORE/nothing-here"
is  "the stamp records the pin it was fetched at" \
    "$(sed -n 's/^pin=//p' "$full/.ultrawidelock-ws.id")" "$PIN_A"

# The clone source is the whole reason a new patch set costs seconds. Picking
# one from another pin would produce a tree whose name is a lie.
is   "clones come from the same upstream"  "$(ws_clone_source "$STORE" "$fa" "$half")" "$full"
no   "an unfinished tree is never cloned"  test "$(ws_clone_source "$STORE" "$fa" "$full" 2>/dev/null)" = "$half"
no   "no source across a different pin"    ws_clone_source "$STORE" "$(ws_fetch_key cccccccccccccccccccccccccccccccccccccccc v3.3.0)" ""

printf '\n== handing a checkout a tree ==\n'

TREE="$WORK/checkout"; mkdir -p "$TREE"
yes "links a checkout at an entry"         ws_link "$TREE" "$full"
is  "and ws_resolved reads it back"        "$(ws_resolved "$TREE")" "$full"
yes "relinks over an existing link"        ws_link "$TREE" "$other"
is  "which now points at the other tree"   "$(ws_resolved "$TREE")" "$other"

# 5.5 GB somebody fetched is never deleted to make room for a link. This is the
# check that keeps ws_link from being a way to lose a workspace.
rm -f "$TREE/workspace"; mkdir -p "$TREE/workspace/.west"
no  "refuses to replace a real directory"  ws_link "$TREE" "$full" 2>/dev/null
yes "and leaves it where it was"           test -d "$TREE/workspace/.west"

printf '\n== adopting a pre-store workspace ==\n'

# Ahead of ws_adopt, because ws_adopt calls this and a false negative here shows
# up there as a confusing "different volumes" on a machine with one. `stat -f`
# is a format string to BSD and --file-system to GNU, and `%d` is valid under
# both -- the device number to one, free inodes to the other -- so getting the
# order wrong does not fail on Linux, it returns a number that changes as the
# filesystem is written to. Two paths under one temp directory are on one
# volume by construction, and the same path is on its own volume by tautology;
# both were false on a Linux runner and true here, which is what let it ship.
yes "one volume: two paths under one tmpdir"  ws_same_volume "$WORK" "$STORE"
yes "one volume: a path against itself"       ws_same_volume "$STORE" "$STORE"
yes "and it survives being asked twice"       ws_same_volume "$WORK" "$STORE"

# The three above pass on macOS whichever order ws_same_volume asks in, so they
# cannot catch the bug on the machine everyone develops on. Stub stat with GNU's
# semantics and they can. The counter is a FILE because ws_same_volume calls
# stat inside $(...): a shell variable resets in the subshell, the drift never
# happens, and a broken order looks fine -- which is how the first version of
# this check passed against code that was still wrong.
GNU_INODES="$WORK/gnu-free-inodes"; echo 900000 >"$GNU_INODES"
stat() {
  case "$1" in
    -c) [ "$2" = '%d' ] && { echo 2049; return 0; } ;;      # device: one volume
    -f) [ "$2" = '%d' ] && {                                # --file-system: free inodes
          local n; n=$(( $(cat "$GNU_INODES") - 7 ))
          echo "$n" >"$GNU_INODES"; echo "$n"; return 0; } ;;
  esac
  return 1
}
yes "one volume under GNU stat semantics"     ws_same_volume "$WORK" "$STORE"
unset -f stat
yes "and the real stat still agrees"          ws_same_volume "$WORK" "$STORE"

# ws_adopt reads the add-on's HEAD, so the fixture needs a real repository. Its
# commit doubles as the pin: adoption is exactly the question "is this tree at
# the revision the name would claim it is at?"
ADDON="$TREE/workspace/ncs-door-lock-and-access-control"
mkdir -p "$ADDON"
git -C "$ADDON" init -q
git -C "$ADDON" -c user.email=t@t -c user.name=t commit -q --allow-empty -m fixture
REAL_PIN="$(git -C "$ADDON" rev-parse HEAD)"

# A tree at another revision under this name would hand every later checkout the
# wrong upstream. die() exits, so this runs in a subshell.
no  "refuses a tree at another revision" \
    bash -c '. "$1/scripts/lib/setup.sh"; . "$1/scripts/lib/ws.sh"; ws_adopt "$2" "$3" "$4" "$5"' \
    _ "$ROOT" "$TREE" "$STORE/adopted" "$PIN_A" "$STORE" 2>/dev/null

adopted="$STORE/$(ws_entry_name "$(ws_fetch_key "$REAL_PIN" v3.3.0)" ffffffffffff)"
yes "adopts a tree at the pinned revision" ws_adopt "$TREE" "$adopted" "$REAL_PIN" "$STORE"
yes "the tree moved into the store"        test -d "$adopted/ncs-door-lock-and-access-control"
no  "and is gone from the checkout"        test -e "$TREE/workspace"
no  "a checkout with no workspace adopts nothing" ws_adopt "$TREE" "$adopted" "$REAL_PIN" "$STORE"

printf '\n== ws-link.sh, end to end ==\n'

# A checkout is its patches plus a pin, so that is all the fixture needs. The
# scripts under test come from the repository, not from copies.
FAKE="$WORK/fake"
mkdir -p "$FAKE/scripts" "$FAKE/integrations/nrfconnect-door-lock/patches"
printf 'PIN="%s"\n' "$REAL_PIN" >"$FAKE/scripts/bootstrap.sh"
printf 'one\n' >"$FAKE/integrations/nrfconnect-door-lock/patches/a.patch"

link_sh() { ULTRAWIDELOCK_WS_STORE="$STORE" "$ROOT/scripts/ws-link.sh" "$@" "$FAKE"; }
# Redirecting at the call site would take the ok row with it: the redirect binds
# to yes(), not to the command it runs, and the check silently stops reporting.
link_quiet() { link_sh "$@" >/dev/null 2>&1; }

out="$(link_sh --print 2>&1)"
yes "--print names the store"              grep -q "$STORE" <<<"$out"
yes "--print says nothing is fetched yet"  grep -q 'not fetched' <<<"$out"

# Nothing in the store shares this fetch key, so there is nothing to clone and
# nothing to link. The one case that has to send someone to bootstrap.
out="$(link_sh 2>&1)"; rc=$?
isnt "an empty store is a failure"         "$rc" 0
yes  "and says to bootstrap"               grep -q 'make bootstrap' <<<"$out"

# Now give the store the entry this fixture asks for, and it becomes a symlink.
patch_id="$("$ROOT/scripts/integration-patch-id.py" "$FAKE/integrations/nrfconnect-door-lock/patches")"
want="$STORE/$(ws_entry_name "$(ws_fetch_key "$REAL_PIN" v3.3.0)" "$patch_id")"
mkdir -p "$want/.west"
ws_stamp "$want" "$REAL_PIN" v3.3.0 "$patch_id"

yes "links when the store has the entry"   link_quiet
is  "at the tree the name picks out"       "$(ws_resolved "$FAKE")" "$want"
yes "and is idempotent"                    link_quiet
is  "still the same tree"                  "$(ws_resolved "$FAKE")" "$want"

# HA=1 used to apply two more patches and so needed a workspace of its own. It
# selects a data model inside the application now, and a build-time flag that
# splits the store is the second failure this file is about: two identical trees,
# one fetched for nothing.
is "HA=1 asks for the same entry" \
   "$(HA=1 ULTRAWIDELOCK_WS_STORE="$STORE" "$ROOT/scripts/ws-link.sh" --print "$FAKE" | sed -n 's/^entry *//p')" \
   "$(basename "$want")"

printf '\n== one way to apply the patches ==\n'

# The lists live in scripts/lib/ws.sh because bootstrap.sh and ws-link.sh both
# reach for them. A second copy in either would be two trees under one name --
# the failure this whole file is about, arriving by the back door.
for s in bootstrap.sh ws-link.sh; do
  yes "$s calls ws_apply_patches"          grep -q 'ws_apply_patches' "$ROOT/scripts/$s"
  no  "$s defines no apply of its own"     grep -qE '^apply_to\(\)' "$ROOT/scripts/$s"
done
is "the patch lists exist once" \
   "$(grep -c 'custom_impl-uwb.patch' "$ROOT/scripts/lib/ws.sh" "$ROOT/scripts/bootstrap.sh" "$ROOT/scripts/ws-link.sh" | awk -F: '{n += $2} END {print n}')" 1

printf '\n== copying a workspace off macOS ==\n'

# `cp -c` is APFS clonefile and exists on no GNU cp, so hardcoding it made the
# clone path macOS-only: a Linux runner with anything already in its store hit
# an invalid option rather than a slower copy. The probe is what keeps the flag
# a property of the filesystem instead of an assumption about the OS.
COW="$WORK/cow"
mkdir -p "$COW"
flag="$(ws_cow_flag "$COW")"
case "$flag" in
  ''|'-c'|'--reflink=auto') ok "cow flag is one this cp understands" ;;
  *) bad "cow flag is one this cp understands"; printf '       got %s\n' "$flag" ;;
esac
is  "the probe leaves nothing behind" "$(find "$COW" -name '.ws-cow-probe.*' | wc -l | tr -d ' ')" 0
# Wrapped, because `yes "..." ws_cow_flag ...` would print the flag into the
# middle of the report -- the same trap that once swallowed two ok rows here.
cow_quiet() { ws_cow_flag "$1" >/dev/null 2>&1; }
yes "a missing directory is not fatal" cow_quiet "$WORK/cow/nope/nope"

# The copy has to work whatever the probe said, so this is the check that would
# have caught the bug: it runs the real helper, on this machine, either way.
mkdir -p "$COW/src/sub"
printf 'content\n' >"$COW/src/sub/file"
yes "ws_cow_copy copies a tree"   ws_cow_copy "$COW/src" "$COW/dst"
is  "the copy has the content"    "$(cat "$COW/dst/sub/file" 2>/dev/null)" content

# The copy this machine cannot reach: on a filesystem with no block cloning the
# flag is empty, and an empty "$flag" is not nothing -- it is an argument, and
# `cp "" -R src dst` fails on a missing file. Every developer here runs APFS, so
# that path exists only on the CI runner unless the stub below stands in for it.
# Shadowing ws_cow_flag is enough: ws_cow_copy calls it by name.
ws_cow_flag() { printf '\n'; }
yes "ws_cow_copy works with no cloning"  ws_cow_copy "$COW/src" "$COW/plain"
is  "the plain copy has the content"     "$(cat "$COW/plain/sub/file" 2>/dev/null)" content
unset -f ws_cow_flag
# shellcheck source=scripts/lib/ws.sh
. "$ROOT/scripts/lib/ws.sh"

# An empty flag must vanish rather than become an argument, which is what the
# stub above proves at runtime and this proves at rest.
no  "no bare cp -c is left in the callers" \
    grep -qE 'cp -c ' "$ROOT/scripts/bootstrap.sh" "$ROOT/scripts/ws-link.sh"

printf '\n== the way out of the store ==\n'

# ULTRAWIDELOCK_WS is how a caller keeps its workspace in its own directory:
# no store, no sharing, and above all no symlink. Everything above this line is
# about the store working; this is about it being possible not to use it, which
# until now nothing checked.
#
# .github/workflows/ci.yml sets it on all three jobs that bootstrap, and cannot
# work without it. Those jobs carry the workspace between runs with
# actions/cache and `path: workspace`. Against a symlink that saves the link
# instead of the tree and restores a dangling one into a fresh container -- and
# the restore still reports a hit, so the build proceeds against a workspace
# that is not there. Losing the guard below would not fail here or there; it
# would fail one run later, in a job with a two-hour timeout.
yes "bootstrap.sh reads ULTRAWIDELOCK_WS" \
    grep -q 'ULTRAWIDELOCK_WS:-' "$ROOT/scripts/bootstrap.sh"

# The link has to be unreachable when it is set, not merely skipped in the usual
# case: pull the guarded block out and require that every ws_link call in the
# file is inside it.
guarded="$(awk '
  /^if \[ -z "\$\{ULTRAWIDELOCK_WS:-\}" \]/ { inblock = 1 }
  inblock && /ws_link "/                    { n++ }
  inblock && /^fi$/                         { inblock = 0 }
  END { print n + 0 }' "$ROOT/scripts/bootstrap.sh")"
is "every ws_link is behind that guard" \
   "$guarded" "$(grep -c 'ws_link "' "$ROOT/scripts/bootstrap.sh")"
isnt "and there is one to guard"        "$guarded" 0

# ws-link.sh --print is that workflow's cache key, computed on a bare checkout
# before any workspace exists. ci.yml matches the result against a 12-12 glob,
# so a change to how ws_entry_name truncates breaks a job no test here runs.
printed="$(ULTRAWIDELOCK_WS_STORE="$STORE" "$ROOT/scripts/ws-link.sh" --print "$FAKE" | sed -n 's/^entry *//p')"
case "$printed" in
  ????????????-????????????) ok   "--print still matches ci.yml's 12-12 glob" ;;
  *) bad "--print still matches ci.yml's 12-12 glob"; printf '       got  %s\n' "$printed" ;;
esac

# And it answers WITH the variable set, which is the combination ci.yml actually
# runs: it sets ULTRAWIDELOCK_WS for the whole job and then asks for the key. The
# refusal below it is for link and adopt, which write; --print does not, so it
# reports the same name either way. These two ran in the wrong order once, and
# the job's first step failed on the variable the job itself had set.
over="$(ULTRAWIDELOCK_WS="$WORK/elsewhere" ULTRAWIDELOCK_WS_STORE="$STORE" \
        "$ROOT/scripts/ws-link.sh" --print "$FAKE" | sed -n 's/^entry *//p')"
is   "--print answers with ULTRAWIDELOCK_WS set"  "$over" "$printed"
link_override_quiet() {
  ULTRAWIDELOCK_WS="$WORK/elsewhere" ULTRAWIDELOCK_WS_STORE="$STORE" \
      "$ROOT/scripts/ws-link.sh" "$FAKE" >/dev/null 2>&1
}
no   "but linking still refuses"                  link_override_quiet

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
printf 'workspace store: PASS\n'
