/* SPDX-License-Identifier: ISC */

/** @file ultrawidelock_uwb_msg.c — setup/notification message codec. */

#include "ultrawidelock_uwb_msg.h"

#include "ultrawidelock_uwb_msg_builder.h"
#include "ultrawidelock_uwb_msg_parser.h"
#include "ultrawidelock_uwb_msg_spec.h"

#include <ultrawidelock_uwb_adapter/ultrawidelock_uwb_adapter.h>

#include "cred_round_config.h" /* ULTRAWIDELOCK_NUM_RESPONDERS — EXPERIMENT-2RESP */
#include "ultrawidelock_alloc.h"
#include <string.h>

#include "ultrawidelock_log.h"
#include "ultrawidelock_util.h"

LOG_MODULE_DECLARE(ultrawidelock_cred_uwb, LOG_LEVEL_INF);

/* Number of slots per round the reader always offers in M3. */
#define ULTRAWIDELOCK_SLOTS_PER_ROUND_DEFAULT 12

/* ULTRAWIDELOCK_CHANNEL_BITMASK_CH5/CH9 come from ultrawidelock_uwb_msg_spec.h, so the device
 * side selecting a channel and the reader validating that selection cannot
 * disagree about which bit means what. */

/* Hopping-capability bit layout used while selecting a hopping config. */
#define HOP_CCC_TO_FIRA(v)           ((v) >> 3)
#define HOP_FIRA_TO_CCC(v)           ((v) << 3)
#define HOP_CAP_AES                  (1 << 0)
#define HOP_CAP_DEFAULT              (1 << 1)
#define HOP_CAP_ADAPTIVE             (1 << 2)
#define HOP_CAP_CONTINUOUS           (1 << 3)
#define HOP_CAP_NO_HOPPING           (1 << 4)
#define HOP_COMBO_CONTINUOUS_DEFAULT (HOP_CAP_CONTINUOUS | HOP_CAP_DEFAULT)
#define HOP_COMBO_ADAPTIVE_DEFAULT   (HOP_CAP_ADAPTIVE | HOP_CAP_DEFAULT)
#define HOP_COMBO_NO_HOPPING         (HOP_CAP_NO_HOPPING)

/* Sets of attribute ids each message must carry, as a bit-per-id mask. */
#define M2_ATTR_MASK                                                                               \
	((1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CONFIGURATION_IDENTIFIER) |                  \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_PULSE_SHAPE_COMBO) |                         \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CHANNEL_BITMASK) |                           \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SYNC_CODE_INDEX_BITMASK) |                   \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_RAN_MULTIPLIER) |                            \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SLOT_BITMASK) |                              \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_HOPPING_CONFIGURATION_BITMASK))
#define M2_ATTR_MASK_VENDOR                                                                        \
	(M2_ATTR_MASK | (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_VENDOR_SPECIFIC))
#define M4_ATTR_MASK                                                                               \
	((1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_STS_INDEX0) |                                \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_UWB_TIME0) |                                 \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_HOP_MODE_KEY) |                              \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SYNC_CODE_INDEX))
#define M4_ATTR_MASK_VENDOR                                                                        \
	(M4_ATTR_MASK | (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_VENDOR_SPECIFIC))
#define SUSPEND_REQUEST_ATTR_MASK  (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SESSION_IDENTIFIER)
#define SUSPEND_RESPONSE_ATTR_MASK (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_STATUS)
#define RESUME_RESPONSE_ATTR_MASK                                                                  \
	((1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_STS_INDEX0) |                                \
	 (1 << ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_UWB_TIME0))

/* ultrawidelock_uwb_msg_free lives in ultrawidelock_uwb_msg_builder.c, next to the qmalloc that
 * pairs with it. It was here, which forced anything that only BUILDS messages to
 * link this whole file and with it the reader's M2/M4 handlers and session
 * lifecycle. The device-side codec (ultrawidelock_device_uwb.c) is exactly that caller. */

/* ---- Builders ------------------------------------------------------------ */

/**
 * @brief Builds the M1 ranging-session-setup message advertising configuration IDs, pulse-shape
 * combos, channel bitmask, and session ID.
 * @param session Session whose credential capabilities and session ID populate the message.
 * @return Newly allocated M1 message, or NULL if builder init or attribute encoding fails.
 */
struct ultrawidelock_uwb_message *
ultrawidelock_uwb_msg_build_m1(struct ultrawidelock_uwb_session *session)
{
	struct ultrawidelock_uwb_msg_builder builder;
	struct cherry_ccc_capabilities *caps = &session->ultrawidelock_ctx->ccc_caps;
	uint16_t payload_length;
	bool ok;

	/* M1 carries the config-id, pulse-shape, channel and session-id attributes. */
	payload_length = caps->uwb_configs.len *
				 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CONFIGURATION_IDENTIFIER_LENGTH +
			 caps->pulse_shape_combos.len *
				 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_PULSE_SHAPE_COMBO_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CHANNEL_BITMASK_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SESSION_IDENTIFIER_LENGTH +
			 (size_t)4 * ULTRAWIDELOCK_ATTRIBUTE_HEADER_LENGTH;

	if (!ultrawidelock_uwb_msg_builder_init(&builder, payload_length)) {
		return NULL;
	}

	ultrawidelock_uwb_msg_builder_header(&builder, ULTRAWIDELOCK_UWB_PROTOCOL_TYPE_UWB_RANGING_SERVICE,
				     ULTRAWIDELOCK_UWB_MESSAGE_SETUP_M1, payload_length);

	ok = ultrawidelock_uwb_msg_builder_add_u16_array(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CONFIGURATION_IDENTIFIER,
		     caps->uwb_configs.len, caps->uwb_configs.items) &&
	     ultrawidelock_uwb_msg_builder_add_bytes(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_PULSE_SHAPE_COMBO,
		     caps->pulse_shape_combos.len, caps->pulse_shape_combos.items) &&
	     ultrawidelock_uwb_msg_builder_add_u8(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CHANNEL_BITMASK,
		     caps->channel_bitmask) &&
	     ultrawidelock_uwb_msg_builder_add_u32(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SESSION_IDENTIFIER,
		     session->session_id);

	if (!ok) {
		ultrawidelock_uwb_msg_free(builder.message);
		return NULL;
	}
	return builder.message;
}

