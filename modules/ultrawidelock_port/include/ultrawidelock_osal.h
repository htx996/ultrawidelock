/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_osal.h - deferred work, semaphores and threads for the shared modules.
 *
 * The second half of the platform contract (ultrawidelock_port.h is the first): exactly
 * the primitives the converted call sites use, nothing speculative. Work and
 * delayable work carry k_work_delayable semantics VERBATIM -- schedule is a
 * no-op while pending (the earlier deadline stands), reschedule restarts the
 * delay -- because uwb_rxdiag's windowing depends on that distinction. There
 * is deliberately no timer API: every periodic need at a real call site is
 * delayable work.
 *
 * Context rules, unenforced: ultrawidelock_work_submit and ultrawidelock_sem_give are ISR-safe on
 * Zephyr only. On ESP-IDF call them from task context -- the DW3000 port
 * already defers its IRQ to a task (ports/esp32/.../dw3000_hw.c), and every
 * converted module runs behind that deferral.
 *
 * Backends: Zephyr maps 1:1 onto k_work/k_sem/k_thread (osal_zephyr.c holds
 * the handler trampolines). ESP-IDF and standalone FreeRTOS each run one
 * dispatch task over a FreeRTOS queue plus a deadline list. The host backend
 * IS the test double: a FIFO and a virtual clock the suite steps by hand
 * (osal_host.c), so converted modules keep deterministic host tests without a
 * faked <zephyr/kernel.h>.
 */
#ifndef ULTRAWIDELOCK_OSAL_H
#define ULTRAWIDELOCK_OSAL_H

#include <stddef.h>
#include <stdint.h>

struct ultrawidelock_work;
struct ultrawidelock_dwork;
typedef void (*ultrawidelock_work_fn)(struct ultrawidelock_work *work);
typedef void (*ultrawidelock_dwork_fn)(struct ultrawidelock_dwork *dwork);

#define ULTRAWIDELOCK_WAIT_FOREVER (-1)

enum ultrawidelock_thread_prio {
	ULTRAWIDELOCK_THREAD_PRIO_LOW,
	ULTRAWIDELOCK_THREAD_PRIO_NORM, /* the NFC transport thread's level */
	ULTRAWIDELOCK_THREAD_PRIO_HIGH,
};

#if defined(__ZEPHYR__)

#include <zephyr/kernel.h>

struct ultrawidelock_work {
	struct k_work kw;
	ultrawidelock_work_fn fn;
};
struct ultrawidelock_dwork {
	struct k_work_delayable kw;
	ultrawidelock_dwork_fn fn;
};
typedef struct k_sem ultrawidelock_sem_t;
typedef struct {
	struct k_thread thread;
} ultrawidelock_thread_t;
typedef k_thread_stack_t ultrawidelock_thread_stack_t;
#define ULTRAWIDELOCK_THREAD_STACK_DEFINE(name, size) K_THREAD_STACK_DEFINE(name, size)
#define ULTRAWIDELOCK_THREAD_STACK_SIZEOF(name)       K_THREAD_STACK_SIZEOF(name)

/* Application-level init hooks; the fn is int (*)(void), 0 on success. The
 * _PRIO form exists for the one caller (the DFU applier inside MCUboot) whose
 * position inside the APPLICATION level is load-bearing. */
#define ULTRAWIDELOCK_INIT_APPLICATION(fn)                                                         \
	SYS_INIT(fn, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY)
#define ULTRAWIDELOCK_INIT_APPLICATION_PRIO(fn, prio) SYS_INIT(fn, APPLICATION, prio)
static inline int ultrawidelock_osal_init_all(void)
{
	return 0; /* SYS_INIT already ran everything */
}
/**
 * @brief The clock the delayable work queue schedules against.
 *
 * A state machine driven by ultrawidelock_dwork_* must measure elapsed time with the
 * SAME clock its deadlines are set on, or the two disagree and it either fires
 * early forever or never fires at all. On target that is just the system
 * uptime. On host it is the suite's virtual clock, which is the whole reason
 * this is a portable call and not a k_uptime_get() at each site: it is what
 * lets a test move a timeout-driven machine through hours in a millisecond.
 *
 * Monotonic, milliseconds, zero at boot (or at ultrawidelock_osal_host_reset()).
 */
