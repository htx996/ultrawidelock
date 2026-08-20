/* SPDX-License-Identifier: ISC */

/*
 * The over-the-air update channel on NimBLE: a second L2CAP CoC, the same
 * frames over GATT, and the button that opens the window.
 *
 * Byte-for-byte the wire protocol of ports/zephyr/dfu/dfu_ble_zephyr.c -- same
 * PSM, same MTU, same two 128-bit UUIDs, same frames -- and that is the point.
 * scripts/cdk-dfu.sh and scripts/ultrawidelock_push.py drive a board without asking which
 * RTOS is on it, and an update path that needed its own tooling would be a
 * second update path to keep working.
 *
 * WHY NOT mcumgr, from the Zephyr side and still true here: SMP-over-BT costs
 * RAM this image does not have, and its permission model either demands pairing
 * (the walk-up unlock depends on never asking) or hands an unauthenticated peer
 * flash writes and a reset command. So the patch rides the CoC transport this
 * board already has, on its own PSM, and authorization is a WINDOW, not a
 * handshake. The window is only a denial-of-service control: the patch header
 * is signed and checked in dfu_receiver.c, and MCUboot re-verifies the patched
 * RESULT, so no peer can install code -- a closed channel just stops strangers
 * spending erase cycles.
 *
 * WHAT IS NOT HERE. The Zephyr file also names Apple Home's "Turn On Pairing
 * Mode" as the primary way in, because modules/ultrawidelock_matter opens this window
 * alongside the commissioning one. This port has no Matter layer yet, so SW2 is
 * currently the only way in rather than the local override it is meant to be.
 * ultrawidelock_dfu_window_open() is public, and the Matter tranche will call it from the
 * same place the Zephyr one does.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hal/nrf_gpio.h>
#include <hal/nrf_gpiote.h>
#include <nrfx.h>

#include <FreeRTOS.h>
#include <timers.h>

#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_l2cap.h>
#include <host/ble_uuid.h>
#include <os/os_mbuf.h>

#include "ultrawidelock_dfu_rx.h"

#include "ultrawidelock_freertos_board.h"
#include "ultrawidelock_freertos_nimble_host.h"
#include "ultrawidelock_freertos_platform.h"

#define TAG "dfu_ble"

/*
 * Its own PSM, one above the reader's.
 *
 * Deliberately not multiplexed onto the credential PSM. That channel's bytes go
 * straight into the reader's APDU parser, which is the most security-sensitive
 * parser on the board; giving it a second message class to distinguish would
 * put update framing inside the unlock path. A separate PSM costs one entry in
 * NimBLE's server pool -- see MYNEWT_VAL_BLE_L2CAP_COC_MAX_NUM, which is 2 for
 * exactly this reason -- and keeps them apart.
 */
#define DFU_L2CAP_PSM 0x0081u

/* One frame each way. The host waits for a reply before sending more, so there
 * is never more than one outstanding in either direction. */
#define DFU_MTU 256u

/*
 * Two buffers, not one: the stack needs a receive buffer armed for the next SDU
 * while the current one is still being handed to the receiver, and a send may
 * be in flight over the same pool. Four would be dead RAM on a protocol that is
 * lock-step by construction.
 */
#define DFU_COC_BUF_COUNT 2u

/* Five minutes, the Kconfig default the Zephyr build ships. Written as a
 * default rather than read from a config system this port does not have. */
#ifndef ULTRAWIDELOCK_DFU_WINDOW_MS
#define ULTRAWIDELOCK_DFU_WINDOW_MS 300000u
#endif

static os_membuf_t s_coc_mem[OS_MEMPOOL_SIZE(DFU_COC_BUF_COUNT, DFU_MTU)];
static struct os_mempool s_coc_mempool;
static struct os_mbuf_pool s_coc_mbuf_pool;

/* ---- the CoC ------------------------------------------------------------- */

/* Give the stack a fresh receive buffer so the next SDU can be assembled. */
static int coc_arm_rx(struct ble_l2cap_chan *chan)
{
	struct os_mbuf *rx = os_mbuf_get_pkthdr(&s_coc_mbuf_pool, 0);

	if (rx == NULL) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG, "out of rx buffers");
		return BLE_HS_ENOMEM;
	}
	return ble_l2cap_recv_ready(chan, rx);
}

