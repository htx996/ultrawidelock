/* SPDX-License-Identifier: ISC */

/**
 * @file matter_ble_freertos.c — the 0xFFF6 GATT service that carries BTP, on NimBLE.
 *
 * A thin adapter, on purpose. All the framing lives in modules/ultrawidelock_matter
 * (matter_btp.c), which has no operating system dependency and is tested on the
 * host under sanitizers. This file does three things and no more: hand C1
 * writes to the reassembler, drive the fragmenter out through C2 indications,
 * and build the commissionable advertisement.
 *
 * This is the ONE piece of the Matter port that could not be shimmed. The Thread
 * transport is compiled from the Zephyr tree unmodified because it is OpenThread
 * calls underneath; this is Zephyr's bt_* GATT against NimBLE's, and the two
 * differ in shape, not just in spelling. What is carried across is the
 * BEHAVIOUR, and specifically the four things the Zephyr file records as having
 * been learned from live hardware. Each is marked below. They cost real
 * debugging sessions against real iPhones and none of them is inferable from the
 * specification.
 *
 * C1 (RX, write) is 18EE2EF5-263D-4559-959F-4F9C429F9D11, C2 (TX, indicate)
 * ...D12, spelled below in the little-endian order NimBLE's UUID macros want.
 *
 * NOT wired into the reader's advertising. The reader owns the single
 * advertising set, and matter_ble_commissionable_svc_data() only BUILDS the
 * payload -- an image with this file compiled in behaves exactly as before until
 * something asks for those bytes.
 */

#include <errno.h>
#include <string.h>

#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_hs_mbuf.h>
#include <os/os_mbuf.h>

#include <FreeRTOS.h>
#include <task.h>

#include "matter_btp.h"
#include "matter_ble_freertos.h"

#include <ultrawidelock_freertos_nimble_host.h>
#include <ultrawidelock_freertos_platform.h>

#define MATTER_BLE_TAG "matter_ble"

/*
 * Sized to match the Zephyr oracle's Kconfig defaults, so both images present
 * the same node and refuse the same messages. Changing one without the other
 * makes the two builds disagree about what fits.
 */
#define MATTER_BLE_RX_BUF      1024u
#define MATTER_BLE_WINDOW      4u
#define MATTER_BLE_DISCRIMINATOR 0xF00u
#define MATTER_BLE_TASK_STACK  1024u /* StackType_t words: 4,096 bytes */
#define MATTER_BLE_TASK_PRIO   (tskIDLE_PRIORITY + 2)

/* C1, written by the commissioner (BLEManagerImpl.cpp:78-79). */
static const ble_uuid128_t k_chr_c1_uuid = BLE_UUID128_INIT(
	0x11, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE,
	0x18);
/* C2, indicated to the commissioner (BLEManagerImpl.cpp:80-81). */
static const ble_uuid128_t k_chr_c2_uuid = BLE_UUID128_INIT(
	0x12, 0x9D, 0x9F, 0x42, 0x9C, 0x4F, 0x9F, 0x95, 0x59, 0x45, 0x3D, 0x26, 0xF5, 0x2E, 0xEE,
	0x18);

/* ---- connection-scoped state --------------------------------------------- */

/*
 * One commissioning connection at a time. That is not a simplification for now:
 * a second commissioner arriving mid-PASE would have to be refused anyway, so
 * the single slot IS the policy.
 */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_c2_val_handle;

static struct matter_btp_rx s_rx;
static uint8_t s_rx_buf[MATTER_BLE_RX_BUF];
static struct matter_btp_tx s_tx;
static uint8_t s_frag[MATTER_BTP_MAX_FRAGMENT];
static bool s_tx_active;
static bool s_indicate_busy;
static bool s_handshaked;