/**
 * @brief Builds the M3 ranging-parameters message, deriving RAN multiplier and
 * chaps-per-slot from the M2-negotiated durations and committing the reader's MAC mode.
 * @param session Session whose negotiated M2 config and reader settings populate the M3
 * attributes.
 * @return Newly allocated M3 message, or NULL if builder init or attribute encoding fails.
 */
static struct ultrawidelock_uwb_message *
ultrawidelock_uwb_msg_build_m3(struct ultrawidelock_uwb_session *session)
{
	struct ultrawidelock_uwb_msg_builder builder;
	struct ultrawidelock_uwb_adapter_reader_config *reader = session->ultrawidelock_ctx->config;
	struct cherry_ccc_capabilities *caps = &session->ultrawidelock_ctx->ccc_caps;
	struct cherry_ccc_ultrawidelock_session_config *cfg = &session->ccc_ultrawidelock_config;
	uint16_t payload_length;
	uint8_t ran_multiplier;
	uint8_t chaps_per_slot;
	bool ok;

	/* M3 carries seven ranging-parameter attributes. */
	payload_length = ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_RAN_MULTIPLIER_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_NUMBER_CHAPS_PER_SLOT_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_NUMBER_RESPONDERS_NODES_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_NUMBER_SLOTS_PER_ROUND_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SYNC_CODE_INDEX_BITMASK_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_HOPPING_CONFIGURATION_BITMASK_LENGTH +
			 ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_MAC_MODE_LENGTH +
			 7 * ULTRAWIDELOCK_ATTRIBUTE_HEADER_LENGTH;

	if (!ultrawidelock_uwb_msg_builder_init(&builder, payload_length)) {
		return NULL;
	}

	/* Derive the count fields from the M2 durations and commit reader choices. */
	ran_multiplier = (uint8_t)(cfg->ranging_duration_ms / 96);
	chaps_per_slot = (uint8_t)(cfg->slot_duration / 400);
	cfg->slot_per_rr = ULTRAWIDELOCK_SLOTS_PER_ROUND_DEFAULT;
	cfg->mac_mode = reader->mac_mode;

	ultrawidelock_uwb_msg_builder_header(&builder, ULTRAWIDELOCK_UWB_PROTOCOL_TYPE_UWB_RANGING_SERVICE,
				     ULTRAWIDELOCK_UWB_MESSAGE_SETUP_M3, payload_length);

	ok = ultrawidelock_uwb_msg_builder_add_u8(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_RAN_MULTIPLIER,
		     ran_multiplier) &&
	     ultrawidelock_uwb_msg_builder_add_u8(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_NUMBER_CHAPS_PER_SLOT,
		     chaps_per_slot) &&
	     ultrawidelock_uwb_msg_builder_add_u8(
		     &builder,
		     /* EXPERIMENT-2RESP: responder count from the shared knob
		      * (ULTRAWIDELOCK_NUM_RESPONDERS, default 1). MUST equal rcfg[12] in
		      * cherry_ccc_shim.c — both feed the RangingConfiguration SaltedHash
		      * the Wallet independently recomputes; a mismatch diverges every
		      * derived STS/key and nothing decodes. See ultrawidelock_round_config.h. */
		     ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_NUMBER_RESPONDERS_NODES,
		     ULTRAWIDELOCK_NUM_RESPONDERS) &&
	     ultrawidelock_uwb_msg_builder_add_u8(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_NUMBER_SLOTS_PER_ROUND,
		     cfg->slot_per_rr) &&
	     ultrawidelock_uwb_msg_builder_add_u32(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SYNC_CODE_INDEX_BITMASK,
		     caps->sync_code_index_bitmask) &&
	     ultrawidelock_uwb_msg_builder_add_u8(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_HOPPING_CONFIGURATION_BITMASK,
		     cfg->hopping_config_bitmask) &&
	     ultrawidelock_uwb_msg_builder_add_u8(
		     &builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_MAC_MODE, reader->mac_mode);

	if (!ok) {
		ultrawidelock_uwb_msg_free(builder.message);
		return NULL;
	}
	return builder.message;
}

/**
 * @brief Builds a suspend or resume request message carrying the session identifier.
 * @param session Session whose session ID is encoded into the message.
 * @param suspend True to build a suspend request, false to build a resume request.
 * @return Newly allocated request message, or NULL if builder init or attribute encoding
 * fails.
 */
struct ultrawidelock_uwb_message *
ultrawidelock_uwb_msg_build_suspend_resume_request(struct ultrawidelock_uwb_session *session,
						   bool suspend)
{
	struct ultrawidelock_uwb_msg_builder builder;
	uint16_t payload_length;

	payload_length = ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SESSION_IDENTIFIER_LENGTH +
			 ULTRAWIDELOCK_ATTRIBUTE_HEADER_LENGTH;

	if (!ultrawidelock_uwb_msg_builder_init(&builder, payload_length)) {
		return NULL;
	}

	ultrawidelock_uwb_msg_builder_header(&builder, ULTRAWIDELOCK_UWB_PROTOCOL_TYPE_UWB_RANGING_SERVICE,
				     suspend ? ULTRAWIDELOCK_UWB_MESSAGE_SUSPEND_REQUEST
					     : ULTRAWIDELOCK_UWB_MESSAGE_RESUME_REQUEST,
				     payload_length);

	if (!ultrawidelock_uwb_msg_builder_add_u32(&builder,
					   ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SESSION_IDENTIFIER,
					   session->session_id)) {
		ultrawidelock_uwb_msg_free(builder.message);
		return NULL;
	}
	return builder.message;
}

