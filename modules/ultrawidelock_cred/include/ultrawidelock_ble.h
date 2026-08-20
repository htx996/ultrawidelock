/* SPDX-License-Identifier: ISC */

// credential BLE-UWB reader transport: GATT service definition, advertised feature flags, and
// transport callbacks connecting the BLE peripheral role to the credential protocol handler in
// ultrawidelock_reader. Callers configure the transport via ultrawidelock_ble_prepare (which builds
// the READ characteristic payload without touching NimBLE), then register the GATT service returned
// by ultrawidelock_ble_service_def with the host's combined service table.
/*
 * ultrawidelock_ble — credential BLE transport (NimBLE) for the ESP32-S3 port.
 * Advertises the credential GATT service, negotiates the BLE-UWB protocol version,
 * and carries the credential transaction over an L2CAP CoC. Independent
 * reimplementation.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** credential BLE-UWB supported-features flags (advertised in the READ char, and
 *  parsed from the device WRITE). Serialized as one byte: bit0/1/2. */
struct ultrawidelock_ble_features {
	bool timesync_procedure_0;
	bool timesync_procedure_1;
	bool le_coded_phy;
};

/** Transport callbacks into the app / Phase-3 credential handler. All optional. */
struct ultrawidelock_ble_callbacks {
	/** An L2CAP SDU arrived from the peer (2.2+). */
	void (*on_data)(uint16_t conn_handle, const uint8_t *data, uint16_t len);
	/** L2CAP channel opened / closed for a peer (2.2+). */
	void (*on_connected)(uint16_t conn_handle);
	void (*on_disconnected)(uint16_t conn_handle);
	/** Connection-RSSI sample (dBm), polled while the CoC is up. Setting this
	 *  turns the transport's RSSI poll on (the reader's ranging power gate);
	 *  leave NULL and the transport never reads RSSI. */
	void (*on_rssi)(uint16_t conn_handle, int8_t rssi_dbm);
};

/** Reader configuration. `proto_versions` are host-order uint16s; they are the
 *  provisioned `aliroSupportedBLEUWBProtocolVersions` (Matter attr 133), NOT a
 *  transport constant, so the caller supplies them. */
struct ultrawidelock_ble_config {
	const uint16_t *proto_versions;
	size_t proto_versions_count;
	struct ultrawidelock_ble_features features;
	struct ultrawidelock_ble_callbacks cb;
};

/* ---- Owned-host mode: the port brings NimBLE up, this registers on it ------ *
 * Bring-up splits in two because only half of it is portable. The port does the
 * platform half -- persistent storage, nimble_port_init(), and starting the host
 * task -- and calls these for the rest. See each port's bring-up file:
 * ports/esp32/components/ultrawidelock_ble/ultrawidelock_ble_esp32.c and
 * ports/freertos-nrf52833/ble/ultrawidelock_ble_freertos.c. */

/** Start NimBLE, register the credential GATT service, and begin advertising.
 *  Implemented by the port, not by the shared backend: it is the platform half
 *  above. Returns 0 on success, negative otherwise. */
int ultrawidelock_ble_start(const struct ultrawidelock_ble_config *cfg);

/** Register the GAP/GATT stubs, the credential 0xFFF2 service, the device name, and
 *  the L2CAP CoC server on an already-initialised host. Call ultrawidelock_ble_prepare()
 *  first, then this after nimble_port_init() and before the host task runs.
 *  0 on success. */
int ultrawidelock_ble_register_gatt(void);

/** Host sync handler: ensure an address exists, infer the type, advertise.
 *  Install as ble_hs_cfg.sync_cb, or call from the port's own sync handler. */
void ultrawidelock_ble_host_sync(void);

/** Host reset handler; logs the reason. Install as ble_hs_cfg.reset_cb. */
void ultrawidelock_ble_host_reset(int reason);

/** The L2CAP SPSM published to peers in the READ characteristic. */
uint16_t ultrawidelock_ble_spsm(void);

