// Classification of RPC errors and carrier failures, and the request retry rules.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace Impulse {

enum class CarrierKind {
	Update,
	Pool,
	Media,
};

enum class AuthErrorClass {
	None,
	RefreshAndRetry,
	ForceLogout,
};

enum class RetryAction {
	Deliver,
	RetryAfter,
	LogoutAndDeliver,
};

struct RpcError {
	int code = 0;
	std::string text;
};

struct CarrierFailure {
	int sdkCode = 0;
	std::string appType;
	int appCode = 0;
	bool migrateDc = false;
};

struct MappedError {
	RpcError error;
	bool temporary = false;
};

// Mirrors the request flags of the retry rules. failOnServerErrors = false
// means server errors are retried, as with flags == 0.
struct RetryOptions {
	bool failOnServerErrors = false;
	bool failOnServerErrorsExceptFloodWait = false;
	bool ignoreFloodWait = false;
	bool download = false;
	bool forceDownload = false;
	int attempt = 0;
};

struct RetryDecision {
	RetryAction action = RetryAction::Deliver;
	std::int64_t delayMs = 0;
	std::optional<RpcError> replacement;
};

[[nodiscard]] AuthErrorClass classifyAuthError(const RpcError &error);

// Seconds from FLOOD_WAIT_X, FLOOD_PREMIUM_WAIT_X or SLOWMODE_WAIT_X; -1 otherwise.
[[nodiscard]] int floodWaitSeconds(const RpcError &error);

// Maps a carrier failure to the app result. Returns nullopt when the failure
// is a network failure and must not become an rpc_error.
[[nodiscard]] std::optional<MappedError> mapCarrierFailure(
	const CarrierFailure &failure,
	CarrierKind carrier);

[[nodiscard]] RetryDecision decideRpcRetry(
	const RpcError &error,
	const RetryOptions &options);

[[nodiscard]] RetryDecision decideNetworkRetry(const RetryOptions &options);

} // namespace Impulse
