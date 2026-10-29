// Routing rules of the ImpulseM proxy: the route of the main traffic, the
// tunnelling of calls and the HTTP endpoint the selector hands out.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Impulse::ProxyRouting {

inline constexpr std::string_view kLoopbackHost = "127.0.0.1";

// The discard port on loopback: connections fail at once. Used instead of a
// direct route when the kill switch blocks the traffic.
inline constexpr int kBlackholePort = 9;

enum class Route {
	Direct,
	Proxy,
	Blocked,
};

// Running core and HTTP inbound port with its Basic credentials.
struct Runtime {
	int httpPort = 0;
	std::string user;
	std::string password;
};

struct HttpEndpoint {
	std::string host;
	int port = 0;
	bool hasCredentials = false;
	std::string user;
	std::string password;
};

// With the transport switched off the main traffic goes direct, whatever the
// core is doing. Otherwise it goes through the core when it runs and is
// blocked (never direct) when it does not.
[[nodiscard]] Route Decide(
	bool enabled,
	bool useForTransport,
	bool running);

// Whether a call must be relayed through the SOCKS5 inbound. Independent of
// whether the core is running: with the core down the tunnelled call fails
// closed instead of going direct.
[[nodiscard]] bool TunnelCalls(
	bool enabled,
	bool useForCalls);

// The HTTP endpoint for a route. Direct gives no proxy. Proxy gives the
// loopback HTTP inbound with credentials when the runtime is known, and the
// blackhole otherwise. Blocked gives the blackhole.
[[nodiscard]] std::optional<HttpEndpoint> SelectEndpoint(
	Route route,
	const std::optional<Runtime> &runtime);

} // namespace Impulse::ProxyRouting
