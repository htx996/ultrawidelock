/* SPDX-License-Identifier: ISC */

/**
 * @file
 * @brief The over-the-air update channel: a second L2CAP CoC, and the button
 *        that opens it.
 *
 * Not mcumgr: SMP-over-BT costs 3,717 B of RAM this image does not have, and
 * its permission model either demands pairing (the walk-up unlock depends on
 * never asking) or hands an unauthenticated peer flash writes and a reset
 * command. So the patch rides the CoC transport this board already has, on its
 * own PSM, and authorization is a WINDOW, not a handshake. The window is only a
 * denial-of-service control: the patch header is signed and checked
 * (dfu_receiver.c) and MCUboot re-verifies the patched RESULT, so no peer can
 * install code -- a closed channel just stops strangers spending erase cycles.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MCUBOOT_RECOVERY_GESTURE)
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>
#endif

#include "ultrawidelock_dfu_rx.h"

LOG_MODULE_DECLARE(ultrawidelock_dfu, CONFIG_ULTRAWIDELOCK_DFU_LOG_LEVEL);

/**
 * Its own PSM, one above the reader's.
 *
 * Deliberately not multiplexed onto the credential PSM. That channel's bytes go
 * straight into the reader's APDU parser, which is the most security-sensitive
 * parser on the board; giving it a second message class to distinguish would
 * put update framing inside the unlock path. A separate PSM costs one
 * bt_l2cap_server and keeps them apart.
 */
#define DFU_L2CAP_PSM 0x0081u

/* One frame each way. The host waits for a reply before sending more, so there
 * is never more than one outstanding in either direction. */
#define DFU_MTU 256u

NET_BUF_POOL_FIXED_DEFINE(s_dfu_rx_pool, 1, BT_L2CAP_SDU_BUF_SIZE(DFU_MTU),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);
NET_BUF_POOL_FIXED_DEFINE(s_dfu_tx_pool, 1, BT_L2CAP_SDU_BUF_SIZE(ULTRAWIDELOCK_DFU_RSP_MAX),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct {
	struct bt_l2cap_le_chan le;
	bool in_use;
} s_ch;

/**
 * DFU RX buffer allocation callback. Allocates a network buffer from the DFU RX pool with no wait,
 * or returns NULL if the pool is exhausted.
 */
static struct net_buf *dfu_alloc_buf(struct bt_l2cap_chan *chan)
{
	ARG_UNUSED(chan);
	return net_buf_alloc(&s_dfu_rx_pool, K_NO_WAIT);
}

/**
 * L2CAP channel RX callback for DFU firmware updates. Processes received data through
 * ultrawidelock_dfu_rx_frame, allocates a TX buffer for any response, and sends it back over the
 * L2CAP channel. Returns 0 on all paths.
 */
static int dfu_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	uint8_t rsp[ULTRAWIDELOCK_DFU_RSP_MAX];
	size_t rsp_len = 0;
	struct net_buf *out;

	(void)ultrawidelock_dfu_rx_frame(ULTRAWIDELOCK_DFU_OWNER_L2CAP, buf->data, buf->len,
					 rsp, &rsp_len);
	if (rsp_len == 0U) {
		return 0;
	}

	out = net_buf_alloc(&s_dfu_tx_pool, K_NO_WAIT);
	if (out == NULL) {
		LOG_WRN("no tx buffer for the update reply");
		return 0;
	}
	net_buf_reserve(out, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
	net_buf_add_mem(out, rsp, rsp_len);

	if (bt_l2cap_chan_send(chan, out) < 0) {
		net_buf_unref(out);
	}
	return 0;
}

/**
 * L2CAP channel connected callback for DFU firmware updates. Logs that the update channel is open.
 */
static void dfu_connected(struct bt_l2cap_chan *chan)
{
	ARG_UNUSED(chan);
	LOG_INF("update channel open");
}

/**
 * L2CAP channel disconnected callback for DFU firmware updates. Marks the channel as no longer in
 * use and resets any staged DFU bytes, so the next attempt starts clean if the connection drops
 * mid-transfer.
 */
static void dfu_disconnected(struct bt_l2cap_chan *chan)
{
	ARG_UNUSED(chan);
	s_ch.in_use = false;
	/* A dropped connection mid-transfer leaves staged bytes with no header
	 * in front of them, which the bootloader ignores. Reset anyway so the
	 * next attempt starts clean rather than resuming someone else's. */
	ultrawidelock_dfu_rx_reset(ULTRAWIDELOCK_DFU_OWNER_L2CAP);
	LOG_INF("update channel closed");
}

static const struct bt_l2cap_chan_ops k_dfu_ops = {
	.alloc_buf = dfu_alloc_buf,
	.recv = dfu_recv,
	.connected = dfu_connected,
	.disconnected = dfu_disconnected,
};

/**
 * L2CAP channel accept callback for DFU firmware updates. Returns -EACCES if no DFU window is open
 * (the gate), -ENOMEM if a channel is already in use, or 0 on success with the channel configured
 * and its pointer assigned to the caller's channel reference.
 */
static int dfu_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
		      struct bt_l2cap_chan **chan)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(server);

	/* THE GATE. Refusing here rather than inside the protocol means a peer
	 * with no window open cannot even open the channel, so none of the
	 * receiver's state is reachable. */
	if (!ultrawidelock_dfu_window_is_open()) {
		return -EACCES;
	}
	if (s_ch.in_use) {
		return -ENOMEM;
	}

	memset(&s_ch.le, 0, sizeof(s_ch.le));
	s_ch.le.chan.ops = &k_dfu_ops;
	s_ch.le.rx.mtu = DFU_MTU;
	s_ch.in_use = true;
	*chan = &s_ch.le.chan;
	return 0;
}

