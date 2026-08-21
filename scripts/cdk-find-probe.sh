#!/usr/bin/env bash
# cdk-find-probe.sh — print the probe triple (VID:PID:Serial) wired to the DWM3001CDK.
#
# Usage: cdk-find-probe.sh <cache-file>
#   stdout   the triple, or nothing when pinning is unnecessary
#   exit 0   triple printed, or nothing to do (probe-rs absent, 0 or 1 probe attached)
#   exit 1   several probes attached and the CDK could not be settled (reason on stderr)
#
# WHY IDENTIFY BY SILICON. Probe enumeration order is not stable across replugs
# (mk/cdk.mk measured it flipping between two `probe-rs list` calls with no cable
# touched), and every J-Link OB calls itself "J-Link", so nothing in the listing
# says which one sits on the DWM3001CDK. What does say so is the part behind the
# probe: FICR INFO.PART at 0x10000100 reads 0x00052833 on an nRF52833 and the
# read FAULTS through a probe wired to anything else (verified on the bench
# against an nRF5340 DK). So with several probes attached, read that word
# through each candidate and the CDK identifies itself.
#
# The winning triple is cached in <cache-file> (under apps/dwm3001cdk-lock/keys/, which is
# deny-all gitignored -- a probe serial is machine-local state and must never be
# committed). While the cached serial is attached it is trusted without touching
# any probe, so the identification cost is paid once per bench, not per flash.
# Unplugged the CDK for good, or moved the cache to the wrong board somehow?
# Delete the cache file and the next probe-touching target re-identifies.
#
# With zero or one probe attached this prints nothing and exits 0: one probe
# needs no pinning (the tools pick it), and probe-rs being absent must not
# become a new reason a flash cannot run -- both per the guard in mk/cdk.mk.

set -u

# Defaults identify the DWM3001CDK; the four FIND_* variables retarget the same
# mechanism at any other board on the bench. mk/satellite.mk passes the nRF5340
# DK's values (its FICR lives at a different base, so the address must move with
# the part pattern or every candidate faults and none matches).
CACHE="${1:?usage: cdk-find-probe.sh <cache-file>}"
CHIP="${FIND_CHIP:-${CDK_CHIP:-nRF52833_xxAA}}"
FICR_PART="${FIND_FICR_ADDR:-0x10000100}"
PART_PAT="${FIND_PART_PAT:-52833}"
LABEL="${FIND_LABEL:-DWM3001CDK}"
PART_NAME="${FIND_PART_NAME:-nRF52833}"

command -v probe-rs >/dev/null 2>&1 || exit 0

# One triple per line, e.g. "1366:0105:<Serial>" out of
# "[0]: J-Link -- 1366:0105:<Serial> (J-Link)".
triples=$(probe-rs list 2>/dev/null |
	sed -n 's/^\[[0-9]*\]: .* -- \([^ ]*\) (.*$/\1/p')
count=$(printf '%s\n' "$triples" | grep -c . || true)

[ "$count" -le 1 ] && exit 0

if [ -f "$CACHE" ]; then
	cached=$(head -n1 "$CACHE")
	if [ -n "$cached" ] && printf '%s\n' "$triples" | grep -qxF "$cached"; then
		printf '%s\n' "$cached"
		exit 0
	fi
	# Stale: the pinned probe is not attached. Fall through and re-identify.
fi

matches=()
while IFS= read -r t; do
	[ -n "$t" ] || continue
	if probe-rs read --chip "$CHIP" --probe "$t" b32 "$FICR_PART" 1 2>/dev/null |
		grep -q "$PART_PAT"; then
		matches+=("$t")
	fi
done <<<"$triples"

if [ "${#matches[@]}" -eq 1 ]; then
	printf '%s\n' "${matches[0]}" >"$CACHE"
	echo "  pinned the $LABEL to probe ${matches[0]}" >&2
	echo "  (silicon-verified; cached in $CACHE -- delete that file to re-identify)" >&2
	printf '%s\n' "${matches[0]}"
	exit 0
fi

if [ "${#matches[@]}" -eq 0 ]; then
	echo "  $count probes attached and none answers as an $PART_NAME (FICR INFO.PART)." >&2
	echo "  Is the $LABEL plugged in and its target powered?" >&2
else
	echo "  ${#matches[@]} probes answer as an $PART_NAME; cannot settle which is the $LABEL." >&2
fi
probe-rs list >&2
echo "  Pick one:  make <target> ${FIND_VAR_HINT:-CDK_PROBE}=<VID:PID:Serial>" >&2
exit 1
