/* SPDX-License-Identifier: ISC */

/** @file ccc_mac.c — UWB MAC: hopping sequence, SP0 frame codec, ranging schedule. */

#include <errno.h>
#include <string.h>

#include "ccc_mac.h"

/** @brief Default-hopping modulus, 2^16 - 15 (prime). */
#define CCC_HOP_MODULUS 65521u

/**
 * @brief Default hopping round index S(i) in [0, n_round) for a ranging block.
 * @param block_index Block index i.
 * @param hop_key_rw HOP_Key_RW value keying the hop calculation.
 * @param n_round Total number of ranging rounds.
 * @return Round index in [0, n_round).
 */
uint16_t ccc_hop_round_index(uint32_t block_index, uint32_t hop_key_rw, uint32_t n_round)
{
	/* t = ((i + HOP_Key_RW) & 0xFFFF)^2 mod (2^16 - 15); S = (t * N_Round) >> 16, a round in
	 * [0, N_Round); uint64 keeps it exact. */
	uint64_t t = (block_index + hop_key_rw) & 0xFFFFu;

	t = (t * t) % CCC_HOP_MODULUS;
	return (uint16_t)((t * n_round) >> 16);
}

/* ── SP0 frame codec ──────────────────────────────────────────────────────── */

/* Fixed MHR field values; see ccc_mac.h for the byte map. */
#define MHR_FRAME_CONTROL    0x2B49u   /* MAC frame control. */
#define MHR_SEC_CONTROL      0x16u     /* ENC-MIC-64, Key Id Mode 2. */
#define MHR_KEY_INDEX        0xAAu     /* Key index. */
#define MHR_VENDOR_IE_HDR    0x0005u   /* Length 5, IE ID 0, Type Header. */
#define MHR_VENDOR_OUI       0x04DF69u /* CCC OUI. */
#define MHR_VENDOR_OUI_CRED 0x4A191Bu /* Apple SP0 frames carry the credential OUI. */
#define MHR_HT2_IE           0x3F80u   /* Header Termination IE HT2, Element ID 0x7F. */

/** @brief Store a uint16 little-endian. */
static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/** @brief Store a uint32 little-endian. */
static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/** @brief Load a uint16 little-endian. */
static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/** @brief Load a uint32 little-endian. */
static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/** @brief True if the 3-byte little-endian OUI at p is the CCC or credential OUI (both accepted).
 */
static bool mhr_vendor_oui_ok(const uint8_t *p)
{
	uint32_t oui = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);

	return oui == MHR_VENDOR_OUI || oui == MHR_VENDOR_OUI_CRED;
}

/**
 * @brief Build the 23-byte SP0 MHR little-endian on the wire.
 * @param f MHR fields (destination address, frame counter, key source, message ID, payload length).
 * @param out Output buffer of at least CCC_MHR_LEN bytes.
 * @return 0 on success, -EINVAL if inputs are null.
 */
int ccc_build_mhr(const struct ccc_mhr_fields *f, uint8_t out[CCC_MHR_LEN])
{
	if (f == NULL || out == NULL) {
		return -EINVAL;
	}
	put_le16(&out[0], MHR_FRAME_CONTROL);
	put_le16(&out[2], f->dest_short_addr);
	out[4] = MHR_SEC_CONTROL;
	put_le32(&out[5], f->frame_counter);
	memcpy(&out[9], f->key_source, CCC_KEYSOURCE_LEN);
	out[13] = MHR_KEY_INDEX;
	put_le16(&out[14], MHR_VENDOR_IE_HDR);
	out[16] = (uint8_t)MHR_VENDOR_OUI; /* OUI little-endian. */
	out[17] = (uint8_t)(MHR_VENDOR_OUI >> 8);
	out[18] = (uint8_t)(MHR_VENDOR_OUI >> 16);
	out[19] = f->msg_id;
	out[20] = f->payload_len;
	put_le16(&out[21], MHR_HT2_IE);
	return 0;
}

/**
 * @brief Parse and validate a 23-byte SP0 MHR, extracting variable fields.
 * @param in Input buffer of exactly CCC_MHR_LEN bytes.
 * @param f Output structure to hold parsed fields (destination address, frame counter, key source,
 * message ID, payload length).
 * @return 0 on success, -EINVAL on mismatch or null input.
 */