/* Hand one frame to the receiver and send back whatever it produced. */
static void coc_pump(struct ble_l2cap_chan *chan, const uint8_t *frame, uint16_t len)
{
	uint8_t rsp[ULTRAWIDELOCK_DFU_RSP_MAX];
	size_t rsp_len = 0;
	struct os_mbuf *sdu;

	(void)ultrawidelock_dfu_rx_frame(ULTRAWIDELOCK_DFU_OWNER_L2CAP, frame, len, rsp,
					 &rsp_len);
	if (rsp_len == 0u) {
		return;
	}

	sdu = os_mbuf_get_pkthdr(&s_coc_mbuf_pool, 0);
	if (sdu == NULL) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, TAG, "no tx buffer for the reply");
		return;
	}
	if (os_mbuf_append(sdu, rsp, rsp_len) != 0 || ble_l2cap_send(chan, sdu) < 0) {
		/* ble_l2cap_send takes the buffer on success and on a queued
		 * result; only an outright failure leaves it ours to free. */
		os_mbuf_free_chain(sdu);
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, TAG, "reply not sent");
	}
}

static int l2cap_event_cb(struct ble_l2cap_event *event, void *arg)
{
	(void)arg;

	switch (event->type) {
	case BLE_L2CAP_EVENT_COC_ACCEPT:
		/*
		 * THE GATE. Refusing the channel rather than the frames means a
		 * peer with no window open cannot reach any of the receiver's
		 * state at all.
		 */
		if (!ultrawidelock_dfu_window_is_open()) {
			return BLE_HS_EAUTHEN;
		}
		return coc_arm_rx(event->accept.chan);

	case BLE_L2CAP_EVENT_COC_CONNECTED:
		if (event->connect.status != 0) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, TAG,
					 "update channel connect status=%d",
					 event->connect.status);
			return 0;
		}
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, TAG, "update channel open");
		return 0;

	case BLE_L2CAP_EVENT_COC_DISCONNECTED:
		/* A dropped connection mid-transfer leaves staged bytes with no
		 * header in front of them, which the bootloader ignores. Reset
		 * anyway so the next attempt starts clean rather than resuming
		 * someone else's. */
		ultrawidelock_dfu_rx_reset(ULTRAWIDELOCK_DFU_OWNER_L2CAP);
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, TAG, "update channel closed");
		return 0;

	case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: {
		struct os_mbuf *om = event->receive.sdu_rx;

		if (om != NULL) {
			uint8_t buf[DFU_MTU];
			uint16_t len = 0;

			if (ble_hs_mbuf_to_flat(om, buf, sizeof(buf), &len) == 0) {
				coc_pump(event->receive.chan, buf, len);
			}
			os_mbuf_free_chain(om);
		}
		return coc_arm_rx(event->receive.chan);
	}

	default:
		return 0;
	}
}

/* ---- the same frames over GATT ------------------------------------------- */
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
 * ONE HONEST DIFFERENCE, unchanged from the Zephyr port. The CoC refuses the
 * connection outright when no window is open, so none of the receiver's state
 * is reachable. A GATT write always reaches the handler and is refused inside
 * it, with ULTRAWIDELOCK_DFU_ERR_CLOSED. That is a weaker gate, but the work it costs is
 * a comparison and a two-byte notification -- no flash, no allocation -- so a
 * peer spamming it achieves nothing it could not achieve by spamming any other
 * characteristic.
 */

/* Same vendor base as the reader's own characteristic, and the same two UUIDs
 * the Zephyr image publishes. ble_uuid128_init takes them least significant
 * byte first, which is the reverse of how they are written above. */
static const ble_uuid128_t k_dfu_svc_uuid = BLE_UUID128_INIT(
	0xa3, 0x80, 0xf9, 0xe5, 0x1e, 0x6b, 0xe4, 0x8b,
	0x3a, 0x4b, 0x23, 0x9e, 0x40, 0xa1, 0xb5, 0xd3);
static const ble_uuid128_t k_dfu_chr_uuid = BLE_UUID128_INIT(
	0xa3, 0x80, 0xf9, 0xe5, 0x1e, 0x6b, 0xe4, 0x8b,
	0x3a, 0x4b, 0x23, 0x9e, 0x41, 0xa1, 0xb5, 0xd3);

static uint16_t s_dfu_val_handle;
static struct ble_gap_event_listener s_gap_listener;

static int dfu_gap_event(struct ble_gap_event *event, void *arg)
{
	(void)arg;
	if (event->type == BLE_GAP_EVENT_DISCONNECT) {
		ultrawidelock_dfu_rx_reset(ULTRAWIDELOCK_DFU_OWNER_GATT);
	}
	return 0;
}

