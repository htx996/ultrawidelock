#!/bin/sh
# SPDX-License-Identifier: ISC
#
# Read the DWM3001CDK's power-determining state off a RUNNING board, over SWD.
#
# WHY THIS EXISTS, AND WHAT IT IS NOT. There is no PPK2 in this repo's loop and
# the nRF52833 has no current sense, so nothing here measures milliamps. What it
# does measure is the set of registers that DECIDE the milliamps: which clock
# sources are running, whether the DC/DC is on, which peripherals are enabled
# with nobody using them, and what state the 2.4 GHz radio is actually sitting
# in. Those are facts about the silicon, read while it runs, and they convert
# most of a power review from PREDICTED to MEASURED without any hardware.
#
# The current column is arithmetic on datasheet figures, NOT a measurement. It
# is labelled PREDICTED everywhere it appears and exists only to rank the terms.
# The moment a PPK2 is on the current-measurement header, that column is what
# gets falsified first. See docs/power-baseline.md.
#
# NON-INVASIVE BY CONSTRUCTION. Every access is a memory read over the AHB-AP.
# The core is never halted and never reset, so the board keeps advertising,
# keeps its Thread attachment and keeps whatever session it had. The RTC1
# liveness check below is the proof: a counter that advances across two reads
# is a kernel that was not stopped to take them.
#
#   ./scripts/power-baseline.sh [samples]     # default 20 radio/clock samples
#
set -eu

CHIP="${PROBE_RS_CHIP:-nRF52833_xxAA}"
SAMPLES="${1:-20}"
export PROBE_RS_CHIP="$CHIP"

command -v probe-rs >/dev/null 2>&1 || {
	printf '  probe-rs is not on PATH. It is what mk/cdk.mk already uses for SWD.\n' >&2
	exit 1
}

# One 32-bit read, hex value only. probe-rs prints "<addr>: <value>"; the tail
# picks the data line off any banner, the awk picks the value off the address.
rd() {
	probe-rs read b32 "$1" 1 2>/dev/null | tail -1 | awk '{ print $2 }'
}

# Bit n of a hex word, as 0 or 1.
bit() {
	printf '%d' $(( (0x$1 >> $2) & 1 ))
}

hdr() { printf '\n%s\n' "$1"; }

printf 'ultrawidelock power baseline  ·  chip %s  ·  %s\n' "$CHIP" "$(date -u '+%Y-%m-%dT%H:%MZ')"

# ---- liveness -------------------------------------------------------------
#
# RTC1 is the kernel's tick source at exactly 32,768 Hz, so a counter that moves
# between two reads proves the kernel ran between them and therefore that
# attaching did not halt or reset the board. docs/dwm3001cdk-surgery.md uses the
# same test on nrf_rtc_timer's last_count.
hdr 'LIVENESS'
C0=$(rd 0x40011504)
C1=$(rd 0x40011504)
DELTA=$(( (0x$C1 - 0x$C0) & 0xffffff ))
printf '  RTC1 COUNTER   0x%s -> 0x%s   %d ticks = %d ms elapsed\n' \
	"$C0" "$C1" "$DELTA" $(( DELTA * 1000 / 32768 ))
[ "$DELTA" -gt 0 ] || printf '  WARNING: counter did not advance. The core may be halted.\n'

# ---- the two clocks that cost the most ------------------------------------
#
# HFXO is the expensive one and it is not free-running by design: the radio
# requests it per event and releases it. Finding it up on every sample means
# something holds it CONTINUOUSLY, which on this board is the 802.15.4 receiver.
hdr 'CLOCKS'
HF=$(rd 0x4000040c)
LF=$(rd 0x40000418)
LFSRC=$(rd 0x40000518)
case $(( 0x$HF & 1 )) in 0) HFS='RC (64 MHz internal)' ;; *) HFS='XTAL (HFXO)' ;; esac
case $(( 0x$LF & 3 )) in 0) LFS='RC' ;; 1) LFS='XTAL (LFXO)' ;; *) LFS='Synth' ;; esac
printf '  HFCLKSTAT      0x%s   src=%-22s running=%s\n' "$HF" "$HFS" "$(bit "$HF" 16)"
printf '  LFCLKSTAT      0x%s   src=%-22s running=%s\n' "$LF" "$LFS" "$(bit "$LF" 16)"
printf '  LFCLKSRC       0x%s   (requested source; must agree with LFCLKSTAT)\n' "$LFSRC"

# ---- the regulator --------------------------------------------------------
#
# DCDCEN is REG1, the one that matters on a 3.3 V module. DCDCEN0 is REG0, the
# high-voltage stage, and reads 0 when VDDH is tied to VDD so REG0 is bypassed:
# that is not a finding, it is the topology.
hdr 'REGULATOR'
DC=$(rd 0x40000578)
DC0=$(rd 0x40000580)
printf '  POWER.DCDCEN   0x%s   REG1 DC/DC %s\n' "$DC" \
	"$( [ "$DC" = "00000001" ] && echo 'ENABLED (optimal)' || echo 'OFF -- LDO mode, ~2x the core current' )"
printf '  POWER.DCDCEN0  0x%s   REG0 (high voltage) %s\n' "$DC0" \
	"$( [ "$DC0" = "00000001" ] && echo 'enabled' || echo 'off / bypassed' )"

