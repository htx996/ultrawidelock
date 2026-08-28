#!/usr/bin/env bash
#
# release-bundle.sh — assemble one publishable firmware bundle.
#
#   scripts/release-bundle.sh --target dwm3001cdk --out build/release/... \
#       --version v0.5.0 --board 'DWM3001CDK (nRF52833)' \
#       --setup-code 12345678 merged.hex
#
# Options:
#   --target <slug>          release/<slug>/ supplies the guide and script
#   --out <dir>              destination, wiped and recreated
#   --version <text>         the tag, or `git describe` when omitted
#   --commit <sha>           defaults to HEAD
#   --board <text>           hardware line in VERSION.txt
#   --setup-code <code>      Matter setup code, when the build knows it
#   --commission-note <text> the line printed under it, or instead of it
#
# Writes the firmware given as positional arguments, plus flash.sh, FLASH.md
# and README.txt from release/<slug>/, plus a generated VERSION.txt
# and SHA256SUMS.txt. Every bundle gets all of them: this is the one place that
# decides what a release zip contains, so the three targets cannot drift.
#
# Exit 0 on a complete bundle, 1 on any failure. There is no partial success —
# a bundle missing a file looks identical to a good one once it is a zip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

die() {
	printf '\n  release-bundle: %s\n' "$1" >&2
	shift
	for line in "$@"; do printf '  %s\n' "$line" >&2; done
	printf '\n' >&2
	exit 1
}

TARGET="" OUT="" VERSION="" COMMIT="" BOARD="" SETUP_CODE="" COMMISSION_NOTE=""
while [ $# -gt 0 ]; do
	case "$1" in
	--target) TARGET="${2:-}"; shift 2 ;;
	--out) OUT="${2:-}"; shift 2 ;;
	--version) VERSION="${2:-}"; shift 2 ;;
	--commit) COMMIT="${2:-}"; shift 2 ;;
	--board) BOARD="${2:-}"; shift 2 ;;
	--setup-code) SETUP_CODE="${2:-}"; shift 2 ;;
	--commission-note) COMMISSION_NOTE="${2:-}"; shift 2 ;;
	--) shift; break ;;
	-*) die "unknown option: $1" ;;
	*) break ;;
	esac
done

