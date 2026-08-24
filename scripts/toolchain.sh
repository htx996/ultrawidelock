#!/usr/bin/env bash
#
# toolchain.sh — report the host tools the suites need and what this machine has.
#
#   scripts/toolchain.sh            report every tool, its suite, and its status
#   scripts/toolchain.sh install    install the missing ones, after confirming
#
# `check` installs nothing. It exits nonzero when a gate tool is missing, so a
# suite skipping quietly is never a surprise; bench and optional tools are
# reported but never affect the exit status. `install` prints the exact command
# list first and runs nothing before the answer -- -y answers it in advance.
#
# Out of scope: the SDKs. NCS is `make bootstrap` and ESP-IDF/esp-matter are
# `make esp-bootstrap`, each multi-gigabyte with its own resume logic; this
# script only says whether they are there.
#
# Env:
#   ASSUME_YES=1   same as `install -y`
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

MODE=check
ASSUME_YES="${ASSUME_YES:-}"
for arg in "$@"; do
	case "$arg" in
	check | install) MODE="$arg" ;;
	-y | --yes) ASSUME_YES=1 ;;
	-h | --help)
		sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*)
		echo "toolchain.sh: unknown argument '$arg' (try: check | install | -y)" >&2
		exit 2
		;;
	esac
done

# shellcheck disable=SC1091  # repo-local, sourced for $PY
. "$ROOT/tests/host/sources.sh"

# Gate tools: a missing one fails this script. Bench tools: flashing/serial,
# reported only. Optional tools: their gate skips loudly rather than failing, so
# a machine without one still runs a green `make check` -- reported, never fatal.
TOOLS=(cc python3 llvm-cov cbmc)
OPT_TOOLS=(cppcheck gitleaks CodeChecker)
FW_TOOLS=(tio nrfutil sdk-manager toolchain esp-idf esp-matter probe-rs mcumgr)

# The NCS version the Zephyr ports build against. Kept in step with the Makefile
# and scripts/bootstrap.sh, and only used to say which toolchain to look for.
NCS_VER="${NCS_VER:-v3.3.0}"

# Where the ESP32 ports look for their two SDKs. Same defaults and same variable
# names as mk/esp32.mk and scripts/esp-bootstrap.sh, so all three agree about
# what "installed" means.
IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}"
ESP_MATTER_PATH="${ESP_MATTER_PATH:-$HOME/esp/esp-matter}"

# The package manager to phrase system installs in, and whether they need sudo.
# Homebrew wins where it exists (including Linuxbrew) because it is the one that
# needs no sudo. Empty is fine: those rows print a pointer instead of a command.
OS="$(uname -s)"
PM=""
SUDO=""
if command -v brew >/dev/null 2>&1; then
	PM=brew
elif command -v apt-get >/dev/null 2>&1; then
	PM=apt SUDO="sudo "
elif command -v dnf >/dev/null 2>&1; then
	PM=dnf SUDO="sudo "
elif command -v pacman >/dev/null 2>&1; then
	PM=pacman SUDO="sudo "
elif command -v zypper >/dev/null 2>&1; then
	PM=zypper SUDO="sudo "
fi

# Which suite (or bench job) stops working without it.
tool_gate() {
	case "$1" in
	cc) echo "test, test-san" ;;
	python3) echo "test, drift, coverage" ;;
	llvm-cov) echo "coverage" ;;
	cbmc) echo "cbmc" ;;
	cppcheck) echo "lint (inside check)" ;;
	gitleaks) echo "the secret scan in ci" ;;
	CodeChecker) echo "sca" ;;
	tio) echo "make term (live serial)" ;;
	nrfutil) echo "make bootstrap / build / flash" ;;
	sdk-manager) echo "nrfutil's own toolchain command" ;;
	toolchain) echo "every Zephyr build here" ;;
	esp-idf) echo "every ESP32 build here" ;;
	esp-matter) echo "make esp-build APP=matter-lock" ;;
	probe-rs) echo "make cdk-rtt (CDK console)" ;;
	mcumgr) echo "make dfu (CDK serial recovery)" ;;
	esac
}

# Where to get a missing one.
tool_note() {
	case "$1" in
	cc | llvm-cov) echo "xcode-select --install / your distro's clang+llvm" ;;
	cbmc) echo "brew install cbmc / apt install cbmc" ;;
	cppcheck) echo "brew install cppcheck / apt install cppcheck" ;;
	gitleaks) echo "brew install gitleaks" ;;
	CodeChecker) echo "python3 -m venv .venv-sca && .venv-sca/bin/pip install codechecker" ;;
	tio) echo "brew install tio / apt install tio" ;;
	nrfutil) echo "make bootstrap installs it" ;;
	sdk-manager | toolchain) echo "make bootstrap" ;;
	esp-idf) echo "make esp-bootstrap APP=reader" ;;
	esp-matter) echo "make esp-bootstrap" ;;
	probe-rs) echo "https://probe.rs/docs/getting-started/installation/" ;;
	mcumgr) echo "needs go: go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest" ;;
	*) echo "install it however this host prefers, then re-run" ;;
	esac
}

