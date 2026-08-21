/* SPDX-License-Identifier: ISC */

/*
 * DWM3001CDK provisioning console.
 *
 * This board cannot be commissioned into Apple Home on its own (no Matter image
 * fits an nRF52833), so it adopts a credential exported from a board that was.
 * The point of this file is WHERE that credential lives: in NVS, as per-device
 * data typed in over USB, never in the firmware image. One image is then the
 * same for everybody, and carries no key.
 *
 * Reachable only in provisioning mode -- hold SW2 through reset -- where main()
 * brings up USB CDC-ACM and deliberately never starts the radios. Everything
 * here therefore reads and writes the settings store directly (ultrawidelock_prov_*)
 * rather than the running engine's state, which in this mode does not exist.
 * The one exception is `import`, which routes through ultrawidelock_reader_import_blob
 * so the engine's own commit path stays the single place that adopts identity.
 *
 * It carries the BLE witness link keys for the same reason, though they belong
 * to a different subsystem: they are per-install secrets that must not be in an
 * image, and the image that uses them has no console. See cmd_witkey.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "ultrawidelock_prov.h"
#include "witness_link.h"
#include <ultrawidelock/reader.h>

/*
 * The SECOND ANCHOR's link key. Separate command and separate record from
 * `witkey`: the anchor is a UWB responder, not a BLE witness, and the two
 * device classes must not share an enrolment path -- the witnesses are retired
 * and nothing new should be enrolled as one.
 */
static int cmd_anckey(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t key[WITNESS_LINK_KEY_LEN];
	size_t hex_len;
	int rc;

	ARG_UNUSED(argc);

	hex_len = strlen(argv[1]);
	if (hex_len != 2u * WITNESS_LINK_KEY_LEN ||
	    hex2bin(argv[1], hex_len, key, sizeof(key)) != sizeof(key)) {
		shell_error(sh, "key must be exactly %u hex characters (%u bytes)",
			    (unsigned int)(2u * WITNESS_LINK_KEY_LEN),
			    (unsigned int)WITNESS_LINK_KEY_LEN);
		return -EINVAL;
	}
	if (all_zero(key, sizeof(key))) {
		shell_error(sh, "refusing an all-zero key; generate one with "
			    "`openssl rand -hex %u`", (unsigned int)WITNESS_LINK_KEY_LEN);
		return -EINVAL;
	}

	rc = settings_subsys_init();
	if (rc != 0) {
		shell_error(sh, "settings init rc=%d; nothing stored", rc);
		return rc;
	}
	/* Split literal for the same reason as witkey's: port_purity_check.sh
	 * matches this against PORTING.md to catch the writer and the reader in
	 * witness_link.c drifting apart. */
	rc = settings_save_one("uwl/anc/k" "", key, sizeof(key));
	if (rc != 0) {
		shell_error(sh, "storing the anchor key rc=%d", rc);
		return rc;
	}

	shell_print(sh, "stored the second-anchor link key. Give the satellite the same "
		    "bytes, then flash the Thread image WITHOUT --erase.");
	return 0;
}

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_HEAP_PROBE)
#include <mbedtls/memory_buffer_alloc.h>
#endif

/* One blob each way, static because the hex form of a full 476-byte blob is 952
 * characters on its own, against a shell thread stack of
 * CONFIG_SHELL_STACK_SIZE. */
static uint8_t s_blob[ULTRAWIDELOCK_PROV_BLOB_MAX];
static char s_hex[2u * ULTRAWIDELOCK_PROV_BLOB_MAX + 1u];

/**
 * Return true if all bytes in the buffer are zero.
 */
static bool all_zero(const uint8_t *p, size_t len)
{
	uint8_t acc = 0;

	for (size_t i = 0; i < len; i++) {
		acc |= p[i];
	}
	return acc == 0;
}

/* The three ways a syntactically valid blob is still useless, named rather than
 * left for the walk-up to discover. Same three reports on a
 * flash dump, checked again here because a hex string can arrive by any route.
 * Returns a reason, or NULL when the blob will actually unlock. */
