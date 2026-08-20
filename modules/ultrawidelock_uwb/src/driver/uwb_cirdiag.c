/* SPDX-License-Identifier: ISC */

/** @file uwb_cirdiag.c — CIA RX-diagnostics latch + [ALAB] emitter (channel-impulse Stage 0/1).
 *
 * Split the work across the two contexts the ALAB contract demands: the RX callback only
 * latches registers into a snapshot (uwb_cirdiag_capture, plain stores + one SPI read), and a
 * task-side uwb_cirdiag_flush formats/prints the line. On the nRF the flush runs on the
 * sysworkq (uwb_rxdiag.c submits it); on the ESP32 the pinned ISR-service task calls it after
 * its IRQ drain loop, so capture and flush are sequential there. A seqlock covers the
 * one real race (nRF: a new capture preempting a flush mid-copy): torn snapshots are dropped,
 * the next reception re-latches.
 *
 * The windowed-CIR dump (independently armed) also reads a fixed window of
 * Ipatov complex taps centred on the first-path index. Taps are never printed
 * on the RX/flush path (~64 serial lines would stall a walk-up): flush appends
 * windows to a RAM ring, drained to `ev=uwb.cir` lines only when the dump is
 * disarmed, in task context. The window READ itself only happens on a Final
 * (deadline_pending), before the SP0 re-arm (ccc_shim_rx_awaiting_final), and
 * decimated to one Final in CIRDIAG_CIR_EVERY -- each of those three gates was
 * measured necessary to keep ranging alive while capturing.
 */

#include "uwb_cirdiag.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deca_device_api.h>

#include "fira_session.h" /* fira_session_last_range — the DS-TWR distance, for `d=` below */
/* ultrawidelock_printf — platform print, same sink as the [ALAB] trace */
#include "ultrawidelock_log.h"
#include "ultrawidelock_port.h"     /* ultrawidelock_uptime_us — the [ALAB] timebase */

/** @brief SYS_CFG + STS-packet-config (CP_SPC): the mode of THIS reception (0=SP0..3=SP3).
 * Same trick as uwb_rxdiag.c's RXDIAG_CP_SPC, duplicated to keep this unit freestanding. */
#define CIRDIAG_SYS_CFG   0x10UL
#define CIRDIAG_CP_SPC(v) (unsigned)(((v) >> 12) & 0x3u)

/** @brief Windowed-CIR dump width, in complex taps, centred on the first-path index. 64 is
 * enough to carry the leading edge + early multipath that separates inside/outside a door,
 * while staying cheap on serial (~64 lines) and RAM (256 B in DWT_CIR_READ_MID: 1 word/tap). */
#define CIRDIAG_CIR_WIN 64u

/** @brief Take a window on one Final in every N. The read is the most expensive thing this unit
 * does — the driver walks the accumulator in CHUNK_CIR_NB_SAMP-sample chunks, three SPI
 * transactions each — and doing it on every block cost every range of the walk-up (bench run 5:
 * 16/16 windows came back as real CIR, and not one block produced a distance). Sampling every
 * fourth block still fills the ring across an approach while leaving three blocks in four
 * untouched. */
#define CIRDIAG_CIR_EVERY 4u

/** @brief ipatovFpIndex is Q10.6 (6 fractional bits); the integer sample index is the high bits.
 * Aliased to the header's macro rather than repeated: the same format is now part of this unit's
 * published contract (struct uwb_cirdiag_ipatov::fp_index), and two copies of a shift can drift. */
#define CIRDIAG_FP_FRAC_BITS UWB_CIRDIAG_FP_FRAC_BITS

/** @brief CLK_CTRL_ID. dwt_readcir ORs the ACC_MCLK_EN|ACC_CLK_EN bits in here on every call and
 * never clears them; the probe reads it back so a failed force shows up as data, not inference. */
#define CIRDIAG_CLK_CTRL 0x110004UL

/** @brief Taps per probe pass. Small enough that three passes at different offsets stay cheap;
 * the point is comparing them, not the width. */
#define CIRDIAG_PROBE_TAPS 8u

/** @brief Runtime arm state (console-toggled; OFF at boot). */
static volatile bool g_on;

/** @brief Windowed-CIR dump arm (independent of g_on; OFF at boot). */
static volatile bool g_dump;

/** @brief Chip-side CIA diagnostic logging enabled (lazily, on the RX path). */
static bool g_cia_armed;

