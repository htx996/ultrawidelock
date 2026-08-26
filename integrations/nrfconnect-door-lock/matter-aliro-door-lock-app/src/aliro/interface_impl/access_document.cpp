/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <aliro/interface.h>

#ifdef CONFIG_DOOR_LOCK_TIME_CONCEPT
#include <aliro/utils.h>
#include <time_utils/time_utils.h>
#endif // CONFIG_DOOR_LOCK_TIME_CONCEPT

#ifdef CONFIG_DOOR_LOCK_TIME_CONCEPT_RATCHET
#include <lib/support/TimeUtils.h>
#include <system/SystemClock.h>
#endif // CONFIG_DOOR_LOCK_TIME_CONCEPT_RATCHET

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(interface_access_document, CONFIG_DOOR_LOCK_APP_LOG_LEVEL);

namespace Aliro::Interface::AccessDocument {

std::optional<bool> VerifyValidityPeriod(const Time &validFrom, const Time &validUntil)
{
#ifdef CONFIG_DOOR_LOCK_TIME_CONCEPT
	const auto currentTimeOpt = DoorLock::TimeUtils::GetCurrentTime();
	VerifyOrReturnValue(currentTimeOpt.has_value(), std::nullopt);

	const auto &currentTime = currentTimeOpt.value();

	LOG_DBG("Current time: %04d-%02d-%02d %02d:%02d:%02d", currentTime.mYear, currentTime.mMonth, currentTime.mDay,
		currentTime.mHour, currentTime.mMinute, currentTime.mSecond);
	LOG_DBG("validFrom   : %04d-%02d-%02d %02d:%02d:%02d", validFrom.mYear, validFrom.mMonth, validFrom.mDay,
		validFrom.mHour, validFrom.mMinute, validFrom.mSecond);
	LOG_DBG("validUntil  : %04d-%02d-%02d %02d:%02d:%02d", validUntil.mYear, validUntil.mMonth, validUntil.mDay,
		validUntil.mHour, validUntil.mMinute, validUntil.mSecond);

#ifdef CONFIG_DOOR_LOCK_TIME_CONCEPT_RATCHET
	/* The stack invokes this hook only after the Access Document signature has been
	 * verified against the provisioned Credential Issuer key, so validFrom is an
	 * issuer-authenticated timestamp. Documents are minted on demand with
	 * validFrom = the user device's current time; the reader wall clock lives in
	 * RAM and is set once at commissioning, so after a reboot the reader runs on
	 * (stale) Last Known Good Time and would reject every fresh document as
	 * not-yet-valid. Ratchet the system clock forward (never backward) to
	 * validFrom instead of failing.
	 */
	if (currentTime < validFrom && !(validUntil < validFrom)) {
		uint32_t unixSeconds;
		if (chip::CalendarTimeToSecondsSinceUnixEpoch(
			    static_cast<uint16_t>(validFrom.mYear), static_cast<uint8_t>(validFrom.mMonth),
			    static_cast<uint8_t>(validFrom.mDay), static_cast<uint8_t>(validFrom.mHour),
			    static_cast<uint8_t>(validFrom.mMinute), static_cast<uint8_t>(validFrom.mSecond),
			    unixSeconds)) {
			chip::System::SystemClock().SetClock_RealTime(
				chip::System::Clock::Microseconds64(static_cast<uint64_t>(unixSeconds) * 1000000ULL));
			LOG_WRN("Clock was behind Access Document validFrom - system time ratcheted forward to "
				"%04d-%02d-%02d %02d:%02d:%02d",
				validFrom.mYear, validFrom.mMonth, validFrom.mDay, validFrom.mHour, validFrom.mMinute,
				validFrom.mSecond);
			/* Now == validFrom and validFrom <= validUntil, so the period holds. */
			return true;
		}
	}
#endif // CONFIG_DOOR_LOCK_TIME_CONCEPT_RATCHET

	const bool isWithinValidityPeriod = currentTime >= validFrom && currentTime <= validUntil;
	if (!isWithinValidityPeriod) {
		LOG_WRN("Current time is outside the Access Document validity period");
	}

	return isWithinValidityPeriod;
#else
	ARG_UNUSED(validFrom);
	ARG_UNUSED(validUntil);
	LOG_WRN("Time concept is not supported");
	return std::nullopt;
#endif // CONFIG_DOOR_LOCK_TIME_CONCEPT
}

} // namespace Aliro::Interface::AccessDocument