/*
 * LESSON 1, from the oracle, and the reason this flag is handled the way it is.
 *
 * Zephyr can be ASKED whether a peer is subscribed, and the oracle's comment is
 * emphatic that a local mirror was wrong: a commissioner subscribes BEFORE its
 * first C1 write, so the flag got set and then cleared by the reset that the C1
 * write triggers. The queued handshake response was never sent, the phone sat on
 * "connecting" until it gave up, and nothing in the log said why.
 *
 * NimBLE has no equivalent query -- subscription state arrives only as a
 * BLE_GAP_EVENT_SUBSCRIBE edge -- so a mirror is unavoidable here. The bug is
 * avoided instead by WHERE it is cleared: this variable is owned by the
 * subscribe and disconnect events alone. reset_link() deliberately does not
 * touch it, because reset_link() runs on the C1 write that claims the link, and
 * clearing it there is precisely the bug.
 */
static bool s_subscribed;

/** Negotiated BTP PDU size, header included. Settled by the handshake. */
static uint16_t s_fragment_size;

/*
 * The tx sequence counter lives HERE, not in struct matter_btp_tx, because BTP
 * numbers per connection rather than per message: a standalone ack consumes a
 * sequence number just as a data fragment does. The fragmenter is handed the
 * current value at init and its position is copied back after every emit.
 */
static uint8_t s_tx_seq;

/*
 * LESSON 2: the BTP handshake response is HELD until the peer subscribes to C2.
 *
 * A commissioner writes the handshake request to C1 FIRST and enables
 * indications on C2 second, so at the moment the request arrives there is
 * nothing to indicate on. CHIP does the same thing for the same reason:
 * HandleCapabilitiesRequestReceived() only queues the response, and
 * BLEEndPoint.cpp:202-231 sends it from HandleSubscribeReceived(). Found against
 * a real iPhone, which got as far as "BTP up" and then dropped the link.
 */
static uint8_t s_hs_resp[MATTER_BTP_RESP_LEN];
static uint8_t s_hs_resp_len;

/** A received fragment we still owe an acknowledgement for. */
static bool s_ack_pending;
static uint8_t s_ack_seq;
/** Fragments received since we last acknowledged anything. */
static uint8_t s_rx_unacked;

static matter_ble_msg_cb s_msg_cb;
static matter_ble_link_cb s_link_cb;
static matter_ble_tx_cb s_tx_cb;
/** True only after matter_ble_send() accepted a caller-owned message. */
static bool s_tx_notifies;

/* ---- deferral ------------------------------------------------------------- */

/*
 * LESSON 3: nothing is indicated from a GATT callback.
 *
 * The oracle indicated inline from its CCC handler and the phone saw an
 * indication for a subscribe it had not yet been told succeeded; it
 * unsubscribed and dropped the link, with the board's own log cheerfully
 * reporting the response as sent. CHIP's Zephyr port posts events from both the
 * CCC write and the C1 write and does the work later on its own thread.
 *
 * This port has a shared OSAL work queue, and it is deliberately NOT used. The
 * oracle owns a queue because it measured the system one at 3,568 of 4,096 bytes
 * during an unlock, and the reader already defers BLE work there. The same
 * argument applies to ultrawidelock_osal here, which the reader also uses. PASE is what
 * will actually load this stack and it must be re-measured then; 4,096 bytes is
 * the oracle's provisional figure, chosen roomy because two stack sizes were
 * guessed low earlier in this project and both faulted on hardware.
 */
static StackType_t s_task_stack[MATTER_BLE_TASK_STACK];
static StaticTask_t s_task_tcb;
static TaskHandle_t s_task;

#define WORK_HANDSHAKE 0x01u
#define WORK_MESSAGE   0x02u

static size_t s_msg_len;

static void post_work(uint32_t bits)
{
	if (s_task != NULL) {
		(void)xTaskNotify(s_task, bits, eSetBits);
	}
}

/* ---- forward declarations -------------------------------------------------- */

static int indicate_raw(const uint8_t *data, size_t len);
static int pump_tx(void);

/**
 * Reset the BTP link state.
 *
 * Note what is NOT here: s_subscribed. See LESSON 1 above -- this function runs
 * on the C1 write that claims the link, and the peer subscribed before that.
 */
