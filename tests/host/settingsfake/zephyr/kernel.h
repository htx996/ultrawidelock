#ifndef SETTINGSFAKE_ZEPHYR_KERNEL_H
#define SETTINGSFAKE_ZEPHYR_KERNEL_H

/* Minimal mutex surface used by the real provisioning backend. Host port tests
 * are single-threaded; target builds exercise Zephyr's actual blocking mutex. */
struct k_mutex {
	int unused;
};

#define K_FOREVER 0
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
