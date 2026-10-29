#include "mtproto/impulse/tests/impulse_test.h"

#include "core/impulse_proxy/xray_runtime.h"
#include "mtproto/impulse/core/json.h"
#include "mtproto/impulse/core/proxy_routing.h"

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
using Impulse::Xray::HealthAction;
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

void SetCoreRunning(FakeCore &core, bool running) {
	core.reply(
		"getXrayState",
		running
			? "{\"success\":true,\"data\":{\"running\":true}}"
			: "{\"success\":true,\"data\":{\"running\":false}}");
}

int CountCalls(const FakeCore &core, const std::string &method) {
	return static_cast<int>(
		std::count(core.methods.begin(), core.methods.end(), method));
}

// Fake clock of the health loop, driven as the owner drives it. Arm schedules
// the next check kHealthCheckSeconds ahead for the current generation.
// Advance runs every check that falls due. After a Recheck or a Restart that
// leaves the core Running, the next check is armed again.
class HealthLoop final {
public:
	explicit HealthLoop(Runtime &runtime)
	: runtime(runtime) {
	}

	void arm() {
		checkedGeneration = runtime.generation();
		dueAt = now + Impulse::Xray::kHealthCheckSeconds;
	}

	void advance(int seconds) {
		const int target = now + seconds;
		while (dueAt && *dueAt <= target) {
			now = *dueAt;
			dueAt.reset();
			const HealthAction action = runtime.checkHealth(checkedGeneration);
			actions.push_back(action);
			const bool rearm = action == HealthAction::Recheck
				|| action == HealthAction::Restart;
			if (rearm && runtime.status() == Status::Running) {
				arm();
			}
		}
		now = target;
	}

	std::vector<HealthAction> actions;
	int now = 0;
	int checkedGeneration = 0;
	std::optional<int> dueAt;

private:
	Runtime &runtime;
};

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

IMPULSE_TEST(XrayRuntimeErrorCodeCoreStoppedIsAndroidText) {
	IMPULSE_CHECK(Impulse::Xray::ErrorCode(Error::CoreStopped) == "err:coreStopped");
}

IMPULSE_TEST(XrayHealthIntervalIsThirtySeconds) {
	IMPULSE_CHECK(Impulse::Xray::kHealthCheckSeconds == 30);
}

IMPULSE_TEST(XrayHealthPolicyTableMatchesAndroid) {
	using Impulse::Xray::DecideHealth;
	IMPULSE_CHECK(DecideHealth(false, false, false) == HealthAction::Stop);
	IMPULSE_CHECK(DecideHealth(false, true, true) == HealthAction::Stop);
	IMPULSE_CHECK(DecideHealth(true, false, true) == HealthAction::Recheck);
	IMPULSE_CHECK(DecideHealth(true, true, true) == HealthAction::Recheck);
	IMPULSE_CHECK(DecideHealth(true, false, false) == HealthAction::Restart);
	IMPULSE_CHECK(DecideHealth(true, true, false) == HealthAction::Fail);
}

IMPULSE_TEST(XrayHealthOffCoreStopsAtOnce) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(false));
	HealthLoop loop(runtime);
	loop.arm();
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	IMPULSE_CHECK(loop.actions == std::vector<HealthAction>{HealthAction::Stop});
	IMPULSE_CHECK(runtime.status() == Status::Off);
	IMPULSE_CHECK(!core.called("runXray"));
}

IMPULSE_TEST(XrayHealthChecksOnlyAtTheInterval) {
	FakeCore core;
	InstallHealthy(core);
	SetCoreRunning(core, true);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	HealthLoop loop(runtime);
	loop.arm();
	loop.advance(Impulse::Xray::kHealthCheckSeconds - 1);
	IMPULSE_CHECK(loop.actions.empty());
	loop.advance(1);
	IMPULSE_CHECK(loop.actions == std::vector<HealthAction>{HealthAction::Recheck});
}

IMPULSE_TEST(XrayHealthDeadCoreRestartsOnceThenFailsWithCoreStopped) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	IMPULSE_CHECK(runtime.status() == Status::Running);
	const int startsBefore = CountCalls(core, "runXray");

	HealthLoop loop(runtime);
	loop.arm();
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	IMPULSE_CHECK(loop.actions == std::vector<HealthAction>{HealthAction::Restart});
	IMPULSE_CHECK(runtime.status() == Status::Running);
	IMPULSE_CHECK(CountCalls(core, "runXray") == startsBefore + 1);

	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	IMPULSE_CHECK(loop.actions == (std::vector<HealthAction>{
		HealthAction::Restart,
		HealthAction::Fail,
	}));
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(runtime.error() == Error::CoreStopped);
	IMPULSE_CHECK(!runtime.endpoints().has_value());
	IMPULSE_CHECK(!loop.dueAt.has_value());
}

