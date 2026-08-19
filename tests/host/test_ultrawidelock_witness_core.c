/**
 * @file test_ultrawidelock_witness_core.c — which advertisers a witness reports.
 *
 * The failure this suite guards is quiet: an advertiser that misses the cut is
 * indistinguishable at the lock from one that was never heard, so a ranking
 * bug shows up as a lock that simply stops opening.
 */

#include "test.h"

#include "ultrawidelock_witness_core.h"

#include <string.h>

static void test_ranks_by_loudness(void)
{
	struct ultrawidelock_witness_core c;
	struct ultrawidelock_witness_msg m;
	uint8_t n;

	t_group("witness_core: loudest first");
	memset(&m, 0, sizeof(m));
	ultrawidelock_witness_core_open(&c);

	for (int i = 0; i < 4; i++) {
		ultrawidelock_witness_core_note(&c, 0x100u, -80);
		ultrawidelock_witness_core_note(&c, 0x200u, -50);
		ultrawidelock_witness_core_note(&c, 0x300u, -65);
	}

	n = ultrawidelock_witness_core_summarize(&c, &m, 2u);
	T_EQ("rank.count", n, 3);
	T_EQ("rank.first", m.tuples[0].hash24, 0x200u);
	T_EQ("rank.first_dbm", m.tuples[0].mean_dbm, -50);
	T_EQ("rank.second", m.tuples[1].hash24, 0x300u);
	T_EQ("rank.third", m.tuples[2].hash24, 0x100u);
	T_EQ("rank.pkts", m.tuples[0].n_pkts, 4);
	T_EQ("rank.msg_n", m.n_tuples, 3);
}

static void test_one_packet_is_not_a_measurement(void)
{
	struct ultrawidelock_witness_core c;
	struct ultrawidelock_witness_msg m;

	t_group("witness_core: a single packet is dropped");
	memset(&m, 0, sizeof(m));
	ultrawidelock_witness_core_open(&c);

	/* Loud, but heard once: a mean with no spread, and no way to tell a
	 * real advertiser from something passing the door. */
	ultrawidelock_witness_core_note(&c, 0x400u, -40);
	ultrawidelock_witness_core_note(&c, 0x500u, -70);
	ultrawidelock_witness_core_note(&c, 0x500u, -72);

	T_EQ("single.count", ultrawidelock_witness_core_summarize(&c, &m, 2u), 1);
	T_EQ("single.kept", m.tuples[0].hash24, 0x500u);
	T_EQ("single.mean", m.tuples[0].mean_dbm, -71);
}

static void test_report_is_capped(void)
{
	struct ultrawidelock_witness_core c;
	struct ultrawidelock_witness_msg m;
	uint8_t n;

	t_group("witness_core: a busy room still fits one frame");
	memset(&m, 0, sizeof(m));
	ultrawidelock_witness_core_open(&c);

	/* Twelve advertisers at descending strength. Only the loudest eight may
	 * travel, and they must be the loudest eight. */
	for (int i = 0; i < 12; i++) {
		for (int k = 0; k < 3; k++) {
			ultrawidelock_witness_core_note(&c, (uint32_t)(0x1000u + i),
							(int8_t)(-40 - i * 3));
		}
	}
	n = ultrawidelock_witness_core_summarize(&c, &m, 2u);
	T_EQ("cap.count", n, ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES);
	T_EQ("cap.loudest", m.tuples[0].hash24, 0x1000u);
	T_EQ("cap.eighth", m.tuples[7].hash24, 0x1007u);
	/* Strictly descending, because the lock walks tuples loudest-first. */
	for (uint8_t i = 1; i < n; i++) {
		T_OK("cap.descending", m.tuples[i].mean_dbm <= m.tuples[i - 1].mean_dbm);
	}
}

static void test_quiet_chatter_cannot_evict_a_near_phone(void)
{
	struct ultrawidelock_witness_core c;
	struct ultrawidelock_witness_msg m;
	bool found = false;

	t_group("witness_core: a burst of distant chatter keeps its place");
	memset(&m, 0, sizeof(m));
	ultrawidelock_witness_core_open(&c);

	/* The phone, close and steady. */
	for (int k = 0; k < 4; k++) {
		ultrawidelock_witness_core_note(&c, 0xAAAAu, -45);
	}
	/* Then far more distinct advertisers than there are slots, all far
	 * away. The phone must survive: eviction picks the quietest, and a
	 * newcomer weaker than the slot it would take is refused outright. */
	for (int i = 0; i < 40; i++) {
		ultrawidelock_witness_core_note(&c, (uint32_t)(0x9000u + i), -90);
		ultrawidelock_witness_core_note(&c, (uint32_t)(0x9000u + i), -91);
	}

	(void)ultrawidelock_witness_core_summarize(&c, &m, 2u);
	for (uint8_t i = 0; i < m.n_tuples; i++) {
		if (m.tuples[i].hash24 == 0xAAAAu) {
			found = true;
		}
	}
	T_OK("evict.phone_survived", found);
	T_EQ("evict.phone_is_loudest", m.tuples[0].hash24, 0xAAAAu);
}

static void test_window_boundaries(void)
{
	struct ultrawidelock_witness_core c;
	struct ultrawidelock_witness_msg m;

	t_group("witness_core: packets outside a window are not attributed");
	memset(&m, 0, sizeof(m));
	ultrawidelock_witness_core_open(&c);
	ultrawidelock_witness_core_note(&c, 0x700u, -55);
	ultrawidelock_witness_core_note(&c, 0x700u, -55);
	T_EQ("bound.first", ultrawidelock_witness_core_summarize(&c, &m, 2u), 1);

	/* Summarising closed the window; a late packet belongs to nothing. */
	ultrawidelock_witness_core_note(&c, 0x700u, -55);
	T_EQ("bound.after_close", ultrawidelock_witness_core_summarize(&c, &m, 2u), 0);
	T_EQ("bound.msg_cleared", m.n_tuples, 0);

	/* A fresh window starts empty rather than inheriting. */
	ultrawidelock_witness_core_open(&c);
	T_EQ("bound.reopened_empty", ultrawidelock_witness_core_summarize(&c, &m, 2u), 0);

	ultrawidelock_witness_core_note(NULL, 0x1u, -50);
	T_EQ("bound.null_core", ultrawidelock_witness_core_summarize(NULL, &m, 2u), 0);
	ultrawidelock_witness_core_open(&c);
	T_EQ("bound.null_msg", ultrawidelock_witness_core_summarize(&c, NULL, 2u), 0);
}

static void test_label_masking(void)
{
	struct ultrawidelock_witness_core c;
	struct ultrawidelock_witness_msg m;

	t_group("witness_core: labels are 24 bits everywhere");
	memset(&m, 0, sizeof(m));
	ultrawidelock_witness_core_open(&c);

	/* The same label with junk in the top byte is the same advertiser, not
	 * a second one -- the wire format only carries 24 bits. */
	ultrawidelock_witness_core_note(&c, 0xFF123456u, -60);
	ultrawidelock_witness_core_note(&c, 0x00123456u, -62);

	T_EQ("mask.merged", ultrawidelock_witness_core_summarize(&c, &m, 2u), 1);
	T_EQ("mask.value", m.tuples[0].hash24, 0x00123456u);
	T_EQ("mask.pkts", m.tuples[0].n_pkts, 2);
}

void test_ultrawidelock_witness_core(void)
{
	test_ranks_by_loudness();
	test_one_packet_is_not_a_measurement();
	test_report_is_capped();
	test_quiet_chatter_cannot_evict_a_near_phone();
	test_window_boundaries();
	test_label_masking();
}