static void reset_link(void)
{
	/* The BTP fragmenter borrows the application's packet. Return that loan
	 * before clearing its cursor, exactly once. */
	if (s_tx_notifies) {
		s_tx_notifies = false;
		if (s_tx_cb != NULL) {
			s_tx_cb(-ECONNRESET);
		}
	}
	matter_btp_rx_init(&s_rx, s_rx_buf, sizeof(s_rx_buf), 0u);
	memset(&s_tx, 0, sizeof(s_tx));
	s_tx_active = false;
	s_indicate_busy = false;
	s_handshaked = false;
	s_fragment_size = MATTER_BTP_MIN_FRAGMENT;
	s_tx_seq = 0u;
	s_ack_pending = false;
	s_rx_unacked = 0u;
	s_hs_resp_len = 0u;

	if (s_link_cb != NULL) {
		s_link_cb();
	}
}

static bool is_subscribed(void)
{
	return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_subscribed;
}

/* ---- the deferred half ----------------------------------------------------- */

static void hs_work_handler(void)
{
	uint8_t n = s_hs_resp_len;

	/* Not subscribed yet is not an error: the subscribe event posts this
	 * again when the subscription arrives. */
	if (n == 0u || !is_subscribed()) {
		return;
	}
	s_hs_resp_len = 0u;
	ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MATTER_BLE_TAG,
			 "BTP handshake response out (%u B)", (unsigned int)n);
	(void)indicate_raw(s_hs_resp, n);
}

static void msg_work_handler(void)
{
	if (s_msg_cb != NULL) {
		s_msg_cb(s_rx_buf, s_msg_len);
	} else {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_WARNING, MATTER_BLE_TAG,
				 "reassembled %u bytes with no handler; dropping",
				 (unsigned int)s_msg_len);
	}
	/* The reassembly area is reused for the next message only after the
	 * handler has had it, which is why this runs here and not in the GATT
	 * write callback. */
	matter_btp_rx_reset(&s_rx);
}

static void matter_ble_task(void *arg)
{
	(void)arg;

	for (;;) {
		uint32_t bits = 0;

		if (xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY) != pdTRUE) {
			continue;
		}
		if ((bits & WORK_HANDSHAKE) != 0u) {
			hs_work_handler();
		}
		if ((bits & WORK_MESSAGE) != 0u) {
			msg_work_handler();
		}
	}
}

/* ---- outbound -------------------------------------------------------------- */

/** Push one raw buffer out on C2 as an indication. */
static int indicate_raw(const uint8_t *data, size_t len)
{
	struct os_mbuf *om;
	int rc;

	if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_indicate_busy) {
		return -1;
	}
	/* Indicating before the peer subscribes is refused by the stack with a
	 * code that says nothing about why. Name it here instead. */
	if (!is_subscribed()) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
				 "indicate before the peer subscribed to C2");
		return -1;
	}

	om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
	if (om == NULL) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG, "no mbuf for indication");
		return -1;
	}

	s_indicate_busy = true;
	/* Takes ownership of om on every path, success or failure. */
	rc = ble_gatts_indicate_custom(s_conn_handle, s_c2_val_handle, om);
	if (rc != 0) {
		s_indicate_busy = false;
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
				 "ble_gatts_indicate_custom failed (%d)", rc);
	}
	return rc;
}

/** Emit the next BTP fragment, if a message is being sent. */
static int pump_tx(void)
{
	size_t n = 0;
	int rc;

	if (!s_tx_active || s_indicate_busy) {
		return 0;
	}
	rc = matter_btp_tx_next(&s_tx, s_ack_pending ? &s_ack_seq : NULL, s_frag, sizeof(s_frag),
				&n);
	if (rc == MATTER_END) {
		s_tx_active = false;
		if (s_tx_notifies) {
			s_tx_notifies = false;
			if (s_tx_cb != NULL) {
				s_tx_cb(0);
			}
		}
		return 0;
	}
	if (rc != MATTER_OK) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
				 "BTP fragmentation failed (%d)", rc);
		s_tx_active = false;
		if (s_tx_notifies) {
			s_tx_notifies = false;
			if (s_tx_cb != NULL) {
				s_tx_cb(-EIO);
			}
		}
		return -1;
	}
	rc = indicate_raw(s_frag, n);
	if (rc != 0) {
		s_tx_active = false;
		if (s_tx_notifies) {
			s_tx_notifies = false;
			if (s_tx_cb != NULL) {
				s_tx_cb(-EIO);
			}
		}
	} else {
		/* Commit transport sequence and piggybacked acknowledgement only
		 * after the stack accepted this fragment. An immediate rejection put
		 * neither on air, so advancing either would desynchronise the retry. */
		s_tx_seq = s_tx.next_seq;
		if (s_ack_pending) {
			s_ack_pending = false;
			s_rx_unacked = 0u;
		}
	}
	return rc;
}