/** @brief Latched snapshot of the most recent reception (latest wins). */
static dwt_rxdiag_t g_diag;
static int64_t g_t_us;
static uint32_t g_status;
static uint16_t g_len;
static unsigned g_sp;
static int16_t g_sts_qual;
static int32_t g_sts_qual_ret;
static uint16_t g_sts_stat;
static int32_t g_sts_stat_ret;
static uint32_t g_n;

/** @brief DWT cycles the last dwt_readdiagnostics() took. Latched with the rest of the
 * snapshot and emitted as `rdcyc`; see uwb_cirdiag_capture() for why it is measured. */
static uint32_t g_rd_cyc;

/** @brief Free-running DWT cycle counter (ports/zephyr/dw3000/dw3000_hw.c:49). Declared
 * here rather than pulled in through a header for the same reason CIRDIAG_SYS_CFG is
 * open-coded above: this unit stays freestanding. */
uint32_t dw3000_dwt_cyccnt(void);

/** @brief Windowed-CIR snapshot: DWT_CIR_READ_MID packs each tap as two int16 (real, imag) in
 * one word, so g_cir doubles as an int16[2*WIN] pair array. Scratch only: the capture reads into
 * it and appends straight to the ring, so it holds nothing between receptions. */
static uint32_t g_cir[CIRDIAG_CIR_WIN];

/** @brief Summary decimation tick. Separate from g_win_tick, which counts Finals only. */
static uint32_t g_sum_tick;

/** @brief True if this reception's summary is due. Always true at the default N of 1, so
 * every existing target keeps latching every reception. */
static bool cirdiag_summary_due(void)
{
#if defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_EVERY) &&                                     \
	CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_EVERY > 1
	return (g_sum_tick++ % (uint32_t)CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_EVERY) == 0u;
#else
	g_sum_tick++;
	return true;
#endif
}

/** @brief Absolute Ipatov sample index of tap 0 for a window centred on the latched first path,
 * clamped into the valid accumulator span [0, DWT_CIR_LEN_IP_PRF64 - WIN]. */
static uint16_t cirdiag_window_base(void)
{
	uint16_t fp_int = (uint16_t)(g_diag.ipatovFpIndex >> CIRDIAG_FP_FRAC_BITS);
	uint16_t base = (fp_int > (CIRDIAG_CIR_WIN / 2u)) ? (fp_int - CIRDIAG_CIR_WIN / 2u) : 0u;

	if ((uint32_t)base + CIRDIAG_CIR_WIN > (uint32_t)DWT_CIR_LEN_IP_PRF64) {
		base = (uint16_t)(DWT_CIR_LEN_IP_PRF64 - CIRDIAG_CIR_WIN);
	}
	return base;
}

/** @brief Seqlock around the snapshot: odd while the RX path writes; the flush copies only
 * between two equal even reads. */
static volatile uint32_t g_seq;
static volatile bool g_pending;

/** @brief Deferred-dump ring. The capture appends each armed reception's window here (a cheap
 * memcpy, no UART) instead of printing it on the ranging path; the taps are emitted only on disarm
 * (cirdiag_drain), off that path, so a live walk-up still unlocks while capturing. Overwrites
 * oldest, so it holds the last RECS receptions — the near-door end of an approach (~272 B/record).
 * Single-producer (the RX capture, gated on g_dump) / single-consumer (drain-after-disarm): the
 * drain runs only after g_dump is cleared, so the two never overlap.
 */
#define CIRDIAG_RING_RECS 16u
/**
 * Channel impulse response diagnostic record: timestamp (microseconds), sample count, base index,
 * and tap array (I/Q magnitude or signed values) for post-processing.
 */
struct cirdiag_rec {
	int64_t t_us;
	uint32_t n;
	uint16_t base;
	uint32_t taps[CIRDIAG_CIR_WIN];
};
static struct cirdiag_rec g_ring[CIRDIAG_RING_RECS];
static uint32_t g_ring_head;  /* next write slot (mod RECS) */
static uint32_t g_ring_count; /* valid records held (<= RECS) */

/** @brief Emit every buffered window as `ev=uwb.cir` lines (oldest first), then empty the ring.
 * Called on dump disarm, off the ranging path. Blocking: up to RECS*WIN serial lines, and it
 * SLEEPS between records when CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_DRAIN_PACE_MS is set, so like the
 * rest of the disarm path this is thread context only, never an ISR. */
