#include "mtproto/impulse/tests/impulse_test.h"

#include "core/impulse_proxy/proxy_links.h"
#include "mtproto/impulse/core/json.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Impulse::Xray::ConvertLinks;
using Impulse::Xray::ConvertShareLines;
using Impulse::Xray::Invoker;
using Impulse::Xray::LinkConversion;
using Impulse::Xray::ServerFromOutbound;
using Impulse::Xray::SubscriptionBodyError;

constexpr std::string_view kVlessOutbound
	= "{\"protocol\":\"vless\",\"tag\":\" Main \",\"settings\":{\"vnext\":"
	"[{\"address\":\"h.example\",\"port\":443,\"users\":[{\"id\":\"u\"}]}]},"
	"\"streamSettings\":{\"network\":\"ws\",\"security\":\"tls\"}}";

class FakeLibrary final {
public:
	explicit FakeLibrary(std::string replyText)
	: reply(std::move(replyText)) {
	}

	[[nodiscard]] Invoker invoker() {
		return [this](
				const std::string &method,
				const std::string &payload) -> std::string {
			methods.push_back(method);
			payloads.push_back(payload);
			return reply;
		};
	}

	std::vector<std::string> methods;
	std::vector<std::string> payloads;
	std::string reply;
};

[[nodiscard]] bool ErrorIs(
		const std::optional<std::string> &value,
		std::string_view expected) {
	return value.has_value() && *value == expected;
}

[[nodiscard]] bool IsLowerHex(std::string_view text) {
	for (const char symbol : text) {
		const bool digit = symbol >= '0' && symbol <= '9';
		const bool letter = symbol >= 'a' && symbol <= 'f';
		if (!digit && !letter) {
			return false;
		}
	}
	return true;
}

} // namespace

IMPULSE_TEST(ProxyLinksConvertReadsVlessOutbound) {
	FakeLibrary library(
		"{\"success\":true,\"data\":{\"outbounds\":["
		+ std::string(kVlessOutbound)
		+ "]}}");
	const LinkConversion result = ConvertLinks(
		library.invoker(),
		"vless://u@h.example:443");

	IMPULSE_CHECK(result.failure.empty());
	IMPULSE_CHECK(result.skipped == 0);
	IMPULSE_CHECK(result.servers.size() == 1);
	IMPULSE_CHECK(library.methods.size() == 1);
	IMPULSE_CHECK(library.methods[0] == "convertShareLinksToXrayJson");
	IMPULSE_CHECK(library.payloads[0]
		== "{\"text\":\"vless://u@h.example:443\"}");
	if (result.servers.size() == 1) {
		const Impulse::ProxyState::Server &server = result.servers[0];
		IMPULSE_CHECK(server.id.size() == 24);
		IMPULSE_CHECK(IsLowerHex(server.id));
		IMPULSE_CHECK(server.name == "Main");
		IMPULSE_CHECK(server.host == "h.example");
		IMPULSE_CHECK(server.port == 443);
		IMPULSE_CHECK(server.network == "ws");
		IMPULSE_CHECK(server.security == "tls");
		IMPULSE_CHECK(server.outbound == kVlessOutbound);
		IMPULSE_CHECK(!server.shareLink.has_value());
	}
}

IMPULSE_TEST(ProxyLinksConvertEscapesPayloadText) {
	FakeLibrary library("{\"success\":true,\"data\":{\"outbounds\":[]}}");
	const LinkConversion result = ConvertLinks(
		library.invoker(),
		"a\"b\\c\n");

	IMPULSE_CHECK(result.servers.empty());
	IMPULSE_CHECK(library.payloads.size() == 1);
	IMPULSE_CHECK(library.payloads[0] == "{\"text\":\"a\\\"b\\\\c\\u000a\"}");
}

IMPULSE_TEST(ProxyLinksServerDefaultsAndNameFallback) {
	const Impulse::Json::ParseResult parsed = Impulse::Json::parse(
		"{\"protocol\":\"vless\",\"settings\":{\"address\":\"edge\","
		"\"port\":8443}}");
	IMPULSE_CHECK(parsed.value.has_value());
	const std::optional<Impulse::ProxyState::Server> server
		= parsed.value ? ServerFromOutbound(*parsed.value) : std::nullopt;
	IMPULSE_CHECK(server.has_value());
	if (server) {
		IMPULSE_CHECK(server->name == "edge");
		IMPULSE_CHECK(server->host == "edge");
		IMPULSE_CHECK(server->port == 8443);
		IMPULSE_CHECK(server->network == "tcp");
		IMPULSE_CHECK(server->security == "none");
	}
}

