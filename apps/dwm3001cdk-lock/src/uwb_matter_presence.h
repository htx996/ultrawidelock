#pragma once

#include <stdint.h>

#include "ultrawidelock_approach.h"

void uwb_matter_presence_init(uint32_t unlock_threshold_cm);
void uwb_matter_presence_update(const struct ultrawidelock_approach *approach,
				int32_t distance_cm, int64_t now_ms);
void uwb_matter_presence_clear(void);
