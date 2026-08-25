#!/usr/bin/env bash
#
# release-notes.sh — render the GitHub release body from release/NOTES.md.in.
#
#   scripts/release-notes.sh v0.5.0                     # preview it
#   scripts/release-notes.sh v0.5.0 out/SHA256SUMS.txt  # what CI publishes
#   scripts/release-notes.sh --self-test                # the edge cases, asserted
#
# Placeholders: @TAG@ @REPO@ @PAGES@ @CHANGELOG@ @SUMS@
# Env: REPO=owner/name          the slug the notes link to and verify against
#      NOTES_ROOT=<dir>         where release/NOTES.md.in and CHANGELOG.md live
#                               (the repository root; the self-test moves it)
#
# These notes are also the release email: GitHub renders them into the
# notification it sends watchers, so the checksums stay inside a <details> and
# nothing load-bearing sits below the fold.
set -euo pipefail

ROOT="${NOTES_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
REPO="${REPO:-ultrawidelock/ultrawidelock}"

# How many commits the git fallback will list before it stops and says so. A
# release body that opens with two hundred bullets is a wall, not notes, and the
# compare link below the cut is a better read than the tail of the list.
CHANGELOG_MAX="${CHANGELOG_MAX:-40}"

# ---- the changelog section ---------------------------------------------------
# Three sources, best first. CHANGELOG.md is the authored one and wins whenever
# it exists; git log is the honest fallback for a repository that keeps no
# changelog (this one does not, today); nothing is a valid answer for a shallow
# checkout with no tag history, and renders as an empty section rather than a
# lie.

changelog_from_file() { # <root> <version> -> section or ""
	local cl="$1/CHANGELOG.md" version="$2" section
	[ -f "$cl" ] || return 0
	# Matched on the bare version, so `## [0.5.0]` and `## [0.5.0] - 2026-08-05`
	# both hit. Falls back to Unreleased: a tag cut before the section was
	# renamed should still say what changed.
	section="$(awk -v v="$version" '
		/^## \[/ {
			if (found) exit
			line = $0
			sub(/^## \[/, "", line)
			sub(/\].*$/, "", line)
			if (line == v) { found = 1; print "## What changed"; next }
		}
		found { print }
	' "$cl")"
	if [ -z "$section" ]; then
		section="$(awk '
			/^## \[/ {
				if (found) exit
				if ($0 ~ /^## \[Unreleased\]/) { found = 1; print "## What changed"; next }
			}
			found { print }
		' "$cl")"
	fi
	printf '%s' "$section"
}

changelog_from_git() { # <root> <tag> <repo> -> section or ""
	local root="$1" tag="$2" repo="$3" prev range subjects count
	git -C "$root" rev-parse --git-dir >/dev/null 2>&1 || return 0
	git -C "$root" rev-parse -q --verify "$tag^{commit}" >/dev/null 2>&1 || return 0

	# The previous tag, if this checkout has one. `git describe` walks the graph,
	# so a shallow clone can answer "no" here even when the tag exists upstream;
	# that is why the whole function is allowed to come back empty rather than
	# failing the release.
	prev="$(git -C "$root" describe --tags --abbrev=0 "$tag^" 2>/dev/null || true)"
	if [ -n "$prev" ]; then range="$prev..$tag"; else range="$tag"; fi

	# --no-merges because a merge subject names a branch, not a change.
	subjects="$(git -C "$root" log --no-merges --pretty=format:'- %s' "$range" 2>/dev/null || true)"
	[ -n "$subjects" ] || return 0
	count="$(printf '%s\n' "$subjects" | wc -l | tr -d ' ')"

	printf '## What changed\n\n'
	printf '%s\n' "$subjects" | head -n "$CHANGELOG_MAX"
	if [ "$count" -gt "$CHANGELOG_MAX" ]; then
		printf -- '- ... and %d more\n' "$((count - CHANGELOG_MAX))"
	fi
	if [ -n "$prev" ]; then
		printf '\nFull diff: <https://github.com/%s/compare/%s...%s>\n' "$repo" "$prev" "$tag"
	fi
}