/** Send an SDU to the peer over its L2CAP channel (2.2+). Returns 0 on success. */
int ultrawidelock_ble_send(uint16_t conn_handle, const uint8_t *data, size_t len);

/** Terminate the BLE connection (reader-initiated; e.g. the RSSI power gate
 *  closing on a departed peer). Returns 0 on success. */
int ultrawidelock_ble_disconnect(uint16_t conn_handle);

/* Marshal a reader->phone status send onto the NimBLE host task (where every sc_ble
 * seal + L2CAP send already runs), so a caller on another task can send without racing
 * the BleSK counter. `cb` runs on the host task and is passed `unsecured`. */
void ultrawidelock_ble_post_reader_status(void (*cb)(bool unsecured), bool unsecured);

/* Marshal the reader's coalesced deadline/replay tick onto the BLE host owner.
 * This has a dedicated queue slot so a frequent tick cannot replace a grant,
 * presence reset, or revocation sweep callback. */
void ultrawidelock_ble_post_reader_tick(void (*cb)(void));

/* Marshal a presence-proof reset onto the NimBLE host task. The callback may
 * inspect the reader session table and terminate links without racing the
 * transaction callbacks that own it. Only one proof command is served at a
 * time by the console, so one queued callback slot is sufficient. */
void ultrawidelock_ble_post_presence_reset(void (*cb)(void));

/* Marshal a post-revocation link sweep onto the NimBLE host task. Its own slot
 * rather than a second user of the presence one: both store a single callback
 * pointer, so sharing would let a bench presence proof and an admin revocation
 * silently cancel each other, and the revocation is the one that must not be
 * dropped -- until the link is gone, an already-revoked phone's established
 * session keeps ranging and keeps opening the door. */
void ultrawidelock_ble_post_revoke_sweep(void (*cb)(void));

/* ---- Attach mode: share a NimBLE host another stack already owns ---------- *
 * Instead of the port owning NimBLE, the reader can attach to a host
 * brought up by e.g. esp-matter, so both coexist on one controller. Three
 * phases: prepare() captures the config; the owner registers our GATT service
 * (ultrawidelock_ble_service_def()) through its extra-services hook BEFORE it starts its
 * GATT server; start_attached() brings up the L2CAP CoC + advertising once the
 * host is synced and the owner has released the advertiser (post-commissioning). */
struct ble_gatt_svc_def; /* NimBLE type, opaque here */

/** Capture config + build the READ payload; does NOT touch NimBLE. 0 on ok. */
int ultrawidelock_ble_prepare(const struct ultrawidelock_ble_config *cfg);

/** The credential GATT service definition, to hand to the host owner's
 *  register-extra-services hook. Valid after ultrawidelock_ble_prepare(). */
const struct ble_gatt_svc_def *ultrawidelock_ble_service_def(void);

/** Bring up the reader on the already-synced shared host: L2CAP CoC +
 *  advertising. Returns 0 on success. */
int ultrawidelock_ble_start_attached(void);

/** Set the provisioned credential advertising params (BLE-UWB approach discovery): the
 *  truncated reader group id (8) + sub id (2), the group resolving key (16) for
 *  the dynamic tag, and the tx-power byte. Call before start_attached(); once set,
 *  the reader advertises the full resolvable 0xFFF2 service data instead of the
 *  bare service UUID, so the phone can approach-connect. */
void ultrawidelock_ble_set_adv_params(const uint8_t group_id8[8], const uint8_t sub_id2[2],
			      const uint8_t grk[16], int8_t tx_power);

/** Re-emit the advertisement with the current adv params (call after
 *  ultrawidelock_ble_set_adv_params updates the GRK post-provisioning). No-op until
 *  start_attached() has brought the advertiser up. */
void ultrawidelock_ble_readvertise(void);

/** The wall clock just stepped (e.g. SNTP first sync): re-derive the dynamic
 *  advertisement tag now instead of waiting out the refresh period. Safe from
 *  any task; marshaled onto the host task. No-op until start_attached(). */
void ultrawidelock_ble_time_updated(void);

#ifdef __cplusplus
}
#endif
