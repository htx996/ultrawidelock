#ifndef SETTINGSFAKE_ZEPHYR_KERNEL_H
#define SETTINGSFAKE_ZEPHYR_KERNEL_H

/* Minimal mutex surface used by the real provisioning backend. Host port tests
 * are single-threaded; target builds exercise Zephyr's actual blocking mutex. */
struct k_mutex {
	int unused;
};

#define K_FOREVER 0

/*
 * Real <zephyr/kernel.h> reaches sys/util.h and so hands every file that
 * includes it this Kconfig probe. The fake owes it the same, because a source
 * that stopped including <zephyr/settings/settings.h> -- as the provisioning
 * backend did when it moved onto the key-value seam -- would otherwise fail to
 * compile here for a reason the target build does not have. Same probe as
 * logfake's: 1 only when the flag is defined to 1, not merely defined.
 */
#ifndef IS_ENABLED
#define SETTINGSFAKE_IE3(ignore_this, val, ...) val
/* The token-paste probe cannot parenthesize its argument. */
/* NOLINTNEXTLINE(bugprone-macro-parentheses) */
#define SETTINGSFAKE_IE2(one_or_two_args)       SETTINGSFAKE_IE3(one_or_two_args 1, 0)
#define _XXXX1                                  SETTINGSFAKE_YYYY,
#define SETTINGSFAKE_IE1(config_macro)          SETTINGSFAKE_IE2(_XXXX##config_macro)
#define IS_ENABLED(config_macro)                SETTINGSFAKE_IE1(config_macro)
#endif
#define K_MUTEX_DEFINE(name) struct k_mutex name = {0}

static inline int k_mutex_lock(struct k_mutex *mutex, int timeout)
{
	(void)mutex;
	(void)timeout;
	return 0;
}

static inline int k_mutex_unlock(struct k_mutex *mutex)
{
	(void)mutex;
	return 0;
}

#endif /* SETTINGSFAKE_ZEPHYR_KERNEL_H */
