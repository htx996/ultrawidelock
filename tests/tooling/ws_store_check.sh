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

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
printf 'workspace store: PASS\n'
