/* SPDX-License-Identifier: ISC */

/** @file fira_session.h — Range + URSK store for the CCC Pre-POLL responder. */

#ifndef ULTRAWIDELOCK_UWB_FIRA_SESSION_H_
#define ULTRAWIDELOCK_UWB_FIRA_SESSION_H_

#include <stdbool.h>
#include <stdint.h>

/** @brief Fetch the most recent valid DS-TWR range; out-params optional (NULL to skip). */
bool fira_session_last_range(int32_t *cm_out, uint16_t *addr_out, uint8_t *nlos_out,
			     uint32_t *block_out, int64_t *age_ms_out);

#if defined(CONFIG_ULTRAWIDELOCK_CRED)
/** @brief Stash a credential URSK for the CCC Pre-POLL STS decode; NULL clears it. */
void fira_session_set_provisioned_ursk(const uint8_t *ursk);

/** @brief The stashed credential URSK (32 bytes), or NULL if none — for the Pre-POLL decode. */
const uint8_t *fira_session_get_ursk(void);

/** Bind authenticated on-air messages to the credential-negotiated session. */
void fira_session_set_id(uint32_t session_id);

/** Return the active credential session id, or zero when no session is bound. */
uint32_t fira_session_id(void);

/** @brief Latch a CCC DS-TWR range so it flows up the credential mRangingData seam. */
void fira_session_set_ccc_range_cm(int32_t cm, uint32_t block);

/** @brief Record the layer-2 STS evidence for the block that is about to latch.
 *
 * Separate from the latch itself because the responder RX path owns the DW3000
 * diagnostics and the store does not. Call it immediately before
 * fira_session_set_ccc_range_cm(); the latch consumes the evidence and clears
 * it, so a latch with no preceding call records "no evidence", which reads as
 * a failed STS rather than a passed one.
 *
 * @param driver_verdict dwt_readstsquality() return (>=0 good, <0 bad).
 * @param quality_index  the signed STS quality index it wrote.
 */
void fira_session_set_ccc_range_sts(int32_t driver_verdict, int16_t quality_index);

/** @brief Layer-2 evidence accumulated over the current agreement run. */
struct fira_range_integrity {
	bool sts_ok;         /**< every block in the run passed the STS floor */
	int16_t sts_quality; /**< worst STS quality index seen in the run */
	uint8_t trust_level; /**< layer-4 run length behind the latched range */
};

/** @brief Read the integrity evidence for the latched range.
 *
 * Reports the run, not the last block: a consumer that fails closed needs to
 * know that every block which built the consensus was well-correlated, since
 * an attacker who can land one good block among three suspect ones has not
 * been stopped by a check that only inspects the last.
 *
 * @return false (leaving @p out untouched) when no range has been latched. */
bool fira_session_last_range_integrity(struct fira_range_integrity *out);

/** @brief Invalidate the old session's range and consensus before a new URSK
 *  session starts. The monotonic generation is retained so callers can prove
 *  that a later latch happened after their checkpoint. */
void fira_session_reset_ranges(void);

/** @brief Monotonic generation incremented after every accepted range latch. */
uint32_t fira_session_range_generation(void);

/** @brief STS-index slot clock (inert without a MAC time base); returns 0. */
uint32_t fira_session_current_slot(void);

/* ── Range-integrity gate (anti-spoof / anti Ghost-Peak) ─────────────────────
 *
 * A DS-TWR distance is trustworthy only when it is (1) physically plausible,
 * (2) backed by a well-correlated STS, and (4) consistent with recent blocks.
 * (Layer 3, an Ipatov first-path check, was removed — untunable on this HW —
 * so the numbering keeps a gap.) Layers 1 and 4 are enforced in the range
 * store (fira_session_set_ccc_range_cm); layer 2 is a pure predicate the
 * responder RX path evaluates, since it owns the DW3000 diagnostics. Every
 * threshold below is a bring-up default — tune on the bench.
 *
 * Layer 2 is recorded rather than enforced here, on purpose, because its two
 * consumers want opposite failure modes. A door lock must not refuse to open
 * on a marginal block — a mis-tuned floor locks a human out of their house —
 * so it keeps the shadow behaviour and only drops blocks under
 * CONFIG_ULTRAWIDELOCK_RANGE_GATE_STRICT. A signed presence assertion is the opposite:
 * it exists to be believed by someone who was not there, so it must refuse to
 * state a distance it cannot vouch for. The store therefore carries the
 * verdict alongside the range and lets each consumer pick.
 */