int matter_ble_send(const uint8_t *msg, size_t len)
{
	int rc;

	if (msg == NULL || len == 0u) {
		return -1;
	}
	if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_handshaked) {
		return -1;
	}
	if (!is_subscribed() || s_tx_active) {
		return -1;
	}

	/* The fragment size is whatever the handshake settled on, not a
	 * constant: sending larger than the peer agreed to is how a
	 * commissioner silently stops reassembling. */
	rc = matter_btp_tx_init(&s_tx, msg, len, s_fragment_size, s_tx_seq);
	if (rc != MATTER_OK) {
		return -1;
	}
	s_tx_active = true;
	rc = pump_tx();
	if (rc == 0) {
		/* The confirmation arrives asynchronously. Immediate rejection is
		 * returned directly and must not also complete through the callback. */
		s_tx_notifies = true;
	}
	return rc;
}

void matter_ble_set_link_handler(matter_ble_link_cb cb)
{
	s_link_cb = cb;
}

void matter_ble_set_tx_handler(matter_ble_tx_cb cb)
{
	s_tx_cb = cb;
}

void matter_ble_set_msg_handler(matter_ble_msg_cb cb)
{
	s_msg_cb = cb;
}

/* ---- connection tracking --------------------------------------------------- */

/*
 * The connection is claimed on the first C1 write, NOT on connect. The reader
 * accepts BLE connections too, and claiming every one of them would let a credential
 * peer occupy the single commissioning slot and lock out a real commissioner. A
 * peer that writes C1 has identified itself as one.
 */
static bool claim_conn(uint16_t conn_handle)
{
	if (s_conn_handle == conn_handle) {
		return true;
	}
	if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
		/*
		 * ERROR, not INFO: this is the only path that turns a
		 * commissioner away, and it did so silently in the oracle. The
		 * peer sees a bare GATT write failure with nothing on the board
		 * to explain it, which is indistinguishable from the write never
		 * arriving -- and the two have opposite fixes.
		 */
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
				 "C1 write refused: commissioning link held by another peer");
		return false;
	}
	s_conn_handle = conn_handle;
	reset_link();
	ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MATTER_BLE_TAG, "commissioning link up");
	return true;
}

/* ---- C1: the commissioner writes ------------------------------------------- */