static void cirdiag_drain(void)
{
	uint32_t start = (g_ring_head + CIRDIAG_RING_RECS - g_ring_count) % CIRDIAG_RING_RECS;

	for (uint32_t k = 0; k < g_ring_count; k++) {
		const struct cirdiag_rec *r = &g_ring[(start + k) % CIRDIAG_RING_RECS];
		const int16_t *s = (const int16_t *)r->taps;

		for (unsigned i = 0; i < CIRDIAG_CIR_WIN; i++) {
			ultrawidelock_printf("[ALAB] t=%lld ev=uwb.cir n=%u i=%u re=%d im=%d\n",
				   (long long)r->t_us, (unsigned)r->n, (unsigned)(r->base + i),
				   (int)s[2u * i], (int)s[2u * i + 1u]);
		}
#if defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_DRAIN_PACE_MS) &&                                     \
	CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_DRAIN_PACE_MS > 0
		/* Let the console catch up between records. Default 0 leaves every
		 * existing target byte-identical; the DWM3001CDK sets it because its
		 * RTT buffer is 8 KB in NO_BLOCK_SKIP mode and this loop writes ~46 KB
		 * faster than probe-rs drains it, discarding the newest lines with no
		 * indication that anything was lost. See the Kconfig help. */
		ultrawidelock_sleep_ms(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_DRAIN_PACE_MS);
#endif
	}
	g_ring_count = 0;
	g_ring_head = 0;
}

/** @brief Append one window to the ring, overwriting the oldest.
 *
 * Called from the capture that just read the accumulator, NOT from the deferred flush. The flush
 * ran on a work item and read a "window valid" flag that every subsequent capture cleared,
 * so on any target where a second frame lands inside one workqueue latency the append never fired
 * at all and the ring stayed empty. That is every CDK round:
 * CONFIG_ULTRAWIDELOCK_UWB_FINAL_SNAPSHOT arms a delayed SP0 RX at the Final_Data slot, so a
 * reception follows the Final by ~1 slot. Appending here also makes the single-producer claim above
 * true, since only this path is gated on g_dump.
 */
static void cirdiag_ring_append(int64_t t_us, uint32_t n, uint16_t base, const uint32_t *taps)
{
	struct cirdiag_rec *r = &g_ring[g_ring_head];

	r->t_us = t_us;
	r->n = n;
	r->base = base;
	memcpy(r->taps, taps, sizeof(r->taps));
	g_ring_head = (g_ring_head + 1u) % CIRDIAG_RING_RECS;
	if (g_ring_count < CIRDIAG_RING_RECS) {
		g_ring_count++;
	}
}

/**
 * Return the count of CIR windows currently in the ring buffer (0 to CIRDIAG_RING_RECS).
 */
uint32_t uwb_cirdiag_ring_count(void)
{
	return g_ring_count;
}

/**
 * Enable or disable CIA diagnostic capture globally.
 */
void uwb_cirdiag_set_enabled(bool on)
{
	g_on = on;
}

/**
 * Return true if CIA diagnostic capture is enabled.
 */
bool uwb_cirdiag_enabled(void)
{
	return g_on;
}

/**
 * Arm or disarm CIR window capture. When arming, also arms the summary diagnostics. When disarming,
 * drains all buffered windows to the console via ev=uwb.cir lines and clears the ring.
 */
void uwb_cirdiag_dump_set_enabled(bool on)
{
	/* The window read needs the summary path armed too (it supplies the first-path index and
	 * the lazy CIA-logging enable). Arming dump implies arming the summary; disarming dump
	 * leaves the summary as-is. Disarm is also the safe moment to drain the buffered windows:
	 * it runs in console/task context after the walk-up, so the burst of serial lines never
	 * touches the ranging path. Clear g_dump first so no further capture appends mid-drain. */
	if (on) {
		g_on = true;
		g_dump = true;
	} else {
		g_dump = false;
		cirdiag_drain();
	}
}

/**
 * Return true if CIR window dump is armed (enabled and CIA logging armed).
 */
bool uwb_cirdiag_dump_enabled(void)
{
	return g_dump;
}

/**
 * Capture one reception's CIR diagnostic snapshot: arm CIA logging on first RX, then latch the
 * status, frame length, STS quality/status, and (if window dump is enabled and the radio is idle) a
 * fixed-size Ipatov-centred CIR window. Returns true if capture succeeded; false on first RX or if
 * already pending. Seqlock-protected; safe to call from RX callback.
 */