/* Layer 1 — plausibility band. Below -NEG_TOL is physically impossible (an
 * early-first-path / Ghost-Peak spoof drives ToF sharply negative); above MAX
 * is outside any credential proximity envelope. A small negative is legitimate
 * point-blank calibration slop and reads as 0 cm rather than being dropped. */
#define FIRA_RANGE_NEG_TOL_CM 30   /* legit point-blank slop; drop beyond */
#define FIRA_RANGE_MAX_CM     3000 /* usable envelope (30 m); tune to radio */

/** @brief Layer 1: true if @p cm is a physically plausible DS-TWR distance. */
bool fira_session_range_plausible(int32_t cm);

/* Layer 2 — STS quality floor. dwt_readstsquality() returns >=0 for good STS,
 * <0 for bad, and its signed index is "good" at >= ~60% of the STS length. A
 * spoofed early path cannot reproduce the scrambled sequence, so its STS
 * quality collapses. Raise MIN toward the 60%-of-length index to tighten. */
#if defined(CONFIG_ULTRAWIDELOCK_STS_QUALITY_MIN)
#define FIRA_STS_QUALITY_MIN CONFIG_ULTRAWIDELOCK_STS_QUALITY_MIN
#else
#define FIRA_STS_QUALITY_MIN 0 /* index floor; 0 = defer to driver verdict */
#endif

/** @brief Layer 2: true if the STS correlated well enough to trust its timestamp.
 *  @param driver_verdict  dwt_readstsquality() return (>=0 good, <0 bad).
 *  @param quality_index   the signed STS quality index it wrote. */
bool fira_session_sts_quality_ok(int32_t driver_verdict, int16_t quality_index);

/* Layer 4 — cross-block consensus. A single injected block cannot move an
 * unlock decision alone: a range is "trusted" only once K consecutive plausible
 * blocks agree to within SPREAD. This does not gate the latched last-range (the
 * shell/telemetry still track live values); it is the trust bit now wired into
 * the unlock path via ultrawidelock_uwb_trusted_range_cm(), which surfaces a distance only
 * once trust is built. */
#if defined(CONFIG_ULTRAWIDELOCK_RANGE_TRUST_K)
#define FIRA_RANGE_TRUST_K CONFIG_ULTRAWIDELOCK_RANGE_TRUST_K
#else
#define FIRA_RANGE_TRUST_K 3
#endif
#define FIRA_RANGE_SPREAD_CM 50 /* max block-to-block delta to stay agreed */

/** @brief Layer 4: true once >= K consecutive plausible, mutually consistent
 *  ranges have been latched. Cleared by any implausible or outlier block. */
bool fira_session_range_trusted(void);

/** @brief Layer 4 diagnostic: the live run length of agreeing plausible blocks
 *  (0..FIRA_RANGE_TRUST_K) behind fira_session_range_trusted(). */
uint8_t fira_session_trust_level(void);

/** @brief Register a callback fired after each accepted range latch (NULL to
 *  clear). Runs on the UWB RX path — keep it to a task wake, nothing heavier. */
void fira_session_set_range_listener(void (*cb)(void));
#endif /* CONFIG_ULTRAWIDELOCK_CRED */

#endif /* ULTRAWIDELOCK_UWB_FIRA_SESSION_H_ */