static inline int64_t ultrawidelock_osal_now_ms(void)
{
	return (int64_t)k_uptime_get();
}

#elif defined(ESP_PLATFORM)

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct ultrawidelock_work {
	ultrawidelock_work_fn fn;
	struct ultrawidelock_work *next;
	volatile int pending;
};
struct ultrawidelock_dwork {
	ultrawidelock_dwork_fn fn;
	struct ultrawidelock_dwork *next;
	int64_t deadline_us;
	volatile int pending;
};
typedef struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t h;
} ultrawidelock_sem_t;
typedef struct {
	StaticTask_t tcb;
	TaskHandle_t handle;
} ultrawidelock_thread_t;
typedef StackType_t ultrawidelock_thread_stack_t;
#define ULTRAWIDELOCK_THREAD_STACK_DEFINE(name, size)                                              \
	static StackType_t name[(size) / sizeof(StackType_t)]
#define ULTRAWIDELOCK_THREAD_STACK_SIZEOF(name)       sizeof(name)

#define ULTRAWIDELOCK_INIT_APPLICATION(fn)                                     \
	__attribute__((constructor)) static void ultrawidelock_reg_##fn(void) \
	{                                                            \
		ultrawidelock_osal_init_register(fn);                          \
	}
#define ULTRAWIDELOCK_INIT_APPLICATION_PRIO(fn, prio) ULTRAWIDELOCK_INIT_APPLICATION(fn)
void ultrawidelock_osal_init_register(int (*fn)(void));
int ultrawidelock_osal_init_all(void); /* app_main calls this once, after the OS is up */

/**
 * @brief The clock the delayable work queue schedules against.
 *
 * A state machine driven by ultrawidelock_dwork_* must measure elapsed time with the
 * SAME clock its deadlines are set on, or the two disagree and it either fires
 * early forever or never fires at all. On target that is just the system
 * uptime. On host it is the suite's virtual clock, which is the whole reason
 * this is a portable call and not a k_uptime_get() at each site: it is what
 * lets a test move a timeout-driven machine through hours in a millisecond.
 *
 * Monotonic, milliseconds, zero at boot (or at ultrawidelock_osal_host_reset()).
 */
static inline int64_t ultrawidelock_osal_now_ms(void)
{
	return (int64_t)xTaskGetTickCount() * (int64_t)portTICK_PERIOD_MS;
}

#elif defined(ULTRAWIDELOCK_PORT_FREERTOS)

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#if configSUPPORT_STATIC_ALLOCATION != 1
#error "ULTRAWIDELOCK_PORT_FREERTOS requires configSUPPORT_STATIC_ALLOCATION=1"
#endif
#if configSUPPORT_DYNAMIC_ALLOCATION != 1
#error "ULTRAWIDELOCK_PORT_FREERTOS requires configSUPPORT_DYNAMIC_ALLOCATION=1 for ultrawidelock_malloc"
#endif
#if configUSE_MUTEXES != 1 || configUSE_COUNTING_SEMAPHORES != 1
#error "ULTRAWIDELOCK_PORT_FREERTOS requires mutexes and counting semaphores"
#endif
#if configMAX_PRIORITIES < 4
#error "ULTRAWIDELOCK_PORT_FREERTOS requires at least four task priorities"
#endif

struct ultrawidelock_work {
	ultrawidelock_work_fn fn;
	struct ultrawidelock_work *next;
	volatile int pending;
};
struct ultrawidelock_dwork {
	ultrawidelock_dwork_fn fn;
	struct ultrawidelock_dwork *next;
	int64_t deadline_us;
	volatile int pending;
};
typedef struct {
	StaticSemaphore_t buf;
	SemaphoreHandle_t h;
} ultrawidelock_sem_t;
typedef struct {
	StaticTask_t tcb;
	TaskHandle_t handle;
} ultrawidelock_thread_t;
typedef StackType_t ultrawidelock_thread_stack_t;
#define ULTRAWIDELOCK_THREAD_STACK_DEFINE(name, size)                                              \
	static StackType_t name[((size) + sizeof(StackType_t) - 1) / sizeof(StackType_t)]