bool uwb_cirdiag_capture(uint32_t status, uint16_t datalength, bool deadline_pending, bool is_final)
{
	if (!g_on) {
		return false;
	}
	if (!g_cia_armed) {
		/* First reception after arming: driver init leaves cia_enable_mask 0, i.e. the
		 * CIA_CONF diagnostic-off bit set and the IP/STS diag banks unpopulated — enable
		 * logging. LEAN narrows that to LOG_ALL, dropping the LOG_MAX double-buffer copy
		 * set: nothing the scalar feature set reads lives in it, and the chip-side cost of
		 * populating it is on the same budget as the ranging deadline. Done here rather
		 * than in set_enabled so the console toggle is safe before the chip is probed: this
		 * path only runs inside a live RX callback. THIS reception was demodulated with
		 * logging still reduced, so skip it; the next one carries a fully populated bank.
		 * Sticky until chip reset — only configciadiag and the 16-bit antenna-delay field
		 * write CIA_CONF, so a later dwt_configure() does not undo it (hence never
		 * re-cleared on `off`). */
#if defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_LEAN)
		dwt_configciadiag((uint8_t)DW_CIA_DIAG_LOG_ALL);
#else
		dwt_configciadiag((uint8_t)(DW_CIA_DIAG_LOG_ALL | DW_CIA_DIAG_LOG_MAX));
#endif
		g_cia_armed = true;
		return false;
	}
	/* A window read supersedes the decimator: those are already one Final in
	 * CIRDIAG_CIR_EVERY, and skipping one drops the only reception in the block with taps.
	 * Everything else is a summary, and on a board where the diagnostics compete with the
	 * arm deadline the cheapest fix is to take fewer of them. */
#if defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_FINAL_ONLY)
	/* A classifier consumes one class per approach sample, and there is one
	 * approach sample per ranging block, so latch on the Final alone: that puts
	 * the ~972 us read in the ~192 ms gap instead of ~2 ms ahead of a Final.
	 * Keyed on is_final, NOT deadline_pending -- the summary call site passes
	 * deadline_pending=true for every reception including the Final (it runs
	 * after the re-arm), and the first mlgate walk (2026-08-07) proved a gate
	 * reading it latches nothing: ranging clean, zero [ALAB] lines. Checked
	 * before the decimator so the two do not multiply into one latch in 8
	 * blocks. */
	if (!is_final) {
		return false;
	}
#else
	(void)is_final;
#endif
	if (!(g_dump && !deadline_pending) && !cirdiag_summary_due()) {
		return false;
	}
	g_seq++; /* odd: writer active */
	g_t_us = ultrawidelock_uptime_us();
	g_status = status;
	g_len = datalength;
	/*
	 * Time the one SPI read that matters, in DWT cycles, and carry it out on the
	 * [ALAB] line as `rdcyc`. Raw cycles rather than microseconds because the
	 * conversion is a board constant this file does not own -- the boot banner
	 * prints it ("cyccnt cal: ... cyc/us") and the host divides.
	 *
	 * WHY MEASURE SOMETHING A COMMENT ALREADY CALLS SAFE. uwb_rxdiag.c takes this
	 * read AFTER arming the next window precisely so the POLL/Final deadlines are
	 * met first, and calls it "bench-proven safe". That ordering is what makes it
	 * safe, and it is sound. What has never had a number is the cost itself, and
	 * the cost is what decides whether a classifier can be fed on EVERY reception
	 * rather than one in CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_EVERY. A claim in a
	 * comment cannot answer that; a distribution can.
	 */
	{
		const uint32_t c0 = dw3000_dwt_cyccnt();

		dwt_readdiagnostics(&g_diag);
		g_rd_cyc = dw3000_dwt_cyccnt() - c0;
	}
#if !defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_LEAN)
	g_sp = CIRDIAG_CP_SPC(dwt_read_reg(CIRDIAG_SYS_CFG));
	g_sts_qual = 0;
	g_sts_qual_ret = dwt_readstsquality(&g_sts_qual, 0);
	g_sts_stat = 0;
	g_sts_stat_ret = dwt_readstsstatus(&g_sts_stat, 0);
#endif
	g_n++;
	if (g_dump && !deadline_pending) {
		/* Centre a fixed window on the integer first-path index, clamped into the valid
		 * Ipatov accumulator span [0, DWT_CIR_LEN_IP_PRF64 - WIN]. dwt_readcir forces the
		 * ACC clocks on itself; MID mode gives int16 real/imag with headroom for the early
		 * taps. deadline_pending false means the caller has established the radio is idle —
		 * on the ranging path that is the Final, sampled before the shim re-arms.
		 * Append here rather than from the flush: the window survives only until the next
		 * reception, which is sooner than the work item runs (see cirdiag_ring_append). */
		const uint16_t base = cirdiag_window_base();

		if (dwt_readcir(g_cir, DWT_ACC_IDX_IP_M, base, CIRDIAG_CIR_WIN, DWT_CIR_READ_MID) ==
		    DWT_SUCCESS) {
			cirdiag_ring_append(g_t_us, g_n, base, g_cir);
		}
	}
	g_seq++; /* even: stable */
	g_pending = true;
	return true;
}

