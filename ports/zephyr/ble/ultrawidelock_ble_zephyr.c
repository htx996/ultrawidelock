/* SPDX-License-Identifier: ISC */

/*
 * ultrawidelock_ble — Zephyr/NCS backend for the credential BLE transport seam
 * (modules/ultrawidelock_cred/include/ultrawidelock_ble.h), for the DWM3001CDK standalone
 * reader. The byte contract is the one the ESP32-S3 NimBLE backend already
 * ships and the host tests already pin: the 0xFFF2 service, the reader-SPSM
 * READ payload, the device-version WRITE, and the credential transaction on an
 * L2CAP CoC at the published SPSM.
 *
 * Deliberately not here yet:
 *   - the connection-RSSI poll that feeds the reader's ranging power gate,
 *   - attach mode, which only exists so the ESP32 reader can share a host with
 *     esp-matter. Nothing shares this host, so it stays -ENOTSUP.
 */
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG)
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include "ultrawidelock_advtag.h"
#include "ultrawidelock_ble.h"
#include "ultrawidelock_lat.h"
#include "ultrawidelock_prov.h" /* ULTRAWIDELOCK_GRK_LEN */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
#include "matter_ble_zephyr.h"
#include "matter_commission.h" /* the commissionable payload, when unprovisioned */
#endif

LOG_MODULE_REGISTER(ultrawidelock_ble, CONFIG_LOG_DEFAULT_LEVEL);

/* Same value the ESP32-S3 backend publishes (ports/esp32/components/ultrawidelock_ble/
 * ultrawidelock_ble.c:42). The dynamic-PSM range is 0x0080..0x00FF and the peer learns
 * the value from the READ characteristic, so it is ours to pick — but picking
 * the same one keeps bench captures comparable across the two ports. */
#define ULTRAWIDELOCK_L2CAP_SPSM 0x0080u
#define ULTRAWIDELOCK_L2CAP_MTU  512u

/* Reader SPSM + BLE-UWB protocol version, D3B5A130-9E23-4B3A-8BE4-6B1EE5F980A3. */
static const struct bt_uuid_128 k_chr_reader_spsm_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd3b5a130, 0x9e23, 0x4b3a, 0x8be4, 0x6b1ee5f980a3));

/* User-device selected BLE-UWB protocol version, BD4B9502-3F54-11EC-B919-0242AC120005. */
static const struct bt_uuid_128 k_chr_device_ver_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xbd4b9502, 0x3f54, 0x11ec, 0xb919, 0x0242ac120005));

#define ULTRAWIDELOCK_MAX_VERSIONS 8u

static uint16_t s_versions[ULTRAWIDELOCK_MAX_VERSIONS];
static size_t s_versions_count;
static struct ultrawidelock_ble_callbacks s_cb;

/* Prebuilt READ payload: [SPSM be16][verLen u8][versions be16*N][featLen u8][features u8]. */
static uint8_t s_read_payload[2u + 1u + (2u * ULTRAWIDELOCK_MAX_VERSIONS) + 1u + 1u];
static uint16_t s_read_payload_len;

/* Resolvable advertising params, set once the reader is provisioned. */
static bool s_adv_ultrawidelock;
static uint8_t s_adv_group_id[8];
static uint8_t s_adv_sub_id[2];
static uint8_t s_adv_grk[ULTRAWIDELOCK_GRK_LEN];
static int8_t s_adv_tx_power;
static struct k_spinlock s_adv_params_lock;

/* These flags cross the Bluetooth callback, application, and system-workqueue
 * contexts. Keep the credential material under the short spinlock above, and
 * use atomics for the state that only needs one-word snapshots. */
static atomic_t s_conn_up;
static atomic_t s_adv_running;
static atomic_t s_adv_dirty;

/* A dynamic tag whose expiry is in the phone's past is silently ignored, so a
 * clock we cannot trust must advertise the "unavailable" form instead. Mirrors
 * the ESP backend's ULTRAWIDELOCK_ADV_TIME_FLOOR / ULTRAWIDELOCK_ADV_TAG_VALID_S. */
#define ULTRAWIDELOCK_ADV_TIME_FLOOR   1700000000
#define ULTRAWIDELOCK_ADV_TAG_VALID_S  600

/* Refresh with a full minute left. A once-per-minute clock check also catches
 * a wall-clock step even when a time source forgot to call the explicit hook;
 * the dynamic tag is only derived when it is actually due. */
#define ULTRAWIDELOCK_ADV_REFRESH_MARGIN_S       60
#define ULTRAWIDELOCK_ADV_CLOCK_POLL_S           60
#define ULTRAWIDELOCK_ADV_CLOCK_STEP_TOLERANCE_S 5

/* One bounded controller-apply attempt per workqueue pass, forever if
 * necessary. The capped backoff avoids both a busy loop and permanent
 * invisibility after the old five-attempt limit was exhausted. */
#define ULTRAWIDELOCK_ADV_RETRY_MIN_MS 100u
#define ULTRAWIDELOCK_ADV_RETRY_MAX_MS 30000u

BUILD_ASSERT(ULTRAWIDELOCK_ADV_REFRESH_MARGIN_S < ULTRAWIDELOCK_ADV_TAG_VALID_S,
	     "advertising refresh margin must be shorter than tag validity");

/* ---- L2CAP CoC: the credential transaction channel ---------------------------- */

/* One peer at a time. CONFIG_BT_MAX_CONN=1 makes that a build-time fact, not a
 * hope, so a single channel record is the whole table. */
