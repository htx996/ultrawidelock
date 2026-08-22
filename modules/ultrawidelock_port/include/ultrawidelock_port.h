/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_port.h
 * Portable platform shim: allocates memory, measures uptime and cycle counts, provides sleep stubs
 * for host tests, and wraps mutexes (no-op on single-threaded host).
 */
/*
 * ultrawidelock_port.h - the platform contract for the UWB engine and the credential reader.
 * This header IS the port specification: a new target is a new branch here plus
 * a DW3000 SPI/GPIO backend, nothing else. Work queues, timers and init hooks
 * are deliberately absent (Zephyr-only diagnostics use them, never the ranging
 * path); the mutex stays a plain blocking lock so every backend is three lines.
 *
 *   ultrawidelock_malloc/ultrawidelock_calloc/ultrawidelock_free  heap
 *   ultrawidelock_uptime_us / ultrawidelock_uptime_ms   monotonic time since boot
 *   ultrawidelock_sleep_ms                    relinquish the CPU for at least ms
 *   ultrawidelock_sleep_us                    short busy-wait, microseconds (deca_sleep)
 *   ultrawidelock_cycle_get_32                free-running counter, RX-arm latency probe
 *   ultrawidelock_mutex_init/lock/unlock      blocking mutex (credential reader trust store)
 *   ultrawidelock_mutex_trylock               the same mutex, for a caller that must not wait
 *   ultrawidelock_atomic_xchg                 read-and-replace a word, across threads
 */
#ifndef ULTRAWIDELOCK_PORT_H
#define ULTRAWIDELOCK_PORT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__ZEPHYR__)

#include <zephyr/kernel.h>

static inline void *ultrawidelock_malloc(size_t size)
{
	return k_malloc(size);
}
static inline void *ultrawidelock_calloc(size_t n, size_t size)
{
	return k_calloc(n, size);
}
static inline void ultrawidelock_free(void *ptr)
{
	k_free(ptr);
}
static inline int64_t ultrawidelock_uptime_us(void)
{
	return (int64_t)k_ticks_to_us_floor64(k_uptime_ticks());
}
static inline int64_t ultrawidelock_uptime_ms(void)
{
	return k_uptime_get();
}
static inline void ultrawidelock_sleep_ms(int32_t ms)
{
	k_msleep(ms);
}
static inline void ultrawidelock_sleep_us(int64_t us)
{
	k_usleep((int32_t)us);
}
static inline uint32_t ultrawidelock_cycle_get_32(void)
{
	return k_cycle_get_32();
}
typedef struct k_mutex ultrawidelock_mutex_t;
static inline void ultrawidelock_mutex_init(ultrawidelock_mutex_t *m)
{
	k_mutex_init(m);
}
static inline void ultrawidelock_mutex_lock(ultrawidelock_mutex_t *m)
{
	k_mutex_lock(m, K_FOREVER);
}
/**
 * Take @p m only if it is free. NEVER blocks.
 *
 * For a caller that cannot wait because something else is already waiting on
 * it: a callback running on a stack's own thread, with that stack's API lock
 * held, where blocking would close a lock-ordering cycle. Such a caller must
 * treat failure as an ordinary outcome and drop the work, not retry in place.
 *
 * @return 0 when the mutex was taken and the caller must unlock it, non-zero
 *         when it was already held and the caller took nothing.
 */
static inline int ultrawidelock_mutex_trylock(ultrawidelock_mutex_t *m)
{
	return k_mutex_lock(m, K_NO_WAIT);
}
typedef atomic_t ultrawidelock_atomic_t;
/**
 * @brief Replace @p *a with @p val and return what it held, indivisibly.
 *
 * The one atomic this codebase needs: a word one thread STORES into and another
 * thread TAKES, where taking must also clear it so the value is consumed once.
 * A plain read-then-write loses the race and the value is handled twice.
 *
 * @return the previous value.
 */
static inline long ultrawidelock_atomic_xchg(ultrawidelock_atomic_t *a, long val)
{
	return (long)atomic_set(a, (atomic_val_t)val);
}
static inline void ultrawidelock_mutex_unlock(ultrawidelock_mutex_t *m)
{
	k_mutex_unlock(m);
}

#elif defined(ESP_PLATFORM)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>

