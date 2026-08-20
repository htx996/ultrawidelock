/* SPDX-License-Identifier: ISC */

/** @file uwb_cirdiag.h — Per-reception CIA first-path/STS diagnostics stream (channel-impulse
 * Stage 0). The RX callback latches the DW3000's CIA diagnostic bank (Ipatov/STS first-path
 * index, F1..F3, power, peak, STS quality, xtal offset); task context emits it as one
 * "[ALAB] t=<us> ev=uwb.diag ..." line. OFF at boot; armed at runtime
 * (nRF `ultrawidelock cir on`, ESP32 rides the `lab on` gate). */

#ifndef ULTRAWIDELOCK_UWB_CIRDIAG_H_
#define ULTRAWIDELOCK_UWB_CIRDIAG_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No-op inlines when the feature is compiled out, so the latch call sites in the two RX shims
 * and the console commands cost nothing in a build that does not want the diagnostics. */
#if defined(ESP_PLATFORM)
#include "sdkconfig.h" /* CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG (Zephyr injects autoconf.h itself) */
#endif

/** @brief Amplitude half of struct uwb_cirdiag_ipatov::peak (IP_DIAG_0_PEAKAMP, bits [20:0]). */
#define UWB_CIRDIAG_PEAK_AMP(peak) ((uint32_t)(peak) & 0x1FFFFFu)

/** @brief Index half of struct uwb_cirdiag_ipatov::peak (IP_DIAG_0_PEAKLOC, bits [30:21]).
 * A whole sample index, unlike fp_index, which is Q10.6. */
#define UWB_CIRDIAG_PEAK_IDX(peak) (((uint32_t)(peak) >> 21) & 0x3FFu)

/** @brief Fractional bits in struct uwb_cirdiag_ipatov::fp_index, so a consumer can reach the
 * whole sample index as (fp_index >> UWB_CIRDIAG_FP_FRAC_BITS) without re-deriving the format. */
#define UWB_CIRDIAG_FP_FRAC_BITS 6u

/**
 * @brief The Ipatov scalars of the latest latched reception, for a classifier.
 *
 * Field for field from dwt_rxdiag_t, and deliberately NOT struct ultrawidelock_ml_cia even
 * though the two are the same numbers: ultrawidelock_uwb is the lower layer and must
 * not acquire a dependency on ultrawidelock_ml to hand out registers it already holds. The
 * caller copies across by name, which is checkable by eye — see
 * apps/dwm3001cdk-lock/src/main.c, and see ultrawidelock_ml.h on why same-typed integers are
 * passed as a struct rather than positionally.
 *
 * EVERY FIELD HERE IS FREE, and that is why fp_index and peak were added rather
 * than left to a second read. dwt_readdiagnostics() fills the whole CIA bank in
 * one burst, so these two were already in g_diag and already on the `ev=uwb.diag`
 * line as ipfp/ippk — including under SUMMARY_LEAN, which drops the three
 * SEPARATE reads (SYS_CFG, STS quality, STS status) and nothing out of the burst.
 * Widening this struct therefore costs no SPI, no chip-side configuration and
 * nothing on the ranging deadline; see the SUMMARY_LEAN Kconfig help.
 */