static struct ultrawidelock_coc {
	struct bt_l2cap_le_chan le;
	struct bt_conn *conn;
	bool in_use;
} s_coc;

/* Separate pools by direction. Sharing one would let a queued outbound SDU
 * starve the receive path mid-transaction, which is the phase where a dropped
 * SDU costs the whole walk-up. */
NET_BUF_POOL_FIXED_DEFINE(s_coc_rx_pool, 2, BT_L2CAP_SDU_BUF_SIZE(ULTRAWIDELOCK_L2CAP_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
NET_BUF_POOL_FIXED_DEFINE(s_coc_tx_pool, 2, BT_L2CAP_SDU_BUF_SIZE(ULTRAWIDELOCK_L2CAP_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

/* The reader engine's transport handle. Zephyr identifies a link by pointer,
 * the seam by uint16_t, so hand out the connection index (0..MAX_CONN-1). */
static uint16_t conn_to_handle(struct bt_conn *conn)
{
	return (uint16_t)bt_conn_index(conn);
}

/**
 * Allocate a receive net_buf from the CoC pool with no wait.
 */
static struct net_buf *coc_alloc_buf(struct bt_l2cap_chan *chan)
{
	ARG_UNUSED(chan);
	return net_buf_alloc(&s_coc_rx_pool, K_NO_WAIT);
}

/**
 * Forward received L2CAP CoC data to the registered on_data callback as a transport handle and byte
 * buffer.
 */
static int coc_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	if (s_cb.on_data != NULL) {
		s_cb.on_data(conn_to_handle(chan->conn), buf->data, buf->len);
	}
	return 0;
}

/**
 * Emit a bench-scrapable SIDE line carrying the peer AdvA/RPA at credential CoC
 * open, and a matching clear at close. The format is fixed for the
 * bench capture tooling, which pushes the address to the BLE witnesses as an
 * advertising filter so they summarise one phone instead of the whole room.
 *
 * That address is personal data: a resolvable private address identifies
 * whoever is standing at the door for as long as it lasts. So this is behind
 * its own Kconfig, default n. It was previously an unconditional printk, which
 * put the address into the console of EVERY lock image, out of reach of log
 * filtering entirely.
 *
 * LOG_INF rather than LOG_DBG only because this module registers at
 * CONFIG_LOG_DEFAULT_LEVEL, so a debug build would raise the level for the
 * whole image and flash is already at 93%. The Kconfig gate is what keeps the
 * address out of shipped builds; the log level is not load-bearing here.
 */
#ifdef CONFIG_ULTRAWIDELOCK_SIDE_PEER_EMIT
static void side_emit_peer(struct bt_conn *conn)
{
	struct bt_conn_info info;
	const bt_addr_le_t *peer;
	const char *type = "unknown";

	if (conn == NULL || bt_conn_get_info(conn, &info) != 0) {
		LOG_INF("SIDE peer=clear");
		return;
	}
	/* Prefer the address used at connection setup: closer to live AdvA. */
	peer = info.le.remote != NULL ? info.le.remote : info.le.dst;
	if (peer == NULL) {
		LOG_INF("SIDE peer=clear");
		return;
	}
	if (peer->type == BT_ADDR_LE_PUBLIC) {
		type = "public";
	} else if (peer->type == BT_ADDR_LE_RANDOM) {
		type = "random";
	}
	LOG_INF("SIDE peer=%02X:%02X:%02X:%02X:%02X:%02X type=%s", peer->a.val[5], peer->a.val[4],
		peer->a.val[3], peer->a.val[2], peer->a.val[1], peer->a.val[0], type);
}

static void side_emit_clear(void)
{
	LOG_INF("SIDE peer=clear");
}
#else
static void side_emit_peer(struct bt_conn *conn)
{
	ARG_UNUSED(conn);
}

static void side_emit_clear(void)
{
}
#endif /* CONFIG_ULTRAWIDELOCK_SIDE_PEER_EMIT */

/**
 * Handle L2CAP CoC connection establishment by notifying the credential engine and logging the
 * event.
 */
static void coc_connected(struct bt_l2cap_chan *chan)
{
	LOG_INF("L2CAP CoC open (SPSM 0x%04x)", (unsigned)ULTRAWIDELOCK_L2CAP_SPSM);
	(void)ultrawidelock_lat_mark(ULTRAWIDELOCK_LAT_L2CAP_OPEN);
	side_emit_peer(chan->conn);
	if (s_cb.on_connected != NULL) {
		s_cb.on_connected(conn_to_handle(chan->conn));
	}
}

/**
 * Handle L2CAP CoC disconnection by releasing the channel, clearing state, and notifying the
 * credential engine.
 */
static void coc_disconnected(struct bt_l2cap_chan *chan)
{
	uint16_t handle = conn_to_handle(chan->conn);

	s_coc.in_use = false;
	s_coc.conn = NULL;
	side_emit_clear();
	LOG_INF("L2CAP CoC closed");
	if (s_cb.on_disconnected != NULL) {
		s_cb.on_disconnected(handle);
	}
}

static const struct bt_l2cap_chan_ops k_coc_ops = {
	.alloc_buf = coc_alloc_buf,
	.recv = coc_recv,
	.connected = coc_connected,
	.disconnected = coc_disconnected,
};

/**
 * Accept an incoming L2CAP CoC connection if no channel is in use, allocate it to the static
 * instance, initialize its MTU and callback ops, and bind it to the peer connection.
 */
static int coc_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
		      struct bt_l2cap_chan **chan)
{
	ARG_UNUSED(server);

	if (s_coc.in_use) {
		return -ENOMEM;
	}
	memset(&s_coc.le, 0, sizeof(s_coc.le));
	s_coc.le.chan.ops = &k_coc_ops;
	s_coc.le.rx.mtu = ULTRAWIDELOCK_L2CAP_MTU;
	s_coc.conn = conn;
	s_coc.in_use = true;
	*chan = &s_coc.le.chan;
	return 0;
}

static struct bt_l2cap_server s_l2cap_server = {
	.psm = ULTRAWIDELOCK_L2CAP_SPSM,
	.sec_level = BT_SECURITY_L1, /* credential encrypts at the application layer */
	.accept = coc_accept,
};

/* ---- GATT: reader-SPSM READ + device-version WRITE ------------------------ */

/**
 * Encode credential feature flags (timesync procedures 0 and 1, LE Coded PHY) into a byte bitmap
 * for the service data advertisement.
 */
static uint8_t encode_features(const struct ultrawidelock_ble_features *f)
{
	uint8_t b = 0;

	if (f->timesync_procedure_0) {
		b |= (uint8_t)(1u << 0);
	}
	if (f->timesync_procedure_1) {
		b |= (uint8_t)(1u << 1);
	}
	if (f->le_coded_phy) {
		b |= (uint8_t)(1u << 2);
	}
	return b;
}

/**
 * Build the credential BLE advertisement payload containing the L2CAP SPSM, supported protocol
 * versions, and feature flags.
 */
static void build_read_payload(const struct ultrawidelock_ble_config *cfg)
{
	uint8_t *p = s_read_payload;

	*p++ = (uint8_t)(ULTRAWIDELOCK_L2CAP_SPSM >> 8);
	*p++ = (uint8_t)(ULTRAWIDELOCK_L2CAP_SPSM & 0xffu);

	*p++ = (uint8_t)(s_versions_count * 2u);
	for (size_t i = 0; i < s_versions_count; i++) {
		*p++ = (uint8_t)(s_versions[i] >> 8);
		*p++ = (uint8_t)(s_versions[i] & 0xffu);
	}

	*p++ = 1u; /* features length: SupportedFeatures is one packed byte */
	*p++ = encode_features(&cfg->features);

	s_read_payload_len = (uint16_t)(p - s_read_payload);
}

/**
 * GATT read callback that returns the static credential reader payload (service data with identity
 * material and features).
 */
static ssize_t reader_spsm_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	(void)ultrawidelock_lat_mark(ULTRAWIDELOCK_LAT_GATT_SPSM_READ);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, s_read_payload, s_read_payload_len);
}

