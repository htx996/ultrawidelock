/* SPDX-License-Identifier: ISC */

/** @file cherry_ccc_shim.c — cherry_ccc_* seam (credential responder) implemented over the
 * lock-native FiRa MAC; maps each call onto ultrawidelock_uwb_facade. */

#include <cherry/cherry.h>
#include <cherry/cherry_ccc.h>
#include <cherry/cherry_session.h>

#include "ultrawidelock_alloc.h"
#include <string.h>

#include "ultrawidelock_log.h"
#include "ultrawidelock_util.h"

#include <ultrawidelock/uwb.h>
#include "cred_round_config.h" /* ULTRAWIDELOCK_NUM_RESPONDERS — EXPERIMENT-2RESP */

LOG_MODULE_REGISTER(ultrawidelock_ccc_shim, LOG_LEVEL_INF);

/** URSK length (bytes) the credential provisioned-STS path expects. */
#define SHIM_URSK_LEN 32u

/**
 * Who wants the join parameters when a credential session starts.
 *
 * Unset by default, so a build that registers nothing behaves exactly as it did
 * before this seam existed. See ultrawidelock_uwb_set_handoff_listener().
 */
static void (*s_handoff_cb)(const struct ultrawidelock_uwb_handoff *h);

void cherry_ccc_set_handoff_listener(void (*cb)(const struct ultrawidelock_uwb_handoff *h))
{
	s_handoff_cb = cb;
}

/**
 * @brief Opaque Cherry context holder.
 * @param core_cb Core callback (never invoked: no UCI).
 * @param user_data Client data from cherry_create().
 */
struct cherry {
	cherry_core_cb_t core_cb; /**< Core callback (never invoked: no UCI). */
	void *user_data;          /**< Client data from cherry_create(). */
};

/**
 * @brief Base session object; first member of cherry_ccc_session for up-casting.
 * @param cb CCC notification callback from create.
 * @param user_data Client data (the ultrawidelock_uwb_session).
 */
struct cherry_session {
	cherry_ccc_cb_t cb; /**< CCC notification callback (from create). */
	void *user_data;    /**< Client data (the ultrawidelock_uwb_session). */
};

/**
 * @brief CCC ranging session bound to FiRa MAC.
 * @param base MUST be first member for up-casting.
 * @param config Borrowed pointer to adapter's negotiated params, valid until destroy.
 * @param ursk Provisioned-STS root key (16 bytes).
 * @param have_ursk True if URSK has been stashed via set_ursk.
 * @param state Last emitted session state (INIT, IDLE, ACTIVE, or DEINIT).
 */
struct cherry_ccc_session {
	struct cherry_session base; /**< MUST be first (see up-cast below). */
	/** Borrowed pointer to the adapter's negotiated params, valid until destroy. */
	struct cherry_ccc_ultrawidelock_session_config *config;
	uint8_t ursk[SHIM_URSK_LEN];         /**< Provisioned-STS root key. */
	bool have_ursk;                      /**< URSK stashed via set_ursk. */
	enum cherry_ccc_session_state state; /**< Last emitted state. */
};

/** @brief Up-cast a base session pointer (base is the first member). */
static inline struct cherry_ccc_session *to_ccc(struct cherry_session *base)
{
	return (struct cherry_ccc_session *)base;
}

/**
 * @brief Allocate and dispatch a SESSION_STATUS event to the CCC callback.
 * @param s CCC session.
 * @param st State to report (INIT, IDLE, ACTIVE, or DEINIT).
 */
static void emit_status(struct cherry_ccc_session *s, enum cherry_ccc_session_state st)
{
	struct cherry_ccc_event *ev = qmalloc(sizeof(*ev));
	// Heap-allocated session status event data; contains a state enum to be reported via the
	// CCC callback.
	struct cherry_ccc_session_event_session_status *status = qmalloc(sizeof(*status));

	if (!ev || !status) {
		LOG_ERR("emit_status: OOM");
		qfree(ev);
		qfree(status);
		return;
	}

	status->session_state = st;
	status->reason_code = CHERRY_CCC_STATE_CHANGE_REASON_MGMT_CMD;
	ev->type = CHERRY_CCC_EVENT_TYPE_SESSION_STATUS;
	ev->session = s;
	ev->data.status = status;

	s->state = st;
	s->base.cb(ev, s->base.user_data);
}

