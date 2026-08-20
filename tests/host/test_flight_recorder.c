/** @file test_flight_recorder.c — the flight recorder end to end.
 *
 * Two halves:
 *   1. Format: fr_writer/fr_reader round-trip for every record type, plus the
 *      malformed / truncated / version-mismatch rejections.
 *   2. Record → replay: arm the recorder, drive one real CCM*-encrypted DS-TWR
 *      round through the instrumented ccc_shim_rx.c listener (same scenario as
 *      test_prepoll_round), then replay the captured trace back through the
 *      listener and assert it re-derives the identical radio-action outputs AND
 *      the same 234 cm range. Divergence would mean the trace lost information
 *      or replay dispatch drifted from device dispatch. The dump→hex→parse path
 *      (the serial transport
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deca_device_api.h>

#include "cred_kdf.h" /* ULTRAWIDELOCK_URSK_LEN */
#include "ccc_kdf.h"
#include "ccc_mac.h"
#include "ccc_shim.h"
#include "fira_session.h"
#include "flight_recorder.h"
#include "fr_replay.h"
#include <ultrawidelock/uwb.h>
#include "test.h"

extern int32_t ultrawidelock_uwb_arm_rx(int32_t mode);

/* ── the recorded DS-TWR scenario (mirrors test_prepoll_round) ───────────── */

#define RND_SID    0x11223344u
#define RND_STS0   0x00400000u
#define RND_IDX1   5000u
#define RND_STRIDE 96u
#define RND_BLOCK  7u

static uint8_t g_ursk[ULTRAWIDELOCK_URSK_LEN];
static uint8_t g_mupsk1[CCC_MUPSK1_LEN];
static uint8_t g_ks[CCC_KEYSOURCE_LEN];

/* On-air KeySource is byte-reversed relative to ccc_uad_addresses()'s
 * KeySourceHigh||KeySourceLow layout. See the same helper in
 * test_prepoll_round.c for the hardware evidence. */
static void mhr_set_keysource(uint8_t dst[CCC_KEYSOURCE_LEN])
{
	size_t i;

	for (i = 0u; i < CCC_KEYSOURCE_LEN; i++) {
		dst[i] = g_ks[CCC_KEYSOURCE_LEN - 1u - i];
	}
}
static uint8_t g_dest[CCC_DEST_SHORT_ADDR_LEN];
static uint8_t g_src_long[CCC_SRC_LONG_ADDR_LEN];
static uint8_t g_rc[17];

/* Outputs snapshotted after each entry call during the record run, to compare
 * against the replay's per-event outputs. */
static struct fr_output g_rec_out[FR_REPLAY_MAX_EV];
static unsigned g_rec_n;

static void rec_snap(uint8_t ep)
{
	struct fr_output *o = &g_rec_out[g_rec_n++];

	o->ep = ep;
	o->rxenable_calls = ultrawidelock_host_rx.rxenable_calls;
	o->last_rxenable_mode = ultrawidelock_host_rx.last_rxenable_mode;
	o->starttx_calls = ultrawidelock_host_rx.starttx_calls;
	o->forcetrxoff_calls = ultrawidelock_host_rx.forcetrxoff_calls;
}

static uint16_t mk_prepoll(uint8_t *out, uint32_t fc, uint32_t poll_idx)
{
	struct ccc_mhr_fields f;
	struct ccc_pre_poll pp;
	uint8_t plain[CCC_PRE_POLL_LEN];

	memset(&pp, 0, sizeof(pp));
	pp.uwb_session_id = RND_SID;
	pp.poll_sts_index = poll_idx;
	pp.ranging_block = RND_BLOCK;
	ccc_pre_poll_pack(&pp, plain);

	memset(&f, 0, sizeof(f));
	f.dest_short_addr = (uint16_t)(((uint16_t)g_dest[0] << 8) | g_dest[1]);
	f.frame_counter = fc;
	mhr_set_keysource(f.key_source);
	f.msg_id = CCC_MSG_ID_PRE_POLL;
	f.payload_len = CCC_PRE_POLL_LEN;
	ccc_build_mhr(&f, out);
	ccc_sp0_encrypt(g_mupsk1, g_src_long, fc, out, CCC_MHR_LEN, plain, CCC_PRE_POLL_LEN,
			&out[CCC_MHR_LEN], &out[CCC_MHR_LEN + CCC_PRE_POLL_LEN]);
	return CCC_MHR_LEN + CCC_PRE_POLL_LEN + CCC_SP0_MIC_LEN;
}