render() { # <tag> <sums-file> -> the notes on stdout
	local tag="$1" sums_file="${2:-}" template owner name pages changelog sums out

	template="$ROOT/release/NOTES.md.in"
	[ -f "$template" ] || {
		echo "release-notes: template not found: $template" >&2
		exit 1
	}

	owner="${REPO%%/*}"
	name="${REPO#*/}"
	pages="https://$owner.github.io/$name/flash/"

	changelog="$(changelog_from_file "$ROOT" "${tag#v}")"
	[ -n "$changelog" ] || changelog="$(changelog_from_git "$ROOT" "$tag" "$REPO")"
	# Trim trailing blank lines so the template controls the spacing.
	changelog="$(printf '%s\n' "$changelog" | sed -e :a -e '/^\n*$/{$d;N;};/\n$/ba')"

	if [ -n "$sums_file" ] && [ -f "$sums_file" ]; then
		sums="$(cat "$sums_file")"
	else
		sums="see the SHA256SUMS.txt asset below"
	fi

	# Bash string replacement, not sed: the changelog and checksum blocks carry
	# slashes, brackets and backticks that must never act as pattern syntax.
	out="$(cat "$template")"
	out="${out//@TAG@/$tag}"
	out="${out//@REPO@/$REPO}"
	out="${out//@PAGES@/$pages}"
	out="${out//@CHANGELOG@/$changelog}"
	out="${out//@SUMS@/$sums}"

	if printf '%s' "$out" | grep -q '@[A-Z_]*@'; then
		echo "release-notes: a placeholder was left unsubstituted:" >&2
		printf '%s' "$out" | grep -o '@[A-Z_]*@' | sort -u >&2
		exit 1
	fi

	printf '%s\n' "$out"
}

# ---- self-test ---------------------------------------------------------------
# What breaks here is silent: a placeholder that stops matching, or a changelog
# selector that picks the neighbouring version's section. Both render a body
# that looks fine and says the wrong thing, and nobody reads it closely on the
# one day it matters. So the shapes are pinned.
self_test() {
	local fails=0 tmp
	tmp="$(mktemp -d)"
	trap 'rm -rf "$tmp"' RETURN

	check() { # <name> <condition-already-evaluated-as-rc>
		if [ "$2" = 0 ]; then
			printf '  ok   %s\n' "$1"
		else
			printf '  FAIL %s\n' "$1"
			fails=$((fails + 1))
		fi
	}

	mkdir -p "$tmp/release"
	printf 'tag @TAG@ repo @REPO@ pages @PAGES@\n\n@CHANGELOG@\n\nsums:\n@SUMS@\n' \
		>"$tmp/release/NOTES.md.in"

	local out rc
	out="$(NOTES_ROOT="$tmp" REPO=acme/widget bash "$0" v1.2.3 2>&1)" || rc=$?
	case "$out" in *"tag v1.2.3"*) rc=0 ;; *) rc=1 ;; esac
	check "@TAG@ is substituted" "$rc"
	case "$out" in *"repo acme/widget"*) rc=0 ;; *) rc=1 ;; esac
	check "@REPO@ takes the environment override" "$rc"
	case "$out" in *"https://acme.github.io/widget/flash/"*) rc=0 ;; *) rc=1 ;; esac
	check "@PAGES@ is derived from the slug" "$rc"
	case "$out" in *'@'*'@'*) rc=1 ;; *) rc=0 ;; esac
	check "no placeholder survives a render" "$rc"

	# No sums file: the body must still name the asset rather than go blank.
	case "$out" in *"see the SHA256SUMS.txt asset below"*) rc=0 ;; *) rc=1 ;; esac
	check "absent checksums fall back to a sentence" "$rc"

	printf 'aaa  one.zip\nbbb  two.bin\n' >"$tmp/SUMS.txt"
	out="$(NOTES_ROOT="$tmp" bash "$0" v1.2.3 "$tmp/SUMS.txt" 2>&1)"
	case "$out" in *"aaa  one.zip"*) rc=0 ;; *) rc=1 ;; esac
	check "a checksums file is inlined verbatim" "$rc"

	# The version's own section, not the one above or below it. The decoys
	# matter: 1.2.3 sits between two neighbours and carries a date suffix.
	cat >"$tmp/CHANGELOG.md" <<-'EOF'
		# Changelog

		## [Unreleased]
		- nothing yet

		## [1.3.0]
		- newer thing

		## [1.2.3] - 2026-01-01
		- the right thing
		- another right thing

		## [1.2.2]
		- older thing
	EOF
	out="$(NOTES_ROOT="$tmp" bash "$0" v1.2.3 2>&1)"
	case "$out" in *"the right thing"*) rc=0 ;; *) rc=1 ;; esac
	check "CHANGELOG.md section is selected by version" "$rc"
	case "$out" in *"newer thing"* | *"older thing"* | *"nothing yet"*) rc=1 ;; *) rc=0 ;; esac
	check "neighbouring sections do not bleed in" "$rc"
	case "$out" in *"## What changed"*) rc=0 ;; *) rc=1 ;; esac
	check "the section gets a heading" "$rc"

	# A tag with no section of its own falls back to Unreleased.
	out="$(NOTES_ROOT="$tmp" bash "$0" v9.9.9 2>&1)"
	case "$out" in *"nothing yet"*) rc=0 ;; *) rc=1 ;; esac
	check "an unlisted version falls back to Unreleased" "$rc"

	# No CHANGELOG.md, but a git history with tags: the commit subjects.
	rm -f "$tmp/CHANGELOG.md"
	local repo="$tmp/repo"
	mkdir -p "$repo/release"
	cp "$tmp/release/NOTES.md.in" "$repo/release/NOTES.md.in"
	(
		# Hermetic: the fixture must not inherit whoever's global git config is
		# on this machine. A developer with tag.gpgSign or tag.forceSignAnnotated
		# set otherwise fails this test for a reason that has nothing to do with
		# release notes, which is the worst kind of red.
		cd "$repo"
		export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null
		export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@example.invalid
		export GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@example.invalid
		git init -q .
		git commit -q --allow-empty -m "before the tag"
		git tag v1.0.0
		git commit -q --allow-empty -m "a shipped fix"
		git tag v1.1.0
	) >/dev/null 2>&1
	out="$(NOTES_ROOT="$repo" REPO=acme/widget bash "$0" v1.1.0 2>&1)"
	case "$out" in *"- a shipped fix"*) rc=0 ;; *) rc=1 ;; esac
	check "git log fills in when there is no CHANGELOG.md" "$rc"
	case "$out" in *"before the tag"*) rc=1 ;; *) rc=0 ;; esac
	check "the git range stops at the previous tag" "$rc"
	case "$out" in *"/compare/v1.0.0...v1.1.0"*) rc=0 ;; *) rc=1 ;; esac
	check "a compare link is offered" "$rc"

	# Neither source. An empty section is the right answer, not a failure: this
	# is what a shallow CI checkout of a repository with no changelog looks like.
	rc=0
	out="$(NOTES_ROOT="$tmp" bash "$0" v1.2.3 2>&1)" || rc=$?
	check "no changelog source still renders" "$rc"

	# A template that grows a placeholder nobody teaches the script about must
	# fail loudly. Rendering it raw would publish '@BOARDS@' to every watcher.
	printf '@TAG@ @REPO@ @PAGES@ @CHANGELOG@ @SUMS@ @BOARDS@\n' >"$tmp/release/NOTES.md.in"
	rc=0
	NOTES_ROOT="$tmp" bash "$0" v1.2.3 >/dev/null 2>&1 || rc=$?
	check "an unknown placeholder fails the render" "$([ "$rc" = 1 ] && echo 0 || echo 1)"

	# Usage errors are exit 2, distinct from a render that failed.
	rc=0
	NOTES_ROOT="$tmp" bash "$0" >/dev/null 2>&1 || rc=$?
	check "a missing tag exits 2" "$([ "$rc" = 2 ] && echo 0 || echo 1)"

	printf '\n  release notes: %s\n' "$([ "$fails" = 0 ] && echo PASS || echo "FAIL ($fails)")"
	[ "$fails" = 0 ]
}

case "${1:-}" in
--self-test)
	self_test
	;;
"")
	echo "usage: scripts/release-notes.sh <tag> [sums-file]" >&2
	exit 2
	;;
*)
	render "$1" "${2:-}"
	;;
esac