bool uwb_cirdiag_last_ipatov(struct uwb_cirdiag_ipatov *out)
{
	if (out == NULL || !g_on) {
		return false;
	}

	for (int tries = 0; tries < 3; tries++) {
		uint32_t s0 = g_seq;

		if ((s0 & 1u) != 0u) {
			continue; /* writer active */
		}
		out->f1 = g_diag.ipatovF1;
		out->f2 = g_diag.ipatovF2;
		out->f3 = g_diag.ipatovF3;
		out->power = g_diag.ipatovPower;
		out->accum_count = g_diag.ipatovAccumCount;
		/* Inside the seqlock with the rest, not read afterwards: these come
		 * from the same dwt_readdiagnostics() burst as F1..F3, and a peak
		 * paired with another reception's first path is a ratio of two
		 * different channels that no consumer could detect. */
		out->fp_index = g_diag.ipatovFpIndex;
		out->peak = g_diag.ipatovPeak;
		out->n = g_n;
		if (g_seq != s0) {
			continue; /* a capture landed mid-copy — retry */
		}
		/* n == 0 is "nothing captured yet", and the two zero checks are a
		 * failed CIA read rather than a weak channel: both are divisors in
		 * ultrawidelock_ml_los_features(), which rejects the same condition. Checked
		 * after the seqlock settles so the values tested are the ones
		 * returned. */
		return out->n != 0u && out->accum_count != 0u && out->power != 0u;
	}
	return false;
}

/** @brief Decimation tick: the shims call this once per Final, so it advances per ranging block. */
static uint32_t g_win_tick;

/**
 * Return true on every CIRDIAG_CIR_EVERY-th call if capture is enabled, CIA logging is armed, and
 * window dump is armed; used to throttle window reads during streaming.
 */
bool uwb_cirdiag_window_due(void)
{
	if (!g_on || !g_dump || !g_cia_armed) {
		return false;
	}
	return (g_win_tick++ % CIRDIAG_CIR_EVERY) == 0u;
}

/**
 * Diagnostic: read and print the CIR at four sample offsets (three distinct addresses plus one
 * repeat) to verify addressing and detect non-determinism. Requires CIA logging armed (one
 * reception taken). Outputs one pass in MID mode (int16 real/imag) and one in FULL mode (raw
 * 24-bit) at the base offset.
 */