/* The peer writes the BLE-UWB protocol version it selected. Both shipped
 * readers require at least 3 bytes here (see 588df2e); we only need to accept
 * it, the reader engine reads the selection off the transaction itself. */
static ssize_t device_ver_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(buf);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	(void)ultrawidelock_lat_mark(ULTRAWIDELOCK_LAT_GATT_VER_WRITE);
	return len;
}

/* No _ENC / _AUTHEN on either permission: credential runs its own application-layer
 * secure channel, and requiring BLE bonding here would break the walk-up. The
 * shipped ESP32 reader is unpaired for the same reason and unlocks a real
 * iPhone Wallet key. */
BT_GATT_SERVICE_DEFINE(ultrawidelock_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0xFFF2)),
	BT_GATT_CHARACTERISTIC(&k_chr_reader_spsm_uuid.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, reader_spsm_read, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&k_chr_device_ver_uuid.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, device_ver_write, NULL),
);

/* ---- advertising --------------------------------------------------------- */

/* credential 1.0 section 11.3 (Table 11-2). 24 payload bytes after the 16-bit UUID:
 *   [0]      flags: bit7 = BLE+UWB supported, bits2:0 = version (0)
 *   [1]      tx power (int8)
 *   [2..9]   truncated reader group id      = reader_id[0..7]
 *   [10..11] truncated reader group sub id  = reader_id[16..17]
 *   [12..15] dynamic-tag expiry, big-endian (0xFFFFFFFF = no clock)
 *   [16]     reserved
 *   [17..23] dynamic tag
 * The derivation wants the identity address MSB-first; bt_id_get hands it out
 * LSB-first, same as NimBLE.
 */
