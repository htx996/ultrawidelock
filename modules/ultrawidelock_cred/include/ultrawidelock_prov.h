/* SPDX-License-Identifier: ISC */

// Persistent reader provisioning storage: identity and credential trust anchors saved to and
// loaded from NVS.
// Declares ultrawidelock_prov_store for committing an identity/trust pair to NVS, and struct
// ultrawidelock_trust_store, the set of trusted credential public keys against which a presented
// credential is authenticated.
/*
 * ultrawidelock_prov — the reader provisioning seam (Phase 3.4): the reader's own
 * identity (a stable reader identifier + P-256 signing key) and the trust store
 * of credential public keys it authenticates presented credentials against.
 * NVS-backed on target, with a clearly-marked dev fallback so the credential-auth
 * transaction can be driven at bench before Phase-4 Matter provisioning writes a
 * real identity.
 *
 * Split like ultrawidelock_crypto: the (de)serialisation + dev default + trust logic is
 * portable (ultrawidelock_prov.c); the NVS load/store is target-only (ultrawidelock_prov_nvs.c,
 * not compiled on host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ULTRAWIDELOCK_READER_ID_LEN   32u
#define ULTRAWIDELOCK_READER_PRIV_LEN 32u
#define ULTRAWIDELOCK_CRED_PUB_LEN    65u /* uncompressed P-256 point: 0x04 | X | Y */
/*
 * 4 was too few and the cost of being wrong was total. An Apple home installs
 * two endpoint keys per pairing, they accumulate across pairings, and nothing
 * evicted them -- so a re-paired reader held 4 stale anchors, rejected the key
 * the phone actually presented, and could not be recovered by pairing again.
 * 6 at 97 B a slot: three pairings before anything is evicted, against a
 * failure mode that used to be permanent. It was briefly 8 and came back down
 * to buy RAM for the OpenThread stack, which the Interaction Model runs on and
 * which was overflowing during commissioning -- this part has 128 KB and the
 * image is at 96%. The number that matters is "more than one pairing's worth",
 * and eviction (below) is what makes running out survivable rather than fatal.
 */
#define ULTRAWIDELOCK_TRUST_MAX       6u  /* trusted credential keys the store holds */
#define ULTRAWIDELOCK_GRK_LEN         16u /* group resolving key (credential BLE-UWB adv tag) */
#define ULTRAWIDELOCK_KPERSISTENT_LEN 32u /* per-credential expedited-fast key (§8.3.1.13) */
/*
 * The credential index of an anchor no Matter admin installed.
 *
 * Matter credential and user indices are 1-based (Door Lock cluster: both
 * fields constrain to `between value="1"`), so zero can never collide with a
 * real one. An anchor carrying it is unaddressable by ClearCredential, which is
 * the truth about a key a bench command added.
 */
#define ULTRAWIDELOCK_CRED_INDEX_NONE 0u
/* The wildcard both Door Lock clear commands use for "every one of these"
 * (ClearUser/ClearCredential, index 0xFFFE). */
#define ULTRAWIDELOCK_USER_INDEX_ALL  0xFFFEu

/*
 * The reader's provisioned identity. reader_id rides AUTH0 and both ECDSA
 * transcripts (tag 0x4D); sign_priv signs the reader-usage transcript. is_dev
 * marks the built-in bench identity, never a real deployment.
 */
struct ultrawidelock_reader_identity {
	uint8_t reader_id[ULTRAWIDELOCK_READER_ID_LEN];
	uint8_t sign_priv[ULTRAWIDELOCK_READER_PRIV_LEN];
	uint8_t grk[ULTRAWIDELOCK_GRK_LEN]; /* group resolving key; all-zero if none */
	bool is_dev;
};

/*
 * Trusted credential public keys. A presented credential authenticates only if
 * its key is in here (or the store is empty and dev policy allows it). A raw-key
 * allowlist is the interim seam; real issuer-chain validation is the Phase-4
 * refinement that plugs in at ultrawidelock_prov_trust_check.
 */
