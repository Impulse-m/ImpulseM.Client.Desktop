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

// The websocket base of an https or http base URL, without a trailing slash.
[[nodiscard]] std::string WebsocketBase(std::string_view rpcBase) {
	std::string_view rest;
	std::string_view scheme;
	if (rpcBase.starts_with("https://")) {
		scheme = "wss://";
		rest = rpcBase.substr(std::string_view("https://").size());
	} else if (rpcBase.starts_with("http://")) {
		scheme = "ws://";
		rest = rpcBase.substr(std::string_view("http://").size());
	} else {
		return std::string();
	}
	while (!rest.empty() && rest.back() == '/') {
		rest.remove_suffix(1);
	}
	if (rest.empty()) {
		return std::string();
	}
	return std::string(scheme) + std::string(rest);
}

} // namespace

std::string UpdateUrl(std::string_view rpcBase) {
	const std::string base = WebsocketBase(rpcBase);
	if (base.empty()) {
		return std::string();
	}
	return base + "/connection/websocket?cf_ws_frame_ping_pong=true";
}

std::string PoolUrl(std::string_view rpcBase) {
	const std::string base = WebsocketBase(rpcBase);
	if (base.empty()) {
		return std::string();
	}
	return base
		+ "/pool/connection/websocket"
		  "?cf_protocol=protobuf&cf_ws_frame_ping_pong=true";
}

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
