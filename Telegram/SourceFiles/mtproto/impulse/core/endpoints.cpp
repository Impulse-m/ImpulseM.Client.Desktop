#include "mtproto/impulse/core/endpoints.h"

#include <cctype>

namespace Impulse::Endpoints {
namespace {

constexpr std::string_view kMediaPath
	= "/connection/websocket?cf_protocol=protobuf&cf_ws_frame_ping_pong=true";

[[nodiscard]] bool IsHostSymbolsValid(std::string_view host) {
	if (host.empty()) {
		return false;
	}
	for (const char symbol : host) {
		const bool allowed = std::isalnum(static_cast<unsigned char>(symbol))
			|| symbol == '.'
			|| symbol == '-'
			|| symbol == ':';
		if (!allowed) {
			return false;
		}
	}
	return true;
}

} // namespace

std::optional<std::string> MediaUrl(std::string_view host, int port) {
	if (port < 1 || port > 65535) {
		return std::nullopt;
	}

	const bool bracketed = host.size() >= 2
		&& host.front() == '['
		&& host.back() == ']';
	const std::string_view inner = bracketed
		? host.substr(1, host.size() - 2)
		: host;
	if (!IsHostSymbolsValid(inner)) {
		return std::nullopt;
	}

	const bool isIpv6 = inner.find(':') != std::string_view::npos;
	std::string result = "wss://";
	if (isIpv6 && !bracketed) {
		result += '[';
		result += inner;
		result += ']';
	} else {
		result += host;
	}
	result += ':';
	result += std::to_string(port);
	result += kMediaPath;
	return result;
}

} // namespace Impulse::Endpoints