static struct bt_l2cap_server s_dfu_server = {
	.psm = DFU_L2CAP_PSM,
	/* Same as the credential channel: no link-layer security, because this board
	 * never pairs. Authenticity is the patch signature. */
	.sec_level = BT_SECURITY_L1,
	.accept = dfu_accept,
};

/* ---- the same frames over GATT -------------------------------------------- */
/*
 * WHY BOTH. The CoC above is the better transport and is what an iPhone app
 * would use: credit-flow-controlled, 256-byte SDUs, no ATT overhead. But NO
 * PYTHON BLUETOOTH LIBRARY CAN OPEN AN L2CAP CoC. CoreBluetooth exposes
 * openL2CAPChannel and BlueZ exposes AF_BLUETOOTH sockets, and bleak -- the
 * only cross-platform option -- wraps neither. So a bench tool on a Mac cannot
 * drive the CoC at all, and an update path nobody can invoke is not one.
 *
 * This costs almost nothing because both paths use the same parser. They pass
 * distinct owner IDs so one endpoint cannot reset or overwrite the other's
 * in-progress transfer.
 *
 * ONE HONEST DIFFERENCE. The CoC refuses the connection outright when no window
 * is open, so none of the receiver's state is reachable. A GATT write always
 * reaches the handler and is refused inside it, with ULTRAWIDELOCK_DFU_ERR_CLOSED. That
 * is a weaker gate, but the work it costs is a comparison and a two-byte
 * notification -- no flash, no allocation -- so a peer spamming it achieves
 * nothing it could not achieve by spamming any other characteristic.
 */

/* Same vendor base as the reader's own characteristic in ultrawidelock_ble_zephyr.c. */
static const struct bt_uuid_128 k_dfu_svc_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd3b5a140, 0x9e23, 0x4b3a, 0x8be4, 0x6b1ee5f980a3));
static const struct bt_uuid_128 k_dfu_chr_uuid = BT_UUID_INIT_128(
	BT_UUID_128_ENCODE(0xd3b5a141, 0x9e23, 0x4b3a, 0x8be4, 0x6b1ee5f980a3));

static ssize_t dfu_gatt_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset,
			      uint8_t flags);

BT_GATT_SERVICE_DEFINE(s_dfu_gatt, BT_GATT_PRIMARY_SERVICE(&k_dfu_svc_uuid),
		       BT_GATT_CHARACTERISTIC(&k_dfu_chr_uuid.uuid,
					      BT_GATT_CHRC_WRITE |
						      BT_GATT_CHRC_WRITE_WITHOUT_RESP |
						      BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_WRITE, NULL,
					      dfu_gatt_write, NULL),
		       BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

/**
 * GATT write callback for DFU firmware updates. Rejects writes with a nonzero offset, processes the
 * frame through ultrawidelock_dfu_rx_frame, and notifies the client of any response. Returns the
 * number of bytes consumed on success.
 */
static ssize_t dfu_gatt_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset,
			      uint8_t flags)
{
	uint8_t rsp[ULTRAWIDELOCK_DFU_RSP_MAX];
	size_t rsp_len = 0;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	(void)ultrawidelock_dfu_rx_frame(ULTRAWIDELOCK_DFU_OWNER_GATT, buf, len, rsp, &rsp_len);
	if (rsp_len > 0U) {
		(void)bt_gatt_notify(conn, &s_dfu_gatt.attrs[1], rsp, rsp_len);
	}
	return (ssize_t)len;
}

