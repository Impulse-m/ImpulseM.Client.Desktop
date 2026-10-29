#include "mtproto/impulse/tests/impulse_test.h"

#include "core/impulse_proxy/xray_runtime.h"
#include "mtproto/impulse/core/json.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Impulse::Xray::Error;
using Impulse::Xray::Runtime;
using Impulse::Xray::Status;

class FakeCore final {
public:
	std::string invoke(
			const std::string &method,
			const std::string &payloadJson) {
		methods.push_back(method);
		payloads[method] = payloadJson;
		const std::map<std::string, std::string>::iterator found
			= replies.find(method);
		return found == replies.end() ? std::string() : found->second;
	}

	void reply(
			const std::string &method,
			const std::string &response) {
		replies[method] = response;
	}

	[[nodiscard]] bool called(const std::string &method) const {
		return std::find(methods.begin(), methods.end(), method)
			!= methods.end();
	}

	std::vector<std::string> methods;
	std::map<std::string, std::string> payloads;
	std::map<std::string, std::string> replies;
};

void InstallHealthy(FakeCore &core) {
	core.reply("getXrayState", "{\"success\":true,\"data\":{\"running\":false}}");
	core.reply("getFreePorts", "{\"success\":true,\"data\":{\"ports\":[10808,10809]}}");
	core.reply("testXray", "{\"success\":true,\"data\":{}}");
	core.reply("runXray", "{\"success\":true,\"data\":{}}");
	core.reply("stopXray", "{\"success\":true,\"data\":{}}");
}

Impulse::ProxyState::State MakeState(bool enabled) {
	Impulse::ProxyState::State state;
	state.enabled = enabled;
	Impulse::ProxyState::Server server;
	server.id = "node1";
	server.name = "Node 1";
	server.host = "node.example.com";
	server.port = 443;
	server.network = "tcp";
	server.security = "none";
	server.outbound = "{\"protocol\":\"vless\",\"settings\":{\"vnext\":[]}}";
	state.manual.push_back(server);
	state.selectedId = std::string("node1");
	return state;
}

bool IsHex(const std::string &text, std::size_t length) {
	if (text.size() != length) {
		return false;
	}
	for (const char symbol : text) {
		const bool digit = symbol >= '0' && symbol <= '9';
		const bool letter = symbol >= 'a' && symbol <= 'f';
		if (!digit && !letter) {
			return false;
		}
	}
	return true;
}

Impulse::Json::Value ParseText(const std::string &text) {
	return Impulse::Json::parse(text).value.value_or(Impulse::Json::Value());
}

// The run config, which is embedded as a string in the {"xrayJson":...}
// payload of runXray.
Impulse::Json::Value ConfigOf(const std::string &payload) {
	const Impulse::Json::Value envelope = ParseText(payload);
	const Impulse::Json::Value *xray = envelope.find("xrayJson");
	const std::string *text = xray ? xray->asString() : nullptr;
	return text ? ParseText(*text) : Impulse::Json::Value();
}

std::string TextMember(const Impulse::Json::Value *node, std::string_view key) {
	if (!node) {
		return std::string();
	}
	const Impulse::Json::Value *member = node->find(key);
	const std::string *text = member ? member->asString() : nullptr;
	return text ? *text : std::string();
}

std::int64_t IntMember(const Impulse::Json::Value *node, std::string_view key) {
	if (!node) {
		return -1;
	}
	const Impulse::Json::Value *member = node->find(key);
	const std::optional<std::int64_t> number = member ? member->asInt() : std::nullopt;
	return number ? *number : -1;
}

const std::vector<Impulse::Json::Value> *ArrayMember(
		const Impulse::Json::Value *node,
		std::string_view key) {
	if (!node) {
		return nullptr;
	}
	const Impulse::Json::Value *member = node->find(key);
	return member ? member->asArray() : nullptr;
}

} // namespace

IMPULSE_TEST(XrayRuntimeErrorCodesAreAndroidTexts) {
	IMPULSE_CHECK(Impulse::Xray::ErrorCode(Error::NoServer) == "err:noServer");
	IMPULSE_CHECK(Impulse::Xray::ErrorCode(Error::InvalidConfig) == "err:invalidConfig");
	IMPULSE_CHECK(Impulse::Xray::ErrorCode(Error::CoreStart) == "err:coreStart");
}

IMPULSE_TEST(XrayRuntimeDisabledStaysOff) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(false));
	IMPULSE_CHECK(runtime.status() == Status::Off);
	IMPULSE_CHECK(!runtime.endpoints().has_value());
	IMPULSE_CHECK(!core.called("getFreePorts"));
	IMPULSE_CHECK(!core.called("runXray"));
}

IMPULSE_TEST(XrayRuntimeNoSelectedServerFailsWithNoServer) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	Impulse::ProxyState::State state = MakeState(true);
	state.selectedId = std::string("missing");
	runtime.restart(state);
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(runtime.error() == Error::NoServer);
	IMPULSE_CHECK(!runtime.endpoints().has_value());
	IMPULSE_CHECK(!core.called("getFreePorts"));
}

IMPULSE_TEST(XrayRuntimeStartsSelectedServer) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	IMPULSE_CHECK(runtime.status() == Status::Running);
	IMPULSE_CHECK(runtime.error() == Error::None);
	const std::optional<Impulse::Xray::Endpoints> endpoints = runtime.endpoints();
	IMPULSE_CHECK(endpoints.has_value());
	if (endpoints) {
		IMPULSE_CHECK(endpoints->httpPort == 10808);
		IMPULSE_CHECK(endpoints->socksPort == 10809);
		IMPULSE_CHECK(IsHex(endpoints->user, 32));
		IMPULSE_CHECK(IsHex(endpoints->password, 48));
	}
	const std::vector<std::string> expected = {
		"getXrayState",
		"getFreePorts",
		"testXray",
		"runXray",
	};
	IMPULSE_CHECK(core.methods == expected);
}