static int dfu_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
			   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	uint8_t frame[DFU_MTU];
	uint8_t rsp[ULTRAWIDELOCK_DFU_RSP_MAX];
	size_t rsp_len = 0;
	uint16_t len = 0;

	(void)attr_handle;
	(void)arg;

	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
		return BLE_ATT_ERR_UNLIKELY;
	}
	if (ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof(frame), &len) != 0) {
		/* Longer than one frame can be. The protocol never produces one,
		 * so this is a peer sending something else. */
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	}

	(void)ultrawidelock_dfu_rx_frame(ULTRAWIDELOCK_DFU_OWNER_GATT, frame, len, rsp,
					 &rsp_len);
	if (rsp_len > 0u) {
		struct os_mbuf *om = ble_hs_mbuf_from_flat(rsp, (uint16_t)rsp_len);

		if (om == NULL) {
			return BLE_ATT_ERR_INSUFFICIENT_RES;
		}
		/* Takes the buffer either way, including on failure. A peer that
		 * has not subscribed simply gets nothing, which the protocol
		 * treats as a timeout -- the same as the Zephyr path. */
		(void)ble_gatts_notify_custom(conn_handle, s_dfu_val_handle, om);
	}
	return 0;
}

static const struct ble_gatt_svc_def k_gatt_svcs[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = &k_dfu_svc_uuid.u,
		.characteristics = (struct ble_gatt_chr_def[]){
			{
				.uuid = &k_dfu_chr_uuid.u,
				.access_cb = dfu_gatt_access,
				.val_handle = &s_dfu_val_handle,
				/* No _ENC or _AUTHEN flag: this board never
				 * pairs, so demanding link-layer security here
				 * would make the channel unreachable rather
				 * than safe. Authenticity is the signature on
				 * the patch header. */
				.flags = BLE_GATT_CHR_F_WRITE |
					 BLE_GATT_CHR_F_WRITE_NO_RSP |
					 BLE_GATT_CHR_F_NOTIFY,
			},
			{0},
		},
	},
	{0},
};

/* ---- the trigger --------------------------------------------------------- */
/*
 * SW2, pressed while the board is running.
 *
 * That press is free. The boot path samples this same button only AT RESET --
 * held through it means provisioning mode, or factory reset -- so nothing else
 * looks at it once the application is up, and a runtime press cannot be
 * confused for either of those.
 */

_Static_assert(ULTRAWIDELOCK_FREERTOS_SW2_GPIOTE_CHANNEL == 1u,
	       "the event and interrupt-mask names below are channel 1's");

/* Runs on the timer daemon task, not in the vector. */
static void button_deferred(void *arg1, uint32_t arg2)
{
	(void)arg1;
	(void)arg2;
	ultrawidelock_dfu_window_open(ULTRAWIDELOCK_DFU_WINDOW_MS);
}

/*
 * The vector's share of the work: check that this edge was ours, clear it, and
 * hand the rest to a task.
 *
 * Opening the window schedules a delayable work item and logs, neither of which
 * an interrupt handler may do. xTimerPendFunctionCallFromISR is the deferral
 * with no task of its own -- the timer daemon already exists because NimBLE's
 * callouts are software timers.
 *
 * Not debounced. A bouncing contact opens the window several times, and
 * ultrawidelock_dfu_window_open() restarts the clock rather than counting, so the visible
 * effect of a bounce is a window that opens once. A press that also lands
 * inside an open window costs one pended call.
 */
static void button_isr(void)
{
	BaseType_t wake = pdFALSE;

	if (!nrf_gpiote_event_check(NRF_GPIOTE, NRF_GPIOTE_EVENT_IN_1)) {
		return;
	}
	nrf_gpiote_event_clear(NRF_GPIOTE, NRF_GPIOTE_EVENT_IN_1);

	(void)xTimerPendFunctionCallFromISR(button_deferred, NULL, 0, &wake);
	portYIELD_FROM_ISR(wake);
}