/** @brief Allocate + dispatch a SESSION_ERROR event to the CCC callback. */
static void emit_error(struct cherry_ccc_session *s, enum cherry_err err)
{
	struct cherry_ccc_event *ev = qmalloc(sizeof(*ev));
	// Heap-allocated error event data; contains an error code to be reported via the CCC
	// callback.
	struct cherry_ccc_session_event_error *e = qmalloc(sizeof(*e));

	if (!ev || !e) {
		LOG_ERR("emit_error: OOM");
		qfree(ev);
		qfree(e);
		return;
	}

	e->status_err = err;
	ev->type = CHERRY_CCC_EVENT_TYPE_SESSION_ERROR;
	ev->session = s;
	ev->data.error = e;

	s->base.cb(ev, s->base.user_data);
}

/* ---- Cherry context lifecycle (no UCI; trivial holder) ------------------- */

/**
 * @brief Allocate and initialize a Cherry context with the given core callback and user data.
 * @param device Device parameter (unused).
 * @param core_cb Core callback (never invoked).
 * @param user_data User data to store in the context.
 * @return Cherry context pointer, or null if allocation fails.
 */
struct cherry *cherry_create(const char *device, cherry_core_cb_t core_cb, void *user_data)
{
	struct cherry *ctx = qmalloc(sizeof(*ctx));

	ARG_UNUSED(device);
	if (!ctx) {
		return NULL;
	}
	ctx->core_cb = core_cb;
	ctx->user_data = user_data;
	return ctx;
}

/**
 * @brief Deallocate a Cherry context; null input is safely ignored.
 */
void cherry_destroy_sync(struct cherry *ctx)
{
	qfree(ctx);
}

/* ---- CCC session create + lifetime --------------------------------------- */

/**
 * @brief Allocate and initialize a credential responder CCC session.
 * @param ctx Cherry context (unused).
 * @param callback CCC notification callback.
 * @param user_data User context to pass to callback.
 * @param config Session configuration (channel, session ID, STS index, timing, slot geometry).
 * @return Session pointer, or null if callback is null, config is null, or allocation fails.
 */
struct cherry_ccc_session *
cherry_ccc_session_create_ultrawidelock_responder(struct cherry *ctx, cherry_ccc_cb_t callback,
					  void *user_data,
					  struct cherry_ccc_ultrawidelock_session_config *config)
{
	struct cherry_ccc_session *s;

	ARG_UNUSED(ctx);
	if (!callback || !config) {
		return NULL;
	}

	s = qcalloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->base.cb = callback;
	s->base.user_data = user_data;
	s->config = config;
	s->state = CHERRY_CCC_SESSION_STATE_INIT;
	return s;
}

/**
 * @brief Cast a CCC session pointer to its embedded base session structure.
 * @param session CCC session.
 * @return Base session pointer, or null if session is null.
 */
struct cherry_session *cherry_ccc_session_to_base(struct cherry_ccc_session *session)
{
	return session ? &session->base : NULL;
}

/**
 * @brief Retrieve the user data pointer stored in the base session.
 * @param session Base session.
 * @return User data pointer, or null if session is null.
 */
void *cherry_session_get_user_data(struct cherry_session *session)
{
	return session ? session->user_data : NULL;
}

/**
 * @brief Stop the UWB radio, emit a DEINIT status event, and deallocate the session.
 * @param session Base session; null or invalid input is safely ignored.
 */
void cherry_session_destroy(struct cherry_session *session)
{
	struct cherry_ccc_session *s = to_ccc(session);

	if (!s) {
		return;
	}
	/* Tear the radio down, then tell the adapter the session is gone (it frees its own
	 * ultrawidelock_uwb_session on DEINIT). */
	ultrawidelock_uwb_stop();
	emit_status(s, CHERRY_CCC_SESSION_STATE_DEINIT);
	qfree(s);
}

/* ---- Start / stop -------------------------------------------------------- */