int ccc_parse_mhr(const uint8_t in[CCC_MHR_LEN], struct ccc_mhr_fields *f)
{
	if (in == NULL || f == NULL) {
		return -EINVAL;
	}
	if (get_le16(&in[0]) != MHR_FRAME_CONTROL || in[4] != MHR_SEC_CONTROL ||
	    in[13] != MHR_KEY_INDEX || get_le16(&in[14]) != MHR_VENDOR_IE_HDR ||
	    !mhr_vendor_oui_ok(&in[16]) || get_le16(&in[21]) != MHR_HT2_IE) {
		return -EINVAL;
	}
	f->dest_short_addr = get_le16(&in[2]);
	f->frame_counter = get_le32(&in[5]);
	memcpy(f->key_source, &in[9], CCC_KEYSOURCE_LEN);
	f->msg_id = in[19];
	f->payload_len = in[20];
	return 0;
}

/**
 * @brief Pack a Pre-POLL payload little-endian.
 * @param p Pre-POLL structure (session ID, STS index, ranging block, hop flag, round index).
 * @param out Output buffer of at least CCC_PRE_POLL_LEN bytes.
 * @return 0 on success, -EINVAL if inputs are null.
 */
int ccc_pre_poll_pack(const struct ccc_pre_poll *p, uint8_t out[CCC_PRE_POLL_LEN])
{
	if (p == NULL || out == NULL) {
		return -EINVAL;
	}
	put_le32(&out[0], p->uwb_session_id);
	put_le32(&out[4], p->poll_sts_index);
	put_le16(&out[8], p->ranging_block);
	out[10] = p->hop_flag;
	put_le16(&out[11], p->round_index);
	return 0;
}

/**
 * @brief Parse a 13-byte Pre-POLL payload.
 * @param in Input buffer of exactly CCC_PRE_POLL_LEN bytes.
 * @param p Output structure to hold parsed fields (session ID, STS index, ranging block, hop flag,
 * round index).
 * @return 0 on success, -EINVAL if inputs are null.
 */
int ccc_pre_poll_parse(const uint8_t in[CCC_PRE_POLL_LEN], struct ccc_pre_poll *p)
{
	if (in == NULL || p == NULL) {
		return -EINVAL;
	}
	p->uwb_session_id = get_le32(&in[0]);
	p->poll_sts_index = get_le32(&in[4]);
	p->ranging_block = get_le16(&in[8]);
	p->hop_flag = in[10];
	p->round_index = get_le16(&in[11]);
	return 0;
}

/**
 * @brief Pack a Final_Data payload little-endian.
 * @param f Final_Data structure (session ID, ranging block, hop flag, round index, STS index,
 * timestamp, responder list).
 * @param out Output buffer.
 * @param cap Capacity of output buffer.
 * @param len Output: number of bytes written.
 * @return 0 on success, -EINVAL if inputs are null, responder count exceeds CCC_MAX_RESPONDERS, or
 * buffer is too small.
 */
int ccc_final_data_pack(const struct ccc_final_data *f, uint8_t *out, size_t cap, size_t *len)
{
	size_t n;

	if (f == NULL || out == NULL || len == NULL || f->num_responders > CCC_MAX_RESPONDERS) {
		return -EINVAL;
	}
	n = CCC_FINAL_DATA_HDR_LEN + (size_t)f->num_responders * CCC_RESPONDER_LEN;
	if (cap < n) {
		return -EINVAL;
	}
	put_le32(&out[0], f->uwb_session_id);
	put_le16(&out[4], f->ranging_block);
	out[6] = f->hop_flag;
	put_le16(&out[7], f->round_index);
	put_le32(&out[9], f->final_sts_index);
	put_le32(&out[13], f->ranging_ts_final_tx);
	out[17] = f->num_responders;
	for (uint8_t i = 0; i < f->num_responders; i++) {
		uint8_t *r = &out[CCC_FINAL_DATA_HDR_LEN + (size_t)i * CCC_RESPONDER_LEN];
		r[0] = f->responders[i].responder_index;
		put_le32(&r[1], f->responders[i].timestamp);
		r[5] = f->responders[i].timestamp_uncertainty;
		r[6] = f->responders[i].ranging_status;
	}
	*len = n;
	return 0;
}

