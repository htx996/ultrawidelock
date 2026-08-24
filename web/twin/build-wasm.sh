#!/usr/bin/env bash
#
# Build the twin's firmware: modules/ultrawidelock_uwb plus the tests/host shim,
# compiled to WASM through web/twin/twin_glue.c. Output is a single
# self-contained web/twin/twin.js (MODULARIZE + SINGLE_FILE, so the .wasm rides
# embedded and the page works from file://).
#
# twin.js is BUILD OUTPUT and is gitignored. The version that shipped in v0.3.0
# was committed: 36 KB of minified emscripten on one line, which conflicted on
# every merge that touched it. Build it, do not commit it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/web/twin/twin.js"
. "$ROOT/tests/host/sources.sh"

if ! command -v emcc >/dev/null 2>&1; then
	if [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
		# shellcheck disable=SC1091  # user-local emsdk, not part of the repo
		. "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
	fi
fi
if ! command -v emcc >/dev/null 2>&1; then
	echo "twin: emcc not found. brew install emscripten, or install emsdk." >&2
	exit 1
fi

# A deliberate, stable set: the decision logic only. Not the logging backends
# (the twin prints through Module.print) and not the STS seam wrapper (the twin
# uses the host radio doubles in dw_rx_stub.c). An explicit allowlist rather
# than a glob over the module, because a glob sweeps target-only and
# coverage-only files in and silently changes what the twin simulates.
UWB_SRCS=(
	"$SRC/ccc/ccc_kdf.c" "$SRC/ccc/ccc_mac.c" "$SRC/ccc/ccc_session.c"
	"$SRC/ccc/ccc_shim.c" "$SRC/ccc/ccc_sts.c" "$SRC/ccc/cherry_ccc_shim.c"
	"$SRC/ccc/ccc_shim_rx.c"
	"$SRC/cred/ultrawidelock_uwb_msg_builder.c"
	"$SRC/cred/ultrawidelock_uwb_msg_parser.c"
	"$SRC/cred/ultrawidelock_uwb_adapter.c"
	"$SRC/cred/ultrawidelock_uwb_msg.c"
	"$SRC/cred/ultrawidelock_uwb_session.c"
	"$SRC/fira/fira_session.c"
	"$SRC/fira/ds_twr.c"
	"$SRC/facade/ultrawidelock_uwb_facade.c"
)
# The CCC AES-ECB compatibility symbol is a role too. Read its manifest rather
# than naming the adapter here, so the twin follows the same source membership
# as Zephyr, ESP-IDF, FreeRTOS and the host suite.
TWIN_CRYPTO_SRCS=()
while IFS= read -r src || [ -n "$src" ]; do
	src="${src%%#*}"
	src="${src#"${src%%[![:space:]]*}"}"
	src="${src%"${src##*[![:space:]]}"}"
	[ -n "$src" ] || continue
	TWIN_CRYPTO_SRCS+=("$ROOT/$src")
done < "$ROOT/modules/ultrawidelock_uwb/roles/crypto_prim.list"
# Radio and STS-register doubles the responder links against: the host shim,
# not the target seam.
TWIN_SHIM_SRCS=("$SHIM/shim.c" "$SHIM/dw_rx_stub.c")

# Fail by name if a listed source moved, rather than on a cryptic emcc error.
for src in "${UWB_SRCS[@]}" "${TWIN_CRYPTO_SRCS[@]}" "${TWIN_SHIM_SRCS[@]}"; do
	if [ ! -f "$src" ]; then
		echo "twin: source not found: $src" >&2
		echo "twin: did modules/ultrawidelock_uwb or the host shim move?" >&2
		exit 1
	fi
done

# The flight recorder is a device diagnostic: a RAM ring dumped over a serial
# console the twin does not have. Its header stands in with no-op inlines when
# the define is absent, which keeps both it and its logging backend out of the
# link. The host suite keeps building it with the define on.
TWIN_DEFS=()
for def in "${DEFS[@]}"; do
	case "$def" in
	-DCONFIG_ULTRAWIDELOCK_FLIGHT_RECORDER*) ;;
	*) TWIN_DEFS+=("$def") ;;
	esac
done

emcc -std=c11 -O2 -w "${TWIN_DEFS[@]}" "${INCS[@]}" \
	-ffile-prefix-map="$ROOT"=. \
	"${UWB_SRCS[@]}" "${TWIN_CRYPTO_SRCS[@]}" "${TWIN_SHIM_SRCS[@]}" \
	"$ROOT/tests/host/aes_ref.c" \
	"$ROOT/tests/host/twin_frames.c" \
	"$ROOT/web/twin/twin_glue.c" \
	-sMODULARIZE=1 -sEXPORT_NAME=createTwin -sSINGLE_FILE=1 \
	-sENVIRONMENT=web,node -sALLOW_MEMORY_GROWTH=1 \
	-o "$OUT"

echo "twin: built web/twin/twin.js ($(wc -c <"$OUT" | tr -d ' ') bytes)"
