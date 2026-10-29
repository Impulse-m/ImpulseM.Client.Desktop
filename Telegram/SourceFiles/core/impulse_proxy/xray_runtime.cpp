#include "core/impulse_proxy/xray_runtime.h"

#include "mtproto/impulse/core/json.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace Impulse::Xray {
namespace {

constexpr std::string_view kLoopbackHost = "127.0.0.1";
constexpr std::string_view kProxyTag = "proxy";
constexpr std::string_view kHttpTag = "http-in";
constexpr std::string_view kSocksTag = "socks-in";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::size_t kUserBytes = 16;
constexpr std::size_t kPasswordBytes = 24;
constexpr std::int64_t kMaxPort = 65535;

struct Reply {
	bool success = false;
	Json::Value data;
};

struct Ports {
	int httpPort = 0;
	int socksPort = 0;
};

struct Credentials {
	std::string user;
	std::string password;
};

// Sends one call and reads the response envelope
// {"success":bool,"data":{...},"error":"..."}. Anything else is a failure.
Reply Call(
		const Invoker &invoker,
		const std::string &method,
		const std::string &payloadJson) {
	Reply reply;
	const Json::ParseResult parsed = Json::parse(
		invoker(method, payloadJson));
	if (!parsed.value || parsed.value->kind() != Json::Kind::Object) {
		return reply;
	}
	const Json::Value *success = parsed.value->find("success");
	std::optional<bool> ok;
	if (success) {
		ok = success->asBool();
	}
	if (!ok || !*ok) {
		return reply;
	}
	reply.success = true;
	if (const Json::Value *data = parsed.value->find("data")) {
		reply.data = *data;
	}
	return reply;
}

// nullopt when the state cannot be read; the caller then treats the core as
// not running, as the Android client does.
[[nodiscard]] std::optional<bool> QueryRunning(const Invoker &invoker) {
	const Reply reply = Call(invoker, "getXrayState", "{}");
	if (!reply.success) {
		return std::nullopt;
	}
	const Json::Value *running = reply.data.find("running");
	if (!running) {
		return std::nullopt;
	}
	return running->asBool();
}

[[nodiscard]] bool IsUsablePort(const std::optional<std::int64_t> &port) {
	return port && *port > 0 && *port <= kMaxPort;
}

[[nodiscard]] std::optional<Ports> FreePorts(const Invoker &invoker) {
	const Reply reply = Call(invoker, "getFreePorts", "{\"count\":2}");
	if (!reply.success) {
		return std::nullopt;
	}
	const Json::Value *list = reply.data.find("ports");
	const std::vector<Json::Value> *items = list ? list->asArray() : nullptr;
	if (!items || items->size() < 2) {
		return std::nullopt;
	}
	const std::optional<std::int64_t> http = (*items)[0].asInt();
	const std::optional<std::int64_t> socks = (*items)[1].asInt();
	if (!IsUsablePort(http) || !IsUsablePort(socks) || *http == *socks) {
		return std::nullopt;
	}
	return Ports{
		static_cast<int>(*http),
		static_cast<int>(*socks),
	};
}

// std::random_device is backed by the operating system random source on
// Windows, the platform this runtime ships on.
[[nodiscard]] std::string RandomHex(std::size_t bytes) {
	std::random_device source;
	std::string result;
	result.reserve(bytes * 2);
	for (std::size_t index = 0; index < bytes; ++index) {
		const std::size_t value = static_cast<std::size_t>(source() & 0xFFu);
		result.push_back(kHexDigits[value >> 4]);
		result.push_back(kHexDigits[value & 0xFu]);
	}
	return result;
}

// A JSON string literal. Used for the run config, which is embedded in the
// payload as a string.
[[nodiscard]] std::string JsonString(const std::string &value) {
	std::string result = "\"";
	for (const char symbol : value) {
		const unsigned char code = static_cast<unsigned char>(symbol);
		if (symbol == '"' || symbol == '\\') {
			result.push_back('\\');
			result.push_back(symbol);
		} else if (code < 0x20) {
			result += "\\u00";
			result.push_back(kHexDigits[code >> 4]);
			result.push_back(kHexDigits[code & 0xFu]);
		} else {
			result.push_back(symbol);
		}
	}
	result.push_back('"');
	return result;
}

[[nodiscard]] std::string ConfigPayload(const std::string &config) {
	return "{\"xrayJson\":" + JsonString(config) + "}";
}

// The server outbound with the tag set to "proxy". The tag is appended as the
// last member, so it wins over an existing "tag" key (Xray decodes members
// in order). nullopt when the stored outbound is not a JSON object.
[[nodiscard]] std::optional<std::string> TaggedOutbound(
		const std::string &outbound) {
	const Json::ParseResult parsed = Json::parse(outbound);
	if (!parsed.value || parsed.value->kind() != Json::Kind::Object) {
		return std::nullopt;
	}
	const std::size_t close = outbound.rfind('}');
	if (close == std::string::npos) {
		return std::nullopt;
	}
	std::string head = outbound.substr(0, close);
	while (!head.empty()
			&& (head.back() == ' '
				|| head.back() == '\t'
				|| head.back() == '\n'
				|| head.back() == '\r')) {
		head.pop_back();
	}
	const std::vector<Json::Value::Member> *members
		= parsed.value->asObject();
	const bool empty = !members || members->empty();
	return head
		+ (empty ? "" : ",")
		+ "\"tag\":\""
		+ std::string(kProxyTag)
		+ "\"}";
}

// The run config: the two loopback inbounds with the same credentials, one
// proxy outbound, and routing of both inbounds to it. The log is quiet and
// records no access. The advanced outbound and DNS options are not applied
// here.
[[nodiscard]] std::optional<std::string> BuildConfig(
		const std::string &outbound,
		const Ports &ports,
		const Credentials &credentials) {
	const std::optional<std::string> proxyOutbound = TaggedOutbound(outbound);
	if (!proxyOutbound) {
		return std::nullopt;
	}
	const std::string accounts = "[{\"user\":"
		+ JsonString(credentials.user)
		+ ",\"pass\":"
		+ JsonString(credentials.password)
		+ "}]";
	const std::string httpInbound = "{\"tag\":\""
		+ std::string(kHttpTag)
		+ "\",\"listen\":\""
		+ std::string(kLoopbackHost)
		+ "\",\"port\":"
		+ std::to_string(ports.httpPort)
		+ ",\"protocol\":\"http\",\"settings\":{\"accounts\":"
		+ accounts
		+ ",\"allowTransparent\":false}}";
	const std::string socksInbound = "{\"tag\":\""
		+ std::string(kSocksTag)
		+ "\",\"listen\":\""
		+ std::string(kLoopbackHost)
		+ "\",\"port\":"
		+ std::to_string(ports.socksPort)
		+ ",\"protocol\":\"socks\",\"settings\":{\"auth\":\"password\",\"accounts\":"
		+ accounts
		+ ",\"udp\":true,\"ip\":\""
		+ std::string(kLoopbackHost)
		+ "\"}}";
	return "{\"log\":{\"loglevel\":\"error\",\"access\":\"none\"},"
		"\"inbounds\":["
		+ httpInbound
		+ ","
		+ socksInbound
		+ "],\"outbounds\":["
		+ *proxyOutbound
		+ "],\"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":[{\"type\":\"field\","
		"\"inboundTag\":[\"http-in\",\"socks-in\"],\"outboundTag\":\"proxy\"}]}}";
}

[[nodiscard]] std::optional<ProxyState::Server> SelectedServer(
		const ProxyState::State &state) {
	if (!state.selectedId) {
		return std::nullopt;
	}
	const std::string &selected = *state.selectedId;
	for (const ProxyState::Server &server : state.manual) {
		if (server.id == selected) {
			return server;
		}
	}
	for (const ProxyState::Subscription &subscription : state.subscriptions) {
		for (const ProxyState::Server &server : subscription.servers) {
			if (server.id == selected) {
				return server;
			}
		}
	}
	return std::nullopt;
}

} // namespace

std::string_view ErrorCode(Error error) {
	switch (error) {
	case Error::NoServer:
		return "err:noServer";
	case Error::InvalidConfig:
		return "err:invalidConfig";
	case Error::CoreStart:
		return "err:coreStart";
	case Error::CoreStopped:
		return "err:coreStopped";
	case Error::None:
		break;
	}
	return "";
}

HealthAction DecideHealth(
		bool current,
		bool alreadyRetried,
		bool alive) {
	if (!current) {
		return HealthAction::Stop;
	}
	if (alive) {
		return HealthAction::Recheck;
	}
	return alreadyRetried ? HealthAction::Fail : HealthAction::Restart;
}

Runtime::Runtime(Invoker libraryInvoker)
: invoker(std::move(libraryInvoker)) {
}

void Runtime::restart(const ProxyState::State &state) {
	std::lock_guard<std::mutex> restartLock(restartMutex);
	autoRetried = false;
	restartLocked(state);
}

void Runtime::restartLocked(const ProxyState::State &state) {
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		++currentGeneration;
	}
	appliedState = state;

	// Fail closed: the endpoints go away before the old core is stopped.
	setState(Status::Off, Error::None, std::nullopt);
	if (QueryRunning(invoker).value_or(false)) {
		Call(invoker, "stopXray", "{}");
	}
	if (!state.enabled) {
		return;
	}

	const std::optional<ProxyState::Server> server = SelectedServer(state);
	if (!server) {
		setState(Status::Failed, Error::NoServer, std::nullopt);
		return;
	}

	setState(Status::Starting, Error::None, std::nullopt);
	const std::optional<Ports> ports = FreePorts(invoker);
	if (!ports) {
		setState(Status::Failed, Error::CoreStart, std::nullopt);
		return;
	}
	const Credentials credentials{
		RandomHex(kUserBytes),
		RandomHex(kPasswordBytes),
	};
	const std::optional<std::string> config = BuildConfig(
		server->outbound,
		*ports,
		credentials);
	if (!config) {
		setState(Status::Failed, Error::InvalidConfig, std::nullopt);
		return;
	}
	if (!Call(invoker, "testXray", ConfigPayload(*config)).success) {
		setState(Status::Failed, Error::InvalidConfig, std::nullopt);
		return;
	}
	if (!Call(invoker, "runXray", ConfigPayload(*config)).success) {
		setState(Status::Failed, Error::CoreStart, std::nullopt);
		return;
	}
	setState(
		Status::Running,
		Error::None,
		Endpoints{
			ports->httpPort,
			ports->socksPort,
			credentials.user,
			credentials.password,
		});
}

HealthAction Runtime::checkHealth(int expectedGeneration) {
	std::lock_guard<std::mutex> restartLock(restartMutex);
	const bool current = expectedGeneration == generation()
		&& status() == Status::Running;
	const bool alive = current && QueryRunning(invoker).value_or(false);
	const HealthAction action = DecideHealth(current, autoRetried, alive);
	if (action == HealthAction::Recheck) {
		autoRetried = false;
	} else if (action == HealthAction::Restart) {
		autoRetried = true;
		if (appliedState) {
			// A copy: restartLocked assigns appliedState while it runs.
			const ProxyState::State last = *appliedState;
			restartLocked(last);
		}
	} else if (action == HealthAction::Fail) {
		setState(Status::Failed, Error::CoreStopped, std::nullopt);
	}
	return action;
}

int Runtime::generation() const {
	std::lock_guard<std::mutex> lock(stateMutex);
	return currentGeneration;
}

Status Runtime::status() const {
	std::lock_guard<std::mutex> lock(stateMutex);
	return currentStatus;
}

Error Runtime::error() const {
	std::lock_guard<std::mutex> lock(stateMutex);
	return currentError;
}

std::optional<Endpoints> Runtime::endpoints() const {
	std::lock_guard<std::mutex> lock(stateMutex);
	return currentEndpoints;
}

void Runtime::setState(
		Status newStatus,
		Error newError,
		std::optional<Endpoints> newEndpoints) {
	std::lock_guard<std::mutex> lock(stateMutex);
	currentStatus = newStatus;
	currentError = newError;
	currentEndpoints = std::move(newEndpoints);
}

} // namespace Impulse::Xray