/**
 * @brief Builds a suspend-response message carrying an accept or reject status.
 * @param session Unused; reserved for a consistent builder signature.
 * @param status Response status; must be ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_ACCEPT or
 * ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_REJECT.
 * @return Newly allocated suspend-response message, or NULL if status is invalid, or if
 * builder init or attribute encoding fails.
 */
static struct ultrawidelock_uwb_message *
ultrawidelock_uwb_msg_build_suspend_response(struct ultrawidelock_uwb_session *session,
					     uint8_t status)
{
	struct ultrawidelock_uwb_msg_builder builder;
	uint16_t payload_length;

	ARG_UNUSED(session);

	if (status != ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_ACCEPT &&
	    status != ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_REJECT) {
		LOG_ERR("suspend response: bad status %u", status);
		return NULL;
	}

	payload_length =
		ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_STATUS_LENGTH + ULTRAWIDELOCK_ATTRIBUTE_HEADER_LENGTH;

	if (!ultrawidelock_uwb_msg_builder_init(&builder, payload_length)) {
		return NULL;
	}

	ultrawidelock_uwb_msg_builder_header(&builder, ULTRAWIDELOCK_UWB_PROTOCOL_TYPE_UWB_RANGING_SERVICE,
				     ULTRAWIDELOCK_UWB_MESSAGE_SUSPEND_RESPONSE, payload_length);

	if (!ultrawidelock_uwb_msg_builder_add_u8(&builder, ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_STATUS,
					  status)) {
		ultrawidelock_uwb_msg_free(builder.message);
		return NULL;
	}
	return builder.message;
}

/**
 * @brief Builds a credential general-error notification message carrying the given error code.
 * @param session Unused; reserved for a consistent builder signature.
 * @param error_code Error code to encode in the general-error notification attribute.
 * @return Newly allocated notification message, or NULL if builder init or attribute encoding
 * fails.
 */
struct ultrawidelock_uwb_message *
ultrawidelock_uwb_msg_build_general_error(struct ultrawidelock_uwb_session *session,
					  uint8_t error_code)
{
	struct ultrawidelock_uwb_msg_builder builder;
	uint16_t payload_length;

	ARG_UNUSED(session);

	payload_length = ULTRAWIDELOCK_UWB_NOTIFICATION_ATTR_EVENT_GENERAL_ERROR_LENGTH +
			 ULTRAWIDELOCK_ATTRIBUTE_HEADER_LENGTH;

	if (!ultrawidelock_uwb_msg_builder_init(&builder, payload_length)) {
		return NULL;
	}

	ultrawidelock_uwb_msg_builder_header(&builder, ULTRAWIDELOCK_UWB_PROTOCOL_TYPE_NOTIFICATION,
				     ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_EVENT, payload_length);

	if (!ultrawidelock_uwb_msg_builder_add_u8(&builder,
					  ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_EVENT_ATTR_GENERAL_ERROR,
					  error_code)) {
		ultrawidelock_uwb_msg_free(builder.message);
		return NULL;
	}
	return builder.message;
}

/* The three header accessors live in ultrawidelock_uwb_msg_parser.c, with the rest of the
 * code that reads raw message bytes. Same reason as ultrawidelock_uwb_msg_free above:
 * they are framing, not session logic, and leaving them here made every reader
 * of a message header link this file's M2/M4 handlers to get three byte reads. */

/* ---- Attribute parsers (fold values into ccc_ultrawidelock_config) --------------- */

/**
 * @brief Parses the UWB configuration ID attribute from M2 and stores it in the session config.
 * @param session Session whose config receives the parsed configuration ID.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_config_id(struct ultrawidelock_uwb_session *session,
					  struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint16_t value;

	if (!ultrawidelock_uwb_msg_read_u16(attr, "config id", &value)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	session->ccc_ultrawidelock_config.uwb_config_id = value;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the pulse-shape-combo attribute from M2 and stores it in the session config.
 * @param session Session whose config receives the parsed pulse shape.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_pulse_shape(struct ultrawidelock_uwb_session *session,
					    struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint8_t value;

	if (!ultrawidelock_uwb_msg_read_u8(attr, "pulse shape", &value)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	session->ccc_ultrawidelock_config.pulse_shape_combo = value;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the session-identifier attribute from M2 and verifies it matches the session's
 * active session ID.
 * @param session Session whose session ID is checked against the parsed value.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the value
 * cannot be read, or ULTRAWIDELOCK_UWB_ERR_INVALID_PARAMETER on a session ID mismatch.
 */
