// Endpoint addresses of the ImpulseM backend.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Impulse::Endpoints {

inline constexpr std::string_view kRpcBase = "https://rpc.impulsem.io";
inline constexpr std::string_view kUpdateUrl
	= "wss://rpc.impulsem.io/connection/websocket?cf_ws_frame_ping_pong=true";
inline constexpr std::string_view kPoolUrl
	= "wss://rpc.impulsem.io/pool/connection/websocket"
	  "?cf_protocol=protobuf&cf_ws_frame_ping_pong=true";

// Builds the media fleet URL from a DC option address. An IPv6 literal is
// wrapped in brackets; an address that is already bracketed is kept as is.
// Returns nullopt for an empty or malformed host and for a port outside 1...65535.
[[nodiscard]] std::optional<std::string> MediaUrl(
	std::string_view host,
	int port);

} // namespace Impulse::Endpoints