# The command that installs it on THIS host. Empty = no packaged route here, and
# the row prints tool_note() instead of a command that would fail.
#
# The four SDK rows -- toolchain, esp-idf, esp-matter and nrfutil off Homebrew --
# are deliberately empty. They are gigabytes with their own resume logic, and
# duplicating a bootstrap script inside a one-command-per-row table is how the
# two quietly disagree about what "installed" means.
tool_install() {
	case "$1" in
	# macOS reaches cc and llvm-cov through the Command Line Tools, so the
	# install is xcode-select, not a package.
	cc | llvm-cov)
		case "$OS" in
		Darwin) echo "xcode-select --install" ;;
		*)
			case "$PM" in
			brew) echo "brew install llvm" ;;
			apt) echo "${SUDO}apt-get install -y clang llvm" ;;
			dnf) echo "${SUDO}dnf install -y clang llvm" ;;
			pacman) echo "${SUDO}pacman -S --needed clang llvm" ;;
			zypper) echo "${SUDO}zypper install -y clang llvm" ;;
			*) echo "" ;;
			esac
			;;
		esac
		;;
	python3)
		case "$PM" in
		brew) echo "brew install python" ;;
		apt) echo "${SUDO}apt-get install -y python3 python3-pip" ;;
		dnf) echo "${SUDO}dnf install -y python3 python3-pip" ;;
		pacman) echo "${SUDO}pacman -S --needed python python-pip" ;;
		zypper) echo "${SUDO}zypper install -y python3 python3-pip" ;;
		*) echo "" ;;
		esac
		;;
	cbmc)
		case "$PM" in
		brew) echo "brew install cbmc" ;;
		apt) echo "${SUDO}apt-get install -y cbmc" ;;
		*) echo "" ;; # no first-party package elsewhere; see tool_note
		esac
		;;
	cppcheck | tio)
		case "$PM" in
		brew) echo "brew install $1" ;;
		apt) echo "${SUDO}apt-get install -y $1" ;;
		dnf) echo "${SUDO}dnf install -y $1" ;;
		pacman) echo "${SUDO}pacman -S --needed $1" ;;
		zypper) echo "${SUDO}zypper install -y $1" ;;
		*) echo "" ;;
		esac
		;;
	# Upstream ships a binary and a Homebrew formula; the distro repos either
	# do not carry it or carry something far behind, so nothing is guessed.
	gitleaks)
		case "$PM" in
		brew) echo "brew install gitleaks" ;;
		*) echo "" ;;
		esac
		;;
	# A python package, into its own venv rather than the repo's .venv: it
	# drags in a large dependency tree that the host suites have no business
	# importing, and tests/tooling/codechecker_sca.sh already looks for it
	# under $CODECHECKER rather than on PATH.
	CodeChecker)
		echo "python3 -m venv .venv-sca && .venv-sca/bin/pip install --quiet --upgrade pip codechecker"
		;;
	# Nordic ships it as a signed binary from their own site, and only Homebrew
	# packages it. Everywhere else this is bootstrap's offer, not ours.
	nrfutil)
		case "$PM" in
		brew) echo "brew install nrfutil" ;;
		*) echo "" ;;
		esac
		;;
	# A command inside nrfutil, so there is nothing to fetch until nrfutil is
	# there -- and when it is, this is the same command bootstrap runs.
	sdk-manager)
		if command -v nrfutil >/dev/null 2>&1; then
			echo "nrfutil install sdk-manager"
		else
			echo ""
		fi
		;;
	# The project's own tap is the packaged route; `brew info probe-rs` resolves
	# to probe-rs/probe-rs, not homebrew/core. The alternative everywhere else
	# is `cargo install probe-rs-tools`, a multi-minute source build, and a
	# toolchain script should not start a compile on someone's behalf.
	probe-rs)
		case "$PM" in
		brew) echo "brew install probe-rs/probe-rs/probe-rs" ;;
		*) echo "" ;;
		esac
		;;
	# Ignores $PM on purpose: no distro or Homebrew formula packages this, and
	# upstream ships it only as a Go module. The question is "is there a go".
	mcumgr)
		if command -v go >/dev/null 2>&1; then
			echo "go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest"
		else
			echo ""
		fi
		;;
	toolchain | esp-idf | esp-matter) echo "" ;; # see the note above
	esac
}