/**
 * @brief Start a credential UWB session by building a RangingConfiguration byte array from session
 * config, calling ultrawidelock_uwb_start_cred, and emitting IDLE then ACTIVE status
 * events.
 * @param session Base session.
 * @return CHERRY_ERR_INVALID_PARAMETER if session or config is null; CHERRY_ERR_SESSION_CONFIG if
 * URSK is not set; CHERRY_ERR_SESSION_INIT if UWB start fails; otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_session_start(struct cherry_session *session)
{
	struct cherry_ccc_session *s = to_ccc(session);
	const struct cherry_ccc_ultrawidelock_session_config *c;
	// Configuration struct for ultrawidelock_uwb_start_cred; holds session ID,
	// channel, sync code, timing, slot geometry, STS index, UWB time, URSK key, and the
	// RangingConfiguration byte array required to derive the CCC SaltedHash.
	struct ultrawidelock_uwb_cred_cfg fcfg;
	uint8_t rcfg[17];
	int rc;

	if (!s || !s->config) {
		return CHERRY_ERR_INVALID_PARAMETER;
	}
	if (!s->have_ursk) {
		LOG_ERR("start without URSK");
		return CHERRY_ERR_SESSION_CONFIG;
	}
	c = s->config;

	fcfg.session_id = c->session_id;
	fcfg.channel = c->channel;
	fcfg.sync_code_index = c->sync_code_index;
	fcfg.slot_duration_rstu = c->slot_duration;
	fcfg.block_duration_ms = c->ranging_duration_ms;
	fcfg.slot_per_round = c->slot_per_rr;
	fcfg.sts_index0 = c->sts_index;
	fcfg.uwb_time_us = c->uwb_time_us;
	fcfg.ursk = s->ursk;

	/* Build the RangingConfiguration (the CCC SaltedHash input the Wallet hashes): BE
	 * Protocol_Version + UWB_Config_Id + UWB_Session_Id + STS_Index0, then five 1-byte fields.
	 */
	rcfg[0] = 0x01u;
	rcfg[1] = 0x00u; /* Selected_Protocol_Version */
	rcfg[2] = (uint8_t)(c->uwb_config_id >> 8);
	rcfg[3] = (uint8_t)(c->uwb_config_id); /* Selected_UWB_Config_Id (BE) */
	rcfg[4] = (uint8_t)(c->session_id >> 24);
	rcfg[5] = (uint8_t)(c->session_id >> 16);
	rcfg[6] = (uint8_t)(c->session_id >> 8);
	rcfg[7] = (uint8_t)(c->session_id); /* UWB_Session_Id (BE) */
	rcfg[8] = (uint8_t)(c->sts_index >> 24);
	rcfg[9] = (uint8_t)(c->sts_index >> 16);
	rcfg[10] = (uint8_t)(c->sts_index >> 8);
	rcfg[11] = (uint8_t)(c->sts_index); /* STS_Index0 (BE) */
	rcfg[12] = (uint8_t)
		ULTRAWIDELOCK_NUM_RESPONDERS; /* Number_Responder_Nodes — EXPERIMENT-2RESP; shared with M3
					 via ultrawidelock_round_config.h so the SaltedHash can't desync. */
	rcfg[13] = (uint8_t)(c->ranging_duration_ms / 96u); /* Session_RAN_Multiplier */
	rcfg[14] = c->slot_per_rr;                          /* Number_Slot_per_Round */
	rcfg[15] = (uint8_t)(c->slot_duration / 400u);      /* Number_Chaps_per_Slot */
	rcfg[16] = c->pulse_shape_combo;                    /* Selected_PulseShape_Combo */
	fcfg.ranging_config = rcfg;
	fcfg.rc_len = sizeof(rcfg);

	LOG_INF("credential start: sid=0x%08x ch=%u code=%u slot=%u blk=%ums spr=%u sts0=0x%08x",
		c->session_id, c->channel, c->sync_code_index, c->slot_duration,
		c->ranging_duration_ms, c->slot_per_rr, c->sts_index);
	LOG_INF("credential RangingConfig: proto=%02x%02x cfg=%02x%02x n=%u ran=%u spr=%u chap=%u "
		"ps=%02x",
		rcfg[0], rcfg[1], rcfg[2], rcfg[3], rcfg[12], rcfg[13], rcfg[14], rcfg[15],
		rcfg[16]);

	/*
	 * The same payload the bench log below prints, handed to whoever
	 * registered for it -- normally the sealed link to the second anchor.
	 * Before the radio start for the log's reason: the handoff can then land
	 * before UWB_Time0, and pre-poll recovery makes a late one cost a block
	 * rather than the session.
	 */
	if (s_handoff_cb != NULL) {
		struct ultrawidelock_uwb_handoff h = {
			.ursk = s->ursk,
			.ursk_len = SHIM_URSK_LEN,
			.rcfg = rcfg,
			.rcfg_len = sizeof(rcfg),
			.channel = c->channel,
			.sync_code_index = c->sync_code_index,
		};

		s_handoff_cb(&h);
	}

