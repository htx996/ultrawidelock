/* SPDX-License-Identifier: ISC */

/**
 * @file door_alarm.h — the two DoorLockAlarms this board can honestly raise.
 *
 * Both are ANCHOR-only, because both need a sensor that only the anchor build
 * has: the LIS2DH12 impact latch (ULTRAWIDELOCK_ANCHOR_SLAM) for a forced door,
 * and the frame-to-leaf swing angle (ultrawidelock_door.h) for one left ajar.
 *
 * Neither decides anything about the bolt. The alarm is recorded only while the
 * Matter node says the bolt is LOCKED, and that check lives with the state it
 * reads, in matter_commission.c — an alarm about a door that is supposed to be
 * open is not an alarm.
 */
#ifndef DOOR_ALARM_H
#define DOOR_ALARM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load the install geometry and clear the ajar dwell. Call once at startup,
 * before or after the Matter node comes up; nothing here touches it.
 *
 * Bad geometry is refused rather than corrected: it disables the ajar alarm and
 * says so once, because an angle computed from a wrong hinge distance is a
 * number, not a measurement.
 */
void door_alarm_init(void);

/**
 * The impact classifier just latched a tamper.
 *
 * At most one alarm per latch, because ultrawidelock_slam_poll() reports the
 * TRANSITION and then latches -- see its own comment on why the condition is
 * not reported repeatedly. Nothing clears that latch today, so a forced door is
 * reported once per boot; whoever wires the recovery path owns
 * ultrawidelock_slam_clear_tamper().
 */
void door_alarm_tamper(void);

/**
 * One frame-to-leaf distance, in millimetres, at @p now_ms on the app's clock.
 *
 * NOTHING FEEDS THIS YET -- the leaf tag's transport is the same missing stage
 * that leaves ultrawidelock_satellite_report() uncalled (main.c) -- and that is
 * a working state rather than a gap: with no distance the door state stays
 * UNKNOWN, UNKNOWN never starts the dwell, and no ajar alarm is ever raised.
 * The seam exists so the transport, when it lands, has one place to deliver to.
 */
void door_alarm_leaf_mm(int32_t leaf_mm, int64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* DOOR_ALARM_H */
