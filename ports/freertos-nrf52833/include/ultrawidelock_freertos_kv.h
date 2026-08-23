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
 * and PSA's trusted storage above them. Matter ids live in the portable
 * contract because that store is shared; only this port's PSA ids remain here.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_KV_H
#define ULTRAWIDELOCK_FREERTOS_KV_H

#include <stddef.h>
#include <stdint.h>

#include "ultrawidelock_kv.h"

/*
 * Ids inside the windows ultrawidelock_kv.h reserves. Matter's ids moved into
 * that contract when its store began calling the seam directly. The PSA
 * backend remains specific to this port, so its assignments remain here.
 */

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
