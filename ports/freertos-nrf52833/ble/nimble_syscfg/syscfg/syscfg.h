/* SPDX-License-Identifier: ISC */

/*
 * NimBLE configuration for this product.
 *
 * NimBLE resolves every setting through MYNEWT_VAL(), and upstream's
 * porting/nimble/include/syscfg/syscfg.h guards all 1500-odd defaults with
 * #ifndef. This header therefore only states the values that must differ, then
 * chains to upstream for the rest, so the vendor tree is neither copied nor
 * patched. Put this directory ahead of upstream's on the include path.
 *
 * Every role setting below has to agree with the controller features that
 * radio/radio_start_freertos.c links with sdc_support_*. Enabling a role here
 * that the controller does not support would make NimBLE issue commands the
 * controller rejects at runtime instead of failing at build time.
 */
#ifndef ULTRAWIDELOCK_FREERTOS_NIMBLE_SYSCFG_H
#define ULTRAWIDELOCK_FREERTOS_NIMBLE_SYSCFG_H

/*
 * Upstream defaults both of these to 1. The controller links neither
 * sdc_support_central nor sdc_support_scan, so they must be off.
 */
#define MYNEWT_VAL_BLE_ROLE_CENTRAL (0)
#define MYNEWT_VAL_BLE_ROLE_OBSERVER (0)

/* sdc_support_adv and sdc_support_peripheral are linked. */
#define MYNEWT_VAL_BLE_ROLE_BROADCASTER (1)
#define MYNEWT_VAL_BLE_ROLE_PERIPHERAL (1)

/*
 * Legacy advertising only. sdc_support_ext_adv is not linked, and leaving this
 * at zero also keeps upstream from raising the transport event buffers to 257
 * bytes for extended advertising reports this build can never receive.
 */
#define MYNEWT_VAL_BLE_EXT_ADV (0)

/* Must equal the controller's SDC_CFG_TYPE_PERIPHERAL_COUNT. */
#define MYNEWT_VAL_BLE_MAX_CONNECTIONS (1)

/*
 * Credit-based L2CAP is how credential moves its larger payloads. Upstream defaults
 * the channel count to zero, which compiles the whole CoC path out.
 *
 * Two, not one, and the second is the update channel on PSM 0x0081. This value
 * sizes ble_l2cap_coc_srv_pool -- one block per ble_l2cap_create_server() --
 * so at one the second server would have failed with BLE_HS_ENOMEM at
 * registration, which reads as a memory problem rather than as this setting.
 * It also adds one block to ble_l2cap_chan_pool, which is what lets a peer hold
 * the reader channel open while an update is pushed.
 */
#define MYNEWT_VAL_BLE_L2CAP_COC_MAX_NUM (2)

/*
 * LE Secure Connections only. the credential protocol's session establishment requires it, and
 * allowing the legacy pairing fallback would let a peer negotiate down.
 */
#define MYNEWT_VAL_BLE_SM_SC (1)
#define MYNEWT_VAL_BLE_SM_LEGACY (0)

/*
 * Bonding stays off until the port has a settings backend to persist keys
 * into. Turning it on without a store would accept pairings that silently do
 * not survive a reboot.
 */
#define MYNEWT_VAL_BLE_SM_BONDING (0)

/*
 * Privacy has no setting here to turn off: NimBLE selects it per call through
 * own_addr_type. The controller does not link sdc_support_le_privacy, so it
 * has no resolving list, and product code must therefore only ever pass
 * BLE_OWN_ADDR_PUBLIC or BLE_OWN_ADDR_RANDOM. Requesting either
 * BLE_OWN_ADDR_RPA_* type would fail at runtime, not at build time.
 */

/*
 * No isochronous buffers. Upstream defaults these to ten blocks in each
 * direction at 300 bytes, and the transport pool allocates them unconditionally
 * whether or not anything can use them.
 *
 * Nothing here can. ble/nimble_sdc_transport.c rejects ISO packets outright,
 * the controller does not link an isochronous feature, and the product needs
 * GATT and credit-based L2CAP rather than LE Audio. The first target link
 * measured the cost at 3,480 bytes of RAM, which is worth naming in a budget
 * where the Zephyr oracle uses more than 92% of its 128 KB.
 */
#define MYNEWT_VAL_BLE_TRANSPORT_ISO_FROM_HS_COUNT (0)
#define MYNEWT_VAL_BLE_TRANSPORT_ISO_FROM_LL_COUNT (0)
#define MYNEWT_VAL_BLE_TRANSPORT_ISO_COUNT (0)

#include_next <syscfg/syscfg.h>

#endif /* ULTRAWIDELOCK_FREERTOS_NIMBLE_SYSCFG_H */
