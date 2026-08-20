/* SPDX-License-Identifier: ISC */

/*
 * Boot trace for a board with no probe and no console.
 *
 * RED, GREEN, BLUE. NOTHING ELSE, AND NOTHING TO COUNT.
 *
 * Every earlier revision of this file asked the reader for something they
 * could get wrong: a number of flashes (7 came back as 4, 8 came back as 7),
 * a hue on a mixed channel ("bluish magenta, cant be sure", "maybe its not
 * white im not sure"), or a 14-symbol transcription that took longer to read
 * than the fault took to happen. All of it was the instrument's fault, not the
 * reader's.
 *
 * What is left is three primary colours and one binary distinction:
 *
 *   SOLID     the board is resting here
 *   BLINKING  same colour, second meaning
 *
 * Six states, read at a glance, no timing and no arithmetic. Where a question
 * needs more than six answers, the firmware narrows it first and the next
 * build asks a different six -- the decoding belongs on this side, not on the
 * bench.
 *
 * BOOT STAMP: BLUE, RED, GREEN, once, at the top. It identifies the image, so
 * a board still running yesterday's firmware cannot be mistaken for one that
 * fails the same way. Change the ORDER on every bench image.
 *
 * PROGRESS, if the boot gets that far:
 *
 *   dark            fault before POST_KERNEL P_93
 *   solid RED       past P_93   usbd_core, net_core, openthread, temperature
 *   solid GREEN     into APPLICATION
 *   solid BLUE      past APPLICATION P_91, about to enter main()
 *   blinking        main() is running; it blinks faster as it gets further
 *
 * FAULT: the handler decodes the fault itself and shows the answer. See the
 * table at k_sys_fatal_error_handler below.
 *
 * BENCH ONLY. CONFIG_WITNESS_BOOT_TRACE defaults n and this file compiles to
 * nothing without it.
 */

#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <cmsis_core.h>
#include <hal/nrf_gpio.h>

#include <string.h>

#include "../../crash_record.h"

/* LD2's three channels, from nrf52840dongle_nrf52840_common.dtsi. Absolute pin
 * numbering: P1.x is 32 + x. ACTIVE_LOW, so clearing a pin lights it. */
#define LD2_RED   8U
#define LD2_GREEN (32U + 9U)
#define LD2_BLUE  12U

#define C_OFF   0u
#define C_RED   1u
#define C_GREEN 2u
#define C_BLUE  4u

/* Delays are spun on the CPU, NOT taken from the kernel timer: MEASURED
 * 2026-08-20, the clock is one of the things that stops, and an instrument
 * that depends on what it measures measures nothing. Roughly seven cycles per
 * volatile iteration at 64 MHz; being off by a factor of two costs nothing,
 * because nothing in the reading is a duration. */
#define SPIN_PER_MS 9000u

static void lamp_rgb(unsigned int c)
{
	nrf_gpio_cfg_output(LD2_RED);
	nrf_gpio_cfg_output(LD2_GREEN);
	nrf_gpio_cfg_output(LD2_BLUE);

	if ((c & C_RED) != 0u) {
		nrf_gpio_pin_clear(LD2_RED);
	} else {
		nrf_gpio_pin_set(LD2_RED);
	}
	if ((c & C_GREEN) != 0u) {
		nrf_gpio_pin_clear(LD2_GREEN);
	} else {
		nrf_gpio_pin_set(LD2_GREEN);
	}
	if ((c & C_BLUE) != 0u) {
		nrf_gpio_pin_clear(LD2_BLUE);
	} else {
		nrf_gpio_pin_set(LD2_BLUE);
	}
}

static void spin_ms(uint32_t ms)
{
	volatile uint32_t i;

	for (i = 0; i < ms * SPIN_PER_MS; i++) {
		__asm__ volatile("nop");
	}
}

static volatile unsigned int s_last_colour = C_OFF;

static void latch(unsigned int c)
{
	s_last_colour = c;
	lamp_rgb(c);
	spin_ms(600);
}

