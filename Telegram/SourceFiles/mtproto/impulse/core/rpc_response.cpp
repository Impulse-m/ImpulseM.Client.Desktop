#include "mtproto/impulse/core/rpc_response.h"

namespace Impulse {

namespace {

constexpr std::string_view kClientPrefix = "CLIENT_";

} // namespace

std::uint64_t OuterMsgIdFromServerTime(std::uint64_t serverTimeMs) {
	// (serverTimeMs << 32) / 1000 split into whole seconds and the
	// remainder, so the intermediate value never overflows.
	const std::uint64_t seconds = serverTimeMs / 1000;
	const std::uint64_t remainderMs = serverTimeMs % 1000;
	const std::uint64_t value = (seconds << 32)
		+ ((remainderMs << 32) / 1000);
	return (value & ~std::uint64_t{3}) | std::uint64_t{1};
}

std::string RpcErrorText(std::string_view type, std::string_view description) {
	std::string result;
	result.reserve(type.size() + 2 + description.size());
	result.append(type);
	result.append(": ");
	result.append(description);
	return result;
}

RpcErrorPayload MakeClientError(std::string_view type, std::string_view description) {
	RpcErrorPayload error;
	error.code = kClientErrorCode;
	if (type.substr(0, kClientPrefix.size()) == kClientPrefix) {
		error.type = std::string(type);
	} else {
		error.type = std::string(kClientPrefix) + std::string(type);
	}
	error.description = std::string(description);
	return error;
}

bool IsRetriedForever(std::int32_t code) {
	return code < 0 || code >= 500;
}

} // namespace Impulse
