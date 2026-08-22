/* SPDX-License-Identifier: ISC */

/*
 * Mbed TLS build configuration for the standalone nRF52833 port.
 *
 * This is a whole replacement for mbedtls_config.h, passed as
 * MBEDTLS_CONFIG_FILE, so anything not named here is off. That is the point:
 * the default config enables TLS, X.509 and a filesystem, none of which this
 * image has any caller for.
 *
 * What gets built is the PSA crypto core and nothing else. The legacy modules
 * (ECP, AES, GCM, SHA-256, CMAC, the bignum layer) are NOT listed below and
 * must not be: with MBEDTLS_PSA_CRYPTO_CONFIG on, include/mbedtls/
 * config_adjust_legacy_from_psa.h derives exactly the legacy set that the
 * PSA_WANT_* list in psa_crypto_config_freertos.h requires. Enabling them by
 * hand here would let the two lists drift apart silently.
 *
 * There is no hardware accelerator on this part. nRF52833 has no CryptoCell,
 * so P-256 is software, and a signature or an ECDH agreement costs tens of
 * milliseconds. That is affordable because the credential protocol's expedited path uses
 * Kpersistent with AES-CMAC and does no asymmetric work inside the
 * ~1.836 ms DW3110 response-arm window; the public-key work happens during
 * the BLE exchange that precedes ranging. Nordic's nrf_oberon would be faster
 * and carries no licence cost this port does not already pay, but binding it
 * under the PSA core is nrf_security's job and nrf_security is Zephyr Kconfig,
 * so it stays out until something is measured to need it.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_MBEDTLS_CONFIG_H
#define ULTRAWIDELOCK_FREERTOS_MBEDTLS_CONFIG_H

/* The PSA core, driven by the PSA_WANT_* list rather than by legacy switches. */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CONFIG

/*
 * Randomness. The nRF52833 RNG is a real noise source, and
 * board/entropy_freertos.c runs it with bias correction on, so it is fit to
 * seed with. It is not fit to draw from directly at volume: bias correction
 * costs on the order of a hundred microseconds a byte, which is why the board
 * pools it and why the DRBG sits in front of it here rather than
 * MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG routing every psa_generate_random() call at
 * the peripheral.
 *
 * MBEDTLS_NO_PLATFORM_ENTROPY is what stops the library reaching for
 * /dev/urandom or a Windows CSP; MBEDTLS_ENTROPY_HARDWARE_ALT is what makes
 * mbedtls_hardware_poll() in mbedtls_platform_freertos.c the only source.
 */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY

/*
 * Threading. Two tasks reach the PSA core: the OpenThread task, and the task
 * that runs the credential exchange behind NimBLE. The PSA key store is global
 * mutable state shared between them, so this is required for correctness and
 * not a hardening option.
 *
 * ALT rather than PTHREAD: the mutex is a FreeRTOS static mutex, defined in
 * crypto/threading_alt.h, which Mbed TLS includes by that exact name.
 * ultrawidelock_freertos_crypto_init() installs the four callbacks before any other
 * library call, which mbedtls_threading_set_alt() requires.
 */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

/*
 * Allocation. The PSA core allocates key slots and bignum limbs. There is no
 * newlib heap in this image -- NimBLE already forced a FreeRTOS heap for its
 * porting layer, so the library uses that one rather than adding a second.
 * The macro form is used in preference to mbedtls_platform_set_calloc_free()
 * so the indirection resolves at compile time and there is no window before
 * registration in which the wrong allocator could be called.
 */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO ultrawidelock_freertos_mbedtls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO ultrawidelock_freertos_mbedtls_free

/*
 * Those two macros expand inside Mbed TLS library sources, which include
 * nothing belonging to this port. This config file is the only header those
 * sources are guaranteed to read -- build_info.h pulls it in ahead of
 * everything else -- so the declarations have to live here rather than in
 * ultrawidelock_freertos_crypto.h.
 */
#include <stddef.h>
void *ultrawidelock_freertos_mbedtls_calloc(size_t count, size_t size);
void ultrawidelock_freertos_mbedtls_free(void *block);