struct ultrawidelock_trust_store {
	uint8_t count;
	uint8_t cred_pub[ULTRAWIDELOCK_TRUST_MAX][ULTRAWIDELOCK_CRED_PUB_LEN];
	/* Expedited-fast state: bit i of kp_valid set = kpersistent[i] holds the
	 * Kpersistent agreed with cred_pub[i] in its last standard phase. */
	uint8_t kp_valid;
	uint8_t kpersistent[ULTRAWIDELOCK_TRUST_MAX][ULTRAWIDELOCK_KPERSISTENT_LEN];
	/*
	 * What the Matter admin calls this anchor.
	 *
	 * ClearCredential names its target by (type, index) and ClearUser by
	 * user index -- neither carries the key bytes -- so without these three
	 * a removal cannot be resolved to a slot and revocation is impossible.
	 * They move with the slot on eviction and removal, and stay
	 * ULTRAWIDELOCK_CRED_INDEX_NONE for anything a bench command added.
	 *
	 * The TYPE is part of the name, not decoration: Matter credential
	 * indices are scoped to their type, so an evictable endpoint key and a
	 * non-evictable one can both be index 1. Matching on the index alone
	 * would revoke whichever of the two came first and leave the one the
	 * admin meant still opening the door.
	 */
	uint8_t cred_type[ULTRAWIDELOCK_TRUST_MAX];
	uint16_t cred_index[ULTRAWIDELOCK_TRUST_MAX];
	uint16_t user_index[ULTRAWIDELOCK_TRUST_MAX];
};

/* Serialised blob v4: magic(4) ver(1) flags(1) reader_id(32) sign_priv(32)
 * grk(16) count(1), count * cred_pub(65), kp_valid(1), count * kpersistent(32),
 * count * (cred_type(1) cred_index(2) user_index(2)), the indices big-endian.
 * (v3 ended at the kpersistent array, v2 at the cred_pub array, v1 also had no
 * grk. All three are still parsed; their anchors carry no Matter index, so a
 * board provisioned before this format cannot have a credential revoked by
 * index until its owner re-installs it.) */
#define ULTRAWIDELOCK_PROV_BLOB_HDR 6u
#define ULTRAWIDELOCK_PROV_BLOB_MAX                                                                \
	(ULTRAWIDELOCK_PROV_BLOB_HDR + ULTRAWIDELOCK_READER_ID_LEN +                               \
	 ULTRAWIDELOCK_READER_PRIV_LEN + ULTRAWIDELOCK_GRK_LEN + 1u +                              \
	 (size_t)ULTRAWIDELOCK_TRUST_MAX * ULTRAWIDELOCK_CRED_PUB_LEN + 1u +                       \
	 (size_t)ULTRAWIDELOCK_TRUST_MAX * ULTRAWIDELOCK_KPERSISTENT_LEN +                         \
	 (size_t)ULTRAWIDELOCK_TRUST_MAX * 5u)

/* ---- portable core (ultrawidelock_prov.c) --------------------------------------- */

/* Populate the built-in clearly-marked dev identity + an empty trust store. */
void ultrawidelock_prov_dev_default(struct ultrawidelock_reader_identity *id,
				    struct ultrawidelock_trust_store *ts);

/* Serialise identity+trust to a self-describing blob. 0 + *out_len on success,
 * -1 on overflow (cap < the assembled length). */
int ultrawidelock_prov_serialize(const struct ultrawidelock_reader_identity *id,
				 const struct ultrawidelock_trust_store *ts, uint8_t *out,
				 size_t cap, size_t *out_len);

/* Parse a blob written by ultrawidelock_prov_serialize. 0 on success; -1 if malformed
 * (bad magic/version/length/count). Outputs are untouched on failure. */
int ultrawidelock_prov_deserialize(const uint8_t *buf, size_t len,
				   struct ultrawidelock_reader_identity *id,
				   struct ultrawidelock_trust_store *ts);

/* Trust decision for a presented credential public key:
 *    0  trusted    (cred_pub matches a stored key)
 *    1  no-anchors (store empty; caller applies dev-open policy)
 *   -1  rejected   (store non-empty and no match). */
int ultrawidelock_prov_trust_check(const struct ultrawidelock_trust_store *ts,
			   const uint8_t cred_pub[ULTRAWIDELOCK_CRED_PUB_LEN]);

/* Add a credential key to the store. 0 added; 1 already present (dedup); -1 full
 * or the point is not an uncompressed P-256 point (leading byte != 0x04). */
int ultrawidelock_prov_trust_add(struct ultrawidelock_trust_store *ts,
				 const uint8_t cred_pub[ULTRAWIDELOCK_CRED_PUB_LEN]);