static uint16_t mk_final_data(uint8_t *out, uint32_t fc, uint32_t armed_idx, uint32_t t_round1,
			      uint32_t t_reply2)
{
	struct ccc_mhr_fields f;
	struct ccc_final_data fd;
	uint8_t plain[64];
	uint8_t dudsk[CCC_DUDSK_LEN];
	size_t pl = 0;

	memset(&fd, 0, sizeof(fd));
	fd.uwb_session_id = RND_SID;
	fd.ranging_block = RND_BLOCK;
	fd.final_sts_index = armed_idx + 2u;
	fd.ranging_ts_final_tx = t_round1 + t_reply2;
	fd.num_responders = 1u;
	fd.responders[0].timestamp = t_round1;
	ccc_final_data_pack(&fd, plain, sizeof(plain), &pl);

	memset(&f, 0, sizeof(f));
	f.dest_short_addr = (uint16_t)(((uint16_t)g_dest[0] << 8) | g_dest[1]);
	f.frame_counter = fc;
	mhr_set_keysource(f.key_source);
	f.msg_id = CCC_MSG_ID_FINAL_DATA;
	f.payload_len = (uint8_t)pl;
	ccc_build_mhr(&f, out);
	ccc_shim_dudsk_for_index(armed_idx, dudsk);
	ccc_sp0_encrypt(dudsk, g_src_long, fc, out, CCC_MHR_LEN, plain, pl, &out[CCC_MHR_LEN],
			&out[CCC_MHR_LEN + pl]);
	return (uint16_t)(CCC_MHR_LEN + pl + CCC_SP0_MIC_LEN);
}

static void stash_frame(const uint8_t *frame, uint16_t len, uint64_t ip40)
{
	memcpy(ultrawidelock_host_rx.rxdata, frame, len);
	ultrawidelock_host_rx.rxdata_len = len;
	ultrawidelock_host_rx.rx_ts40 = ip40;
}

/* Fire a captured RX callback (prepoll_rx_rearm) the way dwt_isr would, then
 * snapshot the post-event outputs for the record run. */
static void fire_rx(uint32_t status)
{
	dwt_cb_data_t d;

	memset(&d, 0, sizeof(d));
	d.status = status;
	d.datalength = ultrawidelock_host_rx.rxdata_len;
	ultrawidelock_host_rx.cbs.cbRxOk(&d);
	rec_snap((uint8_t)FR_EP_RX_REARM);
}

static void fire_tx(uint32_t status)
{
	dwt_cb_data_t d;

	memset(&d, 0, sizeof(d));
	d.status = status;
	ultrawidelock_host_rx.cbs.cbTxDone(&d);
	rec_snap((uint8_t)FR_EP_TX_DONE);
}

static void fire_try(uint16_t len)
{
	ccc_shim_rx_try_prepoll(len);
	rec_snap((uint8_t)FR_EP_TRY_PREPOLL);
}

#define ST_GOOD (DWT_INT_CIADONE_BIT_MASK | DWT_INT_RXPHD_BIT_MASK | DWT_INT_RXFCG_BIT_MASK)

/* Run the DS-TWR round while armed; returns the trace length (into *trace, a
 * pointer into the recorder ring — copy it before re-arming). */
