/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace Impulse {

// Returns a value in [0, 1). The refresh schedule draws from it.
using RandomUnit = std::function<double()>;

// Claims read from a connection identity or access JWT. Times are unix
// seconds; a value of 0 or less means the claim is absent.
struct TokenClaims {
	std::int64_t exp = 0;
	std::int64_t iat = 0;
	std::int64_t origIat = 0;
	std::string sub;
};

inline constexpr std::int64_t RefreshWindowStartBeforeExpirySeconds = 300;
inline constexpr std::int64_t RefreshWindowEndBeforeExpirySeconds = 120;
inline constexpr std::int64_t IdentityExpiryMarginSeconds = 60;
inline constexpr std::int64_t IdentityFirstRetryMs = 1000;
inline constexpr std::int64_t IdentityMaxRetryMs = 60000;
inline constexpr std::int64_t RefreshGraceDefaultSeconds = 30;

// Moment, in milliseconds, at which the token is refreshed. The point is drawn
// uniformly in [exp - 300 s, exp - 120 s], clipped to now. When less than
// 120 s remain, the result is now.
[[nodiscard]] std::int64_t refreshPointMs(
	std::int64_t expSeconds,
	std::int64_t nowMs,
	const RandomUnit &random);

// True while the identity is valid for more than 60 s.
[[nodiscard]] bool identityUsable(
	std::int64_t expSeconds,
	std::int64_t nowMs);

// Same-identity rule of a renewal: same sub and orig_iat, and orig_iat is not
// equal to the new iat. A same identity gets the new token in place; a
// different one replaces the current identity.
[[nodiscard]] bool isSameIdentity(
	const TokenClaims &before,
	const TokenClaims &after);

// Delay of the first renewal retry after a failure.
[[nodiscard]] std::int64_t firstRetryDelayMs();

// Delay after the given one: doubled, capped at 60 s.
[[nodiscard]] std::int64_t nextRetryDelayMs(std::int64_t delayMs);

// True when a retry scheduled at nowMs + delayMs lands before exp - 120 s.
// Renewal retries and FLOOD deferrals both use this rule.
[[nodiscard]] bool retryLandsBeforeWindowEnd(
	std::int64_t expSeconds,
	std::int64_t nowMs,
	std::int64_t delayMs);

// FLOOD_WAIT on refresh: deferred only when the retry lands before exp - 120 s.
[[nodiscard]] bool floodDeferralAllowed(
	std::int64_t expSeconds,
	std::int64_t nowMs,
	std::int64_t waitSeconds);

// Refresh grace in milliseconds. The server value is used when positive,
// otherwise the 30 s default.
[[nodiscard]] std::int64_t refreshGraceMs(std::int64_t serverGraceSeconds);

// True while a replaced refresh token still answers the same session.
[[nodiscard]] bool withinRefreshGrace(
	std::int64_t replacedAtMs,
	std::int64_t nowMs,
	std::int64_t graceMs);

} // namespace Impulse
