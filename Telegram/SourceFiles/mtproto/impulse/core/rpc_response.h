// RPC response helpers: outer message id of a reply, rpc_error text and
// the local permanent error class.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Impulse {

// Code of every local permanent failure. Local errors never use a server
// code name and never use a code of 500 or above, or a negative code.
inline constexpr std::int32_t kClientErrorCode = 400;

struct RpcErrorPayload {
	std::int32_t code = 0;
	std::string type;
	std::string description;
};

// Outer message id of a server reply. The server time in milliseconds is
// converted to (serverTimeMs << 32) / 1000 (computed without overflow) and
// the low two bits are set to 01, so the reply time reads back as
// msgId >> 32.
[[nodiscard]] std::uint64_t OuterMsgIdFromServerTime(
	std::uint64_t serverTimeMs);

// The rpc_error text in the form "TYPE: description".
[[nodiscard]] std::string RpcErrorText(
	std::string_view type,
	std::string_view description);

// A local permanent failure: code 400 and a CLIENT_* type. A type given
// without the CLIENT_ prefix gets it added, so the local class is kept.
[[nodiscard]] RpcErrorPayload MakeClientError(
	std::string_view type,
	std::string_view description);

// True for the codes that the carrier retries forever: 500 and above, and
// negative codes. Permanent failures must not have such a code.
[[nodiscard]] bool IsRetriedForever(std::int32_t code);

} // namespace Impulse