/* Index of a credential key in the store, or -1 if not present. */
int ultrawidelock_prov_trust_find(const struct ultrawidelock_trust_store *ts,
			  const uint8_t cred_pub[ULTRAWIDELOCK_CRED_PUB_LEN]);

/**
 * Drop the anchor at idx and close the gap.
 *
 * Everything above idx shifts down one slot and carries its Kpersistent, its
 * kp_valid bit and its Matter indices with it; the vacated top slot is zeroed.
 * The bit and the key MUST move together: the expedited-fast path trial-derives
 * under kpersistent[i] paired with cred_pub[i] and never re-checks the trust
 * store, so a bit left pointing at a shifted row would authenticate the wrong
 * credential with no signature at all.
 *
 * @return 0 removed; -1 if ts is NULL or idx is not an occupied slot.
 */
int ultrawidelock_prov_trust_remove_at(struct ultrawidelock_trust_store *ts, int idx);

/* Drop a credential key wherever it sits. 0 removed; 1 absent (a removal that
 * has already happened is not an error); -1 if ts is NULL. */
int ultrawidelock_prov_trust_remove(struct ultrawidelock_trust_store *ts,
			    const uint8_t cred_pub[ULTRAWIDELOCK_CRED_PUB_LEN]);

/* Bind the Matter credential type/index and user index a SetCredential installed
 * this anchor under, so ClearCredential and ClearUser can find it again. Pass
 * ULTRAWIDELOCK_CRED_INDEX_NONE for an index the caller does not have.
 * 0 on success; -1 if idx is not a stored credential. */
int ultrawidelock_prov_cred_bind_set(struct ultrawidelock_trust_store *ts, int idx,
				     uint8_t cred_type, uint16_t cred_index, uint16_t user_index);

/* Slot holding a given Matter (credential type, credential index), or -1 if none
 * does. Indices are scoped to their type, so both halves must match. Type 0 and
 * ULTRAWIDELOCK_CRED_INDEX_NONE never match: an unbound anchor is not addressable. */
int ultrawidelock_prov_find_cred_index(const struct ultrawidelock_trust_store *ts,
				       uint8_t cred_type, uint16_t cred_index);

/* Bind a Kpersistent (§8.3.1.13) to the credential at idx (from
 * ultrawidelock_prov_trust_find), replacing any earlier one. 0 on success; -1 if idx is
 * not a stored credential. */
int ultrawidelock_prov_kpersistent_set(struct ultrawidelock_trust_store *ts, int idx,
			       const uint8_t kp[ULTRAWIDELOCK_KPERSISTENT_LEN]);

/* ---- target NVS backend (ultrawidelock_prov_nvs.c) ------------------------------ */

/* A store with no provisioning record is distinct from a broken store. The
 * former may be enabled explicitly for a lab image; the latter must never be
 * mistaken for an empty trust store. */
#define ULTRAWIDELOCK_PROV_LOAD_EMPTY 1

/* Load identity+trust from NVS. Outputs receive the marked dev identity on
 * EMPTY/error so diagnostics and recovery APIs remain usable, but callers must
 * treat every negative result as unavailable and fail closed.
 *    0                                  stored blob loaded
 *    ULTRAWIDELOCK_PROV_LOAD_EMPTY      no record exists
 *   -EBADMSG (or another negative errno) malformed/store failure. */
int ultrawidelock_prov_load(struct ultrawidelock_reader_identity *id,
			    struct ultrawidelock_trust_store *ts);

/* Persist identity+trust to NVS. 0 on success, negative on an NVS error. */
int ultrawidelock_prov_store(const struct ultrawidelock_reader_identity *id,
			     const struct ultrawidelock_trust_store *ts);

/**
 * Forget the stored identity and every trust anchor.
 *
 * Half of a factory reset; the Matter fabrics are the other half and belong to
 * whoever owns that store. Deliberately NOT an erase of the whole settings
 * partition: OpenThread keeps its SRP client key there, the SRP host name is
 * the factory EUI-64 and outlives any erase, and name ownership on the border
 * router is first-come-first-served by key -- so wiping the key asks for the
 * same name with a new one and is refused until the lease expires, up to 14
 * days, during which the node attaches to Thread and is unreachable on it.
 *
 * @return 0, or a negative errno from the settings backend.
 */
int ultrawidelock_prov_erase(void);

#ifdef __cplusplus
}
#endif