IMPULSE_TEST(ProxyLinksSkipsOtherProtocolsAndBrokenEndpoints) {
	FakeLibrary library(
		"{\"success\":true,\"data\":{\"outbounds\":["
		"{\"protocol\":\"trojan\",\"settings\":{\"address\":\"t\",\"port\":1}},"
		"{\"protocol\":\"vless\",\"settings\":{\"vnext\":[{\"address\":\"\","
		"\"port\":443}]}},"
		"{\"protocol\":\"vless\",\"settings\":{\"vnext\":[{\"address\":\"h\","
		"\"port\":0}]}},"
		"{\"protocol\":\"vless\",\"settings\":{\"address\":\"h\",\"port\":1}}"
		"]}}");
	const LinkConversion result = ConvertLinks(
		library.invoker(),
		"text");

	IMPULSE_CHECK(result.failure.empty());
	IMPULSE_CHECK(result.skipped == 3);
	IMPULSE_CHECK(result.servers.size() == 1);
}

IMPULSE_TEST(ProxyLinksMalformedAndRefusedCallsKeepTheFailure) {
	FakeLibrary empty("");
	const LinkConversion malformed = ConvertLinks(empty.invoker(), "x");
	IMPULSE_CHECK(malformed.failure
		== "convertShareLinksToXrayJson: malformed response");
	IMPULSE_CHECK(malformed.servers.empty());

	FakeLibrary refused(
		"{\"success\":false,\"error\":\"no valid outbound found\"}");
	const LinkConversion failed = ConvertLinks(refused.invoker(), "junk");
	IMPULSE_CHECK(failed.failure
		== "convertShareLinksToXrayJson: no valid outbound found");
}

IMPULSE_TEST(ProxyLinksSubscriptionErrorMapping) {
	LinkConversion noOutbound;
	noOutbound.failure
		= "convertShareLinksToXrayJson: no valid outbound found";
	IMPULSE_CHECK(ErrorIs(SubscriptionBodyError(noOutbound), "err:noLinks"));

	LinkConversion broken;
	broken.failure = "convertShareLinksToXrayJson: malformed response";
	IMPULSE_CHECK(ErrorIs(
		SubscriptionBodyError(broken),
		"err:invalidSubscription"));

	LinkConversion skippedOnly;
	skippedOnly.skipped = 2;
	IMPULSE_CHECK(ErrorIs(SubscriptionBodyError(skippedOnly), "err:noLinks:2"));

	LinkConversion none;
	IMPULSE_CHECK(ErrorIs(SubscriptionBodyError(none), "err:noLinks"));

	LinkConversion usable;
	usable.servers.push_back(Impulse::ProxyState::Server());
	IMPULSE_CHECK(!SubscriptionBodyError(usable).has_value());
}

IMPULSE_TEST(ProxyLinksShareLinesConvertOnlyVlessLines) {
	FakeLibrary library(
		"{\"success\":true,\"data\":{\"outbounds\":["
		"{\"protocol\":\"vless\",\"settings\":{\"address\":\"a\",\"port\":1}}"
		"]}}");
	const std::vector<Impulse::ProxyState::Server> servers
		= ConvertShareLines(
			library.invoker(),
			"junk\r\n  VLESS://u@a:1#x \nhttps://skip.example\nvless://u@b:2\n");

	IMPULSE_CHECK(library.payloads.size() == 2);
	IMPULSE_CHECK(library.payloads.size() == 2
		&& library.payloads[0] == "{\"text\":\"VLESS://u@a:1#x\"}");
	IMPULSE_CHECK(library.payloads.size() == 2
		&& library.payloads[1] == "{\"text\":\"vless://u@b:2\"}");
	IMPULSE_CHECK(servers.size() == 2);
	if (servers.size() == 2) {
		IMPULSE_CHECK(servers[0].shareLink == "VLESS://u@a:1#x");
		IMPULSE_CHECK(servers[1].shareLink == "vless://u@b:2");
	}
}

IMPULSE_TEST(ProxyLinksShareLinesDropFailedLines) {
	FakeLibrary library(
		"{\"success\":false,\"error\":\"no valid outbound found\"}");
	const std::vector<Impulse::ProxyState::Server> servers
		= ConvertShareLines(library.invoker(), "vless://broken");

	IMPULSE_CHECK(library.payloads.size() == 1);
	IMPULSE_CHECK(servers.empty());
}
