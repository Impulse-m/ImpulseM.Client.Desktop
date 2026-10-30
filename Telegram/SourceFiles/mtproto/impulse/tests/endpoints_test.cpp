#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/endpoints.h"

#include <string>

IMPULSE_TEST(EndpointsDerivedFromRpcBase) {
	IMPULSE_CHECK(Impulse::Endpoints::UpdateUrl("https://rpc.example.org")
		== "wss://rpc.example.org/connection/websocket?cf_ws_frame_ping_pong=true");
	IMPULSE_CHECK(Impulse::Endpoints::PoolUrl("https://rpc.example.org")
		== "wss://rpc.example.org/pool/connection/websocket"
		"?cf_protocol=protobuf&cf_ws_frame_ping_pong=true");
	IMPULSE_CHECK(Impulse::Endpoints::UpdateUrl("https://rpc.example.org/")
		== "wss://rpc.example.org/connection/websocket?cf_ws_frame_ping_pong=true");
	IMPULSE_CHECK(Impulse::Endpoints::UpdateUrl("http://127.0.0.1:8080")
		== "ws://127.0.0.1:8080/connection/websocket?cf_ws_frame_ping_pong=true");
}

IMPULSE_TEST(EndpointsEmptyRpcBaseGivesNoEndpoint) {
	IMPULSE_CHECK(Impulse::Endpoints::UpdateUrl("").empty());
	IMPULSE_CHECK(Impulse::Endpoints::PoolUrl("").empty());
	IMPULSE_CHECK(Impulse::Endpoints::UpdateUrl("rpc.example.org").empty());
	IMPULSE_CHECK(Impulse::Endpoints::PoolUrl("https://").empty());
}

IMPULSE_TEST(EndpointsMediaUrlHostname) {
	const std::optional<std::string> url = Impulse::Endpoints::MediaUrl(
		"media.example.org",
		443);
	IMPULSE_CHECK(url.has_value());
	IMPULSE_CHECK(*url
		== "wss://media.example.org:443/connection/websocket"
		"?cf_protocol=protobuf&cf_ws_frame_ping_pong=true");
}

IMPULSE_TEST(EndpointsMediaUrlIpv4) {
	const std::optional<std::string> url = Impulse::Endpoints::MediaUrl(
		"149.154.167.51",
		8443);
	IMPULSE_CHECK(url.has_value());
	IMPULSE_CHECK(*url
		== "wss://149.154.167.51:8443/connection/websocket"
		"?cf_protocol=protobuf&cf_ws_frame_ping_pong=true");
}

IMPULSE_TEST(EndpointsMediaUrlIpv6IsBracketed) {
	const std::optional<std::string> url = Impulse::Endpoints::MediaUrl(
		"2001:db8::1",
		443);
	IMPULSE_CHECK(url.has_value());
	IMPULSE_CHECK(*url
		== "wss://[2001:db8::1]:443/connection/websocket"
		"?cf_protocol=protobuf&cf_ws_frame_ping_pong=true");
}

IMPULSE_TEST(EndpointsMediaUrlAlreadyBracketedIpv6NotDoubled) {
	const std::optional<std::string> url = Impulse::Endpoints::MediaUrl(
		"[2001:db8::1]",
		8443);
	IMPULSE_CHECK(url.has_value());
	IMPULSE_CHECK(*url
		== "wss://[2001:db8::1]:8443/connection/websocket"
		"?cf_protocol=protobuf&cf_ws_frame_ping_pong=true");
}

IMPULSE_TEST(EndpointsMediaUrlRejectsBadInput) {
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("", 443).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("[]", 443).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("media.example.org", 0).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("media.example.org", 65536).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("evil.example/path", 443).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("host@evil.example", 443).has_value());
}
