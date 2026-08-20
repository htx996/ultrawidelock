/*
 * Fake <zephyr/settings/settings.h> for the host suite.
 *
 * Signatures match Zephyr's exactly so the port's settings sources compile
 * UNMODIFIED against it -- the point of a fake here is to test the real source,
 * not a host-shaped copy of it. Backed by settingsfake.c, which is an in-RAM
 * key/value store with per-call failure injection, mirroring the pattern
 * tests/ports/esp32/sdkfake/fake_nvs.c already uses for the ESP port.
 */
#ifndef SETTINGSFAKE_ZEPHYR_SETTINGS_H
#define SETTINGSFAKE_ZEPHYR_SETTINGS_H

#include <stddef.h>
#include <sys/types.h>

/*
 * The port's settings sources return -EINVAL and -ENOSPC without including
 * errno.h, because on target Zephyr's own header chain has already provided it
 * by the time this header is reached. Reproduced here so the real source needs
 * no host-only include.
 */
#include <errno.h>

/* The production header chain provides this Kconfig helper. This test defines
 * every flag consumed by the real settings sources to 0 or 1. */
#ifndef IS_ENABLED
#define IS_ENABLED(config_flag) (config_flag)
#endif

/** Zephyr hands a loader this to pull the stored value out. */
typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

struct settings_handler_static {
	const char *name;
	int (*h_set)(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg);
};

void settingsfake_register(const struct settings_handler_static *h);

/*
 * Registered by a constructor rather than by a linker section, because the real
 * macro places the struct in one and the host link has no such section. The
 * effect the source cares about is identical: by the time any test runs, the
 * handler is findable by subtree name.
 */
#define SETTINGS_STATIC_HANDLER_DEFINE(_hname, _tree, _get, _set, _commit, _export)                 \
	static const struct settings_handler_static settingsfake_h_##_hname = {                    \
		.name = (_tree),                                                                   \
		.h_set = (_set),                                                                   \
	};                                                                                         \
	__attribute__((constructor)) static void settingsfake_reg_##_hname(void)                   \
	{                                                                                          \
		settingsfake_register(&settingsfake_h_##_hname);                                   \
	}

/**
 * Called once per matching record. @p key is the path below the subtree, which
 * is the empty string when the subtree names a leaf outright.
 */
typedef int (*settings_load_direct_cb)(const char *key, size_t len, settings_read_cb read_cb,
				       void *cb_arg, void *param);

int settings_subsys_init(void);
int settings_save_one(const char *name, const void *value, size_t val_len);
int settings_delete(const char *name);
int settings_load_subtree(const char *subtree);
int settings_load_subtree_direct(const char *subtree, settings_load_direct_cb cb, void *param);
int settings_name_steq(const char *name, const char *key, const char **next);

#endif /* SETTINGSFAKE_ZEPHYR_SETTINGS_H */