/**
 * @brief Parse a Final_Data payload.
 * @param in Input buffer.
 * @param len Length of input buffer.
 * @param f Output structure to hold parsed fields (session ID, ranging block, hop flag, round
 * index, STS index, timestamp, responder list).
 * @return 0 on success, -EINVAL if inputs are null, input length is inconsistent with responder
 * count, or responder count exceeds CCC_MAX_RESPONDERS.
 */
int ccc_final_data_parse(const uint8_t *in, size_t len, struct ccc_final_data *f)
{
	if (in == NULL || f == NULL || len < CCC_FINAL_DATA_HDR_LEN) {
		return -EINVAL;
	}
	memset(f, 0, sizeof(*f));
	f->uwb_session_id = get_le32(&in[0]);
	f->ranging_block = get_le16(&in[4]);
	f->hop_flag = in[6];
	f->round_index = get_le16(&in[7]);
	f->final_sts_index = get_le32(&in[9]);
	f->ranging_ts_final_tx = get_le32(&in[13]);
	f->num_responders = in[17];
	if (f->num_responders > CCC_MAX_RESPONDERS ||
	    len != CCC_FINAL_DATA_HDR_LEN + (size_t)f->num_responders * CCC_RESPONDER_LEN) {
		return -EINVAL;
	}
	for (uint8_t i = 0; i < f->num_responders; i++) {
		const uint8_t *r = &in[CCC_FINAL_DATA_HDR_LEN + (size_t)i * CCC_RESPONDER_LEN];
		f->responders[i].responder_index = r[0];
		f->responders[i].timestamp = get_le32(&r[1]);
		f->responders[i].timestamp_uncertainty = r[5];
		f->responders[i].ranging_status = r[6];
	}
	return 0;
}

/* ── Ranging schedule ─────────────────────────────────────────────────────── */

/** @brief STS-index offset of a slot within its ranging round. */
static uint32_t slot_offset(const struct ccc_ran_params *p, enum ccc_slot slot, uint8_t responder)
{
	switch (slot) {
	case CCC_SLOT_PRE_POLL:
		return 0u;
	case CCC_SLOT_POLL:
		return 1u;
	case CCC_SLOT_RESPONSE:
		return 2u + responder;
	case CCC_SLOT_FINAL:
		return (uint32_t)p->n_responder + 2u;
	case CCC_SLOT_FINAL_DATA:
		return (uint32_t)p->n_responder + 3u;
	}
	return 0u;
}

/**
 * @brief The ranging round a block uses (block 0 uses round 0; continuous hopping uses S(i) for
 * i>=1).
 * @param p Ranging parameters.
 * @param block Block index.
 * @return Round index in [0, n_round).
 */
uint16_t ccc_block_round(const struct ccc_ran_params *p, uint32_t block)
{
	/* Ranging starts in round 0 of block 0; only blocks i >= 1 hop, and no hopping keeps every
	 * block at 0. */
	if (p == NULL || p->hop_mode == CCC_HOP_NONE || block == 0u) {
		return 0u;
	}
	return ccc_hop_round_index(block, p->hop_key_rw, p->n_round);
}

/**
 * @brief STS index for one slot of a ranging round (wraps mod 2^32).
 * @param p Ranging parameters.
 * @param block Block index.
 * @param round Round index.
 * @param slot Slot type (POLL, Response, or Final).
 * @param responder Responder index.
 * @return STS index for the slot.
 */
uint32_t ccc_slot_sts_index(const struct ccc_ran_params *p, uint32_t block, uint16_t round,
			    enum ccc_slot slot, uint8_t responder)
{
	uint32_t base;

	if (p == NULL) {
		return 0u;
	}
	/* +N_Slot_per_Round per round, +N_Slot_per_Round*N_Round per block. */
	base = p->sts_index0 + block * ((uint32_t)p->n_slot_per_round * p->n_round) +
	       (uint32_t)round * p->n_slot_per_round;
	return base + slot_offset(p, slot, responder);
}

/**
 * @brief The initiator's hop decision for the block after block, written into its Final_Data.
 * @param p Ranging parameters.
 * @param block Block index.
 * @return Hop decision (hop flag and next round index); both zero if hopping is disabled or params
 * are null.
 */