/* MAKE THE BUS FAULT PRECISE.
 *
 * MEASURED 2026-08-20: the fault reason was 26, K_ERR_ARM_BUS_IMPRECISE_DATA_BUS.
 * Imprecise means the offending store was posted to the write buffer and the
 * core moved on, so the fault surfaces wherever the buffer happens to drain --
 * which is why every frame pointed at the idle thread in WFI, and why the stop
 * point moved between builds. Neither was ever saying where the bad access was.
 *
 * ACTLR.DISDEFWBUF disables the write buffer. Stores then complete in order and
 * a failing one faults AT the instruction, which turned reason 26 into 25
 * (BUS_PRECISE_DATA_BUS) and made BFAR meaningful. It costs write throughput,
 * which does not matter in a build whose only job is to survive its own boot.
 */
static int precise_bus_faults(void)
{
	SCnSCB->ACTLR |= SCnSCB_ACTLR_DISDEFWBUF_Msk;
	__DSB();
	__ISB();
	return 0;
}

/* Three colours, in an order that changes every bench image. */
static int mark_frame(void)
{
	lamp_rgb(C_OFF);
	spin_ms(400);
	lamp_rgb(C_BLUE);
	spin_ms(700);
	lamp_rgb(C_OFF);
	spin_ms(400);
	lamp_rgb(C_RED);
	spin_ms(700);
	lamp_rgb(C_OFF);
	spin_ms(400);
	lamp_rgb(C_GREEN);
	spin_ms(700);
	lamp_rgb(C_OFF);
	spin_ms(700);
	return 0;
}

static int mark_post_kernel(void)
{
	latch(C_RED);
	return 0;
}

static int mark_application(void)
{
	latch(C_GREEN);
	return 0;
}

static int mark_application_end(void)
{
	latch(C_BLUE);
	return 0;
}

/* THE FAULT ANSWERS ITSELF, IN TWO FIELDS.
 *
 * MEASURED 2026-08-20: the single-colour version fell through to its catch-all,
 * which said only "not a precise bus fault reaching a peripheral" -- true, and
 * useless. Collapsing two independent facts into one lamp threw away the half
 * that mattered. So they are shown one after the other, each solid, with a
 * clear gap: WHAT the fault was, then WHERE it was reaching.
 *
 * Still red, green, blue, still solid or blinking, still nothing to count.
 */
static void show(unsigned int colour, bool blink, uint32_t hold_ms)
{
	uint32_t t;

	if (!blink) {
		lamp_rgb(colour);
		spin_ms(hold_ms);
		return;
	}
	for (t = 0; t < hold_ms; t += 800u) {
		lamp_rgb(colour);
		spin_ms(400);
		lamp_rgb(C_OFF);
		spin_ms(400);
	}
}

/* THE DEAD DROP. See crash_record.h for why a live dump is impossible here.
 *
 * Raw NVMC, not the flash driver: this runs in a fault handler where no
 * driver, mutex or thread can be trusted, and the NVMC is four registers.
 * The CPU stalls on flash fetches during the erase (~85 ms); the handler
 * runs from flash, so the stall just makes this function slow, which is
 * free in a function whose successor is an infinite loop.
 */
