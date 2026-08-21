#!/usr/bin/env bash
#
# twin_suite.sh — the WASM firmware twin, replaying its ranging scenarios.
#
# web/twin/ compiles the real modules/ultrawidelock_uwb through emcc, so the page
# a visitor drives is the firmware's own state machine rather than a mock of it.
# That is only worth anything while it still behaves: selftest.cjs replays a
# legitimate approach, the Ghost-Peak negative-ToF spoof and the K-block trust
# earn through the same glue entry points the page calls.
#
# Skipped loudly, not failed, when emcc or node is missing -- the emscripten SDK
# is a multi-hundred-megabyte install and this is the only thing in the tree that
# needs it. A skip that looked like a pass would be worse, so it says so.
#
#   tests/tooling/twin_suite.sh   # run as the `twin` suite; `make test-twin` too
#
# Exit 0 clean or skipped, 1 on a failing scenario.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

missing=
command -v emcc >/dev/null 2>&1 || missing="emcc"
command -v node >/dev/null 2>&1 || missing="${missing:+$missing and }node"

if [ -n "$missing" ]; then
	printf '\n── wasm twin · SKIPPED\n'
	printf '  %s not installed, so the twin was not built or replayed.\n' "$missing"
	printf '  This suite proved nothing on this machine. Install the emscripten\n'
	printf '  SDK (https://emscripten.org/docs/getting_started/downloads.html)\n'
	printf '  and node, then re-run `make test-twin`.\n\n'
	exit 0
fi

printf '\n── wasm twin · build\n'
"$ROOT/web/twin/build-wasm.sh"

printf '\n── wasm twin · scenarios\n'
node "$ROOT/web/twin/selftest.cjs"