static bool build_ultrawidelock_svc_data(uint8_t out[24], uint32_t *expiry_out,
					 time_t *wall_time_out)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	uint8_t group_id[sizeof(s_adv_group_id)];
	uint8_t sub_id[sizeof(s_adv_sub_id)];
	uint8_t grk[sizeof(s_adv_grk)];
	int8_t tx_power;
	k_spinlock_key_t key;

	bt_id_get(addrs, &count);
	if (count == 0) {
		LOG_WRN("adv: no identity address for the dynamic tag");
		return false;
	}

	uint8_t adva_msb[6];

	for (int i = 0; i < 6; i++) {
		adva_msb[i] = addrs[0].a.val[5 - i];
	}

	uint32_t expiry = ULTRAWIDELOCK_ADVTAG_EXPIRY_UNAVAILABLE;
	time_t now = time(NULL);

	if (now >= ULTRAWIDELOCK_ADV_TIME_FLOOR &&
	    (uint64_t)now <= UINT32_MAX - ULTRAWIDELOCK_ADV_TAG_VALID_S) {
		expiry = (uint32_t)now + ULTRAWIDELOCK_ADV_TAG_VALID_S;
	}

	key = k_spin_lock(&s_adv_params_lock);
	memcpy(group_id, s_adv_group_id, sizeof(group_id));
	memcpy(sub_id, s_adv_sub_id, sizeof(sub_id));
	memcpy(grk, s_adv_grk, sizeof(grk));
	tx_power = s_adv_tx_power;
	k_spin_unlock(&s_adv_params_lock, key);

	uint8_t dyn_tag[ULTRAWIDELOCK_ADVTAG_LEN];
	int rc = ultrawidelock_advtag_derive(grk, adva_msb, expiry, dyn_tag);

	if (rc != 0) {
		LOG_ERR("adv: dynamic-tag derive rc=%d", rc);
		return false;
	}

	uint8_t *p = out;

	*p++ = 0x80u; /* flags: BLE+UWB supported, version 0 */
	*p++ = (uint8_t)tx_power;
	memcpy(p, group_id, sizeof(group_id));
	p += sizeof(group_id);
	memcpy(p, sub_id, sizeof(sub_id));
	p += sizeof(sub_id);
	*p++ = (uint8_t)(expiry >> 24);
	*p++ = (uint8_t)(expiry >> 16);
	*p++ = (uint8_t)(expiry >> 8);
	*p++ = (uint8_t)expiry;
	*p++ = 0x00u; /* reserved */
	memcpy(p, dyn_tag, ULTRAWIDELOCK_ADVTAG_LEN);
	*expiry_out = expiry;
	*wall_time_out = now;
	return true;
}

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_DFU_SMP_IMG)
/*
 * The SMP service, in the SCAN RESPONSE, so that a phone can find this board.
 *
 * mcumgr clients -- nRF Device Manager among them -- filter their scan list on
 * this UUID, and Zephyr's SMP transport registers the GATT service WITHOUT
 * advertising it, because advertising data belongs to the application. The
 * result is a board whose mcumgr works perfectly and which is simply absent
 * from the app's list, indistinguishable from one that is switched off.
 * MEASURED before this existed: the advertisement carried no service UUIDs at
 * all, and the name a Mac displayed came from the cached GATT one.
 *
 * The scan response and not the advert, because a 128-bit UUID is 18 of the 31
 * bytes available and the advert is already carrying the credential payload an
 * iPhone needs in order to resolve an approach. The scan response is a second
 * 31 bytes that costs the advert nothing, and every mcumgr client scans
 * actively, so it always asks for it. The name goes here too: 18 + 11 = 29,
 * which fits, and a device with no name in a scanner list is not findable by a
 * human even when the filter passes it.
 */
static const struct bt_data smp_sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, SMP_BT_SVC_UUID_VAL),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
#define SMP_SD     smp_sd
#define SMP_SD_LEN ARRAY_SIZE(smp_sd)
#else
#define SMP_SD     NULL
#define SMP_SD_LEN 0
#endif

struct advertising_result {
	bool as_reader;
	bool reader_expected;
	bool time_valid;
	uint32_t expiry;
	time_t wall_time;
};

struct advertising_payload {
	uint8_t svc_data[2 + 24]; /* BT_DATA_SVC_DATA16 carries the UUID inline */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
	uint8_t matter_svc_data[MATTER_BLE_SVC_DATA_LEN];
#endif
	struct bt_data ad[3];
	size_t ad_len;
	struct advertising_result result;
};

static bool s_applied_as_reader;
static bool s_applied_time_valid;
static uint32_t s_applied_expiry;
static time_t s_applied_wall_time;
static int64_t s_applied_uptime_ms;
static uint32_t s_adv_retry_ms = ULTRAWIDELOCK_ADV_RETRY_MIN_MS;
static uint32_t s_adv_retry_attempts;

static void advertising_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(s_advertising_work, advertising_work_fn);

static bool advertising_reader_configured(void)
{
	bool configured;
	k_spinlock_key_t key = k_spin_lock(&s_adv_params_lock);

	configured = s_adv_ultrawidelock;
	k_spin_unlock(&s_adv_params_lock, key);
	return configured;
}

/** Build one self-contained payload. Its buffers only need to live until the
 * synchronous Zephyr advertising call copies them into the host advertising
 * set. */
static void advertising_payload_build(struct advertising_payload *payload)
{
	static const uint8_t uuid16[2] = {0xF2u, 0xFFu};
	bool commissioned;

	memset(payload, 0, sizeof(*payload));
	payload->svc_data[0] = 0xF2u; /* 0xFFF2, little-endian */
	payload->svc_data[1] = 0xFFu;
	payload->ad[0] = (struct bt_data)BT_DATA_BYTES(BT_DATA_FLAGS,
						       BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR);

	/* Being FINDABLE outranks being approach-resolvable. Only one of the two
	 * payloads fits a legacy advert. */
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
	commissioned = matter_commission_has_fabric() && !matter_commission_window_open();
#else
	commissioned = true;
#endif
	payload->result.reader_expected = commissioned && advertising_reader_configured();

	if (payload->result.reader_expected &&
	    build_ultrawidelock_svc_data(&payload->svc_data[2], &payload->result.expiry,
					      &payload->result.wall_time)) {
		payload->ad[1] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16, payload->svc_data,
							 sizeof(payload->svc_data));
		payload->ad_len = 2;
		payload->result.as_reader = true;
		payload->result.time_valid =
			payload->result.expiry != ULTRAWIDELOCK_ADVTAG_EXPIRY_UNAVAILABLE;
		return;
	}

	/* Unprovisioned / no GRK, or a transient tag-build failure: keep the node
	 * findable while the worker retries the desired reader payload. */
	payload->ad[1] =
		(struct bt_data)BT_DATA(BT_DATA_UUID16_ALL, uuid16, sizeof(uuid16));
	payload->ad_len = 2;

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MATTER_BLE)
	if (matter_ble_commissionable_svc_data(payload->matter_svc_data,
					       sizeof(payload->matter_svc_data)) == 0) {
		payload->ad[2] = (struct bt_data)BT_DATA(BT_DATA_SVC_DATA16,
							 payload->matter_svc_data,
							 sizeof(payload->matter_svc_data));
		payload->ad_len = 3;
	}
