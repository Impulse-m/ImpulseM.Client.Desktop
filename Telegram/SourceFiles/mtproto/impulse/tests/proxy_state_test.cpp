#include "mtproto/impulse/tests/impulse_test.h"

#include "mtproto/impulse/core/proxy_state.h"

#include <cstdint>
#include <string>

namespace {

[[nodiscard]] Impulse::ProxyState::Server MakeServer(
		const std::string &id,
		const std::string &security) {
	Impulse::ProxyState::Server server;
	server.id = id;
	server.name = "Node " + id;
	server.host = "node.example.com";
	server.port = 443;
	server.network = "ws";
	server.security = security;
	server.outbound = "{\"tag\":\"" + id + "\"}";
	return server;
}

} // namespace

IMPULSE_TEST(ProxyStateDecodeNonObjectGivesDefaults) {
	const Impulse::ProxyState::State defaults;
	IMPULSE_CHECK(Impulse::ProxyState::Decode("[]") == defaults);
	IMPULSE_CHECK(Impulse::ProxyState::Decode("\"text\"") == defaults);
	IMPULSE_CHECK(Impulse::ProxyState::Decode("{") == defaults);
	IMPULSE_CHECK(Impulse::ProxyState::Decode("") == defaults);
}

IMPULSE_TEST(ProxyStateDecodeEmptyObjectDefaults) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode("{}");
	IMPULSE_CHECK(!state.enabled);
	IMPULSE_CHECK(state.useForTransport);
	IMPULSE_CHECK(!state.useForCalls);
	IMPULSE_CHECK(!state.selectedId.has_value());
	IMPULSE_CHECK(state.manual.empty());
	IMPULSE_CHECK(state.subscriptions.empty());
}

IMPULSE_TEST(ProxyStateNewStateUseForCallsDefaultsToTrue) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::State();
	IMPULSE_CHECK(state.useForTransport);
	IMPULSE_CHECK(state.useForCalls);
	IMPULSE_CHECK(Impulse::ProxyState::Decode(Impulse::ProxyState::Encode(state)).useForCalls);
}

IMPULSE_TEST(ProxyStateMissingUseForCallsReadsFalse) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(R"({"v":1,"useForTransport":true})");
	IMPULSE_CHECK(state.useForTransport);
	IMPULSE_CHECK(!state.useForCalls);
}

IMPULSE_TEST(ProxyStateMissingUseForCallsWithBothOffIsRepairedToTransport) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(R"({"v":1,"useForTransport":false})");
	IMPULSE_CHECK(state.useForTransport);
	IMPULSE_CHECK(!state.useForCalls);
}

IMPULSE_TEST(ProxyStateExplicitUseForCallsFalseIsKept) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(R"({"v":1,"useForCalls":false})");
	IMPULSE_CHECK(state.useForTransport);
	IMPULSE_CHECK(!state.useForCalls);
}

IMPULSE_TEST(ProxyStateBothTogglesOffIsRepairedOnDecode) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(R"({"useForTransport":false,"useForCalls":false})");
	IMPULSE_CHECK(state.useForTransport);
	IMPULSE_CHECK(!state.useForCalls);
}

IMPULSE_TEST(ProxyStateBothTogglesOffIsRepairedOnEncode) {
	Impulse::ProxyState::State state;
	state.useForTransport = false;
	state.useForCalls = false;
	const Impulse::ProxyState::State restored = Impulse::ProxyState::Decode(Impulse::ProxyState::Encode(state));
	IMPULSE_CHECK(restored.useForTransport);
	IMPULSE_CHECK(!restored.useForCalls);
}

IMPULSE_TEST(ProxyStateEncodeStartsWithVersion) {
	const std::string text = Impulse::ProxyState::Encode(Impulse::ProxyState::State());
	IMPULSE_CHECK(text.rfind("{\"v\":1,", 0) == 0);
}

