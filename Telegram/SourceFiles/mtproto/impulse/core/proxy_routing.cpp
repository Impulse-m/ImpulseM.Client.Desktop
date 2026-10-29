#include "mtproto/impulse/core/proxy_routing.h"

namespace Impulse::ProxyRouting {
namespace {

[[nodiscard]] HttpEndpoint Blackhole() {
	HttpEndpoint endpoint;
	endpoint.host = std::string(kLoopbackHost);
	endpoint.port = kBlackholePort;
	return endpoint;
}

} // namespace

Route Decide(
		bool enabled,
		bool useForTransport,
		bool running) {
	if (!enabled || !useForTransport) {
		return Route::Direct;
	}
	return running ? Route::Proxy : Route::Blocked;
}

bool TunnelCalls(
		bool enabled,
		bool useForCalls) {
	return enabled && useForCalls;
}

std::optional<HttpEndpoint> SelectEndpoint(
		Route route,
		const std::optional<Runtime> &runtime) {
	if (route == Route::Direct) {
		return std::nullopt;
	}
	if (route == Route::Proxy && runtime.has_value()) {
		HttpEndpoint endpoint;
		endpoint.host = std::string(kLoopbackHost);
		endpoint.port = runtime->httpPort;
		endpoint.hasCredentials = true;
		endpoint.user = runtime->user;
		endpoint.password = runtime->password;
		return endpoint;
	}
	return Blackhole();
}

} // namespace Impulse::ProxyRouting
