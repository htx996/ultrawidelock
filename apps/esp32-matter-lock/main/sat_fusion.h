/* SPDX-License-Identifier: ISC */

/**
 * @file sat_fusion.h — the lock's half of the two-anchor inside/outside gate.
 *
 * Owns the second anchor's reports, this node's own measurements, and the
 * verdict the two produce together. The approach loop asks one question of it —
 * may a passive unlock proceed — and everything else here exists to make that
 * answer honest.
 *
 * THE PAIRING RULE IS ABSOLUTE. Both distances must come from the SAME ranging
 * block. The block travels inside the sealed report for exactly that reason.
 * Mispairing does not degrade gracefully: it produces triangle-inequality
 * rejections that read, on a bench, as a broken radio.
 *
 * FAIL-CLOSED IN THE PERMISSIVE DIRECTION, which is the confusing part and is
 * deliberate. With no satellite mounted, no report, or a stale one, the verdict
 * is UNKNOWN and UNKNOWN PERMITS prediction — so a lock with the link switched
 * off behaves exactly as it does today. What the gate withholds is the case
 * where the geometry positively says INSIDE. Absence is not evidence of
 * outsideness, and treating it as such would lock every user out the first time
 * a satellite lost power.
 */

#ifndef SAT_FUSION_H
#define SAT_FUSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the sealed link and the fusion state.
 *
 * Safe with no link key stored: the geometry stays UNKNOWN and the door behaves
 * as it did before the second anchor existed. Registers the console commands.
 */
void sat_fusion_init(void);

/**
 * Record one of THIS node's trusted ranges, keyed by the block it was measured
 * in so a report that took a block or two to arrive still finds its partner.
 */
void sat_fusion_observe(int32_t self_mm, uint32_t self_block, int64_t now_ms);

/**
 * May a passive unlock proceed?
 *
 * True unless the geometry positively says the phone is inside. Consulted for
 * the PREDICT and THRESHOLD paths only — a credential the user physically
 * presented is not a passive unlock and is never gated here.
 */
bool sat_fusion_may_predict(int64_t now_ms);

/**
 * Hand the satellite the session it must join: the URSK and ranging config the
 * phone negotiated with us. Sealed under the link key, sent at session start.
 */
void sat_fusion_send_handoff(const uint8_t *ursk, size_t ursk_len, const uint8_t *rcfg,
			     size_t rcfg_len, uint8_t channel, uint8_t sync_code_index);

#ifdef __cplusplus
}
#endif

#endif /* SAT_FUSION_H */
