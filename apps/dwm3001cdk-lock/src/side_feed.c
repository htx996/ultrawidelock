/* SPDX-License-Identifier: ISC */

/**
 * @file side_feed.c — SF1 parser + optional RTT-down lab ingest for ultrawidelock_side.
 */

#include "side_feed.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_ULTRAWIDELOCK_SIDE_FEED_RTT) && CONFIG_ULTRAWIDELOCK_SIDE_FEED_RTT
#include <SEGGER_RTT.h>
#endif

static struct k_mutex s_lock;
static struct ultrawidelock_side_features s_pending;
static bool s_have;
static bool s_inited;
static uint32_t s_seq;
static int s_bl_cmd;
static int32_t s_bl_mm;

static void ensure_init(void)
{
	if (!s_inited) {
		k_mutex_init(&s_lock);
		s_inited = true;
	}
}

bool side_feed_parse_sf1(const char *line, struct ultrawidelock_side_features *out)
{
	int in_dbm = 0, out_dbm = 0, th_dbm = 0;
	int ni = 0, no = 0, nt = 0;
	const char *p;

	if (line == NULL || out == NULL) {
		return false;
	}
	while (*line && isspace((unsigned char)*line)) {
		line++;
	}
	if (strncmp(line, "SF1", 3) != 0) {
		return false;
	}
	p = line + 3;
	/* Token scan: in= out= th= ni= no= nt= */
	while (*p) {
		while (*p && isspace((unsigned char)*p)) {
			p++;
		}
		if (strncmp(p, "in=", 3) == 0) {
			in_dbm = (int)strtol(p + 3, (char **)&p, 10);
		} else if (strncmp(p, "out=", 4) == 0) {
			out_dbm = (int)strtol(p + 4, (char **)&p, 10);
		} else if (strncmp(p, "th=", 3) == 0) {
			th_dbm = (int)strtol(p + 3, (char **)&p, 10);
		} else if (strncmp(p, "ni=", 3) == 0) {
			ni = (int)strtol(p + 3, (char **)&p, 10);
		} else if (strncmp(p, "no=", 3) == 0) {
			no = (int)strtol(p + 3, (char **)&p, 10);
		} else if (strncmp(p, "nt=", 3) == 0) {
			nt = (int)strtol(p + 3, (char **)&p, 10);
		} else {
			while (*p && !isspace((unsigned char)*p)) {
				p++;
			}
		}
	}

	memset(out, 0, sizeof(*out));
	out->uwb_range_mm = -1;
	out->uwb_vel_mm_s = INT32_MIN;
	out->uwb_peer_mm = -1;
	out->ble_rssi_inside_dbm = (ni > 0) ? (int16_t)in_dbm : INT16_MIN;
	out->ble_rssi_outside_dbm = (no > 0) ? (int16_t)out_dbm : INT16_MIN;
	out->ble_rssi_threshold_dbm = (nt > 0) ? (int16_t)th_dbm : INT16_MIN;
	out->ble_pkts_inside = (uint8_t)CLAMP(ni, 0, 255);
	out->ble_pkts_outside = (uint8_t)CLAMP(no, 0, 255);
	out->ble_pkts_threshold = (uint8_t)CLAMP(nt, 0, 255);
	out->classifier_ver = 1;
	out->calibration_ver = 1;
	out->anchor_health_mask =
		(uint8_t)(((ni > 0) ? ULTRAWIDELOCK_SIDE_ANCHOR_BLE_INSIDE : 0) |
			  ((no > 0) ? ULTRAWIDELOCK_SIDE_ANCHOR_BLE_OUTSIDE : 0) |
			  ((nt > 0) ? ULTRAWIDELOCK_SIDE_ANCHOR_BLE_THRESHOLD : 0));
	return ni > 0 || no > 0;
}

void side_feed_push(const struct ultrawidelock_side_features *feat)
{
	ensure_init();
	if (feat == NULL) {
		return;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	s_pending = *feat;
	s_have = true;
	k_mutex_unlock(&s_lock);
}

bool side_feed_take(struct ultrawidelock_side_features *out)
{
	bool got = false;

	ensure_init();
	if (out == NULL) {
		return false;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (s_have) {
		*out = s_pending;
		s_have = false;
		got = true;
	}
	k_mutex_unlock(&s_lock);
	return got;
}

int side_feed_take_baseline(int32_t *out_mm)
{
	int cmd;

	ensure_init();
	k_mutex_lock(&s_lock, K_FOREVER);
	cmd = s_bl_cmd;
	s_bl_cmd = 0;
	if (cmd == 1 && out_mm != NULL) {
		*out_mm = s_bl_mm;
	}
	k_mutex_unlock(&s_lock);
	return cmd;
}

#if defined(CONFIG_ULTRAWIDELOCK_SIDE_FEED_RTT) && CONFIG_ULTRAWIDELOCK_SIDE_FEED_RTT
static void bl_line(const char *p)
{
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	ensure_init();
	k_mutex_lock(&s_lock, K_FOREVER);
	if (strncmp(p, "cal", 3) == 0) {
		s_bl_cmd = 2;
	} else {
		long v = strtol(p, NULL, 10);

		if (v >= 300 && v <= 10000) {
			s_bl_cmd = 1;
			s_bl_mm = (int32_t)v;
		}
	}
	k_mutex_unlock(&s_lock);
}
#endif

void side_feed_rtt_poll(void)
{
#if defined(CONFIG_ULTRAWIDELOCK_SIDE_FEED_RTT) && CONFIG_ULTRAWIDELOCK_SIDE_FEED_RTT
	static char buf[128];
	static size_t len;
	unsigned n;
	struct ultrawidelock_side_features feat;

	/* A line longer than the buffer would otherwise leave no room to read
	 * into, and a zero-length read returns before the overflow reset below
	 * can ever run -- one oversized line would wedge the feed forever. */
	if (len + 1 >= sizeof(buf)) {
		len = 0;
	}
	n = SEGGER_RTT_Read(0, buf + len, (unsigned)(sizeof(buf) - 1 - len));
	if (n == 0) {
		return;
	}
	len += n;
	buf[len] = '\0';
	for (;;) {
		char *nl = memchr(buf, '\n', len);
		size_t line_len;
		char line[128];

		if (nl == NULL) {
			if (len + 1 >= sizeof(buf)) {
				len = 0;
			}
			return;
		}
		line_len = (size_t)(nl - buf);
		if (line_len >= sizeof(line)) {
			line_len = sizeof(line) - 1;
		}
		memcpy(line, buf, line_len);
		line[line_len] = '\0';
		memmove(buf, nl + 1, len - line_len - 1);
		len -= line_len + 1;
		buf[len] = '\0';
		{
			const char *t = line;

			while (*t == ' ' || *t == '\t') {
				t++;
			}
			if (strncmp(t, "BL", 2) == 0 &&
			    (t[2] == '\0' || t[2] == ' ' || t[2] == '\t')) {
				bl_line(t + 2);
			} else if (side_feed_parse_sf1(line, &feat)) {
				feat.seq = ++s_seq;
				side_feed_push(&feat);
			}
		}
	}
#else
	ARG_UNUSED(s_seq);
#endif
}
