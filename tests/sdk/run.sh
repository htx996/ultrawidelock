#!/usr/bin/env bash
# Build the plain-CMake SDK through its source and installed interfaces.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

check_declarations() {
	local canonical="$ROOT/$1"

	[ -f "$canonical" ] &&
		grep -Eq '^(struct |enum |[A-Za-z_][A-Za-z0-9_ *]*\()' "$canonical" || {
		echo "sdk API: FAIL (canonical header has no declarations: $1)" >&2
		exit 1
	}
}

check_declarations modules/ultrawidelock_cred/include/ultrawidelock/reader.h
check_declarations modules/ultrawidelock_cred/include/ultrawidelock/device.h
check_declarations modules/ultrawidelock_cred/include/ultrawidelock/tlv.h
check_declarations modules/ultrawidelock_uwb/include/ultrawidelock/uwb.h

# The flat headers this API replaced, banned so they cannot come back. This list
# used to spell them in their pre-rename names, and the rename carried those
# across with everything else -- which left it holding each new name twice and
# banning none of the originals. Writing the old names back is not the fix.
# What this list owns is the flat shape under our own prefix. Run
# `git log -p pre-rename -- tests/sdk/run.sh` for the names themselves.
for legacy in modules/ultrawidelock_cred/include/ultrawidelock_reader.h \
	modules/ultrawidelock_cred/include/ultrawidelock_device.h modules/ultrawidelock_cred/include/ultrawidelock_tlv.h \
	modules/ultrawidelock_uwb/include/ultrawidelock_uwb_facade.h modules/ultrawidelock_port/include/ultrawidelock_hal.h; do
	[ ! -e "$ROOT/$legacy" ] || {
		echo "sdk API: FAIL (legacy API header returned: $legacy)" >&2
		exit 1
	}
done
if git grep -nE '#include [<"](ultrawidelock_reader|ultrawidelock_device|ultrawidelock_tlv|ultrawidelock_uwb_facade|ultrawidelock_hal)[.]h[>"]' \
	-- apps examples integrations modules ports tests/host tests/ports tests/shared tests/tooling \
	>/dev/null; then
	echo "sdk API: FAIL (legacy API include returned)" >&2
	exit 1
fi

find_cmake() {
	if [ -n "${CMAKE:-}" ]; then
		printf '%s\n' "$CMAKE"
		return
	fi
	if command -v cmake >/dev/null 2>&1; then
		command -v cmake
		return
	fi

	local candidate
	for candidate in "${IDF_TOOLS_PATH:-$HOME/.espressif}"/tools/cmake/*/bin/cmake \
		"${IDF_TOOLS_PATH:-$HOME/.espressif}"/tools/cmake/*/CMake.app/Contents/bin/cmake; do
		if [ -x "$candidate" ]; then
			printf '%s\n' "$candidate"
			return
		fi
	done
	return 1
}

CMAKE_BIN="$(find_cmake)" || {
	echo "sdk package: FAIL (cmake not found; set CMAKE=/path/to/cmake)" >&2
	exit 1
}

SDK_VERSION="$(sed -n '1p' "$ROOT/VERSION")"
if ! printf '%s\n' "$SDK_VERSION" | grep -Eq '^[0-9]+[.][0-9]+[.][0-9]+$'; then
	echo "sdk package: FAIL (VERSION is not major.minor.patch)" >&2
	exit 1
fi
SDK_SERIES="${SDK_VERSION%.*}"
SDK_NEXT_MINOR="$(printf '%s\n' "$SDK_VERSION" |
	awk -F. '{ printf "%d.%d", $1, $2 + 1 }')"

TMP="$(mktemp -d -t ultrawidelock-sdk.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

"$CMAKE_BIN" -G "Unix Makefiles" -S "$ROOT" -B "$TMP/sdk" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$TMP/install" >/dev/null
MAKEFLAGS= MFLAGS= "$CMAKE_BIN" --build "$TMP/sdk" >/dev/null
"$CMAKE_BIN" --install "$TMP/sdk" >/dev/null

expected_headers=$(printf '%s\n' \
	ultrawidelock_advtag.h ultrawidelock_apdu.h ultrawidelock_ble.h ultrawidelock_ble_central.h \
	ultrawidelock_crypto.h ultrawidelock_device_apdu.h ultrawidelock_prov.h device.h dw3000_hw.h \
	dw3000_spi.h ultrawidelock.h reader.h tlv.h uwb.h ultrawidelock_hal.h | LC_ALL=C sort)
installed_headers=$(find "$TMP/install/include/ultrawidelock" -type f -name '*.h' \
	-exec basename {} \; | LC_ALL=C sort)
[ "$installed_headers" = "$expected_headers" ] || {
	echo "sdk API: FAIL (installed header set drifted)" >&2
	exit 1
}

"$CMAKE_BIN" -G "Unix Makefiles" -S "$ROOT/examples/cmake/consumer" -B "$TMP/consumer" \
	-DCMAKE_BUILD_TYPE=Release \
	-DULTRAWIDELOCK_REQUIRED_VERSION="$SDK_SERIES" \
	-DCMAKE_PREFIX_PATH="$TMP/install" >/dev/null
MAKEFLAGS= MFLAGS= "$CMAKE_BIN" --build "$TMP/consumer" >/dev/null
"$TMP/consumer/ultrawidelock_consumer"

if "$CMAKE_BIN" -G "Unix Makefiles" -S "$ROOT/examples/cmake/consumer" \
	-B "$TMP/incompatible" \
	-DULTRAWIDELOCK_REQUIRED_VERSION="$SDK_NEXT_MINOR" \
	-DCMAKE_PREFIX_PATH="$TMP/install" >/dev/null 2>&1; then
	echo "sdk package: FAIL (next minor version was accepted)" >&2
	exit 1
fi

"$CMAKE_BIN" -G "Unix Makefiles" -S "$ROOT/examples/cmake/consumer" \
	-B "$TMP/source-consumer" \
	-DCMAKE_BUILD_TYPE=Release \
	-DULTRAWIDELOCK_SOURCE_DIR="$ROOT" >/dev/null
MAKEFLAGS= MFLAGS= "$CMAKE_BIN" --build "$TMP/source-consumer" >/dev/null
"$TMP/source-consumer/ultrawidelock_consumer"

printf 'sdk package: PASS (10 checks)\n'