void uwb_cirdiag_probe(void)
{
	/* Offsets to sample, in taps, relative to the window base. The first three are distinct
	 * accumulator addresses; the fourth repeats the first. Reading the same address twice
	 * separates "the offset is ignored" from "the read is non-deterministic": if passes 0..2
	 * agree the addressing is dead, and if 0 and 3 disagree the read is racing something. */
	static const uint16_t offs[] = {0u, CIRDIAG_PROBE_TAPS, 2u * CIRDIAG_PROBE_TAPS, 0u};
	/* Sized for DWT_CIR_READ_FULL, which writes 6 bytes per tap (2 words) — MID uses half. */
	uint32_t buf[2u * CIRDIAG_PROBE_TAPS];
	uint16_t base;

	if (!g_cia_armed) {
		ultrawidelock_printf("cir.probe: not ready: the chip-side CIA enable happens on the "
			   "RX path, so arm the stream and take one reception first\n");
		return;
	}
	base = cirdiag_window_base();
	ultrawidelock_printf("cir.probe: base=%u fp=%u clk=%lu\n", (unsigned)base,
		   (unsigned)g_diag.ipatovFpIndex, (unsigned long)dwt_read_reg(CIRDIAG_CLK_CTRL));

	for (unsigned p = 0; p < (sizeof(offs) / sizeof(offs[0])); p++) {
		const int16_t *s = (const int16_t *)buf;
		uint16_t at = (uint16_t)(base + offs[p]);
		int rc;

		memset(buf, 0, sizeof(buf));
		rc = dwt_readcir(buf, DWT_ACC_IDX_IP_M, at, CIRDIAG_PROBE_TAPS, DWT_CIR_READ_MID);
		ultrawidelock_printf("cir.probe: pass=%u at=%u rc=%d clk=%lu\n", p, (unsigned)at, rc,
			   (unsigned long)dwt_read_reg(CIRDIAG_CLK_CTRL));
		for (unsigned i = 0; i < CIRDIAG_PROBE_TAPS; i++) {
			ultrawidelock_printf("cir.probe:   p=%u i=%u re=%d im=%d\n", p, i, (int)s[2u * i],
				   (int)s[2u * i + 1u]);
		}
	}

	/* One FULL-mode pass at the base: 24-bit real/imag straight off the wire, before the
	 * sign-extend/shift/saturate that MID applies. A blob that saturates to +-32767 in MID is
	 * unreadable; the raw words identify which memory the read actually landed on. */
	memset(buf, 0, sizeof(buf));
	{
		const uint8_t *b = (const uint8_t *)buf;
		int rc = dwt_readcir(buf, DWT_ACC_IDX_IP_M, base, CIRDIAG_PROBE_TAPS,
				     DWT_CIR_READ_FULL);

		ultrawidelock_printf("cir.probe: full at=%u rc=%d\n", (unsigned)base, rc);
		for (unsigned i = 0; i < CIRDIAG_PROBE_TAPS; i++) {
			const uint8_t *t = &b[6u * i];

			ultrawidelock_printf("cir.probe:   full i=%u re24=%lu im24=%lu\n", i,
				   (unsigned long)((uint32_t)t[0] | ((uint32_t)t[1] << 8) |
						   ((uint32_t)t[2] << 16)),
				   (unsigned long)((uint32_t)t[3] | ((uint32_t)t[4] << 8) |
						   ((uint32_t)t[5] << 16)));
		}
	}
}

/**
 * Format the DS-TWR distance as ` d=<cm> dage=<ms>`, or an empty string when there is none.
 *
 * WHY THE KEYS ARE ABSENT RATHER THAN ZERO OR -1, which is the same rule the LEAN summary
 * below states for its own fields: a parser cannot tell `d=0` meaning "measured zero" from
 * `d=0` meaning "no round has completed yet", and one of those is a data point while the
 * other is a lie. An absent key is neither.
 *
 * `dage` is not decoration. A DS-TWR round completes on its own cadence while diagnostics
 * are read per reception, so most receptions carry a range from a previous round, and how
 * stale it is decides whether the row belongs in a fitted set at all. The alternative the
 * host used before this existed -- scraping the bench TUI's rendered status line and
 * aligning on its reception counter -- had to drop 12 of 556 receptions that fell outside
 * any fresh line, and could not say how stale the ones it kept were.
 */
static void cirdiag_range_fields(char *buf, size_t buf_len)
{
	int32_t cm;
	int64_t age_ms;

	buf[0] = '\0';
	if (!fira_session_last_range(&cm, NULL, NULL, NULL, &age_ms)) {
		return;
	}
	(void)snprintf(buf, buf_len, " d=%d dage=%lld", (int)cm, (long long)age_ms);
}

/**
 * Emit the pending CIR snapshot: write the summary line ([ALAB] ev=uwb.diag) with Ipatov and STS
 * peak/power/quality fields. The window itself is not handled here — the capture appends it to the
 * ring, because it does not survive to this work item. Retry up to 3 times if the seqlock detects
 * concurrent capture. Idempotent.
 */