# Present here? Echo a version line and return 0; return 1 when absent.
#   llvm-cov  macOS keeps it inside the Xcode SDK, reachable only via xcrun —
#             which is how tests/host/coverage.sh calls it.
#   mcumgr    has no --version flag; `mcumgr version` is a subcommand.
tool_probe() {
	case "$1" in
	llvm-cov)
		if command -v llvm-cov >/dev/null 2>&1; then
			llvm-cov --version 2>&1 | grep -i version | head -1
		elif command -v xcrun >/dev/null 2>&1 && xcrun --find llvm-cov >/dev/null 2>&1; then
			echo "$(xcrun llvm-cov --version 2>&1 | grep -i version | head -1) (via xcrun)"
		else
			return 1
		fi
		;;
	sdk-manager)
		# nrfutil ships as a launcher with no commands inside it, so having
		# nrfutil says nothing about having this. bootstrap adds it.
		command -v nrfutil >/dev/null 2>&1 || return 1
		got="$(nrfutil sdk-manager --version 2>/dev/null)" || return 1
		printf '%s\n' "$got" | head -1
		;;
	toolchain)
		command -v nrfutil >/dev/null 2>&1 || return 1
		nrfutil sdk-manager --version >/dev/null 2>&1 || return 1
		# Ask nrfutil, never a path: `toolchain launch` is how every build here
		# reaches the compiler, so a toolchain nrfutil cannot see is one no
		# build could have used. JSON because a column layout is not an API.
		# Captured, not piped into `grep -q`: -q exits on the first match and
		# the SIGPIPE that gives nrfutil becomes this pipeline's status under
		# `set -o pipefail`, which would report an installed toolchain as absent.
		tl="$(nrfutil --json sdk-manager toolchain list 2>/dev/null || true)"
		[ "$(printf '%s\n' "$tl" | grep -c "\"$NCS_VER\"" || true)" -gt 0 ] || return 1
		echo "NCS $NCS_VER installed"
		;;
	esp-idf)
		# The export script the build sources, not an idf.py somewhere on PATH:
		# mk/esp32.mk sources exactly this file, so this is the only copy that
		# matters. A git checkout names itself with a tag; version.txt is the
		# fallback, because the release archives ship one and no .git at all.
		[ -f "$IDF_EXPORT" ] || return 1
		v="$(git -C "$(dirname "$IDF_EXPORT")" describe --tags --always 2>/dev/null)"
		[ -n "$v" ] || v="$(cat "$(dirname "$IDF_EXPORT")/version.txt" 2>/dev/null)"
		echo "${v:-installed}  ·  $(dirname "$IDF_EXPORT")"
		;;
	esp-matter)
		[ -f "$ESP_MATTER_PATH/export.sh" ] || return 1
		v="$(git -C "$ESP_MATTER_PATH" rev-parse --short HEAD 2>/dev/null)"
		# Cloned is not installed: the connectedhomeip submodule and the Python
		# environment are the slow half, and a tree without them fails at build
		# time rather than here. esp-bootstrap leaves this behind when it finishes.
		if [ -f "$ESP_MATTER_PATH/.ultrawidelock-install-done" ]; then
			echo "${v:-installed}  ·  $ESP_MATTER_PATH"
		else
			echo "${v:-present}  ·  $ESP_MATTER_PATH (not installed by esp-bootstrap)"
		fi
		;;
	CodeChecker)
		# Where the gate looks, in the gate's order: $CODECHECKER, then the
		# .venv-sca that `install` creates, then PATH. Probing PATH alone
		# would report a working install as missing.
		cc_bin="${CODECHECKER:-}"
		if [ -z "$cc_bin" ] && [ -x "$ROOT/.venv-sca/bin/CodeChecker" ]; then
			cc_bin="$ROOT/.venv-sca/bin/CodeChecker"
		fi
		cc_bin="${cc_bin:-CodeChecker}"
		command -v "$cc_bin" >/dev/null 2>&1 || return 1
		# No --version flag: `version` is a subcommand, and it prints a table.
		# The base package version is the row that names the analyzer release.
		v="$("$cc_bin" version 2>/dev/null | sed -n 's/^Base package version *| *\([^ ]*\).*/\1/p' | head -1)"
		# Relative when it is inside the checkout: the absolute path is most of
		# a home directory and says nothing the column needs.
		echo "CodeChecker ${v:-installed}  ·  ${cc_bin#"$ROOT"/}"
		;;
	mcumgr)
		command -v mcumgr >/dev/null 2>&1 || return 1
		mcumgr version 2>&1 | head -1
		;;
	*)
		command -v "$1" >/dev/null 2>&1 || return 1
		"$1" --version </dev/null 2>&1 | head -1
		;;
	esac
}

