#include "mtproto/impulse/tests/impulse_test.h"
#include "mtproto/impulse/core/endpoints.h"

#include <string>

IMPULSE_TEST(EndpointsFixedUrls) {
	IMPULSE_CHECK(Impulse::Endpoints::kRpcBase == "https://rpc.impulsem.io");
	IMPULSE_CHECK(Impulse::Endpoints::kUpdateUrl
		== "wss://rpc.impulsem.io/connection/websocket?cf_ws_frame_ping_pong=true");
	IMPULSE_CHECK(Impulse::Endpoints::kPoolUrl
		== "wss://rpc.impulsem.io/pool/connection/websocket"
		"?cf_protocol=protobuf&cf_ws_frame_ping_pong=true");
}

IMPULSE_TEST(EndpointsMediaUrlHostname) {
	const std::optional<std::string> url = Impulse::Endpoints::MediaUrl(
		"media.impulsem.io",
		443);
	IMPULSE_CHECK(url.has_value());
	IMPULSE_CHECK(*url
		== "wss://media.impulsem.io:443/connection/websocket"
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
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("media.impulsem.io", 0).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("media.impulsem.io", 65536).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("evil.example/path", 443).has_value());
	IMPULSE_CHECK(!Impulse::Endpoints::MediaUrl("host@evil.example", 443).has_value());
}