static const char *dead_blob_reason(const struct ultrawidelock_reader_identity *id,
				    const struct ultrawidelock_trust_store *ts)
{
	if (id->is_dev) {
		return "it is the built-in DEV identity: the source board was never "
		       "provisioned, or was factory-reset after it was";
	}
	if (all_zero(id->grk, ULTRAWIDELOCK_GRK_LEN)) {
		return "the GroupResolvingKey is all zero: SetAliroReaderConfig never "
		       "landed on the source, so no phone can resolve this reader";
	}
	if (ts->count == 0u) {
		return "there are no trust anchors: no phone key was enrolled on the "
		       "source";
	}
	return NULL;
}

/**
 * Shell command: display the reader's provisioning state — whether it is provisioned, its reader
 * ID, GRK status, enrolled anchors, and usability verdict.
 */
static int cmd_prov(const struct shell *sh, size_t argc, char **argv)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = ultrawidelock_prov_load(&id, &ts);

	if (rc < 0) {
		shell_error(sh, "settings store unreadable; showing the dev fallback");
	}

	shell_print(sh, "identity   : %s", id.is_dev ? "DEV (not provisioned)" : "provisioned");
	shell_print(sh, "reader_id  : %02x%02x%02x%02x...%02x%02x%02x%02x", id.reader_id[0],
		    id.reader_id[1], id.reader_id[2], id.reader_id[3], id.reader_id[28],
		    id.reader_id[29], id.reader_id[30], id.reader_id[31]);
	shell_print(sh, "GRK        : %s", all_zero(id.grk, ULTRAWIDELOCK_GRK_LEN) ? "all zero (no "
		    "phone will approach)" : "set");
	shell_print(sh, "trust      : %u of %u anchor(s)", (unsigned int)ts.count,
		    (unsigned int)ULTRAWIDELOCK_TRUST_MAX);

	const char *why = dead_blob_reason(&id, &ts);

	shell_print(sh, "verdict    : %s", why ? why : "will unlock a phone enrolled on the "
		    "source home");
	return 0;
}

/**
 * Deserialize a hex-encoded identity blob, reject it if syntactically valid but useless (no
 * identity, expired, or no trust anchors), then import it to settings storage if safe.
 */
static int cmd_import(const struct shell *sh, size_t argc, char **argv)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;

	ARG_UNUSED(argc);

	size_t hex_len = strlen(argv[1]);
	size_t len = hex2bin(argv[1], hex_len, s_blob, sizeof(s_blob));

	if (len == 0u) {
		shell_error(sh, "%u hex chars did not decode (odd length, non-hex, or over "
			    "the %u-byte cap)", (unsigned int)hex_len, (unsigned int)sizeof(s_blob));
		return -EINVAL;
	}

	if (ultrawidelock_prov_deserialize(s_blob, len, &id, &ts) != 0) {
		shell_error(sh, "not an APRV blob (bad magic, version, or length)");
		return -EINVAL;
	}

	/* Refuse before writing, not after: a board that silently adopted a blob
	 * which cannot unlock is the failure this whole path exists to avoid. */
	const char *why = dead_blob_reason(&id, &ts);

	if (why != NULL) {
		shell_error(sh, "refusing: %s", why);
		return -EINVAL;
	}

	int rc = ultrawidelock_reader_import_blob(s_blob, len);

	if (rc != 0) {
		/* -1 malformed (already ruled out above), -2 settings write failed. */
		shell_error(sh, "import of %u bytes failed rc=%d", (unsigned int)len, rc);
		return rc;
	}

	shell_print(sh, "adopted %u bytes: %u trust anchor(s). Reboot without SW2 to run.",
		    (unsigned int)len, (unsigned int)ts.count);
	return 0;
}

/**
 * Serialize and print the reader identity and trust store as hex after confirming with "ultrawidelock
 * export yes"; the resulting string holds the private key and can impersonate the lock.
 */
static int cmd_export(const struct shell *sh, size_t argc, char **argv)
{
	struct ultrawidelock_reader_identity id;
	struct ultrawidelock_trust_store ts;
	size_t len = 0;

	ARG_UNUSED(argc);

	if (strcmp(argv[1], "yes") != 0) {
		shell_error(sh, "this prints the reader PRIVATE KEY. Confirm: ultrawidelock export yes");
		return -EINVAL;
	}

	(void)ultrawidelock_prov_load(&id, &ts);
	if (ultrawidelock_prov_serialize(&id, &ts, s_blob, sizeof(s_blob), &len) != 0) {
		shell_error(sh, "serialise failed");
		return -EINVAL;
	}
	if (bin2hex(s_blob, len, s_hex, sizeof(s_hex)) == 0u) {
		shell_error(sh, "hex buffer too small for %u bytes", (unsigned int)len);
		return -ENOMEM;
	}

	shell_print(sh, "%s", s_hex);
	shell_warn(sh, "that string is the reader identity: whoever holds it can impersonate "
		   "this lock");
	return 0;
}

