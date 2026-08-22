#!/usr/bin/env bash
# hitl-run.sh — one unattended pass of the DK-as-iPhone loop: enrol the
# initiator against the live reader, build it, flash it, and judge the result
# off what both boards actually say.
#
# Usage:
#   scripts/hitl-run.sh                       # the whole loop
#   scripts/hitl-run.sh --skip-enrol          # reuse src/bench_identity.h as-is
#   scripts/hitl-run.sh --skip-enrol --skip-build --skip-flash
#                                             # judge the boards as they run now
#   TIMEOUT=180 scripts/hitl-run.sh           # give the verdict window longer
#   PORT=/dev/cu.usbmodemXXXX scripts/hitl-run.sh   # name the DK console port
#
# Options:
#   --skip-enrol   keep the existing bench_identity.h. Without this flag a run
#                  MINTS A FRESH CREDENTIAL, which unbinds any DK image built
#                  against the previous header -- that is why --skip-build and
#                  --skip-flash are refused unless enrolment is skipped too:
#                  a fresh credential with a stale board can never pass.
#   --skip-build   do not rebuild the initiator
#   --skip-flash   do not touch the DK (implies the image on it is current)
#   --no-reader    judge from the DK console alone, without the reader's RTT
#   Environment: NODE (0x1234), STORAGE (~/.ultrawidelock-chip-tool), FABRIC (alpha),
#                TIMEOUT (verdict window seconds, 120), PORT (DK console)
#
# Exit: 0 PASS · 1 FAIL (verdict window closed without the markers, or a fail
# marker appeared) · 2 a stage refused to run (reason on stderr)
#
# What PASS means, and why these markers:
#   DK console  "=== ESTABLISHED: URSK agreed with the reader ===" -- the whole
#               BLE Access Protocol succeeded against the enrolled identity.
#   DK console  "Pre-POLL #" -- M1..M3 negotiated AND the transmitter armed; a
#               repeating needle on purpose, the 5 Hz cadence outlives the
#               serial capture's occasional byte drops where a one-shot
#               banner did not.
#   reader RTT  "PREPOLL OK" -- the reader DECRYPTED a Pre-POLL whose CCM* MIC
#               is keyed by mUPSK1 from the URSK (ccc_shim_rx.c), so the key the
#               protocol produced is the same key on both ends, on air. This is
#               the one marker a BLE-only fake cannot produce, which is why the
#               reader watch is on by default and --no-reader is the opt-out.
#
# The reader is watched over its J-Link (apps/dwm3001cdk-lock/keys/cdk-probe) against the
# ELF recorded at deploy time (build/cdk-deployed/zephyr.elf) -- the build tree
# holds the image being built NEXT, not the one running, per mk/cdk.mk.
#
# Artifacts land in build/hitl-run/<timestamp>/: enrol.log, dk-serial.log,
# cdk-rtt.log, verdict.txt. The serial capture starts BEFORE the flash so the
# boot banner and a fast session are never missed.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Everything below assumes repo-root cwd; invoked from scripts/ the enrol step
# once wrote its header to scripts/ports/... and the mismatch surfaced two
# stages later as an untrusted credential.
cd "$REPO_ROOT" || exit 2

NODE="${NODE:-0x1234}"
STORAGE="${STORAGE:-$HOME/.ultrawidelock-chip-tool}"
FABRIC="${FABRIC:-alpha}"
TIMEOUT="${TIMEOUT:-120}"
PORT="${PORT:-}"

SKIP_ENROL=0 SKIP_BUILD=0 SKIP_FLASH=0 NO_READER=0
for arg in "$@"; do
	case "$arg" in
	--skip-enrol) SKIP_ENROL=1 ;;
	--skip-build) SKIP_BUILD=1 ;;
	--skip-flash) SKIP_FLASH=1 ;;
	--no-reader)  NO_READER=1 ;;
	-h|--help) sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $arg (see --help)" >&2; exit 2 ;;
	esac
done

die() { printf '  %s\n' "$*" >&2; exit 2; }