struct ccc_hop_decision ccc_initiator_next_hop(const struct ccc_ran_params *p, uint32_t block)
{
	struct ccc_hop_decision d = {0u, 0u};

	if (p != NULL && p->hop_mode == CCC_HOP_CONTINUOUS) {
		d.hop_flag = 1u;
		d.round_index = ccc_hop_round_index(block + 1u, p->hop_key_rw, p->n_round);
	}
	return d;
}

/* ── Double-sided two-way ranging ─────────────────────────────────────────── */

/* The estimator itself is ds_twr_tof_signed() in src/fira/ds_twr.c. It used to
 * be here as an unsigned ccc_ds_twr_tof(), which underflowed near contact and
 * was never called in production because this file's own consumer open-coded a
 * signed version to avoid it. */

/**
 * @brief The record for @p responder, found by its TAG rather than its position.
 *
 * CCC reports only the responders the initiator actually validated, in no
 * guaranteed order, and tags each record with @c responder_index. Indexing the
 * array by position is therefore wrong the moment the initiator omits or
 * reorders one: a lone record tagged 1 -- exactly what a satellite gets when the
 * initiator validated it and not the lock -- would be read as responder 0's
 * timestamps and yield a confidently wrong distance.
 *
 * Fall back to position only when the tags cannot be trusted (out of range, or
 * duplicated), which preserves the old behaviour for an initiator that does not
 * populate them.
 */
static const struct ccc_responder_ts *fd_record_for(const struct ccc_final_data *fd,
						    uint8_t responder)
{
	uint16_t seen = 0u;
	uint8_t i;
	bool tags_usable = true;

	for (i = 0u; i < fd->num_responders; i++) {
		uint8_t tag = fd->responders[i].responder_index;

		if (tag >= CCC_MAX_RESPONDERS || (seen & (uint16_t)(1u << tag)) != 0u) {
			tags_usable = false;
			break;
		}
		seen |= (uint16_t)(1u << tag);
	}

	if (tags_usable) {
		for (i = 0u; i < fd->num_responders; i++) {
			if (fd->responders[i].responder_index == responder) {
				return &fd->responders[i];
			}
		}
		return NULL; /* tags are sound and this responder simply was not reported */
	}
	return (responder < fd->num_responders) ? &fd->responders[responder] : NULL;
}

int ccc_responder_ds_twr(const struct ccc_final_data *fd, uint8_t responder, uint32_t t_reply1,
			 uint32_t t_round2, struct ds_twr *out)
{
	const struct ccc_responder_ts *rec;

	/* Bound against the protocol maximum, NOT against num_responders: a record
	 * tagged 1 can legitimately arrive in a frame carrying one record, and the
	 * old `responder >= fd->num_responders` test rejected precisely that case. */
	if (fd == NULL || out == NULL || responder >= CCC_MAX_RESPONDERS ||
	    fd->num_responders == 0u || fd->num_responders > CCC_MAX_RESPONDERS) {
		return -EINVAL;
	}
	rec = fd_record_for(fd, responder);
	if (rec == NULL) {
		return -ENOENT;
	}
	/* From Final_Data: t_round1 = t4−t1, and t_reply2 = (t5−t1)−(t4−t1). */
	out->t_round1 = rec->timestamp;
	out->t_reply1 = t_reply1;
	out->t_round2 = t_round2;
	out->t_reply2 = fd->ranging_ts_final_tx - rec->timestamp;
	return 0;
}

/* ── URSK lifetime ────────────────────────────────────────────────────────── */

bool ccc_ursk_exhausted(const struct ccc_ran_params *p, uint32_t block)
{
	uint64_t span, highest_plus1;

	if (p == NULL) {
		return true;
	}
	/* Block i spans STS indices [base_i, base_i + span - 1], base_i = STS_Index0 + i*span;
	 * exhausted once the last exceeds max. */
	span = (uint64_t)p->n_slot_per_round * p->n_round;
	highest_plus1 = (uint64_t)p->sts_index0 + (uint64_t)(block + 1u) * span;
	return highest_plus1 > (uint64_t)CCC_STS_INDEX_MAX + 1u;
}
