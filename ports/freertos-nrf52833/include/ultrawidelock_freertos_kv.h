/* SPDX-License-Identifier: ISC */

/*
 * This port's persistent key-value store: the key ids it assigns, and the one
 * call that is not part of the contract.
 *
 * The contract itself -- the result codes, the key windows, and the five
 * operations -- lives in modules/ultrawidelock_port/include/ultrawidelock_kv.h
 * and is the same on every port. It was derived from this file, which reached
 * the numeric-key design first, so the move onto the seam is a deletion rather
 * than a translation: what stood here was already the same enum and the same
 * window bases, spelled twice.
 *
 * The store is four physical flash pages, the same region the Zephyr oracle
 * reserves for its settings partition. Two consumers need one on this part: the
 * reader's provisioning blob and OpenThread's settings, with Matter's records
 * and PSA's trusted storage above them.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_KV_H
#define ULTRAWIDELOCK_FREERTOS_KV_H

#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_kv.h"

/*
 * Ids inside the windows ultrawidelock_kv.h reserves. They are assigned here
 * because this port owns the Matter and PSA storage backends that read them; a
 * port that implements neither needs none of this file.
 */

/*
 * Matter's own records, above OpenThread's window. Only the shared Thread
 * transport's SRP host-name suffix lives at the base: it is deliberately NOT
 * under a tree the factory reset clears, because the SRP client's ECDSA key
 * survives that too and the two have to be erased together or not at all.
 */
#define ULTRAWIDELOCK_KV_KEY_MATTER_SRP_HOST_ID 0x2000u
/*
 * The operational identity, one record per settings path the fabric store
 * writes. Numbered explicitly rather than hashed: a hash could alias two
 * records, and the set is small and fixed.
 *
 * These keys describe only the retired v0.3 schema. They remain mapped so the
 * mf2 clean-break loader can reclaim them; it never treats them as identity.
 */
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_VER 0x2010u
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_OK 0x2011u
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_TD 0x2012u
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_XP 0x2013u
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_ICLEN 0x2014u
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_ICAC 0x2015u
/* Legacy fabric slots. The clean-break mf2 records have their own window. */
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT0 0x2020u
#define ULTRAWIDELOCK_KV_KEY_MATTER_FAB_SLOT_LIMIT 0x2030u
/* One per persisted subscription ("msub/N"); one per CASE session, the window
 * holds 16. */
#define ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT0 0x2040u
#define ULTRAWIDELOCK_KV_KEY_MATTER_SUB_SLOT_LIMIT 0x2050u
/*
 * Door Lock attributes a controller writes and expects to read back after a
 * reboot: AutoRelockTime and the Approach Direction bitmap.
 */
#define ULTRAWIDELOCK_KV_KEY_MATTER_DL_AUTO_RELOCK 0x2060u
#define ULTRAWIDELOCK_KV_KEY_MATTER_DL_APPROACH 0x2061u
/* Clean-break, per-record Matter identity schema ("mf2"). */
#define ULTRAWIDELOCK_KV_KEY_MATTER_MF2_META 0x2070u
#define ULTRAWIDELOCK_KV_KEY_MATTER_MF2_NET 0x2071u
#define ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ICAC 0x2072u
#define ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB0 0x2080u
#define ULTRAWIDELOCK_KV_KEY_MATTER_MF2_FAB_LIMIT 0x2085u
#define ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL0 0x2090u
#define ULTRAWIDELOCK_KV_KEY_MATTER_MF2_ACL_LIMIT 0x2095u

/*
 * PSA Internal Trusted Storage, which exists for exactly one reason: OpenThread
 * signs its SRP registrations with an ECDSA key that must survive a reboot, and
 * routing that through PSA key references is what keeps Mbed TLS's PK, ECP and
 * BIGNUM modules out of the image.
 *
 * A directory record plus a fixed set of slots. The directory maps the 64-bit
 * PSA uid onto a slot, because a uid cannot be hashed into 16 bits without the
 * possibility of aliasing two keys -- and two aliased keys is a node that signs
 * with the wrong one.
 */
#define ULTRAWIDELOCK_KV_KEY_PSA_ITS_DIR 0x3000u
#define ULTRAWIDELOCK_KV_KEY_PSA_ITS_SLOT0 0x3001u
#define ULTRAWIDELOCK_KV_KEY_PSA_ITS_SLOTS 8u

/*
 * Bytes of the active page still available, for headroom reporting.
 *
 * Off the seam on purpose: "the active page" is this backend's own structure,
 * and a store built on settings or on NVS has no page to report. A caller that
 * needs it is already writing to this port.
 */
size_t ultrawidelock_freertos_kv_free_bytes(void);

#endif /* ULTRAWIDELOCK_FREERTOS_KV_H */