IMPULSE_TEST(ProxyStateRoundTripKeepsEverything) {
	Impulse::ProxyState::State state;
	state.enabled = true;
	state.useForTransport = true;
	state.useForCalls = false;
	state.selectedId = std::string("abc123");

	Impulse::ProxyState::Server manual = MakeServer("m1", "tls");
	manual.shareLink = std::string("vless://uuid@node.example.com:443?type=ws#Node");
	state.manual.push_back(manual);

	Impulse::ProxyState::Subscription subscription;
	subscription.id = "0123456789abcdef01234567";
	subscription.url = "https://sub.example.com/a?token=1\"quoted\"";
	subscription.meta.title = std::string("Panel \"main\"\n\xD0\x9F\xD1\x80\xD0\xB8");
	subscription.meta.upload = 12;
	subscription.meta.download = 34;
	subscription.meta.total = 1000;
	subscription.meta.expire = 1700000000;
	subscription.meta.updateIntervalHours = 6;
	subscription.updatedAt = 9007199254740993LL;
	subscription.lastError = std::string("err:http:503");
	subscription.skipped = 2;
	subscription.servers.push_back(MakeServer("s1", "reality"));
	state.subscriptions.push_back(subscription);

	state.advanced.fragmentMode = "classic";
	state.advanced.fingerprint = "chrome";
	state.advanced.muxEnabled = true;
	state.advanced.muxConcurrency = 16;
	state.advanced.tcpMaxSeg = 1200;
	state.advanced.dnsMode = "custom";
	state.advanced.dnsCustom = "1.1.1.1";
	state.advanced.pingUrl = "https://example.com/ping";
	state.advanced.pingSkipWhileConnected = false;

	const Impulse::ProxyState::State restored = Impulse::ProxyState::Decode(Impulse::ProxyState::Encode(state));
	IMPULSE_CHECK(restored == state);
}

IMPULSE_TEST(ProxyStateSelectedIdAbsentStaysAbsent) {
	Impulse::ProxyState::State state;
	state.selectedId = std::string("x");
	IMPULSE_CHECK(Impulse::ProxyState::Decode(Impulse::ProxyState::Encode(state)).selectedId == std::string("x"));
	IMPULSE_CHECK(!Impulse::ProxyState::Decode(Impulse::ProxyState::Encode(Impulse::ProxyState::State())).selectedId.has_value());
}

IMPULSE_TEST(ProxyStateServerSecurityIsReadAsStored) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(
		R"({"manual":[)"
		R"({"id":"a","name":"A","host":"h","port":1,"network":"ws","security":"xtls","outbound":"{}"},)"
		R"({"id":"b","name":"B","host":"h","port":2,"network":"ws","security":"tls","outbound":"{}"}]})");
	IMPULSE_CHECK(state.manual.size() == 2);
	if (state.manual.size() != 2) {
		return;
	}
	IMPULSE_CHECK(state.manual[0].security == "xtls");
	IMPULSE_CHECK(state.manual[1].security == "tls");
}

IMPULSE_TEST(ProxyStateMalformedServerIsSkipped) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(
		R"({"manual":[)"
		R"({"id":"a","name":"A","host":"h","network":"ws","security":"none","outbound":"{}"},)"
		R"("not an object",)"
		R"({"id":"b","name":"B","host":"h","port":2,"network":"ws","security":"none","outbound":"{}"}]})");
	IMPULSE_CHECK(state.manual.size() == 1);
	if (state.manual.size() != 1) {
		return;
	}
	IMPULSE_CHECK(state.manual[0].id == "b");
}

IMPULSE_TEST(ProxyStateSubscriptionWithoutUrlIsSkipped) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(
		R"({"subscriptions":[{"id":"x"},{"id":"y","url":"https://e.test/s"}]})");
	IMPULSE_CHECK(state.subscriptions.size() == 1);
	if (state.subscriptions.size() != 1) {
		return;
	}
	IMPULSE_CHECK(state.subscriptions[0].id == "y");
	IMPULSE_CHECK(state.subscriptions[0].meta.upload == -1);
	IMPULSE_CHECK(state.subscriptions[0].meta.updateIntervalHours == 24);
}

IMPULSE_TEST(ProxyStateMistypedFieldKeepsDefault) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(R"({"enabled":"yes","useForCalls":1})");
	IMPULSE_CHECK(!state.enabled);
	IMPULSE_CHECK(!state.useForCalls);
}

IMPULSE_TEST(ProxyStateStringTrueReadsAsTrueForEnabledAndUseForCalls) {
	const Impulse::ProxyState::State lower = Impulse::ProxyState::Decode(R"({"enabled":"true","useForCalls":"true"})");
	IMPULSE_CHECK(lower.enabled);
	IMPULSE_CHECK(lower.useForCalls);

	const Impulse::ProxyState::State mixed = Impulse::ProxyState::Decode(R"({"enabled":"TrUe","useForCalls":"TRUE"})");
	IMPULSE_CHECK(mixed.enabled);
	IMPULSE_CHECK(mixed.useForCalls);
}