IMPULSE_TEST(XrayRuntimeRunningCoreIsStoppedFirst) {
	FakeCore core;
	InstallHealthy(core);
	core.reply("getXrayState", "{\"success\":true,\"data\":{\"running\":true}}");
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	const std::vector<std::string> expected = {
		"getXrayState",
		"stopXray",
		"getFreePorts",
		"testXray",
		"runXray",
	};
	IMPULSE_CHECK(core.methods == expected);
	IMPULSE_CHECK(runtime.status() == Status::Running);
}

IMPULSE_TEST(XrayRuntimeRunConfigIsLoopbackAndQuiet) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	const std::optional<Impulse::Xray::Endpoints> endpoints = runtime.endpoints();
	IMPULSE_CHECK(endpoints.has_value());
	if (!endpoints) {
		return;
	}
	const Impulse::Json::Value config = ConfigOf(core.payloads["runXray"]);

	const Impulse::Json::Value *log = config.find("log");
	IMPULSE_CHECK(TextMember(log, "loglevel") == "error");
	IMPULSE_CHECK(TextMember(log, "access") == "none");

	const std::vector<Impulse::Json::Value> *inbounds = ArrayMember(&config, "inbounds");
	IMPULSE_CHECK(inbounds != nullptr);
	if (!inbounds || inbounds->size() != 2) {
		return;
	}
	const Impulse::Json::Value &http = (*inbounds)[0];
	const Impulse::Json::Value &socks = (*inbounds)[1];
	IMPULSE_CHECK(TextMember(&http, "tag") == "http-in");
	IMPULSE_CHECK(TextMember(&http, "listen") == "127.0.0.1");
	IMPULSE_CHECK(TextMember(&http, "protocol") == "http");
	IMPULSE_CHECK(IntMember(&http, "port") == endpoints->httpPort);
	IMPULSE_CHECK(TextMember(&socks, "tag") == "socks-in");
	IMPULSE_CHECK(TextMember(&socks, "listen") == "127.0.0.1");
	IMPULSE_CHECK(TextMember(&socks, "protocol") == "socks");
	IMPULSE_CHECK(IntMember(&socks, "port") == endpoints->socksPort);

	const Impulse::Json::Value *httpSettings = http.find("settings");
	const std::vector<Impulse::Json::Value> *httpAccounts = ArrayMember(httpSettings, "accounts");
	IMPULSE_CHECK(httpAccounts != nullptr);
	if (httpAccounts && httpAccounts->size() == 1) {
		IMPULSE_CHECK(TextMember(&(*httpAccounts)[0], "user") == endpoints->user);
		IMPULSE_CHECK(TextMember(&(*httpAccounts)[0], "pass") == endpoints->password);
	}

	const std::vector<Impulse::Json::Value> *outbounds = ArrayMember(&config, "outbounds");
	IMPULSE_CHECK(outbounds != nullptr);
	if (outbounds && outbounds->size() == 1) {
		IMPULSE_CHECK(TextMember(&(*outbounds)[0], "tag") == "proxy");
		IMPULSE_CHECK(TextMember(&(*outbounds)[0], "protocol") == "vless");
	}

	const Impulse::Json::Value *routing = config.find("routing");
	const std::vector<Impulse::Json::Value> *rules = ArrayMember(routing, "rules");
	IMPULSE_CHECK(rules != nullptr);
	if (rules && rules->size() == 1) {
		IMPULSE_CHECK(TextMember(&(*rules)[0], "outboundTag") == "proxy");
	}
}

IMPULSE_TEST(XrayRuntimeTestFailureGivesInvalidConfig) {
	FakeCore core;
	InstallHealthy(core);
	core.reply("testXray", "{\"success\":false,\"error\":\"bad config\"}");
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(runtime.error() == Error::InvalidConfig);
	IMPULSE_CHECK(!runtime.endpoints().has_value());
	IMPULSE_CHECK(!core.called("runXray"));
}

IMPULSE_TEST(XrayRuntimeRunFailureGivesCoreStart) {
	FakeCore core;
	InstallHealthy(core);
	core.reply("runXray", "{\"success\":false,\"error\":\"start failed\"}");
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(runtime.error() == Error::CoreStart);
	IMPULSE_CHECK(!runtime.endpoints().has_value());
}

IMPULSE_TEST(XrayRuntimeShortPortListGivesCoreStart) {
	FakeCore core;
	InstallHealthy(core);
	core.reply("getFreePorts", "{\"success\":true,\"data\":{\"ports\":[10808]}}");
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(runtime.error() == Error::CoreStart);
	IMPULSE_CHECK(!core.called("testXray"));
}

IMPULSE_TEST(XrayRuntimeNonObjectOutboundGivesInvalidConfig) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	Impulse::ProxyState::State state = MakeState(true);
	state.manual[0].outbound = "not json";
	runtime.restart(state);
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(runtime.error() == Error::InvalidConfig);
	IMPULSE_CHECK(!core.called("testXray"));
}

IMPULSE_TEST(XrayRuntimeRestartClearsEndpointsBeforeStart) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	IMPULSE_CHECK(runtime.status() == Status::Running);

	core.reply("testXray", "{\"success\":false,\"error\":\"bad config\"}");
	runtime.restart(MakeState(true));
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(!runtime.endpoints().has_value());
}