static int c1_write(uint16_t conn_handle, const uint8_t *p, uint16_t len)
{
	int rc;

	if (!claim_conn(conn_handle)) {
		return BLE_ATT_ERR_INSUFFICIENT_RES;
	}

	if (!s_handshaked) {
		struct matter_btp_handshake_req req;
		struct matter_btp_handshake_resp resp;
		uint8_t out[MATTER_BTP_RESP_LEN];
		size_t n = 0;

		rc = matter_btp_req_decode(p, len, &req);
		if (rc != MATTER_OK) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
					 "bad BTP handshake request (%d)", rc);
			return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
		}
		rc = matter_btp_accept(&req, ble_att_mtu(conn_handle), MATTER_BLE_WINDOW, &resp);
		if (rc != MATTER_OK) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
					 "no mutually supported BTP version");
			return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
		}
		if (matter_btp_resp_encode(&resp, out, sizeof(out), &n) != MATTER_OK) {
			return BLE_ATT_ERR_UNLIKELY;
		}

		/*
		 * LESSON 4: sequence numbering after the handshake is NOT
		 * symmetric, and getting it wrong is invisible until a real
		 * commissioner rejects the first data fragment.
		 *
		 * BLEEndPoint.cpp:512-514: "If end point plays peripheral role,
		 * expect ack for indication sent as last step of BTP handshake",
		 * i.e. expectInitialAck = (role == kBleRole_Peripheral). That
		 * selects BtpEngine::Init's expect_first_ack branch
		 * (BtpEngine.cpp:90-95): mTxNextSeqNum = 1, mRxNextSeqNum = 0.
		 *
		 * So the handshake indication queued below IS sequence 0 even
		 * though it carries no sequence byte, our first framed fragment
		 * is 1, and the peer's first data fragment is 0.
		 *
		 * The oracle had this backwards until an iPhone refused: the log
		 * said "BTP up" and then "BTP fragment refused (-4)" on Apple's
		 * very first data fragment. Nothing on a host could catch it --
		 * matter_btp.c takes the first sequence as a parameter and is
		 * correct either way; the wrong number is chosen HERE.
		 */
		matter_btp_rx_init(&s_rx, s_rx_buf, sizeof(s_rx_buf), 0u);
		s_tx_seq = 1u;
		s_fragment_size = resp.fragment_size;
		s_handshaked = true;
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MATTER_BLE_TAG,
				 "BTP up: fragment=%u window=%u (att mtu %u)",
				 (unsigned int)resp.fragment_size, (unsigned int)resp.window_size,
				 (unsigned int)ble_att_mtu(conn_handle));

		/* The reply is NOT BTP-framed; it goes out as-is. But it cannot
		 * go out yet -- see LESSON 2. */
		memcpy(s_hs_resp, out, n);
		s_hs_resp_len = (uint8_t)n;
		/* Post unconditionally. If the peer subscribed first the handler
		 * sends immediately; if not, it does nothing and the subscribe
		 * event posts again. Either order works and neither indicates
		 * from a GATT callback. */
		post_work(WORK_HANDSHAKE);
		return 0;
	}

	rc = matter_btp_rx_fragment(&s_rx, p, len);
	if (rc == MATTER_OK || rc == MATTER_END) {
		s_ack_pending = true;
		s_ack_seq = s_rx.last_seq;
		if (s_rx_unacked < UINT8_MAX) {
			s_rx_unacked++;
		}
	}
	if (rc == MATTER_END) {
		s_msg_len = s_rx.len;
		post_work(WORK_MESSAGE);
	} else if (rc != MATTER_OK) {
		/* BTP has no gap recovery: once the framing desynchronises the
		 * only move is to drop the link. */
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
				 "BTP fragment refused (%d); dropping the connection", rc);
		(void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
		return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
	}

	/*
	 * Acknowledge before the peer's window fills, or it stops sending and
	 * the commissioning stalls with no error anywhere. Half the window is
	 * the trigger rather than every fragment, which would double the packet
	 * count; a completed message is always acknowledged so the peer can move
	 * on.
	 *
	 * Nothing is sent here if a transmission is already in flight: pump_tx()
	 * piggybacks the pending ack onto the next fragment for free.
	 */
	if (s_ack_pending && !s_tx_active && !s_indicate_busy &&
	    (rc == MATTER_END || s_rx_unacked >= (MATTER_BLE_WINDOW / 2))) {
		uint8_t ackbuf[3];
		size_t n = 0;

		if (matter_btp_standalone_ack(s_ack_seq, s_tx_seq, ackbuf, sizeof(ackbuf), &n) ==
		    MATTER_OK) {
			s_tx_seq++;
			s_ack_pending = false;
			s_rx_unacked = 0u;
			(void)indicate_raw(ackbuf, n);
		}
	}
	return 0;
}

static int matter_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
			      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	uint8_t buf[MATTER_BTP_MAX_FRAGMENT];
	uint16_t len = 0;
	int rc;

	(void)attr_handle;
	(void)arg;

	if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
		return BLE_ATT_ERR_UNLIKELY;
	}
	/*
	 * A write longer than any legal BTP fragment is refused rather than
	 * truncated. Truncating would hand the reassembler a fragment whose
	 * header promises more than its body carries, which desynchronises the
	 * framing one fragment later -- and the error would then be reported
	 * against the innocent fragment.
	 */
	rc = ble_hs_mbuf_to_flat(ctxt->om, buf, (uint16_t)sizeof(buf), &len);
	if (rc != 0) {
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
				 "C1 write of %u bytes exceeds the largest BTP fragment",
				 (unsigned int)OS_MBUF_PKTLEN(ctxt->om));
		return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
	}
	return c1_write(conn_handle, buf, len);
}

