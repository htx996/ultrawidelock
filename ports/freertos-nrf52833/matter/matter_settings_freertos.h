/* SPDX-License-Identifier: ISC */

/**
 * @file matter_settings_freertos.h — the settings API the shared Matter sources use.
 *
 * THE CANONICAL DECLARATIONS LIVE HERE, in the port's own spelling. The
 * Zephyr-named header beside it in matter_compat/ forwards to this one; it is a
 * translation of a foreign API's file NAME and nothing else.
 *
 * The direction matters, and it is the same one matter_ble_freertos.h already
 * takes. Port code includes this. Only the shared sources -- which live in the
 * Zephyr tree and are compiled unmodified -- reach the Zephyr spelling, and they
 * do it through the shim's include path. That keeps every file under
 * ports/freertos-nrf52833/ free of Zephyr includes, which is a property worth
 * having for its own sake and which the purity guard enforces.
 *
 * The backend is a fixed path table over the port's uint16_t key-value store:
 * see the .c for why it is a table rather than a hash, and why an unknown path
 * is refused rather than reported as "nothing stored".
 */
#ifndef ULTRAWIDELOCK_FREERTOS_MATTER_SETTINGS_H
#define ULTRAWIDELOCK_FREERTOS_MATTER_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/**
 * Hands the stored bytes to the caller. Returns the number read, negative on
 * error, matching Zephyr.
 */
typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

/**
 * Called once per matching record. @p key is the path below the subtree, which
 * is empty here because every path this backend knows is a leaf.
 */
typedef int (*settings_load_direct_cb)(const char *key, size_t len, settings_read_cb read_cb,
				       void *cb_arg, void *param);

int settings_subsys_init(void);
int settings_load_subtree_direct(const char *subtree, settings_load_direct_cb cb, void *param);
int settings_save_one(const char *name, const void *value, size_t val_len);
int settings_delete(const char *name);

/**
 * Registered handler, called once per stored record under its subtree.
 *
 * @param name the path BELOW the subtree, e.g. "f0" for "mf2/f0".
 */
typedef int (*settings_set_cb)(const char *name, size_t len, settings_read_cb read_cb,
			       void *cb_arg);

struct settings_handler_static {
	const char *name;
	settings_set_cb h_set;
};

/*
 * Zephyr puts these in a linker section and walks it. This port has one
 * registrant -- the Matter fabric store -- so the "section" is a single slot,
 * claimed at definition time. A second registrant is refused loudly rather than
 * overwriting the first, because a silently dropped handler means an identity
 * that saves and never loads.
 */
void ultrawidelock_settings_register(const struct settings_handler_static *h);

#define SETTINGS_STATIC_HANDLER_DEFINE(sym, tree, get_fn, set_fn, commit_fn, export_fn)            \
	static const struct settings_handler_static ultrawidelock_settings_##sym = {                         \
		.name = (tree),                                                                    \
		.h_set = (set_fn),                                                                 \
	};                                                                                         \
	__attribute__((constructor)) static void ultrawidelock_settings_reg_##sym(void)                      \
	{                                                                                          \
		ultrawidelock_settings_register(&ultrawidelock_settings_##sym);                                        \
	}

/** Walk every stored record under @p subtree, calling the registered handler. */
int settings_load_subtree(const char *subtree);

/**
 * Zephyr's name matcher: true when @p name's first component equals @p key, with
 * @p next left pointing at whatever follows it (NULL when nothing does).
 */
bool settings_name_steq(const char *name, const char *key, const char **next);

#endif /* ULTRAWIDELOCK_FREERTOS_MATTER_SETTINGS_H */