IMPULSE_TEST(ProxyStateOtherStringsReadAsFalseForEnabledAndUseForCalls) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(
		R"({"enabled":"false","useForCalls":"false","useForTransport":true})");
	IMPULSE_CHECK(!state.enabled);
	IMPULSE_CHECK(!state.useForCalls);
	IMPULSE_CHECK(state.useForTransport);

	const Impulse::ProxyState::State other = Impulse::ProxyState::Decode(R"({"enabled":"tru","useForCalls":"truee"})");
	IMPULSE_CHECK(!other.enabled);
	IMPULSE_CHECK(!other.useForCalls);
}

IMPULSE_TEST(ProxyStateNumericFlagsReadAsFalseForEnabledAndUseForCalls) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(R"({"enabled":1,"useForCalls":0})");
	IMPULSE_CHECK(!state.enabled);
	IMPULSE_CHECK(!state.useForCalls);
}

IMPULSE_TEST(ProxyStateStringFlagKeepsFallbackForOtherBooleans) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(
		R"({"useForTransport":"false","advanced":{"muxEnabled":"true","tcpFastOpen":"false"}})");
	IMPULSE_CHECK(state.useForTransport);
	IMPULSE_CHECK(!state.advanced.muxEnabled);
	IMPULSE_CHECK(state.advanced.tcpFastOpen == Impulse::ProxyState::Advanced().tcpFastOpen);
}

IMPULSE_TEST(ProxyStateAdvancedIsSanitized) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(
		R"({"advanced":{"fragmentMode":"bogus","muxConcurrency":0,)"
		R"("dnsMode":"custom","dnsCustom":"","pingUrl":"http://x",)"
		R"("pingTimeoutSeconds":99,"tcpMaxSeg":10,"fingerprint":"nope"}})");
	const Impulse::ProxyState::Advanced &advanced = state.advanced;
	IMPULSE_CHECK(advanced.fragmentMode == "off");
	IMPULSE_CHECK(advanced.muxConcurrency == 8);
	IMPULSE_CHECK(advanced.dnsMode == "system");
	IMPULSE_CHECK(advanced.pingUrl == "https://www.gstatic.com/generate_204");
	IMPULSE_CHECK(advanced.pingTimeoutSeconds == 5);
	IMPULSE_CHECK(advanced.tcpMaxSeg == 0);
	IMPULSE_CHECK(advanced.fingerprint.empty());
}

IMPULSE_TEST(ProxyStateAdvancedDefaultsWhenMissing) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode("{}");
	IMPULSE_CHECK(state.advanced == Impulse::ProxyState::Advanced());
	IMPULSE_CHECK(state.advanced.fragmentFromLink);
	IMPULSE_CHECK(state.advanced.pingSkipWhileConnected);
}

IMPULSE_TEST(ProxyStateLargeIntegerIsExact) {
	const Impulse::ProxyState::State state = Impulse::ProxyState::Decode(
		R"({"subscriptions":[{"id":"x","url":"https://e.test/s","updatedAt":9007199254740993}]})");
	IMPULSE_CHECK(state.subscriptions.size() == 1);
	if (state.subscriptions.size() != 1) {
		return;
	}
	IMPULSE_CHECK(state.subscriptions[0].updatedAt == 9007199254740993LL);
}

IMPULSE_TEST(ProxyStateCoreKeyIgnoresPingSettings) {
	Impulse::ProxyState::Advanced first;
	Impulse::ProxyState::Advanced second = first;
	second.pingMode = "sequential";
	second.pingPauseMillis = 250;
	second.pingTimeoutSeconds = 9;
	second.pingUrl = "https://example.test/generate_204";
	second.pingSkipWhileConnected = false;
	IMPULSE_CHECK(Impulse::ProxyState::CoreKey(first) == Impulse::ProxyState::CoreKey(second));
	second.muxEnabled = true;
	IMPULSE_CHECK(Impulse::ProxyState::CoreKey(first) != Impulse::ProxyState::CoreKey(second));
}