static void crash_record_burn(unsigned int reason, const struct arch_esf *esf)
{
	struct crash_record rec;
	const volatile uint32_t *old = (const volatile uint32_t *)CRASH_RECORD_ADDR;
	volatile uint32_t *dst = (volatile uint32_t *)CRASH_RECORD_ADDR;
	const uint32_t *src = (const uint32_t *)&rec;
	const char *name = k_thread_name_get(k_current_get());

	memset(&rec, 0, sizeof(rec));
	rec.magic = CRASH_RECORD_MAGIC;
	rec.seq = (old[0] == CRASH_RECORD_MAGIC) ? old[1] + 1u : 1u;
	rec.reason = reason;
	rec.cfsr = SCB->CFSR;
	rec.hfsr = SCB->HFSR;
	rec.bfar = (uint32_t)SCB->BFAR;
	rec.mmfar = (uint32_t)SCB->MMFAR;
	if (esf != NULL) {
		rec.pc = esf->basic.pc;
		rec.lr = esf->basic.lr;
		rec.xpsr = esf->basic.xpsr;
	}
	strncpy(rec.thread, (name != NULL) ? name : "?", sizeof(rec.thread) - 1u);

	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
	__DSB();
	NRF_NVMC->ERASEPAGE = CRASH_RECORD_ADDR;
	while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
	__DSB();
	for (size_t i = 0; i < sizeof(rec) / 4u; i++) {
		dst[i] = src[i];
		while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
		}
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
	__DSB();
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	uint32_t bfar = (uint32_t)SCB->BFAR;
	unsigned int what_c, where_c;
	bool what_b, where_b;

	crash_record_burn(reason, esf);

	/* FIELD 1 -- WHAT BROKE
	 *   solid RED      CPU exception / hard fault, no finer code
	 *   solid GREEN    precise data bus fault   (BFAR is trustworthy)
	 *   solid BLUE     imprecise data bus fault (BFAR means nothing)
	 *   blinking RED   stack check failed
	 *   blinking GREEN kernel panic or oops
	 *   blinking BLUE  something else again
	 */
	switch (reason) {
	case K_ERR_CPU_EXCEPTION:
		what_c = C_RED; what_b = false; break;
	case K_ERR_ARM_BUS_PRECISE_DATA_BUS:
		what_c = C_GREEN; what_b = false; break;
	case K_ERR_ARM_BUS_IMPRECISE_DATA_BUS:
		what_c = C_BLUE; what_b = false; break;
	case K_ERR_STACK_CHK_FAIL:
		what_c = C_RED; what_b = true; break;
	case K_ERR_KERNEL_PANIC:
	case K_ERR_KERNEL_OOPS:
		what_c = C_GREEN; what_b = true; break;
	default:
		what_c = C_BLUE; what_b = true; break;
	}

	/* FIELD 2 -- WHERE IT WAS REACHING, from BFAR
	 *   solid RED       a peripheral   0x4xxxxxxx
	 *   solid GREEN     RAM            0x2xxxxxxx
	 *   solid BLUE      flash / code   0x0xxxxxxx, nonzero
	 *   blinking RED    the CryptoCell 0x5002Axxx / 0x5002Bxxx
	 *   blinking GREEN  address zero   a write through a NULL pointer
	 *   blinking BLUE   anywhere else
	 *
	 * The previous decoder folded the last three into one lamp, and the
	 * 2026-08-20 reading ("neither a peripheral nor RAM") is that lamp, not
	 * a location. Two of the three deserve names. An exhaustive scan of the
	 * ELF's address literals (2026-08-20) shows exactly one region this
	 * image can touch whose accessibility is conditional: the nRF52840's
	 * CC310 at 0x5002B000 behind its wrapper at 0x5002A000, which BUS
	 * FAULTS whenever the wrapper's ENABLE is off -- the nrf_cc3xx library
	 * powers it down between operations. GPIO at 0x500003xx and the ARM
	 * system space at 0xE000Exxx are always accessible. The DWM3001CDK's
	 * nRF52833 has no CryptoCell, which is why the lock never sees this.
	 */
	if (bfar >= 0x5002A000u && bfar < 0x5002C000u) {
		where_c = C_RED; where_b = true;
	} else if (bfar == 0u) {
		where_c = C_GREEN; where_b = true;
	} else {
		switch (bfar >> 28) {
		case 0x4u:
			where_c = C_RED; where_b = false; break;
		case 0x2u:
			where_c = C_GREEN; where_b = false; break;
		case 0x0u:
			where_c = C_BLUE; where_b = false; break;
		default:
			where_c = C_BLUE; where_b = true; break;
		}
	}

	for (;;) {
		show(what_c, what_b, 4000);
		lamp_rgb(C_OFF);
		spin_ms(2000);
		show(where_c, where_b, 4000);
		lamp_rgb(C_OFF);
		spin_ms(3500);
	}
}

/* main()'s own ladder blinks, and blinks faster the further it gets, so it can
 * never be confused with a resting colour or with the fault display. */
static void pulse(unsigned int c, uint32_t on_ms)
{
	unsigned int i;

	s_last_colour = c;
	for (i = 0; i < 6u; i++) {
		lamp_rgb(c);
		spin_ms(on_ms);
		lamp_rgb(C_OFF);
		spin_ms(on_ms);
	}
}

void witness_boot_trace_main(void)
{
	pulse(C_GREEN, 500);
}

void witness_boot_trace_phase(unsigned int n)
{
	pulse((n >= 4u) ? C_BLUE : C_GREEN, (n >= 4u) ? 150 : 300);
}

SYS_INIT(precise_bus_faults, PRE_KERNEL_1, 0);
SYS_INIT(mark_frame, POST_KERNEL, 51);
SYS_INIT(mark_post_kernel, POST_KERNEL, 93);
SYS_INIT(mark_application, APPLICATION, 0);
SYS_INIT(mark_application_end, APPLICATION, 91);
