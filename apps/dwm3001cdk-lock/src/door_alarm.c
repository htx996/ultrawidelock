/* SPDX-License-Identifier: ISC */

/**
 * @file door_alarm.c — impact latch and swing angle to DoorLockAlarm.
 *
 * The thresholds this file reads are PLACEHOLDERS, all of them, in the same
 * sense overlay-anchor.conf already says of the ones beside them: the impact
 * threshold has to come from a capture of this door being closed and slammed,
 * and the geometry is a tape-measure fact about where the two anchors ended up.
 * Until both exist, this raises alarms whose timing is a guess -- which is why
 * every number lives in Kconfig rather than here, and why the ajar dwell is
 * long enough that a guess costs a late alarm rather than a false one.
 */

#include "door_alarm.h"

#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "matter_clusters.h"
#include "matter_commission.h"
#include "ultrawidelock_door.h"

LOG_MODULE_DECLARE(main, CONFIG_LOG_DEFAULT_LEVEL);

static struct ultrawidelock_door s_door;
/** False when the geometry was refused; the ajar alarm stays off for the run. */
static bool s_door_ready;
/** Whether the leaf has been away from CLOSED since the last close. */
static bool s_open_seen;
static int64_t s_open_since_ms;
/** One ajar alarm per opening, not one per tick for as long as it stands open. */
static bool s_ajar_reported;

/**
 * Hand an alarm to the Matter node, which is also what applies the bolt test.
 */
static void raise_alarm(uint8_t alarm_code)
{
	matter_commission_record_alarm(alarm_code);
}

void door_alarm_init(void)
{
	const struct ultrawidelock_door_cfg cfg = {
		.hinge_to_frame_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_DOOR_HINGE_FRAME_MM,
		.hinge_to_leaf_mm = CONFIG_ULTRAWIDELOCK_ANCHOR_DOOR_HINGE_LEAF_MM,
		.offset_mddeg = CONFIG_ULTRAWIDELOCK_ANCHOR_DOOR_OFFSET_MDDEG,
	};

	s_open_seen = false;
	s_open_since_ms = 0;
	s_ajar_reported = false;

	/* NULL thresholds: ultrawidelock_door_defaults() -- 2/5 degrees closed,
	 * 35/30 open, dwell 3. Angles rather than distances, so re-hanging the
	 * door changes the geometry above and leaves these alone. */
	s_door_ready = ultrawidelock_door_init(&s_door, &cfg, NULL);
	if (!s_door_ready) {
		LOG_WRN("door geometry rejected; the ajar alarm is off");
	}
}

void door_alarm_tamper(void)
{
	raise_alarm(MATTER_DL_ALARM_DOOR_FORCED_OPEN);
}

void door_alarm_leaf_mm(int32_t leaf_mm, int64_t now_ms)
{
	enum ultrawidelock_door_state state;

	if (!s_door_ready) {
		return;
	}

	state = ultrawidelock_door_feed(&s_door, leaf_mm);
	if (state == ULTRAWIDELOCK_DOOR_CLOSED) {
		s_open_seen = false;
		s_ajar_reported = false;
		return;
	}
	/*
	 * UNKNOWN is the state before the first dwell completes and after a
	 * reading the geometry cannot explain. It is not evidence that the door
	 * is open, so it neither starts the clock nor stops one already running.
	 */
	if (state == ULTRAWIDELOCK_DOOR_UNKNOWN) {
		return;
	}

	if (!s_open_seen) {
		s_open_seen = true;
		s_open_since_ms = now_ms;
		return;
	}
	if (s_ajar_reported) {
		return;
	}
	/* A backwards clock reads as "not long enough yet" rather than as an
	 * instant alarm: a late alarm is recoverable, a false one teaches the
	 * owner to ignore the next. */
	if ((now_ms - s_open_since_ms) >=
	    ((int64_t)CONFIG_ULTRAWIDELOCK_ANCHOR_AJAR_DWELL_S * MSEC_PER_SEC)) {
		s_ajar_reported = true;
		LOG_WRN("door standing open past the ajar dwell");
		raise_alarm(MATTER_DL_ALARM_DOOR_AJAR);
	}
}
