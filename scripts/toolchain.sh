#!/usr/bin/env bash
#
# toolchain.sh — report the host tools the suites need and what this machine has.
# Installs nothing. Exits nonzero when a gate tool is missing, so a suite
# skipping quietly is never a surprise. Bench tools are reported but never
# affect the exit status.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

# shellcheck disable=SC1091  # repo-local, sourced for $PY
. "$ROOT/tests/host/sources.sh"

# Gate tools: a missing one fails this script. Bench tools: flashing/serial,
# reported only. Optional tools: their gate skips loudly rather than failing, so
# a machine without one still runs a green `make check` -- reported, never fatal.
TOOLS=(cc python3 llvm-cov cbmc)
OPT_TOOLS=(cppcheck gitleaks CodeChecker)
FW_TOOLS=(tio nrfutil sdk-manager toolchain probe-rs mcumgr)

# The NCS version the Zephyr ports build against. Kept in step with the Makefile
# and scripts/bootstrap.sh, and only used to say which toolchain to look for.
NCS_VER="${NCS_VER:-v3.3.0}"

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
	nrfutil) echo "make bootstrap offers to install it · https://www.nordicsemi.com/Products/Development-tools/nrf-util" ;;
	sdk-manager | toolchain) echo "make bootstrap" ;;
	probe-rs) echo "https://probe.rs/docs/getting-started/installation/" ;;
	mcumgr) echo "go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest" ;;
	*) echo "install it however this host prefers, then re-run" ;;
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
		nrfutil --json sdk-manager toolchain list 2>/dev/null \
			| grep -q "\"$NCS_VER\"" || return 1
		echo "NCS $NCS_VER installed"
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

printf '\n  host tools  ·  %s %s\n\n' "$(uname -s)" "$(uname -m)"
nmiss=0
for t in "${TOOLS[@]}" __hr__ "${OPT_TOOLS[@]}" __hr__ "${FW_TOOLS[@]}"; do
	if [ "$t" = __hr__ ]; then
		printf '  %s\n' "----------------------------------------------------------------"
		continue
	fi
	if got="$(tool_probe "$t")"; then
		printf '  +  %-12s %-32s %s\n' "$t" "$(tool_gate "$t")" "$got"
	else
		case " ${FW_TOOLS[*]} ${OPT_TOOLS[*]} " in
		*" $t "*)
			case " ${FW_TOOLS[*]} " in
			*" $t "*) printf '  ~  %-12s %-32s not installed · bench only\n' "$t" "$(tool_gate "$t")" ;;
			*) printf '  ~  %-12s %-32s not installed · that gate skips\n' "$t" "$(tool_gate "$t")" ;;
			esac
			;;
		*)
			nmiss=$((nmiss + 1))
			printf '  x  %-12s %-32s MISSING · %s\n' "$t" "$(tool_gate "$t")" "$(tool_note "$t")"
			;;
		esac
	fi
done
printf '\n'

if [ "$nmiss" -gt 0 ]; then
	printf '  %d gate tool(s) missing — the suites above will not run.\n\n' "$nmiss"
	exit 1
fi
exit 0