void uwb_cirdiag_flush(void)
{
	dwt_rxdiag_t d;
	int64_t t_us;
	uint32_t status, n, rd_cyc;
	unsigned sp;
	uint16_t len, sts_stat;
	int16_t sts_qual;
	int32_t sts_qual_ret, sts_stat_ret;
	/* " d=-2147483648 dage=-9223372036854775808" is 41 including the NUL, and the
	 * range is a centimetre count rather than either extreme; 48 is slack, not a
	 * guess, and snprintf truncates rather than overruns if it were ever wrong. */
	char rng[48];

	if (!g_pending) {
		return;
	}
	g_pending = false;

	for (int tries = 0; tries < 3; tries++) {
		uint32_t s0 = g_seq;

		if ((s0 & 1u) != 0u) {
			continue;
		}
		memcpy(&d, &g_diag, sizeof(d));
		t_us = g_t_us;
		status = g_status;
		len = g_len;
		sp = g_sp;
		sts_qual = g_sts_qual;
		sts_qual_ret = g_sts_qual_ret;
		sts_stat = g_sts_stat;
		sts_stat_ret = g_sts_stat_ret;
		n = g_n;
		rd_cyc = g_rd_cyc;
		if (g_seq != s0) {
			continue; /* a capture landed mid-copy — retry */
		}
		/* Read after the seqlock settles: the range is not part of the latched
		 * snapshot, so reading it earlier would pair a retried reception with a
		 * distance from the attempt that lost. */
		cirdiag_range_fields(rng, sizeof(rng));
		/* One line, all-decimal for ultrawidelock_lab.py's k=v parser. Keys: n capture#,
		 * sp STS mode, len/st the callback frame info; ip.. Ipatov and s.. STS CIR
		 * diagnostics — fp first-path index (Q10.6), pk peak (idx[30:21]|amp[20:0]),
		 * pw channel power, f1..f3 first-path amplitudes, ac accumulated symbols;
		 * sq/sqr STS quality index + verdict, ss/ssr STS status bits + verdict,
		 * xtal remote crystal offset, cd1 CIA_DIAG_1. */
#if defined(CONFIG_ULTRAWIDELOCK_UWB_CIRDIAG_SUMMARY_LEAN)
		/* LEAN: the keys for fields this build never read are ABSENT, not zero. A parser
		 * cannot tell `sq=0` meaning "STS quality measured zero" from `sq=0` meaning "we
		 * did not look", and one of those is a data point while the other is a lie. */
		ultrawidelock_printf("[ALAB] t=%lld ev=uwb.diag n=%u len=%u st=%u "
			   "ipfp=%u ippk=%u ippw=%u ipf1=%u ipf2=%u ipf3=%u ipac=%u "
			   "xtal=%d cd1=%u rdcyc=%u%s\n",
			   (long long)t_us, (unsigned)n, (unsigned)len, (unsigned)status,
			   (unsigned)d.ipatovFpIndex, (unsigned)d.ipatovPeak,
			   (unsigned)d.ipatovPower, (unsigned)d.ipatovF1, (unsigned)d.ipatovF2,
			   (unsigned)d.ipatovF3, (unsigned)d.ipatovAccumCount, (int)d.xtalOffset,
			   (unsigned)d.ciaDiag1, (unsigned)rd_cyc, rng);
		(void)sp;
		(void)sts_qual;
		(void)sts_qual_ret;
		(void)sts_stat;
		(void)sts_stat_ret;
#else
		ultrawidelock_printf("[ALAB] t=%lld ev=uwb.diag n=%u sp=%u len=%u st=%u "
			   "ipfp=%u ippk=%u ippw=%u ipf1=%u ipf2=%u ipf3=%u ipac=%u "
			   "sfp=%u spk=%u spw=%u sf1=%u sf2=%u sf3=%u sac=%u "
			   "sq=%d sqr=%d ss=%u ssr=%d xtal=%d cd1=%u rdcyc=%u%s\n",
			   (long long)t_us, (unsigned)n, sp, (unsigned)len, (unsigned)status,
			   (unsigned)d.ipatovFpIndex, (unsigned)d.ipatovPeak,
			   (unsigned)d.ipatovPower, (unsigned)d.ipatovF1, (unsigned)d.ipatovF2,
			   (unsigned)d.ipatovF3, (unsigned)d.ipatovAccumCount,
			   (unsigned)d.stsFpIndex, (unsigned)d.stsPeak, (unsigned)d.stsPower,
			   (unsigned)d.stsF1, (unsigned)d.stsF2, (unsigned)d.stsF3,
			   (unsigned)d.stsAccumCount, (int)sts_qual, (int)sts_qual_ret,
			   (unsigned)sts_stat, (int)sts_stat_ret, (int)d.xtalOffset,
			   (unsigned)d.ciaDiag1, (unsigned)rd_cyc, rng);
#endif
		return;
	}
	/* Persistently torn — drop this snapshot; the next reception re-latches. */
}
