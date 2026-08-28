#!/usr/bin/env bash
# cdk-dfu.sh: push a signed image to the DWM3001CDK over MCUboot serial recovery.
#
# WHY THIS IS A SCRIPT AND NOT A MAKE RECIPE. A normal boot no longer waits for
# a host. The operator explicitly holds SW2 while the application is running;
# after five seconds it writes Zephyr's retained boot mode and warm-resets into
# MCUboot. This script probes while that gesture is made, then performs the
# single-slot upload only after MCUboot has answered.
#
# A short runtime SW2 press still opens the signed application DFU window. The
# hold is deliberately longer and enters the cable-only recovery floor. SW2 at
# boot retains its factory-reset/provisioning meaning; MCUboot never claims the
# GPIO directly.
#
# WHAT DOES NOT WORK, AND IS NOT UNDERSTOOD. Serial recovery completed exactly
# one real upload (2026-08-02 ~22:00) and has not been reproducible since, on
# the same config, binary and board. Ruled out by measurement, none of them the
# cause: the window duration (400 ms and 30000 ms fail identically), a J-Link
# session blocking the VCOM (a cold boot with no debugger fails too), a stale
# process on the port, a wedged probe, board health, the provisioned state, and
# the reset mechanism. Also verified WORKING: UART TX (3,392 bytes out of the
# app), UART RX electrically (EVENTS_RXDRDY=1 and ERRORSRC=0x1 after 200 bytes
# in), the pinctrl in both images, and MCUboot reaching its wait window at all
# (the application used to appear ~5 s after reset, which was the old window
# elapsing).
#
# So MCUboot sits in its window on a working UART and does not answer.
#
# NARROWED 2026-08-27, and this is the useful part. The APPLICATION now speaks
# mcumgr on this same uart0 (CONFIG_MCUMGR_TRANSPORT_UART in overlay-smp.conf),
# and it answers on the first try -- image list, and multi-frame requests too: a
# 384 B payload arrives as five back-to-back 127-byte frames, intact, and is
# refused with rc=11 by the window gate. The same board reported the same
# sha=000f654e8c031181 over Bluetooth minutes later.
#
# That exonerates everything underneath MCUboot: the pins, the probe's VCOM, the
# 115200 rate, the nRF's legacy non-DMA UART driver, and the host end. Whatever
# CDK-16 is, it is inside MCUboot's serial adapter. Reach for
# `ultrawidelock_smp.py --serial` when testing it -- it is a second, independent
# host implementation, so a failure there is the bootloader's and not the Go
# mcumgr's.
#
# ONE CORRECTION TO THE LIST ABOVE. "ERRORSRC=0x1" is filed under verified
# WORKING. On nRF52, ERRORSRC bit 0 is OVERRUN -- so that measurement says bytes
# arrived AND were dropped, which is not the same as RX being healthy. Given the
# application now receives full frames on this UART it is unlikely to be the
# cause, but it does not belong in the "ruled out" column either.
#
# The next measurement worth taking is still instrumenting MCUboot itself rather
# than inferring it from outside: CONFIG_MCUBOOT_INDICATION_LED with an
# mcuboot-led0 alias, or logging over RTT, to see whether boot_serial_check_start
# is entered and with what timeout.

# Single slot: the upload OVERWRITES the running image. A torn transfer leaves no
# application, which is recoverable and not a brick -- MCUboot stays in recovery
# (CONFIG_BOOT_SERIAL_NO_APPLICATION=y) and `make flash` over SWD always works.
# What a torn transfer must never be answered with is an erase: `make flash-erase`
# and `nrfjprog --recover` both destroy settings_storage, and with it the Matter
# fabrics and the reader identity.
set -uo pipefail

PORT="${1:?usage: cdk-dfu.sh <port> <baud> <signed-image> [chip]}"
BAUD="${2:?}"
IMAGE="${3:?}"
# Fourth argument is retained for makefile compatibility; recovery entry no
# longer uses SWD or needs a chip name.
: "${4:-nRF52833_xxAA}"
CONN="dev=${PORT},baud=${BAUD}"
PROBES="${DFU_PROBES:-120}"

command -v mcumgr >/dev/null 2>&1 || {
	echo "  mcumgr not found  ·  see \`make tools\`" >&2; exit 1; }
[ -f "$IMAGE" ] || { echo "  no signed image at $IMAGE  ·  run \`make build\` first" >&2; exit 1; }

# Probe device connectivity by listing MCUmgr images over serial connection with 0.4 second timeout.
# Returns true if device responds, false otherwise.
probe() { mcumgr --conntype serial --connstring "$CONN" -t 0.4 image list >/dev/null 2>&1; }

printf '  upload %s\n      to %s @ %s\n' "$IMAGE" "$PORT" "$BAUD"

# Already sitting in recovery from an earlier run? Skip the gesture.
if probe; then
	printf '  MCUboot is already in recovery\n'
else
	printf '  hold SW2 continuously for 5 seconds while the application is running\n'
	i=0
	hit=''
	while [ "$i" -lt "$PROBES" ]; do
		i=$((i + 1))
		if probe; then hit="$i"; break; fi
	done
	if [ -z "$hit" ]; then
		echo "  MCUboot never answered across $PROBES probes." >&2
		echo "  Confirm the app logged the SW2 recovery hold, then instrument MCUboot serial recovery." >&2
		exit 1
	fi
	printf '  MCUboot answered on probe %s\n' "$hit"
fi

printf '  uploading %s bytes  ·  DO NOT INTERRUPT (60-90 s at %s baud)\n' \
	"$(wc -c <"$IMAGE" | tr -d ' ')" "$BAUD"
if ! mcumgr --conntype serial --connstring "$CONN" -t 30 image upload "$IMAGE"; then
	echo "  upload failed mid-transfer  ·  the board is still in recovery, just re-run" >&2
	exit 1
fi
printf '  upload OK  ·  resetting into it\n'
mcumgr --conntype serial --connstring "$CONN" -t 5 reset >/dev/null 2>&1 || true