struct uwb_cirdiag_ipatov {
	uint32_t f1;          /**< dwt_rxdiag_t::ipatovF1 */
	uint32_t f2;          /**< dwt_rxdiag_t::ipatovF2 */
	uint32_t f3;          /**< dwt_rxdiag_t::ipatovF3 */
	uint32_t power;       /**< dwt_rxdiag_t::ipatovPower, a 2^17-scaled area */
	uint16_t accum_count; /**< dwt_rxdiag_t::ipatovAccumCount */
	/**
	 * dwt_rxdiag_t::ipatovFpIndex — the first-path sample index, Q10.6 (six
	 * fractional bits), as the register holds it. Handed over unconverted
	 * because the integer part and the fractional part answer different
	 * questions and this layer does not know which the caller wants:
	 * uwb_cirdiag.c's own window centring takes >> 6, while a training set
	 * wants the sub-sample position too.
	 */
	uint16_t fp_index;
	/**
	 * dwt_rxdiag_t::ipatovPeak — the packed peak sample, index in bits
	 * [30:21] and amplitude in bits [20:0] (IP_DIAG_0_PEAKLOC/PEAKAMP).
	 * PACKED, NOT SPLIT, for the same reason fp_index is not converted: the
	 * split is a mask the consumer can apply and this layer would only be
	 * guessing which half is wanted. UWB_CIRDIAG_PEAK_AMP/IDX below do it.
	 */
	uint32_t peak;
	/**
	 * Capture counter at the moment of the read. Monotonic, and the caller's
	 * only way to tell a fresh reception from the same one read twice: the
	 * latch is latest-wins with no queue, so re-reading without checking this
	 * would feed one reception's channel to several ranging rounds and let a
	 * single obstructed sample carry a whole median window.
	 */
	uint32_t n;
};

#if defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG)

/**
 * @brief Copy the latest latched Ipatov scalars out, under the seqlock.
 *
 * Task context, like uwb_cirdiag_flush(), and independent of it: the flush is
 * one-shot on a pending latch while this one always returns the newest snapshot.
 * A consumer that also wants the [ALAB] line gets both, and neither consumes the
 * other's state.
 *
 * @param out filled only on success.
 * @return false if the stream is disarmed, nothing has been captured yet, or the
 *         seqlock could not settle in three tries. Also false when the CIA read
 *         produced a zero accumulator count or channel area, which is a failed
 *         read rather than a very weak channel — ultrawidelock_ml_los_features() rejects
 *         the same condition, and the training data drops those receptions.
 */
bool uwb_cirdiag_last_ipatov(struct uwb_cirdiag_ipatov *out);

/** @brief Arm or disarm the summary stream. Safe any time, even before the chip is probed: the
 * chip-side CIA logging enable happens lazily on the first armed reception. */
void uwb_cirdiag_set_enabled(bool on);

/** @brief Whether the summary stream is currently armed. */
bool uwb_cirdiag_enabled(void);

/** @brief Arm or disarm the windowed-CIR dump (Stage 1): while armed, a ~64-tap Ipatov window
 * around the first path is buffered to RAM — NOT printed, because printing on the RX path stalls
 * ranging. Disarming drains the buffer to `ev=uwb.cir` lines off the ranging hot path, so a live
 * walk-up still unlocks while capturing. Arming implies the summary stream too; disarming leaves
 * it. Costs one CIR SPI read per non-POLL reception (see uwb_cirdiag_capture) + a burst of serial
 * lines on disarm. Safe any time (the read only runs inside a live armed reception). */
void uwb_cirdiag_dump_set_enabled(bool on);

/** @brief Whether the windowed-CIR dump is currently armed. */
bool uwb_cirdiag_dump_enabled(void);

/** @brief Number of CIR windows currently buffered awaiting a drain (0..CIRDIAG_RING_RECS).
 * The drain on dump disarm empties it back to 0. Exposed mainly for tests. */
uint32_t uwb_cirdiag_ring_count(void);

/**
 * @brief Latch the CIA diagnostics of the reception just serviced (latest wins).
 * Call from the RX-good callback, after the re-arm/decode work — it costs one ~220-byte SPI
 * read. No-op unless armed.
 * @param status RX callback status word (0 if the callback data was NULL).
 * @param datalength RX frame length (0 if the callback data was NULL).
 * @param deadline_pending Whether the radio is busy again: the windowed-CIR read (~260 us of
 * SPI plus dwt_readcir's own ACC clock forcing) happens only when this is false, because an
 * armed dump inside a live block cost every range of the walk-up on the bench. The summary
 * read is taken either way (bench-proven harmless). NOTE this says nothing about WHICH
 * reception is being serviced: the summary call site runs after the re-arm and passes true
 * unconditionally, Final included. A gate that read it as "is this the Final" latched zero
 * receptions across a whole walk while ranging ran clean (2026-08-07); that is what is_final
 * is for.
 * @param is_final The reception being serviced is the Final — sample
 * ccc_shim_rx_awaiting_final() BEFORE chaining to the blob's RX handler, because the arm
 * consumes the flag. Read only by CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_FINAL_ONLY: one latch
 * per ranging block, on the reception with the ~192 ms inter-block gap behind it.
 * @return true if a snapshot was latched (caller should schedule a flush).
 */