static size_t record_round(const uint8_t **trace, int32_t *range_cm_out)
{
	uint8_t frame[128];
	uint16_t len;
	uint8_t mupsk2[CCC_MUPSK2_LEN], uad[CCC_UAD_LEN];
	const uint32_t widx = RND_IDX1 + 2u * RND_STRIDE;
	uint32_t fc = 100u;
	struct ultrawidelock_uwb_cred_cfg c;
	int32_t cm = -1;

	for (size_t i = 0; i < sizeof(g_ursk); i++) {
		g_ursk[i] = (uint8_t)(0xA0u + i);
	}
	for (size_t i = 0; i < sizeof(g_rc); i++) {
		g_rc[i] = (uint8_t)i;
	}
	ccc_derive_mupsk1(g_ursk, g_mupsk1);
	ccc_derive_mupsk2(g_ursk, mupsk2);
	ccc_derive_uad(mupsk2, RND_STS0, uad);
	ccc_uad_addresses(uad, g_ks, g_dest, g_src_long);

	memset(&c, 0, sizeof(c));
	c.session_id = RND_SID;
	c.channel = 9u;
	c.sync_code_index = 9u;
	c.slot_per_round = 12u;
	c.sts_index0 = RND_STS0;
	c.ursk = g_ursk;
	c.ranging_config = g_rc;
	c.rc_len = sizeof(g_rc);

	g_rec_n = 0;
	fr_clear();
	fr_set_enabled(true); /* META */

	ultrawidelock_host_rx_reset();
	T_EQ("rec.start", ultrawidelock_uwb_start_cred(&c), 0); /* CONFIG */

	/* Bootstrap: two Pre-POLL decodes learn index + stride. */
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x1000000ull);
	fire_try(len);
	len = mk_prepoll(frame, fc++, RND_IDX1 + RND_STRIDE);
	stash_frame(frame, len, 0x2000000ull);
	fire_try(len);

	/* Pre-POLL event arms the SP3 POLL window off the warm. */
	stash_frame(frame, len, 0x3000000ull);
	fire_rx(ST_GOOD);
	/* Next block's Pre-POLL arrives while armed: stash + defer. */
	len = mk_prepoll(frame, fc++, widx);
	stash_frame(frame, len, 0x3100000ull);
	fire_try(len);

	/* POLL result (cper=0) fires the delayed Response TX. */
	ultrawidelock_host_rx.rx_ts40 = 0x40000000ull;
	fire_rx(DWT_INT_CIADONE_BIT_MASK);

	/* TXFRS arms the Final window and flushes the deferred decode. */
	ultrawidelock_host_rx.tx_ts40 = 0x40000000ull + 100000u;
	fire_tx(DWT_INT_TXFRS_BIT_MASK);

	/* Final result reverts to SP0. */
	ultrawidelock_host_rx.rx_ts40 = 0x40000000ull + 300000u;
	ultrawidelock_host_rx.stsq_ret = 0;
	ultrawidelock_host_rx.stsq_val = 100;
	fire_rx(DWT_INT_CIADONE_BIT_MASK);

	/* Final_Data decrypt latches the DS-TWR range (234 cm). */
	len = mk_final_data(frame, fc++, widx, 101000u, 199000u);
	stash_frame(frame, len, 0x3200000ull);
	fire_try(len);

	T_OK("rec.range_latched", fira_session_last_range(&cm, NULL, NULL, NULL, NULL));
	if (range_cm_out != NULL) {
		*range_cm_out = cm;
	}

	fr_set_enabled(false); /* disarm before finalize/replay so replay can't
				  re-enter the recorder */
	return fr_finalize(trace);
}

/* ── format-level tests ─────────────────────────────────────────────────── */

static void test_format_roundtrip(void)
{
	uint8_t buf[512];
	fr_writer_t w;
	fr_reader_t r;
	struct fr_record rec;
	struct fr_config c;
	struct fr_ev e;

	t_group("writer/reader round-trip every record type");
	fr_writer_init(&w, buf, sizeof(buf));
	T_OK("w.no_overflow", !w.overflow);
	T_EQ("w.meta", fr_write_meta(&w, FR_PORT_ESP32, "abc1234"), 0);

	memset(&c, 0, sizeof(c));
	c.session_id = 0xDEADBEEFu;
	c.channel = 9;
	c.sync_code_index = 9;
	c.sts_index0 = 0x00400000u;
	c.slot_per_round = 12;
	for (int i = 0; i < (int)FR_URSK_LEN; i++) {
		c.ursk[i] = (uint8_t)(0xA0 + i);
	}
	c.rc_len = 17;
	for (int i = 0; i < 17; i++) {
		c.rc[i] = (uint8_t)i;
	}
	T_EQ("w.config", fr_write_config(&w, &c), 0);

	memset(&e, 0, sizeof(e));
	e.ep = FR_EP_RX_REARM;
	e.status = 0x12345678u;
	e.datalength = 49;
	e.rx_ts40 = 0x1122334455ull;
	e.tx_ts40 = 0x66778899AAull;
	e.systime = 0xCAFEu;
	e.stsq_valid = 1;
	e.stsq_val = 100;
	e.stsq_ret = 0;
	e.frame_len = 8;
	for (int i = 0; i < 8; i++) {
		e.frame[i] = (uint8_t)(0x40 + i);
	}
	T_EQ("w.ev", fr_write_ev(&w, &e), 0);
	T_EQ("w.end", fr_write_end(&w, 1, false), 0);

	fr_reader_init(&r, buf, w.len);
	T_EQ("r.meta", fr_read_next(&r, &rec), FR_REC_META);
	T_EQ("r.meta.ver", rec.u.meta.version, FR_VERSION);
	T_EQ("r.meta.port", rec.u.meta.port, FR_PORT_ESP32);
	T_OK("r.meta.sha", strcmp(rec.u.meta.sha, "abc1234") == 0);

	T_EQ("r.config", fr_read_next(&r, &rec), FR_REC_CONFIG);
	T_EQ("r.config.sid", (long)rec.u.config.session_id, (long)0xDEADBEEFu);
	T_EQ("r.config.rc_len", rec.u.config.rc_len, 17);
	T_OK("r.config.ursk", memcmp(rec.u.config.ursk, c.ursk, FR_URSK_LEN) == 0);
	T_OK("r.config.rc", memcmp(rec.u.config.rc, c.rc, 17) == 0);

	T_EQ("r.ev", fr_read_next(&r, &rec), FR_REC_EV);
	T_EQ("r.ev.ep", rec.u.ev.ep, FR_EP_RX_REARM);
	T_EQ("r.ev.status", (long)rec.u.ev.status, (long)0x12345678u);
	T_EQ("r.ev.rxts", (long long)rec.u.ev.rx_ts40, (long long)0x1122334455ull);
	T_EQ("r.ev.frame_len", rec.u.ev.frame_len, 8);
	T_OK("r.ev.frame", memcmp(rec.u.ev.frame, e.frame, 8) == 0);

	T_EQ("r.end", fr_read_next(&r, &rec), FR_REC_END);
	T_EQ("r.end.n", rec.u.end.n_events, 1);
	T_EQ("r.eof", fr_read_next(&r, &rec), 0);
}