/**
 * Erase the reader identity and all trust anchors after confirming with "ultrawidelock erase yes",
 * returning to the DEV identity with no anchors.
 */
static int cmd_erase(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	if (strcmp(argv[1], "yes") != 0) {
		shell_error(sh, "this erases the identity and every trust anchor. "
			    "Confirm: ultrawidelock erase yes");
		return -EINVAL;
	}

	int rc = ultrawidelock_reader_provision_clear();

	if (rc != 0) {
		shell_error(sh, "erase failed rc=%d", rc);
		return rc;
	}
	shell_print(sh, "erased: back to the DEV identity with no trust anchors");
	return 0;
}

/*
 * Witness link keys.
 *
 * WHY THEY ARE ENROLLED FROM HERE, on an image that does not run the latch.
 * The lock image is a Thread build and overlay-thread.conf sets CONFIG_SHELL=n
 * (reader + console + Thread overflows this part's RAM), so the image that
 * READS these keys can never have a console to type one into. This image can.
 * They land in the settings partition, which `west flash` without --erase
 * preserves, so the enrollment survives the reflash to the Thread image -- the
 * same route the reader identity already takes.
 *
 * A key is write-only from here on. There is no `witkey show`: this console
 * already prints a private key on demand and one such command is one more than
 * this file should carry. A key typed wrong presents as a lock that accepts no
 * report from that witness, which witness_link.c names in its log.
 */
static const char *const s_witness_roles[] = {
	NULL,        /* 0 is ULTRAWIDELOCK_WITNESS_ROLE_UNKNOWN, not a mounting */
	"inside",
	"outside",
	"threshold",
};

/* Role word -> the number witness_link.c indexes its table by. 0 = no match. */
static unsigned int witness_role_of(const char *word)
{
	for (unsigned int i = 1u; i < ARRAY_SIZE(s_witness_roles); i++) {
		if (strcmp(word, s_witness_roles[i]) == 0) {
			return i;
		}
	}
	return 0u;
}

/**
 * Shell command: store one witness's 16-byte link key, addressed by the same
 * role word the dongle's own PROV command takes, so the two ends are typed
 * alike rather than one by name and one by number.
 */
static int cmd_witkey(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t key[WITNESS_LINK_KEY_LEN];
	char name[32];
	unsigned int role;
	size_t hex_len;

	ARG_UNUSED(argc);

	role = witness_role_of(argv[1]);
	if (role == 0u) {
		shell_error(sh, "role must be inside, outside or threshold");
		return -EINVAL;
	}

	hex_len = strlen(argv[2]);
	if (hex_len != 2u * WITNESS_LINK_KEY_LEN ||
	    hex2bin(argv[2], hex_len, key, sizeof(key)) != sizeof(key)) {
		shell_error(sh, "key must be exactly %u hex characters (%u bytes)",
			    (unsigned int)(2u * WITNESS_LINK_KEY_LEN),
			    (unsigned int)WITNESS_LINK_KEY_LEN);
		return -EINVAL;
	}

	/* An all-zero key is what an uninitialised buffer and a mistyped
	 * `openssl rand` both produce, and it would seal reports that anyone
	 * could forge. Refuse before it reaches the store. */
	if (all_zero(key, sizeof(key))) {
		shell_error(sh, "refusing an all-zero key; generate one with "
			    "`openssl rand -hex %u`", (unsigned int)WITNESS_LINK_KEY_LEN);
		return -EINVAL;
	}

	/* Spelled here, and split so the record name appears as its own literal:
	 * tests/tooling/port_purity_check.sh reads it out of this file and
	 * matches it against PORTING.md, which is the only check there is that
	 * the writer and witness_link.c's reader still agree. */
	(void)snprintf(name, sizeof(name), "uwl/wit/k" "/%u", role);

	int rc = settings_subsys_init();

	if (rc != 0) {
		shell_error(sh, "settings init rc=%d; nothing stored", rc);
		return rc;
	}
	rc = settings_save_one(name, key, sizeof(key));
	if (rc != 0) {
		shell_error(sh, "storing %s rc=%d", name, rc);
		return rc;
	}

	shell_print(sh, "stored the %s witness link key. Provision that dongle with the "
		    "same bytes, then flash the Thread image WITHOUT --erase.",
		    s_witness_roles[role]);
	return 0;
}