bool uwb_cirdiag_capture(uint32_t status, uint16_t datalength, bool deadline_pending,
			 bool is_final);

/** @brief Emit the latched snapshot as one [ALAB] line, if any is pending. Task context only —
 * never call on the RX event path. */
void uwb_cirdiag_flush(void);

/** @brief Whether this ranging block should take a windowed-CIR read. Call once per Final, before
 * chaining to the blob's RX handler: it advances an internal counter, so it returns true on one
 * Final in CIRDIAG_CIR_EVERY and false on the rest. False whenever the dump is disarmed or the
 * chip-side CIA logging has not been enabled yet. Reading every block returns real CIR but costs
 * every range of the walk-up (bench run 5); this is the throttle that buys both. */
bool uwb_cirdiag_window_due(void);

/** @brief One-shot accumulator read diagnostic, for chasing a windowed-CIR dump that returns
 * non-physical taps. Reads the same 8-tap burst at three distinct accumulator offsets, then
 * repeats the first, then repeats it once more in DWT_CIR_READ_FULL so the raw 24-bit words are
 * visible before MID's sign-extend/shift/saturate. Emits plain `cir.probe:` lines (deliberately
 * NOT the [ALAB] prefix, so
 *
 * Reads as: passes 0..2 identical means the sample offset is being ignored; pass 0 differing from
 * pass 3 at the same offset means the read is racing something else on the bus; clk without the
 * ACC enable bits means dwt_readcir's own clock forcing did not stick.
 *
 * Console/task context only, and only worth running with the radio idle — it is several SPI
 * transactions with no regard for ranging deadlines. No-op until the chip-side CIA logging has
 * been enabled on the RX path (arm the stream and take one reception first). */
void uwb_cirdiag_probe(void);

#else

/**
 * Stub: set_enabled is a no-op.
 */
static inline void uwb_cirdiag_set_enabled(bool on)
{
	(void)on;
}
/**
 * Stub: enabled returns false.
 */
static inline bool uwb_cirdiag_enabled(void)
{
	return false;
}
/**
 * Stub: dump_set_enabled is a no-op.
 */
static inline void uwb_cirdiag_dump_set_enabled(bool on)
{
	(void)on;
}
/**
 * Stub: dump_enabled returns false.
 */
static inline bool uwb_cirdiag_dump_enabled(void)
{
	return false;
}
/**
 * Stub: ring_count returns 0.
 */
static inline uint32_t uwb_cirdiag_ring_count(void)
{
	return 0u;
}
/**
 * Stub: capture returns false.
 */
static inline bool uwb_cirdiag_capture(uint32_t status, uint16_t datalength, bool deadline_pending,
				       bool is_final)
{
	(void)status;
	(void)datalength;
	(void)deadline_pending;
	(void)is_final;
	return false;
}
/**
 * Stub: flush is a no-op.
 */
static inline void uwb_cirdiag_flush(void)
{
}
/**
 * Stub: window_due returns false.
 */
static inline bool uwb_cirdiag_window_due(void)
{
	return false;
}
/**
 * Stub: probe is a no-op.
 */
static inline void uwb_cirdiag_probe(void)
{
}
/**
 * Stub: last_ipatov returns false and writes nothing.
 */
static inline bool uwb_cirdiag_last_ipatov(struct uwb_cirdiag_ipatov *out)
{
	(void)out;
	return false;
}

#endif /* CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG */

#ifdef __cplusplus
}
#endif

#endif /* ULTRAWIDELOCK_UWB_CIRDIAG_H_ */
