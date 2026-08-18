/* SPDX-License-Identifier: ISC */

/**
 * @file ultrawidelock_witness_msg.c — WV2 codec and replay state (implementation).
 */

#include "ultrawidelock_witness_msg.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
	put_u32(p, (uint32_t)(v >> 32));
	put_u32(p + 4, (uint32_t)v);
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
	       (uint32_t)p[3];
}

static uint64_t get_u64(const uint8_t *p)
{
	return ((uint64_t)get_u32(p) << 32) | (uint64_t)get_u32(p + 4);
}

static bool role_known(uint8_t role)
{
	return role == ULTRAWIDELOCK_WITNESS_ROLE_INSIDE ||
	       role == ULTRAWIDELOCK_WITNESS_ROLE_OUTSIDE ||
	       role == ULTRAWIDELOCK_WITNESS_ROLE_THRESHOLD;
}

size_t ultrawidelock_witness_msg_encode(const struct ultrawidelock_witness_msg *msg, uint8_t *buf,
					size_t cap)
{
	size_t need;
	uint8_t *p;

	if (msg == NULL || buf == NULL) {
		return 0;
	}
	if (msg->n_tuples > ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES) {
		return 0;
	}
	if (!role_known(msg->role)) {
		return 0;
	}
	need = ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN +
	       (size_t)msg->n_tuples * ULTRAWIDELOCK_WITNESS_MSG_TUPLE_LEN;
	if (cap < need) {
		return 0;
	}

	p = buf;
	*p++ = msg->ver;
	*p++ = msg->role;
	put_u32(p, msg->boot_id);
	p += 4;
	put_u32(p, msg->ctr);
	p += 4;
	put_u64(p, msg->echo_nonce);
	p += 8;
	put_u16(p, msg->window_ms);
	p += 2;
	*p++ = msg->n_tuples;

	for (uint8_t i = 0; i < msg->n_tuples; i++) {
		uint32_t h = msg->tuples[i].hash24 & 0x00FFFFFFu;

		*p++ = (uint8_t)(h >> 16);
		*p++ = (uint8_t)(h >> 8);
		*p++ = (uint8_t)h;
		*p++ = (uint8_t)msg->tuples[i].mean_dbm;
		*p++ = msg->tuples[i].n_pkts;
	}
	return need;
}

bool ultrawidelock_witness_msg_decode(const uint8_t *buf, size_t len,
				      struct ultrawidelock_witness_msg *out)
{
	const uint8_t *p;
	uint8_t n;

	if (buf == NULL || out == NULL || len < ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN) {
		return false;
	}
	if (buf[0] != ULTRAWIDELOCK_WITNESS_MSG_VER) {
		return false;
	}
	if (!role_known(buf[1])) {
		return false;
	}
	n = buf[ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN - 1u];
	if (n > ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES) {
		return false;
	}
	/* Exact, not at-least: a sealed datagram with trailing bytes means the
	 * sender and this decoder disagree about the format. */
	if (len != ULTRAWIDELOCK_WITNESS_MSG_HDR_LEN +
			   (size_t)n * ULTRAWIDELOCK_WITNESS_MSG_TUPLE_LEN) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	p = buf;
	out->ver = *p++;
	out->role = *p++;
	out->boot_id = get_u32(p);
	p += 4;
	out->ctr = get_u32(p);
	p += 4;
	out->echo_nonce = get_u64(p);
	p += 8;
	out->window_ms = get_u16(p);
	p += 2;
	out->n_tuples = *p++;

	for (uint8_t i = 0; i < n; i++) {
		out->tuples[i].hash24 =
			((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
		out->tuples[i].mean_dbm = (int8_t)p[3];
		out->tuples[i].n_pkts = p[4];
		p += ULTRAWIDELOCK_WITNESS_MSG_TUPLE_LEN;
	}
	return true;
}

const struct ultrawidelock_witness_tuple *
ultrawidelock_witness_msg_find(const struct ultrawidelock_witness_msg *msg, uint32_t hash24)
{
	if (msg == NULL) {
		return NULL;
	}
	for (uint8_t i = 0; i < msg->n_tuples && i < ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES; i++) {
		/* n_pkts == 0 is an empty slot, not a silent phone at 0 dBm. */
		if (msg->tuples[i].n_pkts > 0u &&
		    msg->tuples[i].hash24 == (hash24 & 0x00FFFFFFu)) {
			return &msg->tuples[i];
		}
	}
	return NULL;
}

const struct ultrawidelock_witness_tuple *
ultrawidelock_witness_msg_at(const struct ultrawidelock_witness_msg *msg, uint8_t idx)
{
	if (msg == NULL || idx >= msg->n_tuples ||
	    idx >= ULTRAWIDELOCK_WITNESS_MSG_MAX_TUPLES) {
		return NULL;
	}
	if (msg->tuples[idx].n_pkts == 0u) {
		return NULL;
	}
	return &msg->tuples[idx];
}

bool ultrawidelock_witness_seen_accept(struct ultrawidelock_witness_seen *seen,
				       const struct ultrawidelock_witness_msg *msg)
{
	if (seen == NULL || msg == NULL) {
		return false;
	}
	if (seen->have && seen->boot_id == msg->boot_id) {
		if (msg->ctr <= seen->ctr) {
			return false; /* replay or reorder; state untouched */
		}
	}
	seen->boot_id = msg->boot_id;
	seen->ctr = msg->ctr;
	seen->have = true;
	return true;
}