#if defined(CONFIG_ULTRAWIDELOCK_SATELLITE_HANDOFF_LOG)
	/* Bench-only (see the Kconfig help): the exact `sat join` line the satellite
	 * responder's shell accepts. Everything it needs beyond the URSK is inside
	 * rcfg[] plus the PHY pair. Redundant once the sealed handoff above is
	 * carrying it, and the reason to turn this off: it prints the URSK. */
	{
		static const char hx[] = "0123456789abcdef";
		char ursk_hex[2 * SHIM_URSK_LEN + 1];
		char rcfg_hex[2 * sizeof(rcfg) + 1];

		for (size_t i = 0; i < SHIM_URSK_LEN; i++) {
			ursk_hex[2 * i] = hx[s->ursk[i] >> 4];
			ursk_hex[2 * i + 1] = hx[s->ursk[i] & 0xfu];
		}
		ursk_hex[2 * SHIM_URSK_LEN] = '\0';
		for (size_t i = 0; i < sizeof(rcfg); i++) {
			rcfg_hex[2 * i] = hx[rcfg[i] >> 4];
			rcfg_hex[2 * i + 1] = hx[rcfg[i] & 0xfu];
		}
		rcfg_hex[2 * sizeof(rcfg)] = '\0';
		LOG_INF("SAT-HANDOFF: sat join %s %s %u %u", ursk_hex, rcfg_hex,
			c->channel, c->sync_code_index);
	}
#endif

	rc = ultrawidelock_uwb_start_cred(&fcfg);
	if (rc != 0) {
		LOG_ERR("ultrawidelock_uwb_start_cred rc=%d", rc);
		emit_error(s, CHERRY_ERR_SESSION_INIT);
		return CHERRY_ERR_SESSION_INIT;
	}

	/* Drive the add-on state machine INIT -> IDLE -> ACTIVE (== Ranging). */
	emit_status(s, CHERRY_CCC_SESSION_STATE_IDLE);
	emit_status(s, CHERRY_CCC_SESSION_STATE_ACTIVE);
	return CHERRY_ERR_NONE;
}

/**
 * @brief Stop the UWB radio and emit an IDLE status event.
 * @param session Base session.
 * @return CHERRY_ERR_INVALID_PARAMETER if session is null or invalid, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_session_stop(struct cherry_session *session)
{
	struct cherry_ccc_session *s = to_ccc(session);

	if (!s) {
		return CHERRY_ERR_INVALID_PARAMETER;
	}
	ultrawidelock_uwb_stop();
	emit_status(s, CHERRY_CCC_SESSION_STATE_IDLE);
	return CHERRY_ERR_NONE;
}

/* ---- Fine session configuration (write-through to the borrowed config) --- */