#define ULTRAWIDELOCK_THREAD_STACK_SIZEOF(name) sizeof(name)

/* The selected startup must execute the C runtime init array before main. */
#define ULTRAWIDELOCK_INIT_APPLICATION(fn)                                     \
	__attribute__((constructor)) static void ultrawidelock_reg_##fn(void) \
	{                                                            \
		ultrawidelock_osal_init_register(fn);                          \
	}
#define ULTRAWIDELOCK_INIT_APPLICATION_PRIO(fn, prio) ULTRAWIDELOCK_INIT_APPLICATION(fn)
void ultrawidelock_osal_init_register(int (*fn)(void));
int ultrawidelock_osal_init_all(void);

/**
 * @brief The clock the delayable work queue schedules against.
 *
 * A state machine driven by ultrawidelock_dwork_* must measure elapsed time with the
 * SAME clock its deadlines are set on, or the two disagree and it either fires
 * early forever or never fires at all. On target that is just the system
 * uptime. On host it is the suite's virtual clock, which is the whole reason
 * this is a portable call and not a k_uptime_get() at each site: it is what
 * lets a test move a timeout-driven machine through hours in a millisecond.
 *
 * Monotonic, milliseconds, zero at boot (or at ultrawidelock_osal_host_reset()).
 */
static inline int64_t ultrawidelock_osal_now_ms(void)
{
	return (int64_t)xTaskGetTickCount() * (int64_t)portTICK_PERIOD_MS;
}

#elif defined(ULTRAWIDELOCK_PORT_HOST)

struct ultrawidelock_work {
	ultrawidelock_work_fn fn;
	struct ultrawidelock_work *next;
	int pending;
};
struct ultrawidelock_dwork {
	ultrawidelock_dwork_fn fn;
	struct ultrawidelock_dwork *next;
	int64_t deadline_ms;
	int pending;
};
typedef struct {
	unsigned count;
	unsigned limit;
} ultrawidelock_sem_t;
/* Host threads never run: create() records entry and arg, and the suite
 * drives the entry function itself, exactly like the other recording fakes. */
typedef struct {
	void (*entry)(void *arg);
	void *arg;
	const char *name;
	int created;
} ultrawidelock_thread_t;
typedef uint8_t ultrawidelock_thread_stack_t;
#define ULTRAWIDELOCK_THREAD_STACK_DEFINE(name, size) static uint8_t name[(size)]
#define ULTRAWIDELOCK_THREAD_STACK_SIZEOF(name)       sizeof(name)

/* Registers for ultrawidelock_osal_init_all() AND exposes the (usually static) init
 * function as a linkable pointer, so a suite can invoke one hook by itself at
 * a controlled point -- the same reach logfake's SYS_INIT fake used to give. */
