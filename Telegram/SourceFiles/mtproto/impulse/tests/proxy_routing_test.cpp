#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/proxy_routing.h"

#include <optional>

IMPULSE_TEST(ProxyRoutingDisabledIsDirect) {
	using Impulse::ProxyRouting::Decide;
	using Impulse::ProxyRouting::Route;
	IMPULSE_CHECK(Decide(false, true, true) == Route::Direct);
	IMPULSE_CHECK(Decide(false, true, false) == Route::Direct);
}

IMPULSE_TEST(ProxyRoutingTransportOffIsDirect) {
	using Impulse::ProxyRouting::Decide;
	using Impulse::ProxyRouting::Route;
	IMPULSE_CHECK(Decide(true, false, true) == Route::Direct);
	IMPULSE_CHECK(Decide(true, false, false) == Route::Direct);
}

IMPULSE_TEST(ProxyRoutingRunningIsProxy) {
	using Impulse::ProxyRouting::Decide;
	using Impulse::ProxyRouting::Route;
	IMPULSE_CHECK(Decide(true, true, true) == Route::Proxy);
}

IMPULSE_TEST(ProxyRoutingStoppedIsBlockedNeverDirect) {
	using Impulse::ProxyRouting::Decide;
	using Impulse::ProxyRouting::Route;
	IMPULSE_CHECK(Decide(true, true, false) == Route::Blocked);
}

IMPULSE_TEST(ProxyRoutingTunnelCallsNeedBothToggles) {
	using Impulse::ProxyRouting::TunnelCalls;
	IMPULSE_CHECK(TunnelCalls(true, true));
	IMPULSE_CHECK(!TunnelCalls(false, true));
	IMPULSE_CHECK(!TunnelCalls(true, false));
	IMPULSE_CHECK(!TunnelCalls(false, false));
}

IMPULSE_TEST(ProxyRoutingSelectDirectGivesNoProxy) {
	using Impulse::ProxyRouting::Route;
	using Impulse::ProxyRouting::Runtime;
	using Impulse::ProxyRouting::SelectEndpoint;
	const std::optional<Runtime> runtime = Runtime{ 10808, "user", "secret" };
	IMPULSE_CHECK(!SelectEndpoint(Route::Direct, runtime).has_value());
	IMPULSE_CHECK(!SelectEndpoint(Route::Direct, std::nullopt).has_value());
}

IMPULSE_TEST(ProxyRoutingSelectProxyGivesLoopbackWithCredentials) {
	using Impulse::ProxyRouting::Route;
	using Impulse::ProxyRouting::Runtime;
	using Impulse::ProxyRouting::SelectEndpoint;
	const std::optional<Runtime> runtime = Runtime{ 10808, "user", "secret" };
	const std::optional<Impulse::ProxyRouting::HttpEndpoint> endpoint = SelectEndpoint(Route::Proxy, runtime);
	IMPULSE_CHECK(endpoint.has_value());
	IMPULSE_CHECK(endpoint->host == "127.0.0.1");
	IMPULSE_CHECK(endpoint->port == 10808);
	IMPULSE_CHECK(endpoint->hasCredentials);
	IMPULSE_CHECK(endpoint->user == "user");
	IMPULSE_CHECK(endpoint->password == "secret");
}

IMPULSE_TEST(ProxyRoutingSelectProxyWithoutRuntimeIsBlackhole) {
	using Impulse::ProxyRouting::Route;
	using Impulse::ProxyRouting::SelectEndpoint;
	const std::optional<Impulse::ProxyRouting::HttpEndpoint> endpoint = SelectEndpoint(Route::Proxy, std::nullopt);
	IMPULSE_CHECK(endpoint.has_value());
	IMPULSE_CHECK(endpoint->host == "127.0.0.1");
	IMPULSE_CHECK(endpoint->port == 9);
	IMPULSE_CHECK(!endpoint->hasCredentials);
}

IMPULSE_TEST(ProxyRoutingSelectBlockedIsBlackhole) {
	using Impulse::ProxyRouting::Route;
	using Impulse::ProxyRouting::Runtime;
	using Impulse::ProxyRouting::SelectEndpoint;
	const std::optional<Runtime> runtime = Runtime{ 10808, "user", "secret" };
	const std::optional<Impulse::ProxyRouting::HttpEndpoint> endpoint = SelectEndpoint(Route::Blocked, runtime);
	IMPULSE_CHECK(endpoint.has_value());
	IMPULSE_CHECK(endpoint->host == "127.0.0.1");
	IMPULSE_CHECK(endpoint->port == 9);
	IMPULSE_CHECK(!endpoint->hasCredentials);
}