static const struct ble_gatt_svc_def k_gatt_svcs[] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = BLE_UUID16_DECLARE(0xFFF6),
		.characteristics = (struct ble_gatt_chr_def[]){
			{
				.uuid = &k_chr_c1_uuid.u,
				.access_cb = matter_gatt_access,
				/*
				 * No _ENC or _AUTHEN: this board never pairs at
				 * the link layer, and demanding it here would
				 * make commissioning unreachable rather than
				 * safe. PASE is the authentication, and it runs
				 * above this transport.
				 */
				.flags = BLE_GATT_CHR_F_WRITE |
					 BLE_GATT_CHR_F_WRITE_NO_RSP,
			},
			{
				.uuid = &k_chr_c2_uuid.u,
				.access_cb = matter_gatt_access,
				.val_handle = &s_c2_val_handle,
				.flags = BLE_GATT_CHR_F_INDICATE,
			},
			{0},
		},
	},
	{0},
};

/* ---- GAP events ------------------------------------------------------------ */

/*
 * Registered as a LISTENER, not as the connection callback. The credential reader
 * owns the advertising set and therefore the GAP callback; NimBLE's listener
 * API exists exactly so a second subsystem can observe events without taking
 * that ownership away. Without it this file would have to be called from inside
 * the reader, coupling two layers that have no reason to meet.
 */
static struct ble_gap_event_listener s_gap_listener;

static int matter_gap_event(struct ble_gap_event *event, void *arg)
{
	(void)arg;

	switch (event->type) {
	case BLE_GAP_EVENT_SUBSCRIBE:
		if (event->subscribe.attr_handle != s_c2_val_handle) {
			break;
		}
		s_subscribed = event->subscribe.cur_indicate != 0u;
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MATTER_BLE_TAG, "C2 indications %s",
				 s_subscribed ? "on" : "off");
		if (s_subscribed && s_hs_resp_len > 0u) {
			post_work(WORK_HANDSHAKE);
		}
		break;

	case BLE_GAP_EVENT_NOTIFY_TX:
		if (event->notify_tx.attr_handle != s_c2_val_handle ||
		    event->notify_tx.indication == 0u) {
			break;
		}
		/*
		 * NimBLE FIRES THIS TWICE FOR AN INDICATION: once with status 0
		 * when the command goes out, and again with BLE_HS_EDONE when the
		 * peer confirms. Only the second is Zephyr's indicate_done().
		 *
		 * Treating status 0 as the confirmation would release the
		 * one-outstanding-indication rule and push the next fragment
		 * before the peer had acknowledged this one -- the transport
		 * would appear to work at close range and fall apart under any
		 * retransmission.
		 */
		if (event->notify_tx.status == 0) {
			break;
		}
		s_indicate_busy = false;
		if (event->notify_tx.status != BLE_HS_EDONE) {
			ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_ERROR, MATTER_BLE_TAG,
					 "indication not confirmed (%d)",
					 event->notify_tx.status);
			s_tx_active = false;
			if (s_tx_notifies) {
				s_tx_notifies = false;
				if (s_tx_cb != NULL) {
					s_tx_cb(-EIO);
				}
			}
			break;
		}
		/* One indication may be outstanding per connection, so the next
		 * fragment only goes once the peer has confirmed this one. */
		(void)pump_tx();
		break;

	case BLE_GAP_EVENT_DISCONNECT:
		if (event->disconnect.conn.conn_handle != s_conn_handle) {
			break;
		}
		ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MATTER_BLE_TAG,
				 "commissioning link down (%d)", event->disconnect.reason);
		s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
		/* Owned by this event and the subscribe event only; see LESSON 1. */
		s_subscribed = false;
		reset_link();
		break;

	default:
		break;
	}
	return 0;
}

/* ---- advertising ----------------------------------------------------------- */

