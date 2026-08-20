/* SPDX-License-Identifier: ISC */

/** @file fira_session.c — Range + URSK store for the CCC Pre-POLL responder. */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ultrawidelock_port.h"

#include "fira_session.h"

#if defined(CONFIG_ULTRAWIDELOCK_CRED)
#include "cred_kdf.h"

/* credential URSK stash: the Pre-POLL decode reads it to derive the CCC STS. */
static bool g_have_ursk;
static uint8_t g_ursk[ULTRAWIDELOCK_URSK_LEN];
static uint32_t g_session_id;

void fira_session_set_provisioned_ursk(const uint8_t *ursk)
{
	if (ursk == NULL) {
		g_have_ursk = false;
		memset(g_ursk, 0, sizeof(g_ursk));
		return;
	}
	memcpy(g_ursk, ursk, sizeof(g_ursk));
	g_have_ursk = true;
}

const uint8_t *fira_session_get_ursk(void)
{
	return g_have_ursk ? g_ursk : NULL;
}

void fira_session_set_id(uint32_t session_id)
{
	g_session_id = session_id;
}

uint32_t fira_session_id(void)
{
	return g_session_id;
}

uint32_t fira_session_current_slot(void)
{
	/* The live index comes from the air, so this diagnostic counter is inert and reads 0. */
	return 0u;
}
#endif /* CONFIG_ULTRAWIDELOCK_CRED */

/* Last valid DS-TWR range, latched for the credential mRangingData seam and the shell. */
static bool g_have_range;
static int32_t g_last_range_cm;
static uint16_t g_last_range_addr;
static uint8_t g_last_range_nlos;
static uint32_t g_last_range_block;
static int64_t g_last_range_ms;
/* Layer 4: run length of consecutive plausible, mutually consistent blocks. */
static uint8_t g_range_trust;
#if defined(CONFIG_ULTRAWIDELOCK_CRED)
/* Layer 2 evidence for the block currently being latched, published by the RX
 * path just before the latch. Cleared on consumption so a latch that arrives
 * without fresh diagnostics cannot inherit the previous block's good verdict. */
static bool g_pending_sts_valid;
static bool g_pending_sts_ok;
static int16_t g_pending_sts_quality;
/* Consecutive blocks whose STS passed, capped at K, and the worst quality index
 * within that good run. Counting rather than AND-ing over the whole agreement
 * run matters: an unbounded AND would let one marginal block poison a stationary
 * phone forever, since the run only restarts on a distance jump. A count of K
 * says "the last K blocks were all well-correlated", which is the same window
 * layer 4 uses to decide trust, and it heals after K clean blocks. */
static uint8_t g_sts_good_run;
static int16_t g_sts_quality_min;
#endif
/* Post-challenge freshness epoch. Written after every accepted latch, so a
 * reader that observes a newer generation also observes the associated range. */
static volatile uint32_t g_range_generation;
#if defined(CONFIG_ULTRAWIDELOCK_CRED)
/* Fired (from the UWB RX path) after each accepted latch, so the unlock seam
 * can wake on fresh ranges instead of polling. Keep the callback tiny. */
static void (*g_range_listener)(void);
#endif

/** @brief Fetch the most recent valid DS-TWR range; out-params optional (NULL to skip). */
bool fira_session_last_range(int32_t *cm_out, uint16_t *addr_out, uint8_t *nlos_out,
			     uint32_t *block_out, int64_t *age_ms_out)
{
	if (!g_have_range) {
		return false;
	}
	if (cm_out) {
		*cm_out = g_last_range_cm;
	}
	if (addr_out) {
		*addr_out = g_last_range_addr;
	}
	if (nlos_out) {
		*nlos_out = g_last_range_nlos;
	}
	if (block_out) {
		*block_out = g_last_range_block;
	}
	if (age_ms_out) {
		*age_ms_out = ultrawidelock_uptime_ms() - g_last_range_ms;
	}
	return true;
}

#if defined(CONFIG_ULTRAWIDELOCK_CRED)
bool fira_session_range_plausible(int32_t cm)
{
	return cm >= -FIRA_RANGE_NEG_TOL_CM && cm <= FIRA_RANGE_MAX_CM;
}

bool fira_session_sts_quality_ok(int32_t driver_verdict, int16_t quality_index)
{
	return driver_verdict >= 0 && quality_index >= FIRA_STS_QUALITY_MIN;
}