printf '\n  host tools  ·  %s %s  ·  %s\n\n' "$(uname -s)" "$(uname -m)" "${PM:-no package manager detected}"
nmiss=0
NEEDED=()
for t in "${TOOLS[@]}" __hr__ "${OPT_TOOLS[@]}" __hr__ "${FW_TOOLS[@]}"; do
	if [ "$t" = __hr__ ]; then
		printf '  %s\n' "----------------------------------------------------------------"
		continue
	fi
	if got="$(tool_probe "$t")"; then
		printf '  +  %-12s %-32s %s\n' "$t" "$(tool_gate "$t")" "$got"
		continue
	fi
	NEEDED+=("$t")
	case " ${FW_TOOLS[*]} ${OPT_TOOLS[*]} " in
	*" $t "*)
		case " ${FW_TOOLS[*]} " in
		*" $t "*) printf '  ~  %-12s %-32s not installed · %s\n' "$t" "$(tool_gate "$t")" "$(tool_note "$t")" ;;
		*) printf '  ~  %-12s %-32s not installed · that gate skips\n' "$t" "$(tool_gate "$t")" ;;
		esac
		;;
	*)
		nmiss=$((nmiss + 1))
		printf '  x  %-12s %-32s MISSING · %s\n' "$t" "$(tool_gate "$t")" "$(tool_note "$t")"
		;;
	esac
done
printf '\n'

if [ "${#NEEDED[@]}" -eq 0 ]; then
	printf '  every tool this repo knows about is here.\n\n'
	exit 0
fi

# ---- what would fill the gaps ----------------------------------------------
# One command list, deduplicated, for both modes: `check` prints it and stops,
# `install` prints the same list and then runs exactly it. Nothing is added
# between the printing and the running.
CMDS=()
STUCK=()
for t in "${NEEDED[@]}"; do
	c="$(tool_install "$t")"
	if [ -z "$c" ]; then
		STUCK+=("$t")
		continue
	fi
	case " ${CMDS[*]:-} " in
	*" $c "*) continue ;;
	esac
	CMDS+=("$c")
done

if [ "${#STUCK[@]}" -gt 0 ]; then
	printf '  nothing here installs these — see the note on each:\n'
	for t in "${STUCK[@]}"; do
		printf '    %-12s %s\n' "$t" "$(tool_note "$t")"
	done
	printf '\n'
fi

if [ "${#CMDS[@]}" -gt 0 ]; then
	printf '  to install %d of them:\n\n' "${#CMDS[@]}"
	for c in "${CMDS[@]}"; do printf '    %s\n' "$c"; done
	printf '\n'
fi

if [ "$MODE" != install ]; then
	[ "${#CMDS[@]}" -gt 0 ] && printf '  run them yourself, or let this script do it: make tools-install\n\n'
	[ "$nmiss" -gt 0 ] || exit 0
	printf '  %d gate tool(s) missing — the suites above will not run.\n\n' "$nmiss"
	exit 1
fi

# ---- install ---------------------------------------------------------------
if [ "${#CMDS[@]}" -eq 0 ]; then
	printf '  nothing here can be installed automatically.\n\n'
	exit 1
fi

if [ -z "$ASSUME_YES" ]; then
	if [ ! -t 0 ]; then
		printf '  not a terminal and no -y — nothing installed.\n\n'
		exit 1
	fi
	printf '  run these %d command(s) now? [y/N] ' "${#CMDS[@]}"
	read -r reply
	case "$reply" in
	y | Y | yes | YES) ;;
	*)
		printf '\n  nothing installed.\n\n'
		exit 1
		;;
	esac
fi

printf '\n'
nfail=0
for c in "${CMDS[@]}"; do
	printf '  ·  %s\n' "$c"
	if bash -c "$c"; then
		printf '  +  done\n\n'
	else
		nfail=$((nfail + 1))
		printf '  x  FAILED (continuing)\n\n'
	fi
done

# CodeChecker lands in a venv, never on PATH: the sca gate resolves it through
# $CODECHECKER, so an install that says nothing here reads as a no-op.
if [ -x "$ROOT/.venv-sca/bin/CodeChecker" ]; then
	printf '  CodeChecker is in .venv-sca — run the gate as:\n'
	printf '    CODECHECKER=.venv-sca/bin/CodeChecker make sca\n\n'
fi
# `go install` lands in GOBIN, or GOPATH/bin when GOBIN is unset. Neither is on
# every PATH, so a successful install can still leave `mcumgr` not found.
if command -v go >/dev/null 2>&1 && ! command -v mcumgr >/dev/null 2>&1; then
	gobin="$(go env GOBIN 2>/dev/null)"
	[ -n "$gobin" ] || gobin="$(go env GOPATH 2>/dev/null)/bin"
	printf '  go tools land in %s — add it to PATH if mcumgr is still not found.\n\n' "$gobin"
fi

if [ "$nfail" -gt 0 ]; then
	printf '  %d command(s) failed. Re-run `make tools` to see what is still missing.\n\n' "$nfail"
	exit 1
fi
printf '  installed. Re-run `make tools` to confirm.\n\n'
exit 0