if [ -z "$TARGET" ]; then
	known=""
	for d in "$ROOT"/release/*/; do known="$known $(basename "$d")"; done
	die "--target is required" "one of:$known"
fi
[ -n "$OUT" ] || die "--out is required"
[ $# -ge 1 ] || die "no firmware files given" "pass the images to ship as positional arguments"

SRC="$ROOT/release/$TARGET"
[ -d "$SRC" ] || die "no such target: $TARGET" "expected $SRC"

for f in FLASH.md flash.sh README.txt; do
	[ -f "$SRC/$f" ] || die "release/$TARGET/$f is missing" \
		"every target ships the same file set; add it rather than skipping it"
done

# Positional arguments are firmware paths. Checked before anything is deleted,
# so a typo cannot destroy an existing bundle on its way to failing.
for f in "$@"; do
	[ -f "$f" ] || die "no such firmware file: $f" "build it before bundling"
done

# Some firmware files are not optional, and their absence is silent.
#
# THE DWM3001CDK HAS ONE MCUBOOT SLOT, so an over-the-air update is a delta
# against the exact bytes already on the board, and building one requires the
# image that board is running. The only copy of that is zephyr.signed.hex as
# published with its own release. It cannot be rebuilt later: the image embeds a
# build timestamp, so two builds of identical sources at the same version differ
# (measured 2026-08-27, 105 bytes of 408,206, hashes 8613358327a47e00 and
# f9443287420eaedf). merged.hex is not a substitute -- a delta cannot be built
# from it, because the first thing in a merged hex is the bootloader and not an
# MCUboot image.
#
# So a CDK release that ships without it strands every board that installs it on
# a J-Link FOREVER, and nothing says so at the time. The next release simply
# reports "no usable base" as a notice, which is the correct behaviour for an
# old bundle and indistinguishable from this mistake. One release too late.
#
# The files arrive as positional arguments, so until now the requirement lived
# only in the mk/cdk.mk call site and a comment beside it. Checked here so a
# local `make release` gets it too, the same reason the private-key check is
# here rather than in the workflow.
case "$TARGET" in
dwm3001cdk*)
	needed="zephyr.signed.hex"
	;;
*)
	needed=""
	;;
esac
for want in $needed; do
	found=""
	for f in "$@"; do
		[ "$(basename "$f")" = "$want" ] && found=1
	done
	[ -n "$found" ] || die "$TARGET must ship $want and does not" \
		"An over-the-air update for this board is a delta against the exact" \
		"bytes on it, so a future release needs this image to build one, and" \
		"the image cannot be reproduced after this build. Publishing without" \
		"it strands every board that installs this release on a J-Link" \
		"permanently. Pass it alongside merged.hex."
done

VERSION="${VERSION:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo unknown)}"
COMMIT="${COMMIT:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)}"
BUILT="$(date -u +%Y-%m-%dT%H:%MZ)"

# A Matter manual pairing code is 11 digits, and the three targets read theirs
# from three different places: two hand back 34970112332 and one 3497-011-2332.
# Somebody is typing this into a phone, so they all get the grouped form. Same
# 4-3-4 split `make nrf-pairing-code` prints. Anything not eleven bare digits is
# left exactly as it came, including an already-grouped code.
case "$SETUP_CODE" in
[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9])
	SETUP_CODE="${SETUP_CODE:0:4}-${SETUP_CODE:4:3}-${SETUP_CODE:7:4}"
	;;
esac

rm -rf "$OUT"
mkdir -p "$OUT"

for f in "$@"; do cp "$f" "$OUT/"; done
cp "$SRC/FLASH.md" "$SRC/flash.sh" "$OUT/"
chmod +x "$OUT/flash.sh"

# ---- VERSION.txt -------------------------------------------------------------
{
	printf 'ultrawidelock %s\n' "$VERSION"
	printf 'commit     %s\n' "$COMMIT"
	printf 'built      %s\n' "$BUILT"
	[ -n "$BOARD" ] && printf 'board      %s\n' "$BOARD"
	printf '\n'
	# The note belongs to the caller: "there is no QR label on this board" is
	# true of the CDK and false of the other two.
	if [ -n "$SETUP_CODE" ]; then
		printf 'SETUP CODE %s\n' "$SETUP_CODE"
		printf '%s\n' "${COMMISSION_NOTE:-Type this into Apple Home.}"
	elif [ -n "$COMMISSION_NOTE" ]; then
		printf '%s\n' "$COMMISSION_NOTE"
	fi
} >"$OUT/VERSION.txt"

# ---- README.txt --------------------------------------------------------------
# Bash string replacement, not sed: a tag name must never act as pattern syntax.
# Targets that know no setup code drop the line carrying @SETUP_CODE@ whole.
readme="$(cat "$SRC/README.txt")"
readme="${readme//@VERSION@/$VERSION}"
if [ -n "$SETUP_CODE" ]; then
	readme="${readme//@SETUP_CODE@/$SETUP_CODE}"
else
	readme="$(printf '%s\n' "$readme" | grep -v '@SETUP_CODE@' || true)"
fi
printf '%s\n' "$readme" >"$OUT/README.txt"

# ---- the signing key must never be in here -----------------------------------
# Only the CDK build is handed a private key, but this runs on all three: the
# cost of missing it once is a published key that can never be unpublished.
if grep -rlI 'PRIVATE KEY' "$OUT" 2>/dev/null | grep -q .; then
	die "a PEM private key reached the bundle" "$OUT" "nothing was published; delete it and find the leak"
fi

# ---- SHA256SUMS.txt ----------------------------------------------------------
# sha256sum on Linux, shasum on macOS: the CI containers have only the first and
# a developer Mac only the second. Sorted, so the same inputs give a
# byte-identical sums file and a diff between releases shows only real changes.
(
	cd "$OUT"
	# LC_ALL=C so the glob's order is the byte order everywhere, not the
	# runner's locale. The redirect below has already created an empty
	# SHA256SUMS.txt by the time this glob runs, hence skipping it by name.
	export LC_ALL=C
	files=()
	for f in *; do
		[ "$f" = "SHA256SUMS.txt" ] && continue
		files+=("$f")
	done
	[ ${#files[@]} -gt 0 ] || die "the bundle is empty"
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "${files[@]}"
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "${files[@]}"
	else
		die "no sha256sum and no shasum" "one of them is needed to checksum the bundle"
	fi
) >"$OUT/SHA256SUMS.txt"
[ -s "$OUT/SHA256SUMS.txt" ] || die "no checksums written" "$OUT/SHA256SUMS.txt is empty"

# ---- report ------------------------------------------------------------------
printf '\n  bundle ready  ·  %s\n\n' "$OUT"
for f in "$OUT"/*; do printf '    %s\n' "$(basename "$f")"; done
printf '\n'
if [ -n "$SETUP_CODE" ]; then
	printf '    SETUP CODE %s\n\n' "$SETUP_CODE"
fi
