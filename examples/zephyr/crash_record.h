/* SPDX-License-Identifier: ISC */

/*
 * crash_record — a fault dump that outlives the fault.
 *
 * The witness dongle's console is USB CDC, which does not exist until the end
 * of a boot that never ends, so a live dump was never going to happen. This is
 * the dead-drop instead: the fault handler (ble-witness/src/boot_trace.c)
 * burns one copy of the machine state into a spare flash page, and a separate
 * radio-free image (crash-reader) that boots the way stock dongle firmware
 * boots reads the page back over USB after the reboot.
 *
 * The page is inside the app partition (0x1000..0xFE000) but far above both
 * images, so DFU reflashes of either image never erase it: nrfutil only
 * touches the pages the incoming image occupies.
 */

#ifndef ULTRAWIDELOCK_CRASH_RECORD_H
#define ULTRAWIDELOCK_CRASH_RECORD_H

#include <stdint.h>

#define CRASH_RECORD_ADDR  0x7F000u
#define CRASH_RECORD_MAGIC 0x434C5755u /* "UWLC" little-endian */

struct crash_record {
	uint32_t magic;
	uint32_t seq;     /* bumped every write, so a stale record is obvious */
	uint32_t reason;  /* k_fatal_error_reason */
	uint32_t cfsr;    /* may already be cleared by Zephyr's fault path */
	uint32_t hfsr;
	uint32_t bfar;
	uint32_t mmfar;
	uint32_t pc;
	uint32_t lr;
	uint32_t xpsr;
	char     thread[16]; /* NUL-padded; "?" if names are compiled out */
};

#endif /* ULTRAWIDELOCK_CRASH_RECORD_H */
