/* SPDX-License-Identifier: ISC */

/**
 * @file test_kv_zephyr.c — ultrawidelock_kv.h on Zephyr settings, over settingsfake.
 *
 * File under test: ports/zephyr/store/kv_zephyr.c.
 *
 * What is worth proving here is the seam's whole reason to exist: the stored
 * name is derived, fixed-width and short, so the framework's name cap cannot be
 * reached by any key a caller can pass. The fake records real names, so that is
 * checkable rather than assertable.
 *
 * The rest is the contract every backend owes and the host backend already
 * proves in test_ultrawidelock_port.c: refuse an undersized read instead of
 * truncating, report NOT_FOUND honestly, and survive a full store.
 *
 * THEATRE, STATED PLAINLY: settingsfake is RAM. Nothing here says anything
 * about flash wear, power-cut durability, or NVS behaviour on a real part.
 */
#include <stdio.h>
#include <string.h>

#include "test.h"

#include <zephyr/settings/settings.h>

#include "settingsfake.h"
#include "ultrawidelock_kv.h"

void test_kv_zephyr(void)
{
	uint8_t out[64];
	uint8_t big[ULTRAWIDELOCK_KV_VALUE_MAX];
	size_t len;

	memset(big, 0x5a, sizeof(big));

	t_group("kv/zephyr: the name is derived, never spelled");
	settingsfake_reset();
	T_EQ("init ok", ultrawidelock_kv_init(), (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("set ok", ultrawidelock_kv_set(0x4001u, "abcd", 4u), (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("one record stored", settingsfake_key_count(), 1);
	/* Eight characters for every key in the 16-bit space, against a 64-char
	 * cap. This is the assertion the whole seam is for. */
	T_OK("name is uwl/0000-style, 8 chars", settingsfake_has("uwl/4001"));
	T_EQ("lowest key is the same width",
	     ultrawidelock_kv_set(0x0001u, "z", 1u), (long)ULTRAWIDELOCK_KV_OK);
	T_OK("low key zero-padded", settingsfake_has("uwl/0001"));
	T_EQ("highest usable key is the same width",
	     ultrawidelock_kv_set(0xfffeu, "z", 1u), (long)ULTRAWIDELOCK_KV_OK);
	T_OK("high key not truncated", settingsfake_has("uwl/fffe"));

	t_group("kv/zephyr: round trip");
	len = sizeof(out);
	T_EQ("get ok", ultrawidelock_kv_get(0x4001u, out, &len), (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("stored length", (long)len, 4L);
	T_OK("value round-trips", memcmp(out, "abcd", 4) == 0);
	len = sizeof(out);
	T_EQ("absent key", ultrawidelock_kv_get(0x4002u, out, &len),
	     (long)ULTRAWIDELOCK_KV_NOT_FOUND);
	T_EQ("set replaces rather than duplicating",
	     ultrawidelock_kv_set(0x4001u, "ef", 2u), (long)ULTRAWIDELOCK_KV_OK);
	len = sizeof(out);
	(void)ultrawidelock_kv_get(0x4001u, out, &len);
	T_EQ("replacement length", (long)len, 2L);

	t_group("kv/zephyr: undersized read refused, not truncated");
	T_EQ("store 40 bytes", ultrawidelock_kv_set(0x4003u, big, 40u),
	     (long)ULTRAWIDELOCK_KV_OK);
	len = 8u;
	memset(out, 0xee, sizeof(out));
	T_EQ("short buffer -> INVALID", ultrawidelock_kv_get(0x4003u, out, &len),
	     (long)ULTRAWIDELOCK_KV_INVALID);
	T_EQ("stored length handed back", (long)len, 40L);
	T_OK("buffer untouched", out[0] == 0xee);
	len = 0u;
	T_EQ("NULL asks length only", ultrawidelock_kv_get(0x4003u, NULL, &len),
	     (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("length answered", (long)len, 40L);

	t_group("kv/zephyr: guards and failures");
	T_EQ("KEY_NONE refused", ultrawidelock_kv_set(ULTRAWIDELOCK_KV_KEY_NONE, "x", 1u),
	     (long)ULTRAWIDELOCK_KV_INVALID);
	T_EQ("oversize refused",
	     ultrawidelock_kv_set(0x4004u, big, ULTRAWIDELOCK_KV_VALUE_MAX + 1u),
	     (long)ULTRAWIDELOCK_KV_INVALID);
	settingsfake_fail_saves_after(0);
	T_EQ("out of space -> FULL", ultrawidelock_kv_set(0x4005u, "x", 1u),
	     (long)ULTRAWIDELOCK_KV_FULL);
	settingsfake_fail_saves_after(-1);

	t_group("kv/zephyr: delete tells the truth");
	T_EQ("delete of a stored key", ultrawidelock_kv_delete(0x4003u),
	     (long)ULTRAWIDELOCK_KV_OK);
	/* settings_delete() returns 0 for a name that was never there, so a
	 * backend that just forwards it would call every clear a success. */
	T_EQ("delete of an absent key is NOT_FOUND", ultrawidelock_kv_delete(0x4003u),
	     (long)ULTRAWIDELOCK_KV_NOT_FOUND);

	t_group("kv/zephyr: erase_all sweeps what exists");
	settingsfake_reset();
	(void)ultrawidelock_kv_init();
	for (uint16_t k = 0x4000u; k < 0x4028u; k++) {
		(void)ultrawidelock_kv_set(k, "v", 1u);
	}
	/* Deliberately more than one ERASE_BATCH (32), so a single-pass sweep
	 * would leave records behind and fail here. */
	T_EQ("40 records stored", settingsfake_key_count(), 40);
	T_EQ("erase_all ok", ultrawidelock_kv_erase_all(), (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("nothing left", settingsfake_key_count(), 0);

	t_group("kv/zephyr: erase_all leaves other subtrees alone");
	settingsfake_reset();
	(void)ultrawidelock_kv_init();
	T_EQ("a kv record", ultrawidelock_kv_set(0x4000u, "v", 1u), (long)ULTRAWIDELOCK_KV_OK);
	/* Matter's fabric table and OpenThread's settings share this partition;
	 * a factory reset of this store must not take them with it. */
	T_EQ("a foreign record", settings_save_one("mfab/0", "v", 1u), 0L);
	T_EQ("erase_all ok", ultrawidelock_kv_erase_all(), (long)ULTRAWIDELOCK_KV_OK);
	T_EQ("the foreign record survived", settingsfake_key_count(), 1);
	T_OK("and it is the right one", settingsfake_has("mfab/0"));
}
