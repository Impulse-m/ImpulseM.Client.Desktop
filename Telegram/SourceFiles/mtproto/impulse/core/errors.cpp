#include "mtproto/impulse/core/errors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

namespace Impulse {
namespace {

constexpr int kSdkTokenInvalid = 103;
constexpr int kSdkTokenExpired = 109;
constexpr int kSdkMethodInvalid = 104;
constexpr int kSdkOverloaded = 111;
constexpr int kSdkTooLarge = 106;

constexpr int kUnauthorizedCode = 401;
constexpr int kRefusalCode = 400;
constexpr int kDefaultAppCode = 400;
constexpr int kMigrateDcCode = 303;
constexpr int kServerErrorCode = 500;
constexpr int kFloodCode = 420;

constexpr int kDefaultFloodWaitSeconds = 2;
constexpr int kMaxBackoffSeconds = 10;
constexpr int kDownloadRetryMax = 6;
constexpr int kForceDownloadRetryMax = 10;
constexpr int kRetryLimitCode = -123;

const std::array<std::string_view, 3> kFloodPrefixes = {
	"FLOOD_WAIT_",
	"FLOOD_PREMIUM_WAIT_",
	"SLOWMODE_WAIT_",
};

[[nodiscard]] bool contains(const std::string &text, std::string_view part) {
	return text.find(part) != std::string::npos;
}

[[nodiscard]] MappedError makeMapped(
	int code,
	std::string text,
	bool temporary) {
	MappedError mapped;
	mapped.error.code = code;
	mapped.error.text = std::move(text);
	mapped.temporary = temporary;
	return mapped;
}

[[nodiscard]] MappedError mapAppType(const CarrierFailure &failure) {
	if (failure.appType == "UNAVAILABLE" || failure.appType == "DEADLINE_EXCEEDED") {
		return makeMapped(kServerErrorCode, failure.appType, true);
	}
	if (failure.appType == "UNKNOWN" || failure.appType == "INTERNAL") {
		return makeMapped(kServerErrorCode, failure.appType, false);
	}
	int code = kDefaultAppCode;
	if (failure.migrateDc) {
		code = kMigrateDcCode;
	} else if (failure.appCode != 0) {
		code = failure.appCode;
	}
	return makeMapped(code, failure.appType, false);
}

[[nodiscard]] RetryDecision deliverAsIs() {
	return RetryDecision();
}

[[nodiscard]] RetryDecision logoutAndDeliver() {
	RetryDecision decision;
	decision.action = RetryAction::LogoutAndDeliver;
	return decision;
}

[[nodiscard]] RetryDecision retryAfterMs(std::int64_t delayMs) {
	RetryDecision decision;
	decision.action = RetryAction::RetryAfter;
	decision.delayMs = delayMs;
	return decision;
}

// Reads the number after the prefix. Missing digits or a value <= 0 give the default of 2 s.
[[nodiscard]] int parseWaitSeconds(const std::string &text, std::string_view prefix) {
	const std::size_t found = text.find(prefix);
	if (found == std::string::npos) {
		return kDefaultFloodWaitSeconds;
	}
	const char *first = text.data() + found + prefix.size();
	const char *last = text.data() + text.size();
	const char *end = first;
	while (end != last && *end >= '0' && *end <= '9') {
		++end;
	}
	if (end == first) {
		return kDefaultFloodWaitSeconds;
	}
	int value = 0;
	const std::from_chars_result parsed = std::from_chars(first, end, value);
	if (parsed.ec != std::errc() || value <= 0) {
		return kDefaultFloodWaitSeconds;
	}
	return value;
}

} // namespace

AuthErrorClass classifyAuthError(const RpcError &error) {
	if (error.text == "AUTH_KEY_UNREGISTERED") {
		return AuthErrorClass::RefreshAndRetry;
	}
	if (error.text == "SESSION_REVOKED"
		|| error.text == "USER_DEACTIVATED"
		|| error.text == "SESSION_EXPIRED"
		|| error.text == "AUTH_TOKEN_EXPIRED"
		|| error.text == "AUTH_KEY_INVALID") {
		return AuthErrorClass::ForceLogout;
	}
	return AuthErrorClass::None;
}

int floodWaitSeconds(const RpcError &error) {
	for (const std::string_view prefix : kFloodPrefixes) {
		if (error.text.starts_with(prefix)) {
			const char *first = error.text.data() + prefix.size();
			const char *last = error.text.data() + error.text.size();
			if (first == last) {
				return -1;
			}
			int value = 0;
			const std::from_chars_result parsed = std::from_chars(first, last, value);
			if (parsed.ec != std::errc() || parsed.ptr != last) {
				return -1;
			}
			return value;
		}
	}
	return -1;
}

std::optional<MappedError> mapCarrierFailure(
		const CarrierFailure &failure,
		CarrierKind carrier) {
	// Refusal check first, regardless of appType.
	if (failure.sdkCode == kSdkTokenInvalid) {
		if (carrier == CarrierKind::Media) {
			return makeMapped(kRefusalCode, "AUTH_TOKEN_INVALID", false);
		}
		return makeMapped(kUnauthorizedCode, "AUTH_KEY_UNREGISTERED", false);
	}
	if (failure.sdkCode == kSdkTokenExpired) {
		return makeMapped(kRefusalCode, "AUTH_TOKEN_EXPIRED", false);
	}
	if (!failure.appType.empty()) {
		return mapAppType(failure);
	}
	// Transport-reported code, not a local refusal.
	if (failure.sdkCode == kSdkMethodInvalid) {
		return makeMapped(kRefusalCode, "METHOD_INVALID", false);
	}
	if (failure.sdkCode == kSdkOverloaded || failure.sdkCode == kSdkTooLarge) {
		return makeMapped(kServerErrorCode, "INTERNAL_SERVER_ERROR", false);
	}
	// SDK 15, 16, 17, 100 without appType, 4, 12, 18, IOException and timeouts.
	return std::nullopt;
}

RetryDecision decideRpcRetry(
		const RpcError &error,
		const RetryOptions &options) {
	if (error.code == kUnauthorizedCode && contains(error.text, "SESSION_PASSWORD_NEEDED")) {
		return deliverAsIs();
	}
	if (classifyAuthError(error) == AuthErrorClass::ForceLogout) {
		return logoutAndDeliver();
	}

	const bool processEvenFailed = error.code == kServerErrorCode
		&& contains(error.text, "AUTH_RESTART");
	const bool isWorkerBusy = error.code == kServerErrorCode
		&& contains(error.text, "WORKER_BUSY_TOO_LONG_RETRY");
	const bool retryServerErrors = !options.failOnServerErrors || processEvenFailed;
	const bool retryFloodWait = retryServerErrors || options.failOnServerErrorsExceptFloodWait;

	// 500 and 503 (gRPC UNAVAILABLE) and negative codes are transient.
	if (retryServerErrors
		&& (error.code == kServerErrorCode || error.code == 503 || error.code < 0)) {
		if (contains(error.text, "MSG_WAIT_FAILED") || isWorkerBusy) {
			return retryAfterMs(0);
		}
		const int attempt = std::max(options.attempt, 0);
		return retryAfterMs(std::min(attempt, kMaxBackoffSeconds) * std::int64_t(1000));
	}

	if (retryFloodWait
		&& error.code == kFloodCode
		&& !options.ignoreFloodWait
		&& !contains(error.text, "STORY_SEND_FLOOD")) {
		if (contains(error.text, "SLOWMODE_WAIT_")) {
			return deliverAsIs();
		}
		if (contains(error.text, "FLOOD_PREMIUM_WAIT_")) {
			const int seconds = parseWaitSeconds(error.text, "FLOOD_PREMIUM_WAIT_");
			return retryAfterMs(std::int64_t(seconds) * 1000);
		}
		if (contains(error.text, "FLOOD_WAIT_")) {
			const int seconds = parseWaitSeconds(error.text, "FLOOD_WAIT_");
			return retryAfterMs(std::int64_t(seconds) * 1000);
		}
		return retryAfterMs(std::int64_t(kDefaultFloodWaitSeconds) * 1000);
	}

	if (retryServerErrors
		&& error.code == kRefusalCode
		&& (contains(error.text, "MSG_WAIT_TIMEOUT") || contains(error.text, "MSG_WAIT_FAILED"))) {
		return retryAfterMs(0);
	}

	return deliverAsIs();
}

RetryDecision decideNetworkRetry(const RetryOptions &options) {
	// Only download connections give up.
	if (options.download) {
		const int retryMax = options.forceDownload
			? kForceDownloadRetryMax
			: kDownloadRetryMax;
		if (options.attempt >= retryMax) {
			RetryDecision decision;
			decision.action = RetryAction::Deliver;
			RpcError limit;
			limit.code = kRetryLimitCode;
			limit.text = "RETRY_LIMIT";
			decision.replacement = limit;
			return decision;
		}
	}
	const int attempt = std::max(options.attempt, 1);
	return retryAfterMs(std::min(attempt, kMaxBackoffSeconds) * std::int64_t(1000));
}

} // namespace Impulse
