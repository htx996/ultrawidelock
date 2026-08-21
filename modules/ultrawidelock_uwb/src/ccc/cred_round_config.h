/* SPDX-License-Identifier: ISC */

/** @file ultrawidelock_round_config.h — one knob for the CCC ranging round's responder count. */

#ifndef CRED_ROUND_CONFIG_H
#define CRED_ROUND_CONFIG_H

/*
 * EXPERIMENT-2RESP: the responder count appears in two places that MUST agree —
 * the M3 NUMBER_RESPONDERS_NODES attribute (ultrawidelock_uwb_msg.c) and rcfg[12]
 * Number_Responder_Nodes (cherry_ccc_shim.c). Both feed the RangingConfiguration
 * SaltedHash the Wallet independently recomputes; if they disagree, every derived
 * STS/dURSK/dUDSK diverges and nothing decodes. Defining the count once here makes
 * that invariant compiler-enforced instead of two literals a future edit can desync.
 *
 * The reader's Final-RFRAME RX offset (ccc_shim_rx.c) is derived from the same
 * knob: the phone puts its Final one slot after the last responder, so the Final
 * moves out by one slot for each extra responder.
 *
 *   ULTRAWIDELOCK_NUM_RESPONDERS 1 = validated 1:1 baseline (default; normal firmware).
 *   ULTRAWIDELOCK_NUM_RESPONDERS 2 = dual-anchor round; needs a real second responder that
 *                            transmits Response_1 at POLL+2 slots / STS index+2.
 */
#ifndef ULTRAWIDELOCK_NUM_RESPONDERS
#if defined(CONFIG_ULTRAWIDELOCK_NUM_RESPONDERS)
#define ULTRAWIDELOCK_NUM_RESPONDERS CONFIG_ULTRAWIDELOCK_NUM_RESPONDERS
#else
#define ULTRAWIDELOCK_NUM_RESPONDERS 1u
#endif
#endif

/*
 * Which responder THIS build is. 0 = the lock (Response_0 at POLL+1 slot /
 * STS index+1, the validated baseline). 1 = the satellite (Response_1 at
 * POLL+2 slots / STS index+2). Threads through ccc_shim_rx.c: the warm STS
 * derive, the Response TX RMARKER, and the Final_Data record the DS-TWR
 * reads. The Final RX offset does NOT depend on it — the phone's Final sits
 * after the LAST responder for everyone.
 */
#ifndef ULTRAWIDELOCK_RESPONDER_INDEX
#if defined(CONFIG_ULTRAWIDELOCK_RESPONDER_INDEX)
#define ULTRAWIDELOCK_RESPONDER_INDEX CONFIG_ULTRAWIDELOCK_RESPONDER_INDEX
#else
#define ULTRAWIDELOCK_RESPONDER_INDEX 0
#endif
#endif

#if ULTRAWIDELOCK_RESPONDER_INDEX >= ULTRAWIDELOCK_NUM_RESPONDERS
#error "ULTRAWIDELOCK_RESPONDER_INDEX must be < ULTRAWIDELOCK_NUM_RESPONDERS"
#endif

/*
 * Which ranging blocks THIS build answers on, for two anchors that share one
 * session. The initiator only ever consumes one responder record (proven on the
 * bench: it honours Number_Responder_Nodes=2 as slot layout but never reports
 * the second), so instead of a second slot the two anchors take turns in the
 * ONE slot the initiator does read: each transmits Response_0 only on ranging
 * blocks whose parity matches. The initiator sees the ordinary one-responder
 * round it already grants on, and each anchor gets its own authenticated DS-TWR
 * distance every second block.
 *
 *   -1 = answer every block (single-anchor default; behaviour unchanged).
 *    0 = answer even ranging blocks.
 *    1 = answer odd ranging blocks.
 *
 * The two anchors MUST be given different parities, or they transmit in the same
 * slot and collide. This is orthogonal to ULTRAWIDELOCK_RESPONDER_INDEX, which
 * stays 0 on both: alternation is about WHICH BLOCKS, not which slot.
 */
#ifndef ULTRAWIDELOCK_BLOCK_PARITY
#if defined(CONFIG_ULTRAWIDELOCK_BLOCK_PARITY)
#define ULTRAWIDELOCK_BLOCK_PARITY CONFIG_ULTRAWIDELOCK_BLOCK_PARITY
#else
#define ULTRAWIDELOCK_BLOCK_PARITY (-1)
#endif
#endif

#if ULTRAWIDELOCK_BLOCK_PARITY >= 0 && ULTRAWIDELOCK_NUM_RESPONDERS != 1u
#error "block-parity alternation shares ONE responder slot; NUM_RESPONDERS must be 1"
#endif

/*
 * Slot offset of the phone's Final RFRAME from the POLL, in ranging slots and,
 * equivalently, in STS index steps: slot_offset(FINAL) relative to POLL = N + 1
 * (responder l replies at POLL+1+l; the Final sits one slot past the last one).
 * n=1 -> POLL+2, n=2 -> POLL+3.
 */
#define ULTRAWIDELOCK_FINAL_SLOT_OFFSET (ULTRAWIDELOCK_NUM_RESPONDERS + 1u)

#endif /* CRED_ROUND_CONFIG_H */
