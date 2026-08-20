/** @file twin_frames.c — the digital twin's peer (iPhone) side, shared.
 *
 * Extracted from test_twin.c so the WASM twin page drives the responder with
 * the very same encrypted-frame machinery the host suite asserts against.
 */
#include <string.h>

#include "twin_frames.h"

#include "ccc_mac.h"
#include "ccc_shim.h"

void twin_peer_init(struct twin_peer *p, const uint8_t *ursk, uint32_t sid, uint32_t sts_index0)
{
	uint8_t mupsk2[CCC_MUPSK2_LEN], uad[CCC_UAD_LEN];

	p->sid = sid;
	ccc_derive_mupsk1(ursk, p->mupsk1);
	ccc_derive_mupsk2(ursk, mupsk2);
	ccc_derive_uad(mupsk2, sts_index0, uad);
	ccc_uad_addresses(uad, p->ks, p->dest, p->src_long);
}

/* DestShort and KeySource go on the wire in TRANSMISSION order, which is the
 * byte-reverse of what ccc_uad_addresses() emits. mhr_context_ok() in
 * ccc_shim_rx.c is the gate that cares, and it sits ahead of the whole DS-TWR
 * chain: get this wrong and every Pre-POLL is dropped, exactly as it was on a
 * DWM3001CDK before that gate was fixed. Same convention as
 * tests/host/test_prepoll_round.c, which builds a frame the way the radio
 * delivers one. */
static uint16_t mhr_dest(const struct twin_peer *p)
{
	return (uint16_t)(((uint16_t)p->dest[0] << 8) | p->dest[1]);
}

static void mhr_set_keysource(const struct twin_peer *p, uint8_t dst[CCC_KEYSOURCE_LEN])
{
	size_t i;

	for (i = 0u; i < CCC_KEYSOURCE_LEN; i++) {
		dst[i] = p->ks[CCC_KEYSOURCE_LEN - 1u - i];
	}
}

uint16_t twin_mk_prepoll(const struct twin_peer *p, uint8_t *out, uint32_t fc, uint32_t poll_idx,
			 uint32_t block)
{
	struct ccc_mhr_fields f;
	struct ccc_pre_poll pp;
	uint8_t plain[CCC_PRE_POLL_LEN];

	memset(&pp, 0, sizeof(pp));
	pp.uwb_session_id = p->sid;
	pp.poll_sts_index = poll_idx;
	pp.ranging_block = block;
	ccc_pre_poll_pack(&pp, plain);

	memset(&f, 0, sizeof(f));
	f.dest_short_addr = mhr_dest(p);
	f.frame_counter = fc;
	mhr_set_keysource(p, f.key_source);
	f.msg_id = CCC_MSG_ID_PRE_POLL;
	f.payload_len = CCC_PRE_POLL_LEN;
	ccc_build_mhr(&f, out);
	ccc_sp0_encrypt(p->mupsk1, p->src_long, fc, out, CCC_MHR_LEN, plain, CCC_PRE_POLL_LEN,
			&out[CCC_MHR_LEN], &out[CCC_MHR_LEN + CCC_PRE_POLL_LEN]);
	return CCC_MHR_LEN + CCC_PRE_POLL_LEN + CCC_SP0_MIC_LEN;
}

uint16_t twin_mk_final_data(const struct twin_peer *p, uint8_t *out, uint32_t fc,
			    uint32_t armed_idx, uint32_t block, uint32_t t_round1,
			    uint32_t t_reply2)
{
	struct ccc_mhr_fields f;
	struct ccc_final_data fd;
	uint8_t plain[64];
	uint8_t dudsk[CCC_DUDSK_LEN];
	size_t pl = 0;

	memset(&fd, 0, sizeof(fd));
	fd.uwb_session_id = p->sid;
	fd.ranging_block = block;
	fd.final_sts_index = armed_idx + 2u;
	fd.ranging_ts_final_tx = t_round1 + t_reply2; /* t5-t1 */
	fd.num_responders = 1u;
	fd.responders[0].timestamp = t_round1; /* t4-t1 */
	ccc_final_data_pack(&fd, plain, sizeof(plain), &pl);

	memset(&f, 0, sizeof(f));
	f.dest_short_addr = mhr_dest(p);
	f.frame_counter = fc;
	mhr_set_keysource(p, f.key_source);
	f.msg_id = CCC_MSG_ID_FINAL_DATA;
	f.payload_len = (uint8_t)pl;
	ccc_build_mhr(&f, out);
	ccc_shim_dudsk_for_index(armed_idx, dudsk);
	ccc_sp0_encrypt(dudsk, p->src_long, fc, out, CCC_MHR_LEN, plain, pl, &out[CCC_MHR_LEN],
			&out[CCC_MHR_LEN + pl]);
	return (uint16_t)(CCC_MHR_LEN + pl + CCC_SP0_MIC_LEN);
}

void twin_stash_frame(const uint8_t *frame, uint16_t len, uint64_t ip40)
{
	memcpy(ultrawidelock_host_rx.rxdata, frame, len);
	ultrawidelock_host_rx.rxdata_len = len;
	ultrawidelock_host_rx.rx_ts40 = ip40;
}

void twin_rx_event(dwt_cb_t cb, uint32_t status)
{
	dwt_cb_data_t d;

	memset(&d, 0, sizeof(d));
	d.status = status;
	d.datalength = ultrawidelock_host_rx.rxdata_len;
	cb(&d);
}