static void dfu_gap_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(reason);
	ultrawidelock_dfu_rx_reset(ULTRAWIDELOCK_DFU_OWNER_GATT);
}

BT_CONN_CB_DEFINE(dfu_conn_callbacks) = {
	.disconnected = dfu_gap_disconnected,
};

/* ---- the trigger ---------------------------------------------------------- */
/*
 * SW2, pressed while the board is running.
 *
 * That press is free. main.c samples this same button only AT BOOT -- held
 * through reset it means provisioning mode, or factory reset -- so nothing else
 * looks at it once the application is up, and a runtime press cannot be
 * confused for either of those.
 *
 * The primary trigger is Apple Home's "Turn On Pairing Mode", which sends
 * AdministratorCommissioning::OpenCommissioningWindow (cluster 0x003C).
 * modules/ultrawidelock_matter implements it (matter_clusters.c, MATTER_CMD_ADMIN_OPEN_WINDOW)
 * and admin_arm() opens this window alongside the commissioning one, for the same
 * timeout (matter_commission.c). So pairing mode and update mode are one gesture
 * on this board, by design. SW2 remains the local override for a bench with no
 * controller in reach.
 */
static const struct gpio_dt_spec s_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback s_button_cb;

#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MCUBOOT_RECOVERY_GESTURE)
/**
 * Enter MCUboot only if SW2 stayed active for the complete hold. The delayed
 * work never sleeps the system queue. A released short press simply leaves the
 * application DFU window open.
 */
static void recovery_hold_work_fn(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);
	if (gpio_pin_get_dt(&s_button) != 1) {
		return;
	}

	rc = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
	if (rc != 0) {
		LOG_ERR("cannot request MCUboot recovery (%d)", rc);
		return;
	}

	LOG_WRN("SW2 held: entering MCUboot serial recovery");
	sys_reboot(SYS_REBOOT_WARM);
}
static K_WORK_DELAYABLE_DEFINE(s_recovery_hold_work, recovery_hold_work_fn);
#endif

/**
 * Work item handler for the DFU button press. Opens a DFU window for the duration specified by
 * CONFIG_ULTRAWIDELOCK_DFU_WINDOW_MS.
 */
static void button_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	ultrawidelock_dfu_window_open(CONFIG_ULTRAWIDELOCK_DFU_WINDOW_MS);
}
static K_WORK_DEFINE(s_button_work, button_work_fn);

/**
 * GPIO interrupt handler for the DFU button. Submits the button work item to be processed off the
 * ISR, since opening the DFU window logs and touches a work queue.
 */
static void button_pressed(const struct device *dev, struct gpio_callback *cb,
			   uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* Off the ISR: opening the window logs and touches a work queue. */
	(void)k_work_submit(&s_button_work);
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MCUBOOT_RECOVERY_GESTURE)
	(void)k_work_reschedule(&s_recovery_hold_work,
				K_MSEC(CONFIG_ULTRAWIDELOCK_MCUBOOT_RECOVERY_HOLD_MS));
#endif
}

/**
 * Initialize the DFU update channel. Registers the L2CAP server, configures the optional button for
 * software window control if available, and logs readiness or warnings if the button is not ready.
 * Returns 0 on success or if button config fails gracefully (software-only mode), negative on L2CAP
 * server registration failure.
 */
int dfu_ble_start(void)
{
	int rc = bt_l2cap_server_register(&s_dfu_server);

	if (rc != 0) {
		LOG_ERR("update PSM 0x%04x register rc=%d", DFU_L2CAP_PSM, rc);
		return rc;
	}

	if (!gpio_is_ready_dt(&s_button)) {
		LOG_WRN("no update button; the window can only be opened in software");
		return 0;
	}

	rc = gpio_pin_configure_dt(&s_button, GPIO_INPUT);
	if (rc == 0) {
		rc = gpio_pin_interrupt_configure_dt(&s_button, GPIO_INT_EDGE_TO_ACTIVE);
	}
	if (rc != 0) {
		LOG_WRN("update button rc=%d; software-only window", rc);
		return 0;
	}

	gpio_init_callback(&s_button_cb, button_pressed, BIT(s_button.pin));
	(void)gpio_add_callback(s_button.port, &s_button_cb);

	LOG_INF("update channel ready on PSM 0x%04x, press SW2 to open a window",
		DFU_L2CAP_PSM);
#if IS_ENABLED(CONFIG_ULTRAWIDELOCK_MCUBOOT_RECOVERY_GESTURE)
	LOG_INF("hold SW2 for %u ms to enter MCUboot recovery",
		(unsigned int)CONFIG_ULTRAWIDELOCK_MCUBOOT_RECOVERY_HOLD_MS);
#endif
	return 0;
}
