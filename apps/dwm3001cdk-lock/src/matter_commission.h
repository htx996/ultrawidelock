/* SPDX-License-Identifier: ISC */

/**
 * @file matter_commission.h — start answering commissioning attempts.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the commissioning handlers on the 0xFFF6 transport.
 *
 * Call after the reader is up. Nothing here touches the radio: whether the
 * board is discoverable as a commissionable node is decided by the advertising
 * branch in ultrawidelock_ble_zephyr.c, which asks for the Matter payload while this
 * node holds no fabric -- see matter_commission_has_fabric().
 *
 * @return 0. A bad verifier is reported by log and refused per attempt rather
 *         than failing startup -- a reader that cannot commission should still
 *         be a reader.
 */
int matter_commission_init(void);

/**
 * Whether this node currently holds a commissioned Matter fabric.
 *
 * Asked by the advertiser, which can carry the Matter commissionable
 * service data OR the credential reader tag but not both: flags 3 + Matter 12 +
 * credential 26 is 41 bytes in a 31-byte legacy packet, and a second advertising
 * set costs 24.8 KB of RAM.
 *
 * A device with no fabric MUST stay commissionable. Provisioning the reader
 * identity used to flip the advert to credential on its own, which left a board
 * that had just been provisioned -- and had lost its fabric to a failed
 * pairing -- invisible to Add Accessory and impossible to recover without
 * erasing it.
 */
bool matter_commission_has_fabric(void);

/**
 * True while an AdministratorCommissioning window is open.
 *
 * The advertiser needs this: a node that HAS a fabric normally advertises as
 * a credential reader, and doing that during a commissioning window hides it from
 * the very ecosystem the window was opened for.
 */
bool matter_commission_window_open(void);

/**
 * True exactly once after an UnlockDoor command was invoked; reading clears it.
 *
 * The inside latch needs this: its per-credential record is only ever created
 * by a DELIBERATE unlock, and the Matter path runs on OpenThread's thread
 * where the latch (owned by the main loop) must not be touched. The main loop
 * polls this and attributes the unlock to whichever credential is currently
 * authenticated -- with none authenticated the event is consumed unattributed,
 * which fails closed: no record, no passive unlock.
 */
bool matter_commission_take_deliberate_unlock(void);

/**
 * Publish the latest trusted UWB observation through the vendor Matter
 * cluster. Reports are internally rate-limited; presence edges are immediate.
 * @p device_id is a privacy-safe credential-derived identifier, or zero.
 */
void matter_commission_update_uwb_presence(bool in_range, int32_t distance_mm,
					   uint32_t device_id);

/**
 * Finish the one packet previously accepted by the Matter BLE transport.
 * Called exactly once by the transport after final confirmation or failure.
 */
void matter_commission_ble_tx_complete(int status);

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_ANCHOR)
/**
 * Record a DoorLockAlarm, if the bolt says it is one.
 *
 * The bolt test lives here rather than in the caller because the lock state is
 * this file's, held under the same mutex as everything else in it: a forced
 * door or a door left ajar is only an alarm while the lock is LOCKED, and a
 * sensor cannot know that on its own. An alarm raised while the owner has the
 * lock open is noise, and this node has one report channel to spend.
 *
 * @param alarm_code MATTER_DL_ALARM_*.
 */
void matter_commission_record_alarm(uint8_t alarm_code);
#endif

#ifdef __cplusplus
}
#endif