#define ULTRAWIDELOCK_INIT_APPLICATION(fn)                                     \
	int (*const ultrawidelock_init_##fn)(void) = (fn);                    \
	__attribute__((constructor)) static void ultrawidelock_reg_##fn(void) \
	{                                                            \
		ultrawidelock_osal_init_register(fn);                          \
	}
#define ULTRAWIDELOCK_INIT_APPLICATION_PRIO(fn, prio) ULTRAWIDELOCK_INIT_APPLICATION(fn)
void ultrawidelock_osal_init_register(int (*fn)(void));
int ultrawidelock_osal_init_all(void);

/* ---- suite controls (host backend only) ---------------------------------
 *
 * The virtual clock starts at 0 and moves only when a suite advances it; it
 * is deliberately independent of ultrawidelock_uptime_ms (real time), so a suite that
 * needs both aligned drives both.
 */

/** @brief Drop all queued work, disarm everything, clock back to 0. */
void ultrawidelock_osal_host_reset(void);

/** @brief Run the immediate work queued right now (not what those handlers
 * queue -- a self-resubmitting handler is stepped one flush at a time).
 * @return how many handlers ran. */
unsigned ultrawidelock_osal_host_flush(void);

/** @brief Advance the virtual clock, running due delayable work in deadline
 * order. Immediate work stays queued for ultrawidelock_osal_host_flush().
 * @return how many handlers ran. */
unsigned ultrawidelock_osal_host_advance_ms(int64_t ms);

/** @brief The virtual clock. */
int64_t ultrawidelock_osal_host_now_ms(void);

/**
 * @brief The clock the delayable work queue schedules against.
 *
 * A state machine driven by ultrawidelock_dwork_* must measure elapsed time with the
 * SAME clock its deadlines are set on, or the two disagree and it either fires
 * early forever or never fires at all. On target that is just the system
 * uptime. On host it is the suite's virtual clock, which is the whole reason
 * this is a portable call and not a k_uptime_get() at each site: it is what
 * lets a test move a timeout-driven machine through hours in a millisecond.
 *
 * Monotonic, milliseconds, zero at boot (or at ultrawidelock_osal_host_reset()).
 */
static inline int64_t ultrawidelock_osal_now_ms(void)
{
	return ultrawidelock_osal_host_now_ms();
}

#else
#error "ultrawidelock_osal.h: no platform backend. Define ULTRAWIDELOCK_PORT_HOST/ULTRAWIDELOCK_PORT_FREERTOS, or build under Zephyr/ESP-IDF."
#endif

/* ---- the contract, identical on every backend --------------------------- */

void ultrawidelock_work_init(struct ultrawidelock_work *work, ultrawidelock_work_fn fn);
/** @brief Queue @p work; a no-op while it is already queued. ISR-safe on
 * Zephyr only. @return 0 queued or already pending, negative on error. */
int ultrawidelock_work_submit(struct ultrawidelock_work *work);

void ultrawidelock_dwork_init(struct ultrawidelock_dwork *dwork, ultrawidelock_dwork_fn fn);
/** @brief Arm @p dwork for @p delay_ms from now; a NO-OP while armed (the
 * earlier deadline stands). 0 means as soon as possible. */
int ultrawidelock_dwork_schedule(struct ultrawidelock_dwork *dwork, int32_t delay_ms);
/** @brief Arm @p dwork for @p delay_ms from now, RESTARTING the delay if it
 * was already armed. */
int ultrawidelock_dwork_reschedule(struct ultrawidelock_dwork *dwork, int32_t delay_ms);
/** @brief Disarm @p dwork if armed; a completed or idle one is left alone. */
int ultrawidelock_dwork_cancel(struct ultrawidelock_dwork *dwork);

/** @brief Counting semaphore. @p limit caps the count (every call site today
 * uses limit 1: a binary "data ready" flag). */
void ultrawidelock_sem_init(ultrawidelock_sem_t *sem, unsigned initial, unsigned limit);
/** @brief Give; saturates at the limit. ISR-safe on Zephyr only. */
void ultrawidelock_sem_give(ultrawidelock_sem_t *sem);
/** @brief Take within @p timeout_ms (ULTRAWIDELOCK_WAIT_FOREVER blocks). On the host
 * backend a timed take advances the virtual clock, running delayable work
 * that falls due -- which is how a "give" arrives in a single-threaded test.
 * @return 0 taken, -1 timed out. */
int ultrawidelock_sem_take(ultrawidelock_sem_t *sem, int32_t timeout_ms);
/** @brief Count back to zero; waiters (Zephyr) fail with -1. */
void ultrawidelock_sem_reset(ultrawidelock_sem_t *sem);

/** @brief Start a static-stacked thread. @p stack_size is
 * ULTRAWIDELOCK_THREAD_STACK_SIZEOF(the ULTRAWIDELOCK_THREAD_STACK_DEFINE'd array).
 * @return 0 on success. */
int ultrawidelock_thread_create(ultrawidelock_thread_t *thread, ultrawidelock_thread_stack_t *stack,
				size_t stack_size, void (*entry)(void *arg), void *arg,
				enum ultrawidelock_thread_prio prio, const char *name);

#endif /* ULTRAWIDELOCK_OSAL_H */
