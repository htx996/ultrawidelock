/*
 * Control surface for the fake settings backend. Tests include this; the code
 * under test only ever sees <zephyr/settings/settings.h>.
 */
#ifndef SETTINGSFAKE_H
#define SETTINGSFAKE_H

#include <stdbool.h>
#include <stddef.h>

/** Empty the store and clear every injected failure. Call before each test. */
void settingsfake_reset(void);

/**
 * Fail the Nth settings_save_one() from now, and every one after it.
 *
 * This makes a torn multi-record commit reproducible on the host: a reset
 * between prerequisite records and the final per-slot authority record is
 * exactly "the first N succeeded".
 *
 * @param n how many saves still succeed. 0 fails the very next one. A negative
 *          value disables the injection.
 */
void settingsfake_fail_saves_after(int n);

/** Fail the Nth stored-value read from now. Negative disables injection. */
void settingsfake_fail_reads_after(int n);

/** How many settings_save_one() calls have succeeded since the last reset. */
int settingsfake_save_count(void);

/** How many settings_delete() calls have been made since the last reset. */
int settingsfake_delete_count(void);

/** True when @p name currently holds a value. */
bool settingsfake_has(const char *name);

/** Flip one stored payload byte without updating its checksum. */
bool settingsfake_corrupt(const char *name);

/** Number of keys currently stored. */
int settingsfake_key_count(void);

#endif /* SETTINGSFAKE_H */