/**
 * @brief Copy the URSK into the session and mark it as present.
 * @param session CCC session.
 * @param ursk 16-byte Unique Responder Session Key.
 * @return CHERRY_ERR_INVALID_PARAMETER if session or ursk is null, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_ccc_session_set_ursk(struct cherry_ccc_session *session, const uint8_t *ursk)
{
	if (!session || !ursk) {
		return CHERRY_ERR_INVALID_PARAMETER;
	}
	memcpy(session->ursk, ursk, SHIM_URSK_LEN);
	session->have_ursk = true;
	return CHERRY_ERR_NONE;
}

/**
 * @brief Validate that the session exists; selected protocol version is accepted but ignored.
 * @param session CCC session.
 * @param selected_protocol_version Protocol version (ignored).
 * @return CHERRY_ERR_INVALID_PARAMETER if session is null, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_ccc_session_set_protocol_version(struct cherry_ccc_session *session,
							uint16_t selected_protocol_version)
{
	ARG_UNUSED(selected_protocol_version);
	return session ? CHERRY_ERR_NONE : CHERRY_ERR_INVALID_PARAMETER;
}

/**
 * @brief Store the STS index on the session config.
 * @param session CCC session.
 * @param sts_index STS index value.
 * @return CHERRY_ERR_INVALID_PARAMETER if session or its config is null, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_ccc_session_set_sts_index(struct cherry_ccc_session *session,
						 uint32_t sts_index)
{
	if (!session || !session->config) {
		return CHERRY_ERR_INVALID_PARAMETER;
	}
	session->config->sts_index = sts_index;
	return CHERRY_ERR_NONE;
}

/**
 * @brief Store the UWB initiation timestamp in microseconds on the session config.
 * @param session CCC session.
 * @param initiation_time_us Initiation timestamp in microseconds.
 * @return CHERRY_ERR_INVALID_PARAMETER if session or its config is null, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_ccc_session_set_initiation_time(struct cherry_ccc_session *session,
						       uint64_t initiation_time_us)
{
	if (!session || !session->config) {
		return CHERRY_ERR_INVALID_PARAMETER;
	}
	session->config->uwb_time_us = initiation_time_us;
	return CHERRY_ERR_NONE;
}

/**
 * @brief Validate that the session exists; TX and RX antenna set parameters are accepted but
 * ignored.
 * @param session CCC session.
 * @param tx_antenna_set TX antenna set (ignored).
 * @param rx_antenna_set RX antenna set (ignored).
 * @return CHERRY_ERR_INVALID_PARAMETER if session is null, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_ccc_session_set_round2_antennas(struct cherry_ccc_session *session,
						       uint8_t tx_antenna_set,
						       uint8_t rx_antenna_set)
{
	ARG_UNUSED(tx_antenna_set);
	ARG_UNUSED(rx_antenna_set);
	return session ? CHERRY_ERR_NONE : CHERRY_ERR_INVALID_PARAMETER;
}

/**
 * @brief Validate that the session exists; TX and RX antenna set parameters are accepted but
 * ignored.
 * @param session Base session.
 * @param tx_antenna_set TX antenna set (ignored).
 * @param rx_antenna_set RX antenna set (ignored).
 * @return CHERRY_ERR_INVALID_PARAMETER if session is null, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_session_set_antennas(struct cherry_session *session, uint8_t tx_antenna_set,
					    uint8_t rx_antenna_set)
{
	ARG_UNUSED(tx_antenna_set);
	ARG_UNUSED(rx_antenna_set);
	return session ? CHERRY_ERR_NONE : CHERRY_ERR_INVALID_PARAMETER;
}

/**
 * @brief Validate that the session exists; the diagnostics settings are accepted but ignored.
 * @return CHERRY_ERR_INVALID_PARAMETER if session is null, otherwise CHERRY_ERR_NONE.
 */
enum cherry_err cherry_session_set_diagnostics(struct cherry_session *session,
					       struct cherry_common_diag_cfg config,
					       bool controlee_only)
{
	ARG_UNUSED(config);
	ARG_UNUSED(controlee_only);
	return session ? CHERRY_ERR_NONE : CHERRY_ERR_INVALID_PARAMETER;
}

/* ---- Event teardown ------------------------------------------------------ */

/**
 * @brief Free a CCC event and its payload; null input is safely ignored.
 */
void cherry_ccc_event_free(struct cherry_ccc_event *event)
{
	if (!event) {
		return;
	}
	switch (event->type) {
	case CHERRY_CCC_EVENT_TYPE_SESSION_STATUS:
		qfree(event->data.status);
		break;
	case CHERRY_CCC_EVENT_TYPE_SESSION_ERROR:
		qfree(event->data.error);
		break;
	default:
		/* We never emit the report/diagnostic variants. */
		break;
	}
	qfree(event);
}