static void test_format_rejections(void)
{
	uint8_t buf[64];
	fr_writer_t w;
	fr_reader_t r;
	struct fr_record rec;

	t_group("reader rejects malformed / short / version-mismatched traces");

	/* Bad magic. */
	memset(buf, 0, sizeof(buf));
	fr_reader_init(&r, buf, sizeof(buf));
	T_EQ("bad_magic", fr_read_next(&r, &rec), -1);

	/* Too short for a magic. */
	fr_reader_init(&r, buf, 3);
	T_EQ("short_magic", fr_read_next(&r, &rec), -1);

	/* Valid magic, then a truncated record header. */
	fr_writer_init(&w, buf, sizeof(buf));
	fr_reader_init(&r, buf, w.len + 2); /* magic + 2 stray bytes < 3-byte header */
	T_EQ("short_header", fr_read_next(&r, &rec), -1);

	/* Version mismatch in META. */
	fr_writer_init(&w, buf, sizeof(buf));
	fr_write_meta(&w, FR_PORT_NRF, "x");
	buf[4 + 3] = (uint8_t)(FR_VERSION + 1u); /* payload byte 0 = version LSB */
	fr_reader_init(&r, buf, w.len);
	T_EQ("bad_version", fr_read_next(&r, &rec), -1);

	/* Truncated payload (claimed length runs past the buffer). */
	fr_writer_init(&w, buf, sizeof(buf));
	fr_write_meta(&w, FR_PORT_NRF, "abc");
	fr_reader_init(&r, buf, w.len - 1); /* drop the last SHA byte */
	T_EQ("short_payload", fr_read_next(&r, &rec), -1);
}

static void test_writer_overflow(void)
{
	uint8_t small[8]; /* room for the 4-byte magic + not much else */
	fr_writer_t w;

	t_group("writer latches overflow and keeps a valid prefix");
	fr_writer_init(&w, small, sizeof(small));
	T_OK("small.magic_ok", !w.overflow);
	T_EQ("small.meta_nofit", fr_write_meta(&w, FR_PORT_HOST, "toolongsha"), -1);
	T_OK("small.overflow", w.overflow);
	T_EQ("small.len_unchanged", (long)w.len, 4L); /* only the magic survived */

	/* A cap below the magic is itself an overflow. */
	fr_writer_init(&w, small, 2);
	T_OK("tiny.overflow", w.overflow);
}

/* ── record → replay ─────────────────────────────────────────────────────── */

static uint8_t g_trace[8192];