/*
 * The SECOND ANCHOR's link key. Separate command and separate record from
 * `witkey`: the anchor is a UWB responder, not a BLE witness, and the two
 * device classes must not share an enrolment path -- the witnesses are retired
 * and nothing new should be enrolled as one.
 */
static int cmd_anckey(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t key[WITNESS_LINK_KEY_LEN];
	size_t hex_len;
	int rc;

	ARG_UNUSED(argc);

	hex_len = strlen(argv[1]);
	if (hex_len != 2u * WITNESS_LINK_KEY_LEN ||
	    hex2bin(argv[1], hex_len, key, sizeof(key)) != sizeof(key)) {
		shell_error(sh, "key must be exactly %u hex characters (%u bytes)",
			    (unsigned int)(2u * WITNESS_LINK_KEY_LEN),
			    (unsigned int)WITNESS_LINK_KEY_LEN);
		return -EINVAL;
	}
	if (all_zero(key, sizeof(key))) {
		shell_error(sh, "refusing an all-zero key; generate one with "
			    "`openssl rand -hex %u`", (unsigned int)WITNESS_LINK_KEY_LEN);
		return -EINVAL;
	}

	rc = settings_subsys_init();
	if (rc != 0) {
		shell_error(sh, "settings init rc=%d; nothing stored", rc);
		return rc;
	}
	/* Split literal for the same reason as witkey's: port_purity_check.sh
	 * matches this against PORTING.md to catch the writer and the reader in
	 * witness_link.c drifting apart. */
	rc = settings_save_one("uwl/anc/k" "", key, sizeof(key));
	if (rc != 0) {
		shell_error(sh, "storing the anchor key rc=%d", rc);
		return rc;
	}

	shell_print(sh, "stored the second-anchor link key. Give the satellite the same "
		    "bytes, then flash the Thread image WITHOUT --erase.");
	return 0;
}

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_HEAP_PROBE)
/* Run this straight after an `import`: the commit path does a software P-256
 * derive, which is the reader's heaviest single crypto step, and the peak is
 * cumulative since boot so one reading covers the whole command. */
static int cmd_heap(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	size_t used = 0;
	size_t blocks = 0;

	mbedtls_memory_buffer_alloc_max_get(&used, &blocks);
	shell_print(sh, "mbedtls heap peak: %u B of %u (%u blocks)", (unsigned int)used,
		    (unsigned int)CONFIG_MBEDTLS_HEAP_SIZE, (unsigned int)blocks);
	return 0;
}
#else
#define cmd_heap NULL
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_ultrawidelock,
	SHELL_CMD(prov, NULL, "Show the stored reader identity and whether it can unlock.",
		  cmd_prov),
	SHELL_CMD_ARG(import, NULL, "Adopt an exported identity: `import <hex>`.", cmd_import, 2,
		      0),
	SHELL_CMD_ARG(export, NULL, "Print the stored identity, private key and all: "
		      "`export yes`.", cmd_export, 2, 0),
	SHELL_CMD_ARG(erase, NULL, "Erase identity and trust anchors: `erase yes`.", cmd_erase, 2,
		      0),
	SHELL_CMD_ARG(witkey, NULL, "Enroll a BLE witness: `witkey <inside|outside|threshold> "
		      "<hex32>`.", cmd_witkey, 3, 0),
	SHELL_CMD_ARG(anckey, NULL, "Enroll the second UWB anchor: `anckey <hex32>`.",
		      cmd_anckey, 2, 0),
	SHELL_COND_CMD(CONFIG_ULTRAWIDELOCK_HEAP_PROBE, heap, NULL,
		       "Peak mbedTLS heap use since boot.", cmd_heap),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ultrawidelock, &sub_ultrawidelock, "credential reader provisioning.", NULL);