#endif
}

/** Apply a payload to the legacy advertising set. When the set is active,
 * update it in place so a tag rotation never creates a stop/start discovery
 * gap. If our state is stale, Zephyr's -EINVAL/-EAGAIN and -EALREADY results
 * let us converge without stopping a valid set. */
static int advertising_apply(struct advertising_result *result)
{
	struct advertising_payload payload;
	bool updated = false;
	int rc;

	if (atomic_get(&s_conn_up) != 0) {
		return -EAGAIN;
	}

	advertising_payload_build(&payload);

	if (atomic_get(&s_adv_running) != 0) {
		rc = bt_le_adv_update_data(payload.ad, payload.ad_len, SMP_SD, SMP_SD_LEN);
		if (rc == 0) {
			updated = true;
		} else if (rc == -EINVAL || rc == -EAGAIN) {
			atomic_set(&s_adv_running, 0);
		} else {
			return rc;
		}
	}

	if (atomic_get(&s_adv_running) == 0) {
		rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, payload.ad, payload.ad_len, SMP_SD,
				     SMP_SD_LEN);
		if (rc == -EALREADY) {
			/* The set is active even though the connection callback made our
			 * hint stale. Update it in place; never stop it just to refresh. */
			rc = bt_le_adv_update_data(payload.ad, payload.ad_len, SMP_SD, SMP_SD_LEN);
			updated = rc == 0;
		}
		if (rc != 0) {
			return rc;
		}
		atomic_set(&s_adv_running, 1);
	}

	*result = payload.result;
	LOG_INF("advertising %s: %s (%u AD elements)", updated ? "updated" : "started",
		 result->as_reader ? "credential reader 0xFFF2" : "commissioning/findability",
		 (unsigned int)payload.ad_len);
	return 0;
}

static bool advertising_refresh_due(void)
{
	time_t now;
	bool time_valid;
	int64_t elapsed_s;
	int64_t expected_wall;
	int64_t clock_delta;

	if (!s_applied_as_reader) {
		return false;
	}

	now = time(NULL);
	time_valid = now >= ULTRAWIDELOCK_ADV_TIME_FLOOR &&
		     (uint64_t)now <= UINT32_MAX - ULTRAWIDELOCK_ADV_TAG_VALID_S;
	if (time_valid != s_applied_time_valid) {
		return true;
	}
	if (!time_valid) {
		return false;
	}
	if ((uint64_t)now + ULTRAWIDELOCK_ADV_REFRESH_MARGIN_S >= s_applied_expiry) {
		return true;
	}

	/* An ordinary clock advances with uptime. A larger difference means it
	 * stepped and the tag should be rebuilt from the new wall time. */
	elapsed_s = (k_uptime_get() - s_applied_uptime_ms) / MSEC_PER_SEC;
	expected_wall = (int64_t)s_applied_wall_time + elapsed_s;
	clock_delta = (int64_t)now - expected_wall;
	return clock_delta > ULTRAWIDELOCK_ADV_CLOCK_STEP_TOLERANCE_S ||
	       clock_delta < -ULTRAWIDELOCK_ADV_CLOCK_STEP_TOLERANCE_S;
}

static void advertising_retry_schedule(int rc)
{
	uint32_t delay_ms = s_adv_retry_ms;
	uint32_t attempt = ++s_adv_retry_attempts;

	if (s_adv_retry_ms < ULTRAWIDELOCK_ADV_RETRY_MAX_MS) {
		s_adv_retry_ms = MIN(s_adv_retry_ms * 2u, ULTRAWIDELOCK_ADV_RETRY_MAX_MS);
	}

	/* Log the first and power-of-two attempts, not every capped retry forever. */
	if (attempt == 1u || (attempt & (attempt - 1u)) == 0u) {
		LOG_ERR("advertising retry %u in %u ms (rc=%d)", (unsigned int)attempt,
			(unsigned int)delay_ms, rc);
	}
	(void)k_work_reschedule(&s_advertising_work, K_MSEC(delay_ms));
}

static void advertising_success_record(const struct advertising_result *result)
{
	s_applied_as_reader = result->as_reader;
	s_applied_time_valid = result->time_valid;
	s_applied_expiry = result->expiry;
	s_applied_wall_time = result->wall_time;
	s_applied_uptime_ms = k_uptime_get();

	if (result->reader_expected && !result->as_reader) {
		/* Findability succeeded, but the requested dynamic tag did not. */
		atomic_set(&s_adv_dirty, 1);
		advertising_retry_schedule(-EIO);
		return;
	}
	s_adv_retry_ms = ULTRAWIDELOCK_ADV_RETRY_MIN_MS;
	s_adv_retry_attempts = 0u;
	if (atomic_get(&s_adv_dirty) != 0) {
		(void)k_work_reschedule(&s_advertising_work, K_NO_WAIT);
	} else if (result->as_reader) {
		(void)k_work_reschedule(&s_advertising_work,
					K_SECONDS(ULTRAWIDELOCK_ADV_CLOCK_POLL_S));
	}
}