static int button_init(void)
{
	/*
	 * Pull-up and active low, matching the board: the switch shorts the pin
	 * to ground, so the press is the falling edge.
	 */
	nrf_gpio_cfg_input(ULTRAWIDELOCK_FREERTOS_PIN_SW2, NRF_GPIO_PIN_PULLUP);

	if (ultrawidelock_freertos_gpiote_add_handler(button_isr) != 0) {
		return -1;
	}

	nrf_gpiote_event_configure(NRF_GPIOTE, ULTRAWIDELOCK_FREERTOS_SW2_GPIOTE_CHANNEL,
				   ULTRAWIDELOCK_FREERTOS_PIN_SW2, NRF_GPIOTE_POLARITY_HITOLO);
	nrf_gpiote_event_enable(NRF_GPIOTE, ULTRAWIDELOCK_FREERTOS_SW2_GPIOTE_CHANNEL);
	/* Cleared before the interrupt is unmasked: the pull-up settling as the
	 * pin is configured is itself an edge, and a window that opened at boot
	 * is the one state this button must never produce. */
	nrf_gpiote_event_clear(NRF_GPIOTE, NRF_GPIOTE_EVENT_IN_1);

	/*
	 * The NVIC priority is not set here. dw3000_hw_init_interrupt() owns it
	 * -- the DW3110 line is the one with a deadline -- and this handler
	 * inherits it. At priority 4 the FromISR API above is legal, which is
	 * the only thing this handler needs from it.
	 */
	NVIC_ClearPendingIRQ(GPIOTE_IRQn);
	NVIC_EnableIRQ(GPIOTE_IRQn);

	nrf_gpiote_int_enable(NRF_GPIOTE, NRF_GPIOTE_INT_IN1_MASK);
	return 0;
}

/* ---- bring-up ------------------------------------------------------------ */

/*
 * Runs inside the host startup sequence: after nimble_port_init(), so the mbuf
 * pools exist, and before the host task consumes events.
 *
 * FIRST IN THE ATTRIBUTE TABLE, which is worth stating because it is not the
 * arrangement the Zephyr image has. main() registers this before the reader,
 * and it has to: the reader's own bring-up is what starts the host, so anything
 * added after it would be adding services to a running host. The consequence is
 * that the update service takes lower handles than GAP and GATT, which
 * ble_svc_gap_init() registers from inside ultrawidelock_ble_register_gatt().
 *
 * That is a difference and not a defect. Nothing on either side of this link
 * addresses a service by handle: iOS and the credential flow both discover by UUID,
 * and the handles change between firmware images regardless. The convention
 * that GAP sits at handle 1 is a convention, not a requirement of the spec.
 */
static int dfu_register_services(void)
{
	int rc = os_mempool_init(&s_coc_mempool, DFU_COC_BUF_COUNT, DFU_MTU, s_coc_mem,
				 "ultrawidelock_dfu_coc");

	if (rc != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG, "coc mempool init rc=%d", rc);
		return -1;
	}
	rc = os_mbuf_pool_init(&s_coc_mbuf_pool, &s_coc_mempool, DFU_MTU, DFU_COC_BUF_COUNT);
	if (rc != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG, "coc mbuf pool init rc=%d", rc);
		return -1;
	}
	rc = ble_l2cap_create_server(DFU_L2CAP_PSM, DFU_MTU, l2cap_event_cb, NULL);
	if (rc != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG, "update PSM 0x%04x rc=%d",
				 (unsigned)DFU_L2CAP_PSM, rc);
		return -1;
	}

	rc = ble_gatts_count_cfg(k_gatt_svcs);
	if (rc != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG, "gatts_count_cfg rc=%d", rc);
		return -1;
	}
	rc = ble_gatts_add_svcs(k_gatt_svcs);
	if (rc != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG, "gatts_add_svcs rc=%d", rc);
		return -1;
	}
	rc = ble_gap_event_listener_register(&s_gap_listener, dfu_gap_event, NULL);
	if (rc != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG,
					 "gap listener rc=%d", rc);
		return -1;
	}
	return 0;
}

static const struct ultrawidelock_freertos_nimble_host_hooks k_hooks = {
	.register_services = dfu_register_services,
	/* No on_sync: this layer never advertises. It is found through the
	 * connection the reader's advertisement produced. */
};

int dfu_ble_start(void)
{
	if (ultrawidelock_freertos_nimble_host_add_hooks(&k_hooks) != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, TAG, "no NimBLE hook slot");
		return -1;
	}

	/*
	 * A button that will not configure is a warning, not a failure: the
	 * window can still be opened in software, and refusing to start the
	 * update channel over it would remove the only way to fix the board.
	 */
	if (button_init() != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, TAG,
				 "no update button; the window can only be opened in software");
	}

	ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, TAG,
			 "update channel ready on PSM 0x%04x, press SW2 to open a window",
			 (unsigned)DFU_L2CAP_PSM);
	return 0;
}