#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static inline void *ultrawidelock_malloc(size_t size)
{
	return malloc(size);
}
static inline void *ultrawidelock_calloc(size_t n, size_t size)
{
	return calloc(n, size);
}
static inline void ultrawidelock_free(void *ptr)
{
	free(ptr);
}
static inline int64_t ultrawidelock_uptime_us(void)
{
	return esp_timer_get_time();
}
static inline int64_t ultrawidelock_uptime_ms(void)
{
	return esp_timer_get_time() / 1000;
}
static inline void ultrawidelock_sleep_ms(int32_t ms)
{
	if (ms > 0) {
		vTaskDelay(pdMS_TO_TICKS(ms));
	}
}
static inline void ultrawidelock_sleep_us(int64_t us)
{
	esp_rom_delay_us((uint32_t)us);
}
static inline uint32_t ultrawidelock_cycle_get_32(void)
{
	return esp_cpu_get_cycle_count();
}
typedef struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t h;
} ultrawidelock_mutex_t;
static inline void ultrawidelock_mutex_init(ultrawidelock_mutex_t *m)
{
	m->h = xSemaphoreCreateMutexStatic(&m->buf);
}
static inline void ultrawidelock_mutex_lock(ultrawidelock_mutex_t *m)
{
	xSemaphoreTake(m->h, portMAX_DELAY);
}
/**
 * Take @p m only if it is free. NEVER blocks.
 *
 * For a caller that cannot wait because something else is already waiting on
 * it: a callback running on a stack's own thread, with that stack's API lock
 * held, where blocking would close a lock-ordering cycle. Such a caller must
 * treat failure as an ordinary outcome and drop the work, not retry in place.
 *
 * @return 0 when the mutex was taken and the caller must unlock it, non-zero
 *         when it was already held and the caller took nothing.
 */
static inline int ultrawidelock_mutex_trylock(ultrawidelock_mutex_t *m)
{
	return xSemaphoreTake(m->h, 0) == pdTRUE ? 0 : -1;
}
static inline void ultrawidelock_mutex_unlock(ultrawidelock_mutex_t *m)
{
	xSemaphoreGive(m->h);
}
typedef long ultrawidelock_atomic_t;
/**
 * @brief Replace @p *a with @p val and return what it held, indivisibly.
 *
 * The one atomic this codebase needs: a word one thread STORES into and another
 * thread TAKES, where taking must also clear it so the value is consumed once.
 * A plain read-then-write loses the race and the value is handled twice.
 *
 * @return the previous value.
 */
static inline long ultrawidelock_atomic_xchg(ultrawidelock_atomic_t *a, long val)
{
	return __atomic_exchange_n(a, val, __ATOMIC_SEQ_CST);
}

#elif defined(ULTRAWIDELOCK_PORT_FREERTOS)

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "ultrawidelock_freertos_platform.h"

static inline void *ultrawidelock_malloc(size_t size)
{
	return pvPortMalloc(size);
}
static inline void *ultrawidelock_calloc(size_t n, size_t size)
{
	void *ptr;

	if (size != 0 && n > SIZE_MAX / size) {
		return NULL;
	}
	ptr = pvPortMalloc(n * size);
	if (ptr != NULL) {
		memset(ptr, 0, n * size);
	}
	return ptr;
}
static inline void ultrawidelock_free(void *ptr)
{
	vPortFree(ptr);
}
static inline int64_t ultrawidelock_uptime_us(void)
{
	return ultrawidelock_freertos_uptime_us();
}
static inline int64_t ultrawidelock_uptime_ms(void)
{
	return ultrawidelock_freertos_uptime_us() / 1000;
}
static inline void ultrawidelock_sleep_ms(int32_t ms)
{
	if (ms > 0) {
		TickType_t ticks = pdMS_TO_TICKS((uint32_t)ms);

		vTaskDelay(ticks == 0 ? 1 : ticks);
	}
}
static inline void ultrawidelock_sleep_us(int64_t us)
{
	if (us > 0) {
		ultrawidelock_freertos_busy_wait_us((uint64_t)us);
	}
}
static inline uint32_t ultrawidelock_cycle_get_32(void)
{
	return ultrawidelock_freertos_cycle_get_32();
}
typedef struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t h;
} ultrawidelock_mutex_t;
static inline void ultrawidelock_mutex_init(ultrawidelock_mutex_t *m)
{
	m->h = xSemaphoreCreateMutexStatic(&m->buf);
}
static inline void ultrawidelock_mutex_lock(ultrawidelock_mutex_t *m)
{
	(void)xSemaphoreTake(m->h, portMAX_DELAY);
}
/**
 * Take @p m only if it is free. NEVER blocks.
 *
 * For a caller that cannot wait because something else is already waiting on
 * it: a callback running on a stack's own thread, with that stack's API lock
 * held, where blocking would close a lock-ordering cycle. Such a caller must
 * treat failure as an ordinary outcome and drop the work, not retry in place.
 *
 * @return 0 when the mutex was taken and the caller must unlock it, non-zero
 *         when it was already held and the caller took nothing.
 */
static inline int ultrawidelock_mutex_trylock(ultrawidelock_mutex_t *m)
{
	return xSemaphoreTake(m->h, 0) == pdTRUE ? 0 : -1;
}
static inline void ultrawidelock_mutex_unlock(ultrawidelock_mutex_t *m)
{
	(void)xSemaphoreGive(m->h);
}
typedef long ultrawidelock_atomic_t;
/**
 * @brief Replace @p *a with @p val and return what it held, indivisibly.
 *
 * The one atomic this codebase needs: a word one thread STORES into and another
 * thread TAKES, where taking must also clear it so the value is consumed once.
 * A plain read-then-write loses the race and the value is handled twice.
 *
 * @return the previous value.
 */