bool fira_session_range_trusted(void)
{
	return g_range_trust >= FIRA_RANGE_TRUST_K;
}

uint8_t fira_session_trust_level(void)
{
	return g_range_trust;
}

void fira_session_reset_ranges(void)
{
	g_have_range = false;
	g_range_trust = 0u;
	g_pending_sts_valid = false;
	g_sts_good_run = 0u;
	g_sts_quality_min = 0;
}

void fira_session_set_ccc_range_sts(int32_t driver_verdict, int16_t quality_index)
{
	g_pending_sts_ok = fira_session_sts_quality_ok(driver_verdict, quality_index);
	g_pending_sts_quality = quality_index;
	g_pending_sts_valid = true;
}

bool fira_session_last_range_integrity(struct fira_range_integrity *out)
{
	if (!g_have_range) {
		return false;
	}
	if (out != NULL) {
		/* Same K as layer 4, so "trusted" and "well-correlated" describe the
		 * same window of blocks rather than two differently-sized ones. */
		out->sts_ok = g_sts_good_run >= FIRA_RANGE_TRUST_K;
		out->sts_quality = g_sts_quality_min;
		out->trust_level = g_range_trust;
	}
	return true;
}

uint32_t fira_session_range_generation(void)
{
	return g_range_generation;
}

void fira_session_set_range_listener(void (*cb)(void))
{
	g_range_listener = cb;
}

void fira_session_set_ccc_range_cm(int32_t cm, uint32_t block)
{
	int32_t delta;
	/* Consume this block's layer-2 evidence up front so every exit below leaves
	 * nothing behind for the next block to inherit. No evidence reads as a
	 * failed STS: a producer that cannot say the timestamp was well-correlated
	 * has not said that it was. */
	bool sts_ok = g_pending_sts_valid && g_pending_sts_ok;
	int16_t sts_quality = g_pending_sts_valid ? g_pending_sts_quality : 0;

	g_pending_sts_valid = false;

	/* Layer 1: reject a physically impossible / out-of-envelope measurement
	 * outright — never let a spoofed negative masquerade as 0 cm. The prior
	 * good range and its trust stay put; an implausible block earns none. */
	if (!fira_session_range_plausible(cm)) {
		g_range_trust = 0u;
		g_sts_good_run = 0u;
		return;
	}
	/* Legitimate point-blank reads dip slightly negative (calibration slop). */
	if (cm < 0) {
		cm = 0;
	}

	/* Layer 4: consecutive plausible blocks that agree build trust; a jump
	 * restarts the run. The block still latches, so telemetry stays live. */
	delta = (cm >= g_last_range_cm) ? (cm - g_last_range_cm) : (g_last_range_cm - cm);
	if (g_have_range && delta <= FIRA_RANGE_SPREAD_CM) {
		if (g_range_trust < FIRA_RANGE_TRUST_K) {
			g_range_trust++;
		}
	} else {
		/* First agreeing sample of a (new) run. The consensus restarted, so its
		 * layer-2 evidence has to be rebuilt from scratch alongside it. */
		g_range_trust = 1u;
		g_sts_good_run = 0u;
	}

	/* Layer 2 accounting, independent of whether the distances agreed: a single
	 * suspect block drops the count to zero, and K clean blocks are needed to
	 * earn it back. The quality floor tracks the worst block still inside that
	 * good run, so it describes the same blocks the verdict does. */
	if (!sts_ok) {
		g_sts_good_run = 0u;
		g_sts_quality_min = sts_quality;
	} else {
		if (g_sts_good_run == 0u || sts_quality < g_sts_quality_min) {
			g_sts_quality_min = sts_quality;
		}
		if (g_sts_good_run < FIRA_RANGE_TRUST_K) {
			g_sts_good_run++;
		}
	}

	/* Latch the CCC responder's distance into the store the mRangingData seam reads. */
	g_have_range = true;
	g_last_range_cm = cm;
	g_last_range_addr = 0u;
	g_last_range_nlos = 0u;
	g_last_range_block = block;
	g_last_range_ms = ultrawidelock_uptime_ms();
	g_range_generation++;
	if (g_range_generation == 0u) {
		g_range_generation++;
	}

	/* Accepted latches only: a rejected block must never wake the unlock seam. */
	if (g_range_listener != NULL) {
		g_range_listener();
	}
}
#endif /* CONFIG_ULTRAWIDELOCK_CRED */