# ---- peripherals nobody is using ------------------------------------------
#
# An enabled peripheral with no traffic still draws, and on nRF52 a legacy UART
# is the classic offender because it also pins HFCLK. ENABLE encodings differ
# per peripheral, which is why each line decodes its own.
hdr 'PERIPHERAL ENABLE  (enabled + unused = a leak)'
enable_line() {
	v=$(rd "$2")
	printf '  %-14s 0x%s   %s\n' "$1" "$v" "$3$(( 0x$v ))"
}
U0=$(rd 0x40002500)
case $(( 0x$U0 )) in
	0) U0S='disabled' ;;
	4) U0S='ENABLED as legacy UART -- draws even idle, and pins HFCLK' ;;
	8) U0S='ENABLED as UARTE' ;;
	*) U0S='unknown encoding' ;;
esac
printf '  %-14s 0x%s   %s\n' 'UART0/UARTE0' "$U0" "$U0S"
T0=$(rd 0x40003500)
case $(( 0x$T0 )) in
	0) T0S='disabled' ;;
	1) T0S='SPI' ;; 5) T0S='TWI' ;; 6) T0S='TWIM (I2C, EasyDMA)' ;;
	7) T0S='SPIM' ;; 8) T0S='SPIS' ;; 9) T0S='TWIS' ;;
	*) T0S='unknown encoding' ;;
esac
printf '  %-14s 0x%s   %s\n' 'TWI/SPI0' "$T0" "$T0S"
enable_line 'UARTE1'  0x40028500 'raw='
enable_line 'TWI/SPI1' 0x40004500 'raw='
enable_line 'SPIM2'   0x40023500 'raw='
enable_line 'SPIM3'   0x4002f500 'raw='
printf '                                (SPIM3 is the DW3110 bus; 0 between transfers is correct)\n'
enable_line 'USBD'    0x40027500 'raw='
enable_line 'SAADC'   0x40007500 'raw='

# ---- what the 2.4 GHz radio is doing --------------------------------------
#
# MODE says which stack owns the radio at this instant (MPSL switches it), and
# STATE says whether it is receiving. Sampling both is the point: a BLE
# advertiser shows RX on a few percent of samples, a Thread MED with
# rx-on-when-idle shows RX on all of them, and the difference is milliamps.
hdr 'RADIO'
MODE=$(rd 0x40001510)
FREQ=$(rd 0x40001508)
TXP=$(rd 0x4000150c)
case $(( 0x$MODE )) in
	0) MS='Nrf_1Mbit' ;; 1) MS='Nrf_2Mbit' ;;
	3) MS='Ble_1Mbit' ;; 4) MS='Ble_2Mbit' ;;
	5) MS='Ble_LR125Kbit' ;; 6) MS='Ble_LR500Kbit' ;;
	15) MS='Ieee802154_250Kbit' ;;
	*) MS='unknown' ;;
esac
# TXPOWER is a signed 8-bit dBm value in the low byte.
TXD=$(( 0x$TXP & 0xff )); [ "$TXD" -gt 127 ] && TXD=$(( TXD - 256 ))
printf '  MODE           0x%s   %s\n' "$MODE" "$MS"
printf '  FREQUENCY      0x%s   %d MHz\n' "$FREQ" $(( 2400 + (0x$FREQ & 0x7f) ))
printf '  TXPOWER        0x%s   %d dBm\n' "$TXP" "$TXD"

hdr "SAMPLING  ($SAMPLES samples of RADIO.STATE and HFCLKSTAT)"
rx=0; hf=0; i=0
while [ "$i" -lt "$SAMPLES" ]; do
	s=$(rd 0x40001550)
	h=$(rd 0x4000040c)
	# STATE 3 is RX. 2 is RXIDLE (ramped up, not receiving), 0 is DISABLED.
	[ "$(( 0x$s ))" -eq 3 ] && rx=$(( rx + 1 ))
	[ "$(bit "$h" 16)" -eq 1 ] && hf=$(( hf + 1 ))
	i=$(( i + 1 ))
done
printf '  RADIO in RX    %d/%d samples  = %d%% duty\n' "$rx" "$SAMPLES" $(( rx * 100 / SAMPLES ))
printf '  HFXO running   %d/%d samples  = %d%% duty\n' "$hf" "$SAMPLES" $(( hf * 100 / SAMPLES ))

# ---- what is flashed ------------------------------------------------------
#
# A baseline that does not say which image produced it is worthless six weeks
# later. The MCUboot header at the pad before the app slot carries both.
hdr 'IMAGE  (MCUboot header at 0x0a000)'
MAGIC=$(rd 0x0000a000)
SIZE=$(rd 0x0000a00c)
VER=$(rd 0x0000a014)
if [ "$MAGIC" = "96f3b83d" ]; then
	printf '  image size     %d B\n' $(( 0x$SIZE ))
	printf '  version        %d.%d.%d\n' \
		$(( 0x$VER & 0xff )) $(( (0x$VER >> 8) & 0xff )) $(( (0x$VER >> 16) & 0xffff ))
else
	printf '  no MCUboot magic at 0x0a000 (got 0x%s)\n' "$MAGIC"
fi

printf '\nNothing above is a current measurement. See docs/power-baseline.md for the\n'
printf 'PPK2 procedure that turns the ranking into milliamps.\n'