/** Refresh or resume advertising with one bounded, non-blocking apply attempt
 * per pass. A connection defers the dirty payload until disconnect; an error
 * retries forever with a bounded delay. */
static void advertising_work_fn(struct k_work *w)
{
	struct advertising_result result;
	int rc;

	ARG_UNUSED(w);

	if (atomic_get(&s_conn_up) != 0) {
		atomic_set(&s_adv_dirty, 1);
		LOG_INF("advertising refresh deferred until disconnect");
		return;
	}
	if (atomic_get(&s_adv_dirty) == 0 && !advertising_refresh_due()) {
		if (s_applied_as_reader) {
			(void)k_work_reschedule(&s_advertising_work,
						K_SECONDS(ULTRAWIDELOCK_ADV_CLOCK_POLL_S));
		}
		return;
	}

	/* Clear before applying. A request arriving during the synchronous call
	 * sets it again and is therefore not lost. */
	atomic_set(&s_adv_dirty, 0);
	rc = advertising_apply(&result);
	if (rc != 0) {
		atomic_set(&s_adv_dirty, 1);
		if (atomic_get(&s_conn_up) == 0) {
			advertising_retry_schedule(rc);
		}
		return;
	}
	advertising_success_record(&result);
}

/*
 * A phone that sends CONNECT_IND and is then never heard from again.
 *
 * The controller reports 0x3E once the establishment window (six connection
 * events) passes with no packet received, and the host prints its own line per
 * attempt -- 13 of them inside 4.3 s on 2026-08-02, then a normal connection.
 * Individually those lines say nothing: one is ordinary RF, a RUN of them is a
 * board that could not answer, and the two look identical unless the run is
 * counted. So count it, and report the run when it ends.
 */
static uint16_t s_estab_fails;
static uint32_t s_estab_first_ms;

/*
 * THE A/B. Change this number, flash, repeat the same walk-up and walk-away,
 * and compare the run line above.
 *
 *   50 (B, here)  what shipped before fa8f5e2
 *    0 (A)        what fa8f5e2 changed it to
 *
 * fa8f5e2 dropped the wait on the argument that a 0x3E never carried a byte, so
 * time spent not advertising is time the phone cannot find us. The measurement
 * that followed does not obviously support it. Bursts predate the change -- 13
 * and 8 failures -- but at 50 ms their gaps were 233 to 362 ms, consistent with
 * six connection events plus the wait; at 0 ms the same burst produced gaps of
 * 125 ms, SHORTER than six connection events can take. Those are not
 * establishment timeouts, so something else is ending them early.
 *
 * The suspicion this tests: restarting an advertising set tears down and
 * re-schedules radio activity, and a CONNECT_IND landing in that window gets a
 * connection with no valid anchor. Restarting instantly makes that window come
 * round more often.
 *
 * If B has fewer failures and slower gaps, the restart race is real and the
 * instant re-advertise has to go. If B differs only in pace, this is innocent
 * and the cause is RF or single-core contention with the DW3110, which needs
 * instrumentation rather than a knob.
 */
#define READVERTISE_AFTER_ESTAB_FAIL_MS 50

/**
 * Mark the BLE connection established on successful controller completion, noting that the callback
 * fires for every connection attempt including those about to fail.
 */
static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);
	/*
	 * NOT where the run is reported, which took a hardware run to learn.
	 * The controller completes the connection first and only discovers the
	 * establishment failure afterwards, so this fires with err == 0 for
	 * EVERY attempt in a run -- including the ones about to fail. Counting
	 * the run here turned eight consecutive failures into eight runs of one.
	 */
	if (err == 0u) {
		atomic_set(&s_conn_up, 1);
		/* Legacy connectable advertising stops when the slot is taken. A
		 * potentially long connection also makes the current tag stale, so
		 * force a fresh payload when the slot becomes free. */
		atomic_set(&s_adv_running, 0);
		atomic_set(&s_adv_dirty, 1);
		ultrawidelock_lat_begin();
	}
}

/**
 * Mark the BLE connection dropped, count establishment failures and report them in one log line
 * when the run ends, then schedule re-advertisement.
 */
static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_set(&s_conn_up, 0);
	atomic_set(&s_adv_running, 0);
	atomic_set(&s_adv_dirty, 1);
	if (reason == BT_HCI_ERR_CONN_FAIL_TO_ESTAB) {
		if (s_estab_fails == 0u) {
			s_estab_first_ms = k_uptime_get_32();
		}
		s_estab_fails++;
	} else if (s_estab_fails > 0u) {
		/*
		 * Any other reason means the link carried something before it
		 * ended, so the run is over and its length is worth having in
		 * one line. 0x3E connections are the ones that never lived.
		 */
		LOG_WRN("%u connection(s) never established over %u ms before this one",
			(unsigned int)s_estab_fails,
			(unsigned int)(k_uptime_get_32() - s_estab_first_ms));
		s_estab_fails = 0u;
	}
	LOG_INF("BLE disconnected (0x%02x); re-advertising", reason);
	(void)k_work_reschedule(&s_advertising_work,
				reason == BT_HCI_ERR_CONN_FAIL_TO_ESTAB
					? K_MSEC(READVERTISE_AFTER_ESTAB_FAIL_MS)
					: K_MSEC(50));
}

