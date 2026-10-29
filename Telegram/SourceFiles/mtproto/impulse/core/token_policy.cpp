/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/impulse/core/token_policy.h"

#include <algorithm>
#include <limits>

namespace Impulse {
namespace {

constexpr std::int64_t MsPerSecond = 1000;
constexpr std::int64_t MaxSecondsConvertibleToMs
	= std::numeric_limits<std::int64_t>::max() / MsPerSecond;

[[nodiscard]] double clampUnit(double value) {
	if (!(value >= 0.0)) {
		return 0.0;
	}
	if (value >= 1.0) {
		return 0.999999999999;
	}
	return value;
}

} // namespace

std::int64_t refreshPointMs(
		std::int64_t expSeconds,
		std::int64_t nowMs,
		const RandomUnit &random) {
	const std::int64_t endMs
		= (expSeconds - RefreshWindowEndBeforeExpirySeconds) * MsPerSecond;
	if (endMs <= nowMs) {
		return nowMs;
	}
	const std::int64_t startMs = std::max(
		nowMs,
		(expSeconds - RefreshWindowStartBeforeExpirySeconds) * MsPerSecond);
	const double unit = clampUnit(random ? random() : 0.0);
	const std::int64_t span = endMs - startMs;
	return startMs + static_cast<std::int64_t>(unit * static_cast<double>(span));
}

bool identityUsable(
		std::int64_t expSeconds,
		std::int64_t nowMs) {
	return expSeconds * MsPerSecond - nowMs
		> IdentityExpiryMarginSeconds * MsPerSecond;
}

bool isSameIdentity(
		const TokenClaims &before,
		const TokenClaims &after) {
	return after.sub == before.sub
		&& after.origIat == before.origIat
		&& after.origIat != after.iat;
}

std::int64_t firstRetryDelayMs() {
	return IdentityFirstRetryMs;
}

std::int64_t nextRetryDelayMs(std::int64_t delayMs) {
	return std::min(delayMs * 2, IdentityMaxRetryMs);
}

bool retryLandsBeforeWindowEnd(
		std::int64_t expSeconds,
		std::int64_t nowMs,
		std::int64_t delayMs) {
	if (delayMs < 0) {
		return false;
	}
	const std::int64_t endMs
		= (expSeconds - RefreshWindowEndBeforeExpirySeconds) * MsPerSecond;
	return delayMs < endMs - nowMs;
}

bool floodDeferralAllowed(
		std::int64_t expSeconds,
		std::int64_t nowMs,
		std::int64_t waitSeconds) {
	if (waitSeconds < 0 || waitSeconds > MaxSecondsConvertibleToMs) {
		return false;
	}
	return retryLandsBeforeWindowEnd(
		expSeconds,
		nowMs,
		waitSeconds * MsPerSecond);
}

std::int64_t refreshGraceMs(std::int64_t serverGraceSeconds) {
	const std::int64_t seconds = serverGraceSeconds > 0
		? serverGraceSeconds
		: RefreshGraceDefaultSeconds;
	return seconds * MsPerSecond;
}

bool withinRefreshGrace(
		std::int64_t replacedAtMs,
		std::int64_t nowMs,
		std::int64_t graceMs) {
	return nowMs >= replacedAtMs && nowMs - replacedAtMs < graceMs;
}

} // namespace Impulse
