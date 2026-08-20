/* SPDX-License-Identifier: ISC */

/*
 * Zephyr's OpenThread integration header, over this port's runtime.
 *
 * The shared Matter Thread transport names four helpers from it. Each has an
 * exact equivalent here, because both builds run the same OpenThread with the
 * same single-lock discipline: the stack is not internally synchronised, and
 * every call into it happens with one mutex held.
 *
 * This is a SEPARATE shim from thread/ot_compat/openthread.h, which is empty on
 * purpose -- the pinned radio platform includes that header and names nothing
 * from it, and a check exists to fail the build if that ever changes. Extending
 * the empty one to serve this file would quietly hand the radio platform a set
 * of functions it is asserted not to use.
 */
#ifndef ULTRAWIDELOCK_MATTER_COMPAT_OPENTHREAD_H
#define ULTRAWIDELOCK_MATTER_COMPAT_OPENTHREAD_H

#include <ultrawidelock_freertos_openthread.h>

#include <openthread/error.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>

/*
 * Zephyr returns the instance without taking the lock, and callers take it
 * separately around the work. Same contract here.
 */
static inline otInstance *openthread_get_default_instance(void)
{
	return ultrawidelock_freertos_openthread_instance();
}

static inline void openthread_mutex_lock(void)
{
	ultrawidelock_freertos_openthread_lock();
}

static inline void openthread_mutex_unlock(void)
{
	ultrawidelock_freertos_openthread_unlock();
}

/* Zephyr multiplexes these records over OpenThread's native callback list.
 * Upstream OpenThread already supports multiple registrations, so the shim can
 * preserve the same lifetime and duplicate-registration semantics directly. */
struct openthread_state_changed_callback {
	otStateChangedCallback otCallback;
	void *user_data;
};

static inline int openthread_state_changed_callback_register(
	struct openthread_state_changed_callback *cb)
{
	otInstance *ot = ultrawidelock_freertos_openthread_instance();
	otError err;

	if (ot == NULL || cb == NULL || cb->otCallback == NULL) {
		return -1;
	}
	ultrawidelock_freertos_openthread_lock();
	err = otSetStateChangedCallback(ot, cb->otCallback, cb->user_data);
	ultrawidelock_freertos_openthread_unlock();
	return err == OT_ERROR_NONE ? 0 : -1;
}

/*
 * openthread_run() BRINGS THE INTERFACE UP. It is not a scheduler hint.
 *
 * Zephyr's version enables IPv6 and then the Thread interface, which is the
 * moment attaching actually begins -- the caller's own comment says so. An
 * early version of this shim mapped it to the port's wake(), which compiles,
 * links, and produces a node that has a dataset, reports no error, and never
 * joins anything. The names are similar and the behaviours are not related.
 *
 * Called with the lock RELEASED, because it takes the lock itself. That is the
 * contract on the Zephyr side and the transport depends on it.
 *
 * Order matters: IPv6 first, then Thread. otThreadSetEnabled on an interface
 * whose IPv6 is still down is refused with OT_ERROR_INVALID_STATE.
 */
static inline int openthread_run(void)
{
	otInstance *ot = ultrawidelock_freertos_openthread_instance();
	otError err;

	if (ot == NULL) {
		return -1;
	}
	ultrawidelock_freertos_openthread_lock();
	err = otIp6SetEnabled(ot, true);
	if (err == OT_ERROR_NONE) {
		err = otThreadSetEnabled(ot, true);
	}
	ultrawidelock_freertos_openthread_unlock();
	if (err != OT_ERROR_NONE) {
		return -1;
	}
	/* Let the task that owns the instance notice there is work now. */
	ultrawidelock_freertos_openthread_wake();
	return 0;
}

#endif /* ULTRAWIDELOCK_MATTER_COMPAT_OPENTHREAD_H */