/*
 * P-256 arithmetic. NIST_OPTIM replaces the generic modular reduction with the
 * fast reduction the curve's prime allows; it is on by default upstream and is
 * repeated here because this file replaces the defaults wholesale.
 *
 * AES tables live in flash, not RAM. The generated form costs about 8 KB of
 * RAM, and RAM is the binding constraint on this part -- the current Zephyr
 * production oracle uses 120,740 B with reader, Matter, Thread, and SMP enabled,
 * so the trade goes the other way here than it would on a part
 * with room.
 */
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_AES_ROM_TABLES

/*
 * The legacy message-digest layer, for one caller: OpenThread.
 *
 * Everything this port wrote reaches hashing through PSA, so this was absent
 * until Thread was linked. OpenThread's crypto_platform.cpp calls
 * mbedtls_md_hmac_* directly for its HMAC-SHA256 -- that is what
 * OPENTHREAD_CONFIG_CRYPTO_LIB_MBEDTLS means, and leaving it at that default is
 * what keeps the port from having to implement otPlatCrypto itself. The cost is
 * this module, and the alternative cost was the whole otPlatCrypto surface.
 *
 * A MATTER BUILD DOES NOT NEED IT. There the crypto library is PSA and
 * OpenThread's HMAC goes through Nordic's crypto_psa.c, so the only caller this
 * module ever had is gone. Left defined it would be a module nothing reaches,
 * which --gc-sections would mostly remove and which would still confuse the
 * next person reading this file for what the image contains.
 */
#if !defined(ULTRAWIDELOCK_HAVE_MATTER) || !ULTRAWIDELOCK_HAVE_MATTER
#define MBEDTLS_MD_C
#endif

#if defined(ULTRAWIDELOCK_HAVE_MATTER) && ULTRAWIDELOCK_HAVE_MATTER
/*
 * PERSISTENT PSA KEYS, and NOT the legacy PK layer.
 *
 * OpenThread's SRP client signs its registrations with an ECDSA key that has to
 * survive a reboot. There are two ways to give it one:
 *
 *   MBEDTLS  crypto_platform.cpp generates the key, writes it as DER and parses
 *            it back on every use. That needs PK, PK_PARSE, PK_WRITE, ECP,
 *            BIGNUM, ASN1 and OID. MEASURED: +9,690 bytes of flash, on an image
 *            that was already 12 KB over.
 *   PSA      the key lives in PSA as a key REFERENCE and never leaves it. The
 *            P-256 arithmetic is already in this image -- the reader has used
 *            PSA since the beginning -- so the marginal cost is the storage
 *            layer, not the curve.
 *
 * The first was tried and is what this replaces. PSA key references need
 * persistent storage, which is why STORAGE_C is on; the ITS backend underneath
 * it is crypto/psa_its_freertos.c, over the port's key-value store, because
 * Mbed TLS's own backend is file-based and there is no filesystem here.
 */
#define MBEDTLS_PSA_CRYPTO_STORAGE_C

/*
 * ASN.1 PARSING ONLY, for one caller.
 *
 * Nordic's crypto_psa.c unwraps a DER-encoded P-256 private key when
 * OpenThread hands it one to import -- mbedtls_asn1_get_tag and
 * mbedtls_asn1_get_int, nothing more. This is the parser alone: no
 * ASN1_WRITE_C, no OID_C, and none of the PK layer that the Mbed TLS crypto
 * path would have needed, because nothing here ENCODES a key.
 */
#define MBEDTLS_ASN1_PARSE_C

#endif

/*
 * Deliberately absent, each because a caller for it does not exist:
 *
 *   MBEDTLS_PK_*, MBEDTLS_ECP_C    the legacy public-key layer. OpenThread
 *   MBEDTLS_BIGNUM_C, ASN1, OID    reaches ECDSA through PSA here, so nothing
 *                                  parses or writes a DER key
 *   MBEDTLS_SSL_*, MBEDTLS_X509_*  no TLS and no certificates in this image
 *   MBEDTLS_ERROR_C                error strings, flash for text nothing reads
 *   MBEDTLS_SELF_TEST              the host suite and the gates test this port
 *   MBEDTLS_FS_IO                  no filesystem
 *   MBEDTLS_TIMING_C               uses a host clock this part does not have
 */

#endif /* ULTRAWIDELOCK_FREERTOS_MBEDTLS_CONFIG_H */
