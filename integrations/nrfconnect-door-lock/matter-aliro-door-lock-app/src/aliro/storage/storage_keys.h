/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <array>
#include <cstddef>

namespace Aliro::StorageKeys {

constexpr size_t kKeyNameMaxLength{ 16 };

using KeyNameBuffer = std::array<char, Aliro::StorageKeys::kKeyNameMaxLength>;

constexpr char kDoorLockBaseKey[] = "dl";

#ifdef CONFIG_DOOR_LOCK_STEP_UP_PHASE

constexpr char kStorageKeyNameCredentialIssuerValidityIteration[] = "VI";

#endif // CONFIG_DOOR_LOCK_STEP_UP_PHASE

#ifdef CONFIG_DOOR_LOCK_TIME_CONCEPT_PERSIST

constexpr char kStorageKeyNameWallClock[] = "UT";

#endif // CONFIG_DOOR_LOCK_TIME_CONCEPT_PERSIST

} // namespace Aliro::StorageKeys