IMPULSE_TEST(XrayHealthHealthyCheckEarnsANewRetry) {
	FakeCore core;
	InstallHealthy(core);
	SetCoreRunning(core, true);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	HealthLoop loop(runtime);
	loop.arm();

	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	SetCoreRunning(core, false);
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	SetCoreRunning(core, true);
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	SetCoreRunning(core, false);
	loop.advance(Impulse::Xray::kHealthCheckSeconds);

	IMPULSE_CHECK(loop.actions == (std::vector<HealthAction>{
		HealthAction::Recheck,
		HealthAction::Restart,
		HealthAction::Recheck,
		HealthAction::Restart,
	}));
	IMPULSE_CHECK(runtime.status() == Status::Running);
}

IMPULSE_TEST(XrayHealthStaleGenerationStopsWithoutProbing) {
	FakeCore core;
	InstallHealthy(core);
	SetCoreRunning(core, true);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	HealthLoop loop(runtime);
	loop.arm();
	runtime.restart(MakeState(true));
	const int probesBefore = CountCalls(core, "getXrayState");

	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	IMPULSE_CHECK(loop.actions == std::vector<HealthAction>{HealthAction::Stop});
	IMPULSE_CHECK(CountCalls(core, "getXrayState") == probesBefore);
	IMPULSE_CHECK(runtime.status() == Status::Running);
}

IMPULSE_TEST(XrayHealthManualRestartClearsRetryAllowance) {
	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	HealthLoop loop(runtime);
	loop.arm();
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	IMPULSE_CHECK(loop.actions == std::vector<HealthAction>{HealthAction::Restart});

	runtime.restart(MakeState(true));
	loop.arm();
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	IMPULSE_CHECK(loop.actions == (std::vector<HealthAction>{
		HealthAction::Restart,
		HealthAction::Restart,
	}));
	IMPULSE_CHECK(runtime.status() == Status::Running);
}

IMPULSE_TEST(XrayKillSwitchBlocksTransportWhileCoreIsDown) {
	using Impulse::ProxyRouting::Decide;
	using Impulse::ProxyRouting::Route;
	using Impulse::ProxyRouting::SelectEndpoint;

	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	HealthLoop loop(runtime);
	loop.arm();
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	loop.advance(Impulse::Xray::kHealthCheckSeconds);
	IMPULSE_CHECK(runtime.status() == Status::Failed);

	const bool running = runtime.endpoints().has_value();
	IMPULSE_CHECK(!running);
	const Route route = Decide(true, true, running);
	IMPULSE_CHECK(route == Route::Blocked);
	const std::optional<Impulse::ProxyRouting::HttpEndpoint> endpoint
		= SelectEndpoint(route, std::nullopt);
	IMPULSE_CHECK(endpoint.has_value());
	if (endpoint) {
		IMPULSE_CHECK(endpoint->host == "127.0.0.1");
		IMPULSE_CHECK(endpoint->port == Impulse::ProxyRouting::kBlackholePort);
		IMPULSE_CHECK(!endpoint->hasCredentials);
	}
}

IMPULSE_TEST(XrayKillSwitchBlocksWhenRestartFailsAfterDeath) {
	using Impulse::ProxyRouting::Decide;
	using Impulse::ProxyRouting::Route;

	FakeCore core;
	InstallHealthy(core);
	Runtime runtime([&core](const std::string &method, const std::string &payload) {
		return core.invoke(method, payload);
	});
	runtime.restart(MakeState(true));
	core.reply("runXray", "{\"success\":false,\"error\":\"start failed\"}");
	HealthLoop loop(runtime);
	loop.arm();
	loop.advance(Impulse::Xray::kHealthCheckSeconds);

	IMPULSE_CHECK(loop.actions == std::vector<HealthAction>{HealthAction::Restart});
	IMPULSE_CHECK(runtime.status() == Status::Failed);
	IMPULSE_CHECK(runtime.error() == Error::CoreStart);
	IMPULSE_CHECK(!loop.dueAt.has_value());
	IMPULSE_CHECK(Decide(true, true, false) == Route::Blocked);
	IMPULSE_CHECK(Decide(true, false, false) == Route::Direct);
}

IMPULSE_TEST(XrayKillSwitchTransportOffStaysDirectWhileCoreIsDown) {
	using Impulse::ProxyRouting::Decide;
	using Impulse::ProxyRouting::Route;
	using Impulse::ProxyRouting::SelectEndpoint;

	const Route route = Decide(true, false, false);
	IMPULSE_CHECK(route == Route::Direct);
	IMPULSE_CHECK(!SelectEndpoint(route, std::nullopt).has_value());
}