static enum ultrawidelock_uwb_err parse_session_id(struct ultrawidelock_uwb_session *session,
					   struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint32_t value;

	if (!ultrawidelock_uwb_msg_read_u32(attr, "session id", &value)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	if (value != session->session_id) {
		LOG_ERR("session id mismatch: got 0x%08x, want 0x%08x", value, session->session_id);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_PARAMETER;
	}
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the channel bitmask attribute from M2, mapping it to channel 5 or 9 in the session
 * config.
 * @param session Session whose config receives the resolved channel number.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the value
 * cannot be read, or ULTRAWIDELOCK_UWB_ERR_INVALID_PARAMETER for an unsupported bitmask.
 */
static enum ultrawidelock_uwb_err parse_channel(struct ultrawidelock_uwb_session *session,
					struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint8_t bitmask;

	if (!ultrawidelock_uwb_msg_read_u8(attr, "channel", &bitmask)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	if (bitmask == ULTRAWIDELOCK_CHANNEL_BITMASK_CH5) {
		session->ccc_ultrawidelock_config.channel = 5;
	} else if (bitmask == ULTRAWIDELOCK_CHANNEL_BITMASK_CH9) {
		session->ccc_ultrawidelock_config.channel = 9;
	} else {
		LOG_ERR("unsupported channel bitmask 0x%02x", bitmask);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_PARAMETER;
	}
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the RAN multiplier attribute from M3, selecting the larger of the peer's value and
 * the reader's minimum, and computes the ranging duration in milliseconds.
 * @param session Session whose config receives the computed ranging duration.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_ran_multiplier(struct ultrawidelock_uwb_session *session,
					       struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint8_t received;
	uint8_t selected;

	if (!ultrawidelock_uwb_msg_read_u8(attr, "ran multiplier", &received)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	/* Take the larger of the peer's value and the reader's minimum. */
	selected = received > session->ultrawidelock_ctx->min_ran_multiplier
			   ? received
			   : session->ultrawidelock_ctx->min_ran_multiplier;
	session->ccc_ultrawidelock_config.ranging_duration_ms = 96 * selected;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the slot bitmask attribute from M3, intersects it with the local capability
 * bitmask, and maps the lowest common bit to a chaps-per-slot count to compute slot duration.
 * @param session Session whose config receives the computed slot duration.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_slot_bitmask(struct ultrawidelock_uwb_session *session,
					     struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint8_t bitmask;
	uint8_t common;
	uint8_t chaps_per_slot;
	uint8_t idx;

	if (!ultrawidelock_uwb_msg_read_u8(attr, "slot bitmask", &bitmask)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	/* Smallest NChap-per-slot supported by both sides = lowest common bit. */
	common = bitmask & session->ultrawidelock_ctx->ccc_caps.slot_bitmask;
	chaps_per_slot = common;
	for (idx = 0; idx < 7; idx++) {
		if ((common >> idx) & 0x1) {
			break;
		}
	}
	switch (idx) {
	case 0:
		chaps_per_slot = 3;
		break;
	case 1:
		chaps_per_slot = 4;
		break;
	case 2:
		chaps_per_slot = 6;
		break;
	case 3:
		chaps_per_slot = 8;
		break;
	case 4:
		chaps_per_slot = 9;
		break;
	case 5:
		chaps_per_slot = 12;
		break;
	case 6:
		chaps_per_slot = 24;
		break;
	default:
		/* No bit 0-6 set in common: keep the seeded raw bitmask value. */
		break;
	}
	/* BENCH PROBE: the offered slot menu, once per M2. We take the LOWEST common
	 * bit, and the arm deadline is one slot minus the ~160 us POLL lead, so any
	 * offered bit above the chosen one is arm headroom we decline. idx==7 = no
	 * common bit (chaps_per_slot is then the raw masked value). */
	LOG_INF("slot bm peer=0x%02x ours=0x%02x common=0x%02x -> bit%u chaps=%u dur=%u rstu",
		(unsigned)bitmask, (unsigned)session->ultrawidelock_ctx->ccc_caps.slot_bitmask,
		(unsigned)common, (unsigned)idx, (unsigned)chaps_per_slot,
		(unsigned)(400u * chaps_per_slot));
	session->ccc_ultrawidelock_config.slot_duration = 400 * chaps_per_slot;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the sync code bitmask attribute from M2 and logs the peer's offered bitmask; the
 * reader retains its own capability bitmask for M3 and does not update the session config.
 * @param session Unused; reserved for a consistent parser signature.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_sync_code_bitmask(struct ultrawidelock_uwb_session *session,
						  struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint32_t bitmask;

	ARG_UNUSED(session);
	/* Offered set; the reader keeps its own capability bitmask for M3. */
	if (!ultrawidelock_uwb_msg_read_u32(attr, "sync code bitmask", &bitmask)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	LOG_DBG("peer sync code bitmask 0x%08x", bitmask);
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the sync code index attribute from M3 and stores it in the session config.
 * @param session Session whose config receives the parsed sync code index.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALDORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_sync_code_index(struct ultrawidelock_uwb_session *session,
						struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint8_t value;

	if (!ultrawidelock_uwb_msg_read_u8(attr, "sync code index", &value)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	session->ccc_ultrawidelock_config.sync_code_index = value;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the hopping configuration bitmask attribute from M2, intersects peer capabilities
 * with local CCC capabilities, and selects the first mutually supported preferred hopping config.
 * @param session Session whose reader preferences are matched and whose config receives the
 * selected hopping mode.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the value
 * cannot be read or no common hopping config is found.
 */
static enum ultrawidelock_uwb_err parse_hopping_bitmask(struct ultrawidelock_uwb_session *session,
						struct ultrawidelock_uwb_msg_attribute *attr)
{
	const struct ultrawidelock_uwb_preferred_hopping_configs *prefs =
		&session->ultrawidelock_ctx->config->preferred_hopping_configs;
	uint8_t peer_caps;
	uint8_t common;
	size_t i;

	if (!ultrawidelock_uwb_msg_read_u8(attr, "hopping bitmask", &peer_caps)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	common = HOP_CCC_TO_FIRA(peer_caps) & session->ultrawidelock_ctx->ccc_caps.hopping_config_bitmask;

	/* Pick the first preferred config both sides fully support. */
	for (i = 0; i < prefs->count; i++) {
		uint8_t combo = 0;

		switch (prefs->configs[i]) {
		case ULTRAWIDELOCK_HOPPING_CONFIG_DISABLED:
			combo = HOP_COMBO_NO_HOPPING;
			break;
		case ULTRAWIDELOCK_HOPPING_CONFIG_CONTINUOUS_DEFAULT:
			combo = HOP_COMBO_CONTINUOUS_DEFAULT;
			break;
		case ULTRAWIDELOCK_HOPPING_CONFIG_ADAPTIVE_DEFAULT:
			combo = HOP_COMBO_ADAPTIVE_DEFAULT;
			break;
		default:
			LOG_ERR("unknown preferred hopping config 0x%02x", prefs->configs[i]);
			break;
		}
		if (combo != 0 && (common & combo) == combo) {
			session->ccc_ultrawidelock_config.hopping_mode =
				(enum cherry_ccc_hopping_mode)prefs->configs[i];
			session->ccc_ultrawidelock_config.hopping_config_bitmask = HOP_FIRA_TO_CCC(combo);
			return ULTRAWIDELOCK_UWB_ERR_NONE;
		}
	}
	LOG_ERR("no common hopping config with peer");
	return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
}

/**
 * @brief Parses the STS index 0 attribute from M2 and stores it in the session config.
 * @param session Session whose config receives the parsed STS index.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_sts_index0(struct ultrawidelock_uwb_session *session,
					   struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint32_t value;

	if (!ultrawidelock_uwb_msg_read_u32(attr, "sts index0", &value)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	session->ccc_ultrawidelock_config.sts_index = value;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the UWB time 0 attribute from M2 and stores it as the session's initial UWB time.
 * @param session Session whose config receives the parsed UWB time.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_uwb_time0(struct ultrawidelock_uwb_session *session,
					  struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint64_t value;

	if (!ultrawidelock_uwb_msg_read_u64(attr, "uwb time0", &value)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	session->ccc_ultrawidelock_config.uwb_time_us = value;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses the hop mode key attribute from M2 and stores the raw key bytes in the session
 * config; unused downstream on this lock.
 * @param session Session whose config receives the parsed hop mode key.
 * @param attr Attribute to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_hop_mode_key(struct ultrawidelock_uwb_session *session,
					     struct ultrawidelock_uwb_msg_attribute *attr)
{
	uint32_t key;

	if (!ultrawidelock_uwb_msg_read_u32(attr, "hop mode key", &key)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	/* Store the raw 4 key bytes as decoded (unused downstream on this lock). */
	memcpy(session->ccc_ultrawidelock_config.hop_mode_key, &key, CHERRY_CCC_HOP_MODE_KEY_SIZE);
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parses a status attribute from a ranging message into the given output parameter.
 * @param attr Attribute to parse.
 * @param status Output parameter receiving the parsed 8-bit status value.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if the
 * value cannot be read.
 */
static enum ultrawidelock_uwb_err parse_status(struct ultrawidelock_uwb_msg_attribute *attr,
					       uint8_t *status)
{
	if (!ultrawidelock_uwb_msg_read_u8(attr, "status", status)) {
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Dispatches a ranging-service session attribute to its type-specific parser and applies it
 * to the session; unknown attributes are logged and ignored.
 * @param attr Attribute to parse and apply.
 * @param session Session updated by the attribute-specific parser.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success or for ignored/unknown attributes, otherwise the
 * error from the specific parser.
 */
static enum ultrawidelock_uwb_err
parse_session_attribute(struct ultrawidelock_uwb_msg_attribute *attr,
			struct ultrawidelock_uwb_session *session)
{
	switch (attr->id) {
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CONFIGURATION_IDENTIFIER:
		return parse_config_id(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_PULSE_SHAPE_COMBO:
		return parse_pulse_shape(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SESSION_IDENTIFIER:
		return parse_session_id(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_CHANNEL_BITMASK:
		return parse_channel(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_RAN_MULTIPLIER:
		return parse_ran_multiplier(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SLOT_BITMASK:
		return parse_slot_bitmask(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SYNC_CODE_INDEX_BITMASK:
		return parse_sync_code_bitmask(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_SYNC_CODE_INDEX:
		return parse_sync_code_index(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_HOPPING_CONFIGURATION_BITMASK:
		return parse_hopping_bitmask(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_STS_INDEX0:
		return parse_sts_index0(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_UWB_TIME0:
		return parse_uwb_time0(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_HOP_MODE_KEY:
		return parse_hop_mode_key(session, attr);
	case ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_VENDOR_SPECIFIC:
		/* Present but unused. */
		return ULTRAWIDELOCK_UWB_ERR_NONE;
	default:
		LOG_WRN("ignoring unknown attribute id %u", attr->id);
		return ULTRAWIDELOCK_UWB_ERR_NONE;
	}
}

/**
 * @brief Parses all ranging-service attributes in a message, applying each to the session and
 * recording which attributes were present.
 * @param session Session updated by attribute-specific parsers.
 * @param message Message whose attributes are parsed.
 * @param attr_mask Output parameter receiving a bitmask of attribute IDs present in the message.
 * @param status Output parameter receiving the parsed status, or
 * ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_UNKNOWN if no status attribute is present.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or the first parsing error encountered.
 */
static enum ultrawidelock_uwb_err parse_ranging(struct ultrawidelock_uwb_session *session,
					struct ultrawidelock_uwb_message *message, uint32_t *attr_mask,
					uint8_t *status)
{
	struct ultrawidelock_uwb_msg_parser parser = ULTRAWIDELOCK_UWB_MSG_PARSER_INIT(message);
	struct ultrawidelock_uwb_msg_attribute *attr;
	enum ultrawidelock_uwb_err err;

	*status = ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_UNKNOWN;

	while ((attr = ultrawidelock_uwb_msg_next_attribute(&parser))) {
		if (attr->id < ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_MAX) {
			*attr_mask |= 1 << attr->id;
		}
		if (attr->id == ULTRAWIDELOCK_UWB_RANGING_SERVICE_ATTR_STATUS) {
			err = parse_status(attr, status);
		} else {
			err = parse_session_attribute(attr, session);
		}
		if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
			return err;
		}
	}
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/* ---- Ranging-service message handlers ------------------------------------ */

/**
 * @brief Sets the session's ranging initiation time from its time offset, using zero if
 * unsynchronized or adding the offset to the existing UWB time otherwise.
 * @param session Session whose config's uwb_time_us is updated in place.
 */
static void compute_initiation_time(struct ultrawidelock_uwb_session *session)
{
	struct cherry_ccc_ultrawidelock_session_config *cfg = &session->ccc_ultrawidelock_config;

	/* Without a synchronized time base, start as soon as possible. */
	if (session->time_offset == 0) {
		cfg->uwb_time_us = 0;
	} else {
		cfg->uwb_time_us += session->time_offset;
	}
}

/**
 * @brief Sets the STS index and initiation time on the CCC session in preparation for re-arming
 * ranging after a suspend.
 * @param session Session whose CCC session and credential config supply the resume parameters.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or ULTRAWIDELOCK_UWB_ERR_INTERNAL if either CCC
 * call fails.
 */
static enum ultrawidelock_uwb_err set_resume_params(struct ultrawidelock_uwb_session *session)
{
	enum cherry_err err;

	err = cherry_ccc_session_set_sts_index(session->ccc_session,
					       session->ccc_ultrawidelock_config.sts_index);
	if (err != CHERRY_ERR_NONE) {
		return ULTRAWIDELOCK_UWB_ERR_INTERNAL;
	}
	err = cherry_ccc_session_set_initiation_time(session->ccc_session,
						     session->ccc_ultrawidelock_config.uwb_time_us);
	if (err != CHERRY_ERR_NONE) {
		return ULTRAWIDELOCK_UWB_ERR_INTERNAL;
	}
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Handles an inbound M2 message by validating its attributes and session state, then
 * building and transmitting M3 and advancing to the M3_SENT state.
 * @param session Session receiving the M2 message.
 * @param message Inbound M2 message to process.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or an error if parsing, attributes, state, or M3
 * construction fail.
 */
static enum ultrawidelock_uwb_err handle_m2(struct ultrawidelock_uwb_session *session,
				    struct ultrawidelock_uwb_message *message)
{
	struct ultrawidelock_uwb_message *m3;
	uint32_t attr_mask = 0;
	uint8_t status;
	enum ultrawidelock_uwb_err err;

	LOG_INF("Message RangingSessionSetupM2 received");

	err = parse_ranging(session, message, &attr_mask, &status);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		return err;
	}
	if (attr_mask != M2_ATTR_MASK && attr_mask != M2_ATTR_MASK_VENDOR) {
		LOG_ERR("M2 missing attributes (mask 0x%08x)", attr_mask);
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	if (session->state != M1_SENT) {
		LOG_ERR("M2 in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}

	m3 = ultrawidelock_uwb_msg_build_m3(session);
	if (!m3) {
		LOG_ERR("failed to build M3");
		return ULTRAWIDELOCK_UWB_ERR_INTERNAL;
	}
	LOG_INF("Sending RangingSessionSetupM3 message");
	session->transmit(m3, session, session->user_data, true);
	session->state = M3_SENT;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Handles an inbound M4 message by validating its attributes and session state, computing
 * the ranging initiation time, initializing the session, and advancing to the RANGING state.
 * @param session Session receiving the M4 message.
 * @param message Inbound M4 message to process.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or an error if parsing, attributes, state, or
 * session initialization fail.
 */
static enum ultrawidelock_uwb_err handle_m4(struct ultrawidelock_uwb_session *session,
				    struct ultrawidelock_uwb_message *message)
{
	uint32_t attr_mask = 0;
	uint8_t status;
	enum ultrawidelock_uwb_err err;

	LOG_INF("Message RangingSessionSetupM4 received");

	err = parse_ranging(session, message, &attr_mask, &status);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		return err;
	}
	if (attr_mask != M4_ATTR_MASK && attr_mask != M4_ATTR_MASK_VENDOR) {
		LOG_ERR("M4 missing attributes (mask 0x%08x)", attr_mask);
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	if (session->state != M3_SENT) {
		LOG_ERR("M4 in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}

	compute_initiation_time(session);

	err = ultrawidelock_uwb_session_init(session);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		LOG_ERR("session init failed: %u", err);
		return err;
	}
	session->state = RANGING;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Handles an inbound suspend request by validating session state, stopping the session, then
 * building and transmitting a suspend response with acceptance or rejection status.
 * @param session Session receiving the suspend request.
 * @param message Inbound suspend-request message to process.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or an error if parsing, attributes, state, or
 * response construction fail.
 */
static enum ultrawidelock_uwb_err handle_suspend_request(struct ultrawidelock_uwb_session *session,
						 struct ultrawidelock_uwb_message *message)
{
	struct ultrawidelock_uwb_message *response;
	uint32_t attr_mask = 0;
	uint8_t status;
	enum ultrawidelock_uwb_err err;

	err = parse_ranging(session, message, &attr_mask, &status);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		return err;
	}
	if (attr_mask != SUSPEND_REQUEST_ATTR_MASK) {
		LOG_ERR("suspend request missing attributes");
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	if (session->state != RANGING) {
		LOG_ERR("suspend request in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}

	err = ultrawidelock_uwb_session_stop(session);
	status = (err == ULTRAWIDELOCK_UWB_ERR_NONE) ? ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_ACCEPT
					     : ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_REJECT;

	response = ultrawidelock_uwb_msg_build_suspend_response(session, status);
	if (!response) {
		LOG_ERR("failed to build suspend response");
		return ULTRAWIDELOCK_UWB_ERR_INTERNAL;
	}
	session->transmit(response, session, session->user_data, false);
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Handles an inbound suspend response by validating its attributes and session state,
 * stopping the session if accepted or returning to the RANGING state if rejected.
 * @param session Session receiving the suspend response.
 * @param message Inbound suspend-response message to process.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success, or an error if parsing, attributes, or state
 * validation fail.
 */
static enum ultrawidelock_uwb_err handle_suspend_response(struct ultrawidelock_uwb_session *session,
						  struct ultrawidelock_uwb_message *message)
{
	uint32_t attr_mask = 0;
	uint8_t status;
	enum ultrawidelock_uwb_err err;

	err = parse_ranging(session, message, &attr_mask, &status);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		return err;
	}
	if (attr_mask != SUSPEND_RESPONSE_ATTR_MASK) {
		LOG_ERR("suspend response missing attributes");
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	if (session->state != SUSPEND_REQ_SENT) {
		LOG_ERR("suspend response in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}
	if (status != ULTRAWIDELOCK_UWB_RANGING_SERVICE_STATUS_ACCEPT) {
		session->state = RANGING;
		return ULTRAWIDELOCK_UWB_ERR_NONE;
	}
	return ultrawidelock_uwb_session_stop(session);
}

/**
 * @brief Handle an inbound resume response, arm timing and CCC state, and start ranging.
 * @param session credential UWB session expected to be in RESUME_REQ_SENT state.
 * @param message Received resume response message to validate and parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success; an error if attributes are missing, state is
 * wrong, or resume setup/session start fails.
 */
static enum ultrawidelock_uwb_err handle_resume_response(struct ultrawidelock_uwb_session *session,
						 struct ultrawidelock_uwb_message *message)
{
	uint32_t attr_mask = 0;
	uint8_t status;
	enum ultrawidelock_uwb_err err;

	err = parse_ranging(session, message, &attr_mask, &status);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		return err;
	}
	if (attr_mask != RESUME_RESPONSE_ATTR_MASK) {
		LOG_ERR("resume response missing attributes");
		return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
	}
	if (session->state != RESUME_REQ_SENT) {
		LOG_ERR("resume response in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}

	compute_initiation_time(session);
	err = set_resume_params(session);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		return err;
	}
	err = ultrawidelock_uwb_session_start(session);
	if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
		return err;
	}
	session->state = RANGING;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Dispatch an inbound ranging-phase message to its handler based on message type.
 * @param session credential UWB session to update.
 * @param message Received ranging message to dispatch.
 * @return Handler's result on success; ULTRAWIDELOCK_UWB_ERR_INVALID_PARAMETER if session or
 * message is NULL; ULTRAWIDELOCK_UWB_ERR_MESSAGE_UNSUPPORTED for unknown message types.
 */
enum ultrawidelock_uwb_err
ultrawidelock_uwb_msg_process_ranging(struct ultrawidelock_uwb_session *session,
				      struct ultrawidelock_uwb_message *message)
{
	if (!session || !message) {
		return ULTRAWIDELOCK_UWB_ERR_INVALID_PARAMETER;
	}

	switch (ultrawidelock_uwb_msg_message_id(message->data)) {
	case ULTRAWIDELOCK_UWB_MESSAGE_SETUP_M2:
		return handle_m2(session, message);
	case ULTRAWIDELOCK_UWB_MESSAGE_SETUP_M4:
		return handle_m4(session, message);
	case ULTRAWIDELOCK_UWB_MESSAGE_SUSPEND_REQUEST:
		return handle_suspend_request(session, message);
	case ULTRAWIDELOCK_UWB_MESSAGE_SUSPEND_RESPONSE:
		return handle_suspend_response(session, message);
	case ULTRAWIDELOCK_UWB_MESSAGE_RESUME_RESPONSE:
		return handle_resume_response(session, message);
	default:
		LOG_INF("ranging message id %u unsupported",
			ultrawidelock_uwb_msg_message_id(message->data));
		return ULTRAWIDELOCK_UWB_ERR_MESSAGE_UNSUPPORTED;
	}
}

/* ---- Notification message handlers --------------------------------------- */

/**
 * @brief Handle an "init ranging later" notification, returning the session to CREATED state.
 * @param session credential UWB session expected to be in M1_SENT state.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success; ULTRAWIDELOCK_UWB_ERR_INVALID_STATE if the session
 * is not in M1_SENT.
 */
static enum ultrawidelock_uwb_err
handle_init_ranging_later(struct ultrawidelock_uwb_session *session)
{
	if (session->state != M1_SENT) {
		LOG_ERR("init-later in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}
	session->state = CREATED;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Handle a "resume later" notification, moving the session to SUSPENDED without re-arming
 * ranging.
 * @param session credential UWB session expected to be in RESUME_REQ_SENT state.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success; ULTRAWIDELOCK_UWB_ERR_INVALID_STATE if the session
 * is not in RESUME_REQ_SENT.
 */
static enum ultrawidelock_uwb_err handle_resume_later(struct ultrawidelock_uwb_session *session)
{
	if (session->state != RESUME_REQ_SENT) {
		LOG_ERR("resume-later in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}
	session->state = SUSPENDED;
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Handle a "ranging suspended" notification by stopping the session and moving it to
 * SUSPENDED.
 * @param session credential UWB session expected to be in RANGING state.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success; ULTRAWIDELOCK_UWB_ERR_INVALID_STATE if the session
 * is not in RANGING; otherwise the result of stopping the session.
 */
static enum ultrawidelock_uwb_err
handle_ranging_suspended(struct ultrawidelock_uwb_session *session)
{
	if (session->state != RANGING) {
		LOG_ERR("ranging-suspended in bad state %u", session->state);
		return ULTRAWIDELOCK_UWB_ERR_INVALID_STATE;
	}
	return ultrawidelock_uwb_session_stop(session);
}

/**
 * @brief Parse a credential event notification message, logging busy, general-error, and
 * reader-descriptor events.
 * @param session credential UWB session (unused).
 * @param message Received event notification message to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success; ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED if a
 * general-error attribute has the wrong length.
 */
static enum ultrawidelock_uwb_err
parse_event_notification(struct ultrawidelock_uwb_session *session,
			 struct ultrawidelock_uwb_message *message)
{
	struct ultrawidelock_uwb_msg_parser parser = ULTRAWIDELOCK_UWB_MSG_PARSER_INIT(message);
	struct ultrawidelock_uwb_msg_attribute *attr;

	ARG_UNUSED(session);

	while ((attr = ultrawidelock_uwb_msg_next_attribute(&parser))) {
		switch (attr->id) {
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_EVENT_ATTR_BUSY:
			LOG_INF("notification: busy");
			break;
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_EVENT_ATTR_GENERAL_ERROR:
			if (attr->length == 1) {
				LOG_INF("notification: general error %u", attr->value[0]);
			} else {
				LOG_ERR("malformed general error");
				return ULTRAWIDELOCK_UWB_ERR_MSG_MALFORMED;
			}
			break;
		/* Differs from default only in LOG_* text, which the host shim
		 * compiles away. NOLINTNEXTLINE(bugprone-branch-clone) */
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_EVENT_ATTR_READER_DESCRIPTOR:
			LOG_INF("notification: reader descriptor (%u bytes)", attr->length);
			LOG_HEXDUMP_INF(attr->value, attr->length, "reader descriptor");
			break;
		default:
			/* Anything the peer sends that this codec does not model was
			 * previously dropped without trace. Surface it: an unmodelled
			 * attribute is the only place a capability we do not implement
			 * could be arriving from the phone.
			 */
			LOG_WRN("notification: unhandled event attr 0x%02x (%u bytes)", attr->id,
				attr->length);
			LOG_HEXDUMP_WRN(attr->value, attr->length, "unhandled event attr");
			break;
		}
	}
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Parse a ranging-setup notification message and dispatch each attribute to its session
 * state handler.
 * @param session credential UWB session to update.
 * @param message Received ranging notification message to parse.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE on success; the first handler error encountered otherwise.
 */
static enum ultrawidelock_uwb_err
parse_ranging_notification(struct ultrawidelock_uwb_session *session,
			   struct ultrawidelock_uwb_message *message)
{
	struct ultrawidelock_uwb_msg_parser parser = ULTRAWIDELOCK_UWB_MSG_PARSER_INIT(message);
	struct ultrawidelock_uwb_msg_attribute *attr;
	enum ultrawidelock_uwb_err err = ULTRAWIDELOCK_UWB_ERR_NONE;

	while ((attr = ultrawidelock_uwb_msg_next_attribute(&parser))) {
		switch (attr->id) {
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_RANGING_ATTR_INIT_RANGING:
			err = ultrawidelock_uwb_session_init_setup(session);
			break;
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_RANGING_ATTR_INIT_RANGING_RESUME:
			err = ultrawidelock_uwb_session_resume(session);
			break;
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_RANGING_ATTR_INIT_RANGING_SETUP_LATER:
			err = handle_init_ranging_later(session);
			break;
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_RANGING_ATTR_INIT_RANGING_RESUME_LATER:
			err = handle_resume_later(session);
			break;
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_RANGING_ATTR_RANGING_SUSPENDED:
			err = handle_ranging_suspended(session);
			break;
		case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_RANGING_ATTR_SECURE_RANGING_FAILED:
			break;
		default:
			LOG_WRN("notification: unhandled ranging attr 0x%02x (%u bytes)", attr->id,
				attr->length);
			LOG_HEXDUMP_WRN(attr->value, attr->length, "unhandled ranging attr");
			break;
		}
		if (err != ULTRAWIDELOCK_UWB_ERR_NONE) {
			return err;
		}
	}
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}

/**
 * @brief Dispatch a received notification message to its parser by message ID. Reader-status
 * notifications are informational and ignored; unknown IDs are logged and ignored.
 * @param session credential UWB session to update with the parsed notification's effects.
 * @param message Received notification message to dispatch.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE for handled, informational and unknown notifications,
 * otherwise the error from the event or ranging parser.
 */
enum ultrawidelock_uwb_err
ultrawidelock_uwb_msg_process_notification(struct ultrawidelock_uwb_session *session,
					   struct ultrawidelock_uwb_message *message)
{
	switch (ultrawidelock_uwb_msg_message_id(message->data)) {
	case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_EVENT:
		return parse_event_notification(session, message);
	case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_RANGING:
		return parse_ranging_notification(session, message);
	case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_READER_STATUS_CHANGED:
	case ULTRAWIDELOCK_UWB_MESSAGE_NOTIFICATION_READER_STATUS_AP:
		/* Informational; nothing to drive on this lock. */
		return ULTRAWIDELOCK_UWB_ERR_NONE;
	default:
		LOG_INF("notification message id %u unsupported",
			ultrawidelock_uwb_msg_message_id(message->data));
		return ULTRAWIDELOCK_UWB_ERR_NONE;
	}
}

/**
 * @brief Log every attribute of a Supplementary Service message (protocol 0x03).
 *
 * The phone sends one of these at session establishment. Nothing is modelled yet
 * and no response is emitted, so this only surfaces the payload: the semantics are
 * unknown and are being characterised by diffing the attributes across conditions.
 *
 * @param session credential UWB session (unused; no state is driven from here yet).
 * @param message Received supplementary-service message to log.
 * @return ULTRAWIDELOCK_UWB_ERR_NONE always.
 */
enum ultrawidelock_uwb_err
ultrawidelock_uwb_msg_process_supplementary(struct ultrawidelock_uwb_session *session,
					    struct ultrawidelock_uwb_message *message)
{
	struct ultrawidelock_uwb_msg_parser parser = ULTRAWIDELOCK_UWB_MSG_PARSER_INIT(message);
	struct ultrawidelock_uwb_msg_attribute *attr;

	ARG_UNUSED(session);

	LOG_INF("supplementary: msg id %u, %u payload B", ultrawidelock_uwb_msg_message_id(message->data),
		ultrawidelock_uwb_msg_payload_length(message->data));

	/* Body is log-only; the host shim compiles LOG_* away, leaving the cursor
	 * unread there. Two analysers have to be told, and CodeChecker's marker only
	 * counts on the line directly above the report -- so clang-tidy gets the
	 * BEGIN/END pair rather than a NOLINTNEXTLINE that would land on the
	 * codechecker comment instead of the loop. */
	/* NOLINTBEGIN(clang-analyzer-deadcode.DeadStores) */
	// codechecker_suppress [deadcode.DeadStores] log-only body, see above
	while ((attr = ultrawidelock_uwb_msg_next_attribute(&parser))) {
		LOG_INF("supplementary: attr 0x%02x (%u B)", attr->id, attr->length);
		LOG_HEXDUMP_INF(attr->value, attr->length, "supplementary attr");
	}
	/* NOLINTEND(clang-analyzer-deadcode.DeadStores) */
	return ULTRAWIDELOCK_UWB_ERR_NONE;
}
