// Endpoint addresses of the ImpulseM transport. They are derived from the
// base URL of the transport host, IMPULSEM_RPC_URL, see build_config.h.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Impulse::Endpoints {

// Websocket endpoints of the update and pool fleets. The base is an https or
// http URL; it becomes wss or ws. An empty or unsupported base gives an empty
// endpoint, which the transport does not dial.
[[nodiscard]] std::string UpdateUrl(std::string_view rpcBase);
[[nodiscard]] std::string PoolUrl(std::string_view rpcBase);

// Builds the media fleet URL from a DC option address. An IPv6 literal is
// wrapped in brackets; an address that is already bracketed is kept as is.
// Returns nullopt for an empty or malformed host and for a port outside 1...65535.
[[nodiscard]] std::optional<std::string> MediaUrl(
	std::string_view host,
	int port);

} // namespace Impulse::Endpoints