static void test_record_replay(void)
{
	const uint8_t *ring = NULL;
	size_t len;
	int32_t rec_range = -1;
	struct fr_replay_result res;

	t_group("record one DS-TWR round, replay it, compare outputs + range");
	len = record_round(&ring, &rec_range);
	T_OK("trace.nonempty", len > 8);
	T_OK("trace.fits", len <= sizeof(g_trace));
	memcpy(g_trace, ring, len); /* copy out of the recorder ring */
	T_EQ("record.range", rec_range, 234);

	/* One-shot golden export: `FR_WRITE_SAMPLE=path ./build/host_test` writes
	 * the finalised trace so can be tested against a
	 * real C-produced trace (cross-format check). No effect under `make test`. */
	{
		const char *sp = getenv("FR_WRITE_SAMPLE");

		if (sp != NULL) {
			FILE *f = fopen(sp, "wb");

			if (f != NULL) {
				fwrite(g_trace, 1, len, f);
				fclose(f);
			}
		}
	}

	T_OK("replay.ok", fr_replay_run(g_trace, len, &res));
	T_EQ("replay.port", res.port, FR_PORT_HOST);
	T_EQ("replay.range", res.range_cm, 234);
	T_OK("replay.range_valid", res.range_valid);
	T_EQ("replay.n_events", res.n_events, g_rec_n);
	T_EQ("replay.end_count", res.end_n_events, g_rec_n);
	T_OK("replay.not_truncated", !res.truncated);
	T_OK("replay.saw_resp_tx", res.out[res.n_events - 1].starttx_calls == 1);

	t_group("replay re-derives the identical radio-action sequence");
	int mism = 0;
	for (unsigned i = 0; i < res.n_events; i++) {
		if (res.out[i].ep != g_rec_out[i].ep ||
		    res.out[i].rxenable_calls != g_rec_out[i].rxenable_calls ||
		    res.out[i].last_rxenable_mode != g_rec_out[i].last_rxenable_mode ||
		    res.out[i].starttx_calls != g_rec_out[i].starttx_calls ||
		    res.out[i].forcetrxoff_calls != g_rec_out[i].forcetrxoff_calls) {
			mism++;
		}
	}
	T_EQ("replay.outputs_match", mism, 0);
}

/* ── serial dump transport ───────────────────────────────────────────────── */

static char g_dump[16384];
static size_t g_dump_len;

static void dump_sink(const char *line)
{
	size_t n = strlen(line);

	if (g_dump_len + n + 1u < sizeof(g_dump)) {
		memcpy(g_dump + g_dump_len, line, n);
		g_dump_len += n;
		g_dump[g_dump_len++] = '\n';
		g_dump[g_dump_len] = '\0';
	}
}

static int hexval(char ch)
{
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return 10 + (ch - 'a');
	}
	return -1;
}

/* Reconstruct the binary trace from the `[FREC]` hex lines a real capture logs,
 * exactly as does, and confirm it parses to an END. */
static void test_dump_transport(void)
{
	const uint8_t *ring = NULL;
	size_t len;
	uint8_t recon[8192];
	size_t rn = 0;
	fr_reader_t r;
	struct fr_record rec;
	int types = 0, saw_end = 0;

	t_group("hex dump → parse round-trip (the serial transport)");
	len = record_round(&ring, NULL);
	memcpy(g_trace, ring, len);

	g_dump_len = 0;
	g_dump[0] = '\0';
	fr_set_dump_sink(dump_sink);
	fr_dump();
	fr_set_dump_sink(NULL);
	T_OK("dump.has_begin", strstr(g_dump, "[FREC] begin bytes=") != NULL);
	T_OK("dump.has_end", strstr(g_dump, "[FREC] end") != NULL);

	/* Decode the data lines (skip begin/end markers). */
	for (char *p = g_dump; p != NULL && *p != '\0';) {
		char *nl = strchr(p, '\n');
		size_t ll = nl ? (size_t)(nl - p) : strlen(p);

		if (ll > 7u && strncmp(p, "[FREC] ", 7) == 0) {
			const char *h = p + 7;
			size_t hl = ll - 7u;
			int all_hex = (hl % 2u) == 0u;

			/* A data line is pure hex; the begin/end markers contain
			 * spaces and '='/letters, so they fail this and are skipped. */
			for (size_t i = 0; all_hex && i < hl; i++) {
				if (hexval(h[i]) < 0) {
					all_hex = 0;
				}
			}
			if (all_hex) {
				for (size_t i = 0; i + 1u < hl && rn < sizeof(recon); i += 2) {
					recon[rn++] = (uint8_t)((hexval(h[i]) << 4) |
								hexval(h[i + 1]));
				}
			}
		}
		p = nl ? nl + 1 : NULL;
	}
	T_EQ("dump.recon_len", (long)rn, (long)len);
	T_OK("dump.recon_bytes", memcmp(recon, g_trace, len) == 0);

	fr_reader_init(&r, recon, rn);
	while (fr_read_next(&r, &rec) > 0) {
		types++;
		if (rec.type == FR_REC_END) {
			saw_end = 1;
		}
	}
	T_OK("dump.parses", types >= 4); /* meta + config + events + end */
	T_OK("dump.reached_end", saw_end == 1);

	fr_clear();
}

void test_flight_recorder(void)
{
	test_format_roundtrip();
	test_format_rejections();
	test_writer_overflow();
	test_record_replay();
	test_dump_transport();
	(void)ultrawidelock_uwb_arm_rx; /* silence unused extern if the round omits it */

	ultrawidelock_uwb_stop();
}