static inline long ultrawidelock_atomic_xchg(ultrawidelock_atomic_t *a, long val)
{
	return __atomic_exchange_n(a, val, __ATOMIC_SEQ_CST);
}

#elif defined(ULTRAWIDELOCK_PORT_HOST)

#include <stdlib.h>
#include <time.h>

/**
 * @brief Allocate size bytes.
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
static inline void *ultrawidelock_malloc(size_t size)
{
	return malloc(size);
}
/**
 * @brief Allocate and zero-initialize n elements of size bytes each.
 * @param n Number of elements.
 * @param size Bytes per element.
 * @return Pointer to allocated and zeroed memory, or NULL on failure.
 */
static inline void *ultrawidelock_calloc(size_t n, size_t size)
{
	return calloc(n, size);
}
/**
 * @brief Deallocate memory.
 * @param ptr Pointer to memory to free (may be NULL).
 */
static inline void ultrawidelock_free(void *ptr)
{
	free(ptr);
}
/**
 * @brief Monotonic microseconds since boot.
 * @return Microseconds elapsed since system start.
 */
static inline int64_t ultrawidelock_uptime_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
/**
 * @brief Monotonic milliseconds since boot.
 * @return Milliseconds elapsed since system start.
 */
static inline int64_t ultrawidelock_uptime_ms(void)
{
	return ultrawidelock_uptime_us() / 1000;
}
/**
 * @brief Sleep for a given number of milliseconds (host-test stub).
 * @param ms milliseconds to sleep; ignored in deterministic host tests.
 */
static inline void ultrawidelock_sleep_ms(int32_t ms)
{
	(void)ms; /* host tests are deterministic; nothing to wait for */
}
/**
 * @brief Sleep for a given number of microseconds (host-test stub).
 * @param us microseconds to sleep; ignored in deterministic host tests.
 */
static inline void ultrawidelock_sleep_us(int64_t us)
{
	(void)us;
}
/**
 * @brief Retrieve a 32-bit cycle counter with microsecond resolution.
 * @return current uptime in microseconds, cast to uint32_t.
 */
static inline uint32_t ultrawidelock_cycle_get_32(void)
{
	return (uint32_t)ultrawidelock_uptime_us(); /* us resolution is plenty for the probe */
}
/**
 * @brief Mutex for host tests: a held-depth counter, never a blocking object.
 *
 * The suite is single-threaded, so nothing can ever wait and no lock is needed
 * for what a mutex is normally for. The count is kept anyway, because
 * ultrawidelock_mutex_trylock() has to be able to FAIL: the branch a caller takes when
 * it cannot get the lock is real code, it runs on target whenever two threads
 * meet, and a stub that always succeeds is a stub that hides it. A test reaches
 * that branch by taking the mutex itself and then calling in.
 */
typedef int ultrawidelock_mutex_t;
/**
 * @brief Initialize a mutex (host): unheld.
 * @param m pointer to mutex to initialize.
 */
static inline void ultrawidelock_mutex_init(ultrawidelock_mutex_t *m)
{
	*m = 0;
}
/**
 * @brief Acquire a mutex (host). Cannot block: records the depth and returns.
 * @param m pointer to mutex to lock.
 */
static inline void ultrawidelock_mutex_lock(ultrawidelock_mutex_t *m)
{
	*m += 1;
}
/**
 * @brief Take a mutex only if free (host).
 *
 * NOT recursive, and deliberately unlike Zephyr's k_mutex, which grants a
 * K_NO_WAIT take to the thread already holding it. The divergence is only
 * reachable by a single thread re-entering its own critical section, which is a
 * bug wherever it happens, so failing here surfaces it instead of hiding it.
 *
 * @param m pointer to mutex to try.
 * @return 0 when taken, non-zero when already held.
 */
static inline int ultrawidelock_mutex_trylock(ultrawidelock_mutex_t *m)
{
	if (*m != 0) {
		return -1;
	}
	*m = 1;
	return 0;
}
/**
 * @brief Release a mutex (host).
 * @param m pointer to mutex to unlock.
 */
static inline void ultrawidelock_mutex_unlock(ultrawidelock_mutex_t *m)
{
	if (*m > 0) {
		*m -= 1;
	}
}
typedef long ultrawidelock_atomic_t;
/**
 * @brief Replace @p *a with @p val and return what it held, indivisibly.
 *
 * The one atomic this codebase needs: a word one thread STORES into and another
 * thread TAKES, where taking must also clear it so the value is consumed once.
 * A plain read-then-write loses the race and the value is handled twice.
 *
 * @return the previous value.
 */
static inline long ultrawidelock_atomic_xchg(ultrawidelock_atomic_t *a, long val)
{
	return __atomic_exchange_n(a, val, __ATOMIC_SEQ_CST);
}

#else
#error "ultrawidelock_port.h: no platform backend. Define ULTRAWIDELOCK_PORT_HOST/ULTRAWIDELOCK_PORT_FREERTOS, or build under Zephyr/ESP-IDF."
#endif

#endif /* ULTRAWIDELOCK_PORT_H */