# A fresh credential with a stale board can never pass; refuse the combination
# rather than let it burn a verdict window looking like a radio fault.
if [ "$SKIP_ENROL" = 0 ] && { [ "$SKIP_BUILD" = 1 ] || [ "$SKIP_FLASH" = 1 ]; }; then
	die "a run that enrols must also build and flash: the fresh credential" \
	    "replaces the one the flashed image carries. Add --skip-enrol, or drop the skips."
fi

TS="$(date +%Y%m%d-%H%M%S)"
ART="$REPO_ROOT/build/hitl-run/$TS"
mkdir -p "$ART"

# ---- preflight ---------------------------------------------------------------

if [ "$SKIP_ENROL" = 0 ]; then
	[ -d "$STORAGE" ] || die "no controller identity at $STORAGE -- commission once" \
	                         "(scripts/ultrawidelock-enroll.py --pairing-code ...) or pass STORAGE="
fi

# The DK console: the ioreg walk picks the serial number exposing the MOST
# usbmodem ports (the DK's interface has several VCOMs; the CDK's J-Link OB has
# one), then the highest port of it (VCOM1 -- VCOM0 is silent). Same heuristic
# as mk/nrf5340dk.mk's nrf-term, lifted rather than shared because make owns
# that recipe's quoting.
if [ -z "$PORT" ]; then
	PORT=$(ioreg -l -w0 2>/dev/null |
		awk '/kUSBSerialNumberString/{s=$0;sub(/.*= "/,"",s);sub(/".*/,"",s);serial=s}
		     /IOCalloutDevice/&&/usbmodem/{c=$0;sub(/.*= "/,"",c);sub(/".*/,"",c);print serial"\t"c}' |
		sort |
		awk -F'\t' '{cnt[$1]++; if($2>max[$1])max[$1]=$2}
		            END{best="";bc=-1; for(x in cnt) if(cnt[x]>bc||(cnt[x]==bc&&x<best)){bc=cnt[x];best=x}
		                if(best!="")print max[best]}')
fi
[ -n "$PORT" ] && [ -e "$PORT" ] || die "no DK console port found; pass PORT=/dev/cu.usbmodemXXXX"

READER_WATCH=0
CDK_PROBE_FILE="$REPO_ROOT/apps/dwm3001cdk-lock/keys/cdk-probe"
CDK_ELF="$REPO_ROOT/build/cdk-deployed/zephyr.elf"
if [ "$NO_READER" = 0 ]; then
	if [ -f "$CDK_PROBE_FILE" ] && [ -f "$CDK_ELF" ] && command -v probe-rs >/dev/null 2>&1; then
		READER_WATCH=1
	else
		die "reader watch needs $CDK_PROBE_FILE, $CDK_ELF and probe-rs;" \
		    "pass --no-reader to judge from the DK console alone"
	fi
fi

printf '  artifacts: %s\n' "$ART"
printf '  DK console: %s · reader watch: %s · verdict window: %ss\n' \
	"$PORT" "$([ "$READER_WATCH" = 1 ] && echo on || echo off)" "$TIMEOUT"

# ---- 1. enrol ----------------------------------------------------------------

if [ "$SKIP_ENROL" = 0 ]; then
	printf '1. enrolling the initiator (headless, fabric %s)\n' "$FABRIC"
	if ! python3 "$REPO_ROOT/scripts/ultrawidelock-enroll.py" --node-id "$NODE" \
		--storage "$STORAGE" --fabric "$FABRIC" >"$ART/enrol.log" 2>&1; then
		tail -5 "$ART/enrol.log" >&2
		die "enrolment failed (full log: $ART/enrol.log)"
	fi
	printf '   credential posted; header written\n'
else
	printf '1. enrolment skipped; using the existing bench_identity.h\n'
	[ -f "$REPO_ROOT/examples/zephyr/nrf5340dk-initiator/src/bench_identity.h" ] ||
		die "--skip-enrol but no bench_identity.h exists; run once without it"
fi

# ---- 2. build ----------------------------------------------------------------

if [ "$SKIP_BUILD" = 0 ]; then
	printf '2. building the initiator\n'
	if ! make -C "$REPO_ROOT" nrf-init-build >"$ART/build.log" 2>&1; then
		tail -15 "$ART/build.log" >&2
		die "initiator build failed (full log: $ART/build.log)"
	fi
else
	printf '2. build skipped\n'
fi

# ---- 3. capture starts, then flash ------------------------------------------

# Serial first: the DK's VCOM rides the J-Link OB, not the target, so it
# survives the flash -- and a session can complete within seconds of boot.
#
# The HOLDER opens the port before stty touches it. In the other order the
# settings revert the moment stty's own descriptor closes (termios resets on
# last-close), cat reopens at the default baud, and the capture is 85 bytes of
# noise that reads as a silent board -- the first live run failed exactly there.
cat "$PORT" >"$ART/dk-serial.log" 2>/dev/null &
SERIAL_PID=$!
sleep 0.2
stty -f "$PORT" 115200 raw 2>/dev/null || true

RTT_PID=
if [ "$READER_WATCH" = 1 ]; then
	# The deployed reader is a pretty-shell build, and pretty mode compiles
	# DIAGK's default to off -- so "PREPOLL OK", the one line that proves a
	# Pre-POLL's URSK-keyed MIC verified, never prints. The gate is a volatile
	# int, so flip it in RAM for this window, the same probe-rs poke
	# `make ota-window` already relies on. Resolved from the deployed ELF at
	# run time because LTO moves it between builds. Left on afterwards: it
	# reverts at the reader's next reboot and costs only trace lines.
	DIAG_ADDR=$( { nm "$CDK_ELF" 2>/dev/null || arm-none-eabi-nm "$CDK_ELF" 2>/dev/null; } |
		awk '/ ultrawidelock_uwb_diag_on$/{print $1}')
	if [ -n "$DIAG_ADDR" ]; then
		probe-rs write --chip "${CDK_CHIP:-nRF52833_xxAA}" \
			--probe "$(cat "$CDK_PROBE_FILE")" b8 "0x$DIAG_ADDR" 1 \
			>/dev/null 2>&1 || true
	fi
	# Under `script`, because probe-rs attach insists on a terminal: with
	# stdout sent straight to a file it panics in its event reader before
	# printing a line ("reader source not set", crossterm). The pty costs the
	# log some control characters, which the -a greps below already tolerate.
	script -q "$ART/cdk-rtt.log" probe-rs attach \
		--chip "${CDK_CHIP:-nRF52833_xxAA}" \
		--probe "$(cat "$CDK_PROBE_FILE")" "$CDK_ELF" \
		>/dev/null 2>&1 &
	RTT_PID=$!
fi

cleanup() {
	[ -n "${SERIAL_PID:-}" ] && kill "$SERIAL_PID" 2>/dev/null
	# probe-rs runs as script's child; killing script alone can orphan it
	# holding the probe, which the next run then cannot open.
	[ -n "${RTT_PID:-}" ] && { pkill -P "$RTT_PID" 2>/dev/null; kill "$RTT_PID" 2>/dev/null; }
	wait 2>/dev/null
}
trap cleanup EXIT

if [ "$SKIP_FLASH" = 0 ]; then
	printf '3. flashing the DK (both cores)\n'
	if ! make -C "$REPO_ROOT" nrf-init-flash >"$ART/flash.log" 2>&1; then
		# An nRF53 with an erased UICR engages APPROTECT on the next POWER
		# CYCLE, so the first flash after a recable fails asking to be
		# recovered. The initiator is stateless -- its identity is compiled
		# in -- so recovery costs nothing here, and an unattended loop that
		# stops for it would fail every first run after recabling the rig.
		if grep -q "must be recovered" "$ART/flash.log"; then
			printf '   APPROTECT engaged (power-cycled since last flash); recovering the DK\n'
			if ! ULTRAWIDELOCK_BUILD="$REPO_ROOT/build/nrf5340dk-initiator" \
				"$REPO_ROOT/scripts/nrf5340dk-build.sh" flash-recover \
				>>"$ART/flash.log" 2>&1; then
				tail -10 "$ART/flash.log" >&2
				die "DK recover-flash failed too (full log: $ART/flash.log)"
			fi
		else
			tail -10 "$ART/flash.log" >&2
			die "DK flash failed (full log: $ART/flash.log)"
		fi
	fi
	# One more hard reset AFTER the flash. The first boot out of the flash
	# sequence hung at bt_enable often enough to burn a whole verdict window
	# (both cores flashed, app silent before the BLE banner, reader never
	# contacted), and a plain reset brought the same image up clean. The
	# serial number is read from the flash's own log, never guessed.
	DK_SNR=$(grep -ao -- '--serial-number [0-9]*' "$ART/flash.log" | /usr/bin/head -1 | cut -d' ' -f2)
	if [ -n "$DK_SNR" ] && command -v nrfutil >/dev/null 2>&1; then
		sleep 1
		nrfutil device reset --serial-number "$DK_SNR" >>"$ART/flash.log" 2>&1 ||
			printf '   (post-flash reset refused; continuing with the flash-time boot)\n'
	fi
else
	printf '3. flash skipped; judging the image already on the board\n'
fi

# ---- 4. verdict --------------------------------------------------------------

printf '4. watching for the session (up to %ss)\n' "$TIMEOUT"

# Short needles on purpose: the 115200 capture drops occasional byte runs
# under burst (observed live: "ultrawidelock_centour reader"), and a marker longer
# than the typical gap can lose a real PASS to line noise.
DK_ESTABLISHED='URSK agreed'
DK_SETUP='Pre-POLL #'
RD_PREPOLL='PREPOLL OK|Pre-POLL accepted'
# Any of these ends the run early as a FAIL: each names a cause, and waiting
# out the window after one appears only obscures it.
DK_FATAL='DEV fallback|DW3000 init failed|Pre-POLL TX did not start|command rejected in phase'

DEADLINE=$(( $(date +%s) + TIMEOUT ))
PASS_DK=0 PASS_SETUP=0 PASS_RD=0 VERDICT=FAIL WHY="verdict window closed"
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
	if grep -qE "$DK_FATAL" "$ART/dk-serial.log" 2>/dev/null; then
		WHY="DK reported: $(grep -m1 -oE "$DK_FATAL" "$ART/dk-serial.log")"
		break
	fi
	grep -qF "$DK_ESTABLISHED" "$ART/dk-serial.log" 2>/dev/null && PASS_DK=1
	grep -qF "$DK_SETUP" "$ART/dk-serial.log" 2>/dev/null && PASS_SETUP=1
	[ "$READER_WATCH" = 1 ] &&
		grep -aqE "$RD_PREPOLL" "$ART/cdk-rtt.log" 2>/dev/null && PASS_RD=1
	if [ "$PASS_DK" = 1 ] && [ "$PASS_SETUP" = 1 ] &&
	   { [ "$READER_WATCH" = 0 ] || [ "$PASS_RD" = 1 ]; }; then
		VERDICT=PASS WHY=""
		break
	fi
	sleep 2
done

# A flash guarantees a boot banner, so zero console bytes after one is a dead
# capture (wrong port, opened-by-another-process), never a quiet session --
# name it, or it reads as a radio failure and costs an evening.
if [ "$SKIP_FLASH" = 0 ] && [ ! -s "$ART/dk-serial.log" ]; then
	WHY="no bytes from the DK console at all: the capture or PORT is wrong, not the session"
fi

{
	printf 'verdict: %s\n' "$VERDICT"
	[ -n "$WHY" ] && printf 'why: %s\n' "$WHY"
	printf 'URSK agreed (DK):          %s\n' "$([ "$PASS_DK" = 1 ] && echo yes || echo NO)"
	printf 'Pre-POLL armed (DK):       %s\n' "$([ "$PASS_SETUP" = 1 ] && echo yes || echo NO)"
	if [ "$READER_WATCH" = 1 ]; then
		printf 'Pre-POLL accepted (reader): %s\n' "$([ "$PASS_RD" = 1 ] && echo yes || echo NO)"
	fi
} | tee "$ART/verdict.txt"

[ "$VERDICT" = PASS ]