/**
 * Report the connection interval the peer actually granted, which is the only proof the requested
 * 15-30 ms took effect (prj.conf sets the preferred parameters and the 300 ms request timer).
 */
static void on_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
				uint16_t timeout)
{
	ARG_UNUSED(conn);
	/* The request is only a request. iOS may grant it, counter it, or ignore
	 * it, and every BLE round-trip on this board is priced off the answer, so
	 * the granted value is worth one line per link rather than a guess. */
	LOG_INF("BLE conn params: interval %u.%02u ms, latency %u, timeout %u ms",
		(unsigned int)(interval * 125u / 100u), (unsigned int)(interval * 125u % 100u),
		(unsigned int)latency, (unsigned int)timeout * 10u);
}

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
/** Record the negotiated Link Layer payload geometry. The controller maximum
 * is only a capability; this callback is the evidence that the peer accepted
 * it. */
static void on_le_data_len_updated(struct bt_conn *conn, struct bt_conn_le_data_len_info *info)
{
	ARG_UNUSED(conn);
	LOG_INF("BLE data length: tx %u B/%u us, rx %u B/%u us", (unsigned int)info->tx_max_len,
		(unsigned int)info->tx_max_time, (unsigned int)info->rx_max_len,
		(unsigned int)info->rx_max_time);
}
#endif

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static const char *phy_name(uint8_t phy)
{
	switch (phy) {
	case BT_GAP_LE_PHY_1M:
		return "1M";
	case BT_GAP_LE_PHY_2M:
		return "2M";
	case BT_GAP_LE_PHY_CODED:
		return "coded";
	default:
		return "unknown";
	}
}

/** Record the PHY actually granted in each direction for the 2M A/B. */
static void on_le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *info)
{
	ARG_UNUSED(conn);
	LOG_INF("BLE PHY: tx %s, rx %s", phy_name(info->tx_phy), phy_name(info->rx_phy));
}
#endif

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_updated = on_le_param_updated,
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
	.le_data_len_updated = on_le_data_len_updated,
#endif
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	.le_phy_updated = on_le_phy_updated,
#endif
};

/* ---- the ultrawidelock_ble.h seam ------------------------------------------------ */

/**
 * Validate and store credential BLE configuration: protocol versions and callback handler. Caller
 * must provide non-null cfg with non-empty proto_versions array sized <=
 * ULTRAWIDELOCK_MAX_VERSIONS; returns 0 on success or -EINVAL if any parameter is invalid.
 */
int ultrawidelock_ble_prepare(const struct ultrawidelock_ble_config *cfg)
{
	if (cfg == NULL || cfg->proto_versions == NULL || cfg->proto_versions_count == 0 ||
	    cfg->proto_versions_count > ULTRAWIDELOCK_MAX_VERSIONS) {
		return -EINVAL;
	}
	s_versions_count = cfg->proto_versions_count;
	memcpy(s_versions, cfg->proto_versions, s_versions_count * sizeof(s_versions[0]));
	s_cb = cfg->cb;
	build_read_payload(cfg);
	return 0;
}

int ultrawidelock_ble_start(const struct ultrawidelock_ble_config *cfg)
{
	struct advertising_result advertising;
	int rc = ultrawidelock_ble_prepare(cfg);

	if (rc != 0) {
		return rc;
	}

	/* Bring-up instrumentation: bt_enable blocks until the controller is up,
	 * and on nRF52 it will block FOREVER if the configured LFCLK source never
	 * starts. Bracketing it is the difference between knowing and guessing. */
	LOG_INF("bt_enable ...");
	rc = bt_enable(NULL);
	LOG_INF("bt_enable = %d", rc);
	if (rc != 0) {
		LOG_ERR("bt_enable rc=%d", rc);
		return rc;
	}

	rc = bt_l2cap_server_register(&s_l2cap_server);
	if (rc != 0) {
		LOG_ERR("l2cap server register rc=%d", rc);
		return rc;
	}

	rc = advertising_apply(&advertising);
	if (rc != 0) {
		LOG_ERR("adv start rc=%d", rc);
		return rc;
	}
	advertising_success_record(&advertising);
	LOG_INF("credential reader up; advertising (SPSM 0x%04x)", (unsigned)ULTRAWIDELOCK_L2CAP_SPSM);
	return 0;
}

/**
 * Return the L2CAP protocol/service multiplexer for the credential reader channel.
 */
uint16_t ultrawidelock_ble_spsm(void)
{
	return ULTRAWIDELOCK_L2CAP_SPSM;
}

/**
 * Send an APDU over the active L2CAP CoC link; fails if not connected. Copies data into a reserved
 * net_buf and asserts the payload fits the pool buffer to catch oversized framing from the reader
 * itself.
 */
int ultrawidelock_ble_send(uint16_t conn_handle, const uint8_t *data, size_t len)
{
	ARG_UNUSED(conn_handle);

	if (!s_coc.in_use) {
		return -ENOTCONN;
	}

	struct net_buf *buf = net_buf_alloc(&s_coc_tx_pool, K_MSEC(100));

	if (buf == NULL) {
		return -ENOMEM;
	}
	net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
	/*
	 * Checked here rather than left to net_buf's own assert, because
	 * CONFIG_ASSERT is not set in any image this board ships: net_buf_add()
	 * compiles its __ASSERT out, so an oversized len would run off the end
	 * of the pool buffer and corrupt whatever follows it, silently. The
	 * length comes from the reader's own encoded APDU rather than from the
	 * wire, so this is a guard on our own framing, not on an attacker --
	 * which is exactly the kind that disappears when the assert does.
	 */
	if (len > net_buf_tailroom(buf)) {
		LOG_ERR("APDU of %u B does not fit the CoC buffer (%u B tailroom)", (unsigned int)len,
			(unsigned int)net_buf_tailroom(buf));
		net_buf_unref(buf);
		return -EMSGSIZE;
	}
	net_buf_add_mem(buf, data, len);

	int rc = bt_l2cap_chan_send(&s_coc.le.chan, buf);

	if (rc < 0) {
		net_buf_unref(buf);
		return rc;
	}
	return 0;
}

