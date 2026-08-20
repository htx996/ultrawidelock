#!/usr/bin/env bash
#
# zephyr_opt_suite.sh — the instrument and dashboard tools, checked without a build.
#
# `make instrument` builds, flashes and renders a page out of a real DWM3001CDK.
# None of that can run in a host suite, but the two halves that turn probe output
# into the page are pure python and were the parts that broke: a schema field
# renamed on one side, a snapshot the renderer could no longer read. Both suites
# are `unittest`, so they pin exactly that seam.
#
#   tests/tooling/zephyr_opt_suite.sh   # both suites, run as the `zopt` suite
#
# Exit 0 clean, 1 if either suite fails.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
rc=0

for t in test_zephyr_opt_instrument.py test_zephyr_opt_dashboard.py; do
	printf '\n── zephyr-opt · %s\n' "$t"
	python3 "$HERE/$t" || rc=1
done

printf '\n'
if [ "$rc" -ne 0 ]; then
	printf '  zephyr-opt: failure(s) above\n\n'
	exit 1
fi
printf '  zephyr-opt: clean\n\n'