/*
 * ChipBLEDeviceIdentificationInfo, 8 bytes after the 16-bit UUID
 * (CHIPBleServiceData.h:52-79):
 *   [0]    opcode, 0x00 = commissionable
 *   [1]    low 8 bits of the 12-bit discriminator
 *   [2]    high 4 bits of the discriminator in the low nibble,
 *          advertisement version in the high nibble
 *   [3..4] vendor ID, little-endian
 *   [5..6] product ID, little-endian
 *   [7]    additional-data flag
 */
static uint16_t s_discriminator_override;

void matter_ble_set_discriminator(uint16_t discriminator)
{
	s_discriminator_override = discriminator;
}

uint16_t matter_ble_discriminator(void)
{
	return s_discriminator_override != 0u ? s_discriminator_override
					      : (uint16_t)MATTER_BLE_DISCRIMINATOR;
}

int matter_ble_commissionable_svc_data(uint8_t *out, size_t cap)
{
	uint8_t *p;

	if (out == NULL || cap < MATTER_BLE_SVC_DATA_LEN) {
		return -1;
	}
	p = &out[2];

	out[0] = 0xF6u; /* 0xFFF6, little-endian inside the service-data element */
	out[1] = 0xFFu;

	p[0] = 0x00u;
	/*
	 * The discriminator is normally the compile-time one, but
	 * OpenCommissioningWindow lets a controller choose its own -- and the
	 * ecosystem being invited in SCANS for that value. Advertising the
	 * factory one while a window is open makes the node undiscoverable to
	 * exactly the peer the window was opened for.
	 */
	p[1] = (uint8_t)(matter_ble_discriminator() & 0xFFu);
	/* Advertisement version 0 in the high nibble. */
	p[2] = (uint8_t)((matter_ble_discriminator() >> 8) & 0x0Fu);
	p[3] = (uint8_t)(CONFIG_ULTRAWIDELOCK_MATTER_VENDOR_ID & 0xFFu);
	p[4] = (uint8_t)(CONFIG_ULTRAWIDELOCK_MATTER_VENDOR_ID >> 8);
	p[5] = (uint8_t)(CONFIG_ULTRAWIDELOCK_MATTER_PRODUCT_ID & 0xFFu);
	p[6] = (uint8_t)(CONFIG_ULTRAWIDELOCK_MATTER_PRODUCT_ID >> 8);
	p[7] = 0x00u;

	return 0;
}

/* ---- init ------------------------------------------------------------------ */

/*
 * Registered through the host hooks rather than called by the application,
 * because the 0xFFF6 service is live the moment BLE comes up: the C1 write
 * handler will run whether or not anything asked for it. The oracle records what
 * happens when the two lifetimes are allowed to disagree -- with no caller,
 * --gc-sections dropped its init function and its work-queue stack with it, so a
 * C1 write would have posted to a queue that was never started. Tying the task
 * to the service registration removes the question.
 */
static int matter_ble_register_services(void)
{
	int rc;

	rc = ble_gatts_count_cfg(k_gatt_svcs);
	if (rc != 0) {
		return rc;
	}
	rc = ble_gatts_add_svcs(k_gatt_svcs);
	if (rc != 0) {
		return rc;
	}
	rc = ble_gap_event_listener_register(&s_gap_listener, matter_gap_event, NULL);
	if (rc != 0) {
		return rc;
	}

	s_task = xTaskCreateStatic(matter_ble_task, "matter_ble",
				   sizeof(s_task_stack) / sizeof(s_task_stack[0]), NULL,
				   MATTER_BLE_TASK_PRIO, s_task_stack, &s_task_tcb);
	if (s_task == NULL) {
		return -1;
	}
	reset_link();
	ultrawidelock_freertos_log(ULTRAWIDELOCK_FREERTOS_LOG_INFO, MATTER_BLE_TAG,
			 "0xFFF6 service ready (rx buffer %u B)", (unsigned int)sizeof(s_rx_buf));
	return 0;
}

int matter_ble_start(void)
{
	static const struct ultrawidelock_freertos_nimble_host_hooks k_hooks = {
		.register_services = matter_ble_register_services,
		.on_sync = NULL,
	};

	return ultrawidelock_freertos_nimble_host_add_hooks(&k_hooks);
}
