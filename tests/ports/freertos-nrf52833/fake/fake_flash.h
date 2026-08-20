/*
 * A model of the nRF52833 internal flash that enforces the part's rules rather
 * than merely storing bytes. A store written against a model that lets you
 * overwrite a word, or write a byte at an odd address, works on the model and
 * bricks the board.
 *
 * Enforced: an erased byte reads 0xff; a write may only clear bits, never set
 * one; writes are word-aligned and a whole number of words; an erase covers
 * whole pages. Every violation is recorded and returns failure.
 */
#ifndef TEST_FAKE_FLASH_H
#define TEST_FAKE_FLASH_H

#include <stddef.h>
#include <stdint.h>

/* Four physical pages form the store's two 8 KB logical pages. */
#define FAKE_FLASH_BASE 0x7c000u
#define FAKE_FLASH_PAGES 4u
#define FAKE_FLASH_SIZE (FAKE_FLASH_PAGES * 4096u)

/*
 * The array lives in a shared mapping so a forked scenario's writes survive
 * into the next one. That is what makes a reboot testable: the second process
 * mounts the store with its own fresh statics and the first one's flash.
 */
extern uint8_t *fake_flash;

/* Rule violations, which a test asserts stay at zero. */
extern unsigned fake_flash_violations;
extern unsigned fake_flash_write_calls;
extern unsigned fake_flash_erase_calls;

/*
 * Fail the next N flash writes, to stand in for a power loss partway through a
 * record. Zero disables it.
 */
extern unsigned fake_flash_fail_write_after;

void fake_flash_reset(void);
/* Erase both pages without going through the driver, as a fresh part reads. */
void fake_flash_blank(void);

#endif /* TEST_FAKE_FLASH_H */
