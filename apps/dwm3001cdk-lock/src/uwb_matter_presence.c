#include "uwb_matter_presence.h"

#include "matter_commission.h"
#include "ultrawidelock_hash.h"

#include <limits.h>
#include <string.h>
#include <ultrawidelock/reader.h>

#define MOVEMENT_ENTER_CM_S      20
#define MOVEMENT_STATIONARY_CM_S  8
#define MOVEMENT_CONFIRM_SAMPLES  2u
#define MOVEMENT_STATIONARY_MS  500
#define MOVEMENT_HOLD_MS        300

struct movement_filter {
	enum matter_uwb_movement_state state;
	enum matter_uwb_movement_state candidate;
	uint8_t candidate_samples;
	int64_t candidate_since_ms;
	int64_t hold_until_ms;
};

static struct movement_filter s_movement;

static uint32_t credential_id(void)
{
	uint8_t cred_pub[65];
	uint8_t digest[ULTRAWIDELOCK_SHA256_LEN];
	uint32_t id;

	if (!ultrawidelock_reader_authenticated_credential(cred_pub)) {
		return 0u;
	}
	ultrawidelock_sha256(cred_pub, sizeof(cred_pub), digest);
	id = ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) |
	     ((uint32_t)digest[2] << 8) | (uint32_t)digest[3];
	return id == 0u || id == UINT32_MAX ? 1u : id;
}

static void movement_reset(void)
{
	memset(&s_movement, 0, sizeof(s_movement));
	s_movement.state = MATTER_UWB_MOVEMENT_STATE_UNKNOWN;
	s_movement.candidate = MATTER_UWB_MOVEMENT_STATE_UNKNOWN;
}

static enum matter_uwb_movement_state movement_state(int32_t velocity_cm_s,
						      int64_t now_ms)
{
	enum matter_uwb_movement_state candidate;
	int64_t dwell_ms;

	if (velocity_cm_s >= MOVEMENT_ENTER_CM_S) {
		candidate = MATTER_UWB_MOVEMENT_STATE_APPROACHING;
	} else if (velocity_cm_s <= -MOVEMENT_ENTER_CM_S) {
		candidate = MATTER_UWB_MOVEMENT_STATE_LEAVING;
	} else if (velocity_cm_s >= -MOVEMENT_STATIONARY_CM_S &&
		   velocity_cm_s <= MOVEMENT_STATIONARY_CM_S) {
		candidate = MATTER_UWB_MOVEMENT_STATE_STATIONARY;
	} else {
		s_movement.candidate = s_movement.state;
		s_movement.candidate_samples = 0u;
		s_movement.candidate_since_ms = now_ms;
		return s_movement.state;
	}
	if (candidate == s_movement.state) {
		s_movement.candidate = candidate;
		s_movement.candidate_samples = 0u;
		s_movement.candidate_since_ms = now_ms;
		return s_movement.state;
	}
	if (candidate != s_movement.candidate) {
		s_movement.candidate = candidate;
		s_movement.candidate_samples = 1u;
		s_movement.candidate_since_ms = now_ms;
	} else if (s_movement.candidate_samples < UINT8_MAX) {
		s_movement.candidate_samples++;
	}
	dwell_ms = candidate == MATTER_UWB_MOVEMENT_STATE_STATIONARY
			   ? MOVEMENT_STATIONARY_MS
			   : 0;
	if (s_movement.candidate_samples >= MOVEMENT_CONFIRM_SAMPLES &&
	    now_ms >= s_movement.hold_until_ms &&
	    now_ms - s_movement.candidate_since_ms >= dwell_ms) {
		s_movement.state = candidate;
		s_movement.candidate_samples = 0u;
		s_movement.hold_until_ms = now_ms + MOVEMENT_HOLD_MS;
	}
	return s_movement.state;
}

void uwb_matter_presence_init(uint32_t unlock_threshold_cm)
{
	movement_reset();
	matter_commission_set_uwb_unlock_threshold(unlock_threshold_cm);
}

void uwb_matter_presence_update(const struct ultrawidelock_approach *approach,
				int32_t distance_cm, int64_t now_ms)
{
	matter_commission_update_uwb_presence(
		true, distance_cm * 10, credential_id(),
		movement_state(ultrawidelock_approach_vel_cm_s(approach), now_ms));
}

void uwb_matter_presence_clear(void)
{
	movement_reset();
	matter_commission_update_uwb_presence(
		false, -1, 0u, MATTER_UWB_MOVEMENT_STATE_UNKNOWN);
}