/**
 * Disconnect the active L2CAP CoC link, terminating the credential protocol exchange.
 */
int ultrawidelock_ble_disconnect(uint16_t conn_handle)
{
	ARG_UNUSED(conn_handle);

	if (s_coc.conn == NULL) {
		return -ENOTCONN;
	}
	return bt_conn_disconnect(s_coc.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

/**
 * Store the credential reader identity material (group ID, sub ID, GRK, TX power) to populate the
 * service data advertisement on the next readvertise call.
 */
void ultrawidelock_ble_set_adv_params(const uint8_t group_id8[8], const uint8_t sub_id2[2],
			      const uint8_t grk[16], int8_t tx_power)
{
	k_spinlock_key_t key = k_spin_lock(&s_adv_params_lock);

	memcpy(s_adv_group_id, group_id8, sizeof(s_adv_group_id));
	memcpy(s_adv_sub_id, sub_id2, sizeof(s_adv_sub_id));
	memcpy(s_adv_grk, grk, sizeof(s_adv_grk));
	s_adv_tx_power = tx_power;
	s_adv_ultrawidelock = true;
	k_spin_unlock(&s_adv_params_lock, key);
}

/**
 * Re-advertise the credential reader service or Matter commissioning availability over BLE after a
 * state change requiring advertisement resume.
 */
void ultrawidelock_ble_readvertise(void)
{
	atomic_set(&s_adv_dirty, 1);
	(void)k_work_reschedule(&s_advertising_work, K_NO_WAIT);
}

/**
 * Re-advertise the credential reader service or Matter commissioning availability over BLE after
 * the system time is updated.
 */
void ultrawidelock_ble_time_updated(void)
{
	atomic_set(&s_adv_dirty, 1);
	(void)k_work_reschedule(&s_advertising_work, K_NO_WAIT);
}

/* The reader engine marshals these onto the transport's own task so a caller
 * elsewhere cannot race the BleSK counter. Zephyr's BLE callbacks all run on
 * the system workqueue, so posting to it is the same guarantee. */
static void (*s_status_cb)(bool);
static bool s_status_unsecured;
static void (*s_presence_cb)(void);

/**
 * Deferred work callback that invokes the reader status callback with the unsecured flag if set.
 */
static void status_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (s_status_cb != NULL) {
		s_status_cb(s_status_unsecured);
	}
}
static K_WORK_DEFINE(s_status_work, status_work_fn);

static void (*s_reader_tick_cb)(void);
static void reader_tick_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (s_reader_tick_cb != NULL) {
		s_reader_tick_cb();
	}
}
static K_WORK_DEFINE(s_reader_tick_work, reader_tick_work_fn);

/**
 * Deferred work callback that invokes the presence reset callback if set.
 */
static void presence_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (s_presence_cb != NULL) {
		s_presence_cb();
	}
}
static K_WORK_DEFINE(s_presence_work, presence_work_fn);

static void (*s_revoke_cb)(void);

/**
 * Deferred work callback that invokes the post-revocation link sweep if set.
 */
static void revoke_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (s_revoke_cb != NULL) {
		s_revoke_cb();
	}
}
static K_WORK_DEFINE(s_revoke_work, revoke_work_fn);

/**
 * Queue a reader status callback with unsecured state to run asynchronously on the work queue.
 */
void ultrawidelock_ble_post_reader_status(void (*cb)(bool unsecured), bool unsecured)
{
	s_status_cb = cb;
	s_status_unsecured = unsecured;
	k_work_submit(&s_status_work);
}

void ultrawidelock_ble_post_reader_tick(void (*cb)(void))
{
	/* The reader installs one immutable callback. Assigning it only before the
	 * first submit avoids racing a later workqueue read on every app-loop tick. */
	if (s_reader_tick_cb == NULL) {
		s_reader_tick_cb = cb;
	} else if (cb != s_reader_tick_cb) {
		LOG_ERR("reader tick callback changed after initialization");
		return;
	}
	k_work_submit(&s_reader_tick_work);
}

/**
 * Queue a presence reset callback to run asynchronously on the work queue.
 */
void ultrawidelock_ble_post_presence_reset(void (*cb)(void))
{
	s_presence_cb = cb;
	k_work_submit(&s_presence_work);
}

/**
 * Queue a post-revocation link sweep to run asynchronously on the work queue.
 */
void ultrawidelock_ble_post_revoke_sweep(void (*cb)(void))
{
	s_revoke_cb = cb;
	k_work_submit(&s_revoke_work);
}

/* Attach mode exists only so the ESP32 reader can share a NimBLE host with
 * esp-matter. Nothing shares this host. */
const struct ble_gatt_svc_def *ultrawidelock_ble_service_def(void)
{
	return NULL;
}

/**
 * Not supported on this target; returns ENOTSUP.
 */
int ultrawidelock_ble_start_attached(void)
{
	return -ENOTSUP;
}
