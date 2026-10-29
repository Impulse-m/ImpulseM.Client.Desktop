#include "core/impulse_proxy/proxy_ping.h"

#include "mtproto/impulse/core/json.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Impulse::Xray {
namespace {

constexpr std::size_t kBatchSize = 5;
constexpr std::size_t kSequentialSize = 1;
constexpr std::string_view kSequentialMode = "sequential";
constexpr std::string_view kProxyTag = "proxy";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kErrorInvalidConfig = "err:invalidConfig";
constexpr std::string_view kErrorPingBatch = "err:pingBatch";
constexpr int kMinTimeoutSeconds = 1;
constexpr int kMaxTimeoutSeconds = 30;
constexpr std::int64_t kMinPauseMillis = 0;
constexpr std::int64_t kMaxPauseMillis = 10000;

struct PingSettings final {
	int timeoutSeconds = 5;
	std::int64_t pauseMillis = 0;
	std::size_t chunkSize = kBatchSize;
	std::string url;
};

// A JSON string literal, escaped for embedding in the payload.
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

// The server outbound with the tag set to "proxy". The tag is appended as the
// last member, so it wins over an existing "tag" key. nullopt when the stored
// outbound is not a JSON object.
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

// The ping config of one server: its proxy outbound alone.
[[nodiscard]] std::optional<std::string> PingConfig(
		const std::string &outbound) {
	const std::optional<std::string> tagged = TaggedOutbound(outbound);
	if (!tagged) {
		return std::nullopt;
	}
	return "{\"outbounds\":[" + *tagged + "]}";
}

// The pingBatch payload {"configs":[{"xrayJson","outboundTag"}],"timeout","url"}.
[[nodiscard]] std::string PingPayload(
		const std::vector<std::string> &configs,
		const PingSettings &settings) {
	std::string items;
	for (const std::string &config : configs) {
		if (!items.empty()) {
			items.push_back(',');
		}
		items += "{\"xrayJson\":"
			+ JsonString(config)
			+ ",\"outboundTag\":\""
			+ std::string(kProxyTag)
			+ "\"}";
	}
	return "{\"configs\":["
		+ items
		+ "],\"timeout\":"
		+ std::to_string(settings.timeoutSeconds)
		+ ",\"url\":"
		+ JsonString(settings.url)
		+ "}";
}

// The data object of a successful call, nullopt otherwise. Reads the envelope
// {"success":bool,"data":{...},"error":"..."} the same way the runtime does.
[[nodiscard]] std::optional<Json::Value> Call(
		const Invoker &invoker,
		const std::string &method,
		const std::string &payloadJson) {
	const Json::ParseResult parsed = Json::parse(
		invoker(method, payloadJson));
	if (!parsed.value || parsed.value->kind() != Json::Kind::Object) {
		return std::nullopt;
	}
	const Json::Value *success = parsed.value->find("success");
	if (!success) {
		return std::nullopt;
	}
	const std::optional<bool> ok = success->asBool();
	if (!ok || !*ok) {
		return std::nullopt;
	}
	if (const Json::Value *data = parsed.value->find("data")) {
		return *data;
	}
	return Json::Value();
}

[[nodiscard]] PingSettings Settings(const ProxyState::Advanced &advanced) {
	PingSettings settings;
	settings.timeoutSeconds = std::clamp(
		advanced.pingTimeoutSeconds,
		kMinTimeoutSeconds,
		kMaxTimeoutSeconds);
	settings.pauseMillis = std::clamp<std::int64_t>(
		advanced.pingPauseMillis,
		kMinPauseMillis,
		kMaxPauseMillis);
	settings.chunkSize = advanced.pingMode == kSequentialMode
		? kSequentialSize
		: kBatchSize;
	settings.url = advanced.pingUrl;
	return settings;
}

// Reads the result entry of one server: {"success":bool,"delay":ms,"error"}.
[[nodiscard]] PingResult ReadEntry(const Json::Value &item) {
	PingResult result;
	const Json::Value *success = item.find("success");
	const std::optional<bool> ok = success ? success->asBool() : std::nullopt;
	if (ok && *ok) {
		const Json::Value *delay = item.find("delay");
		const std::optional<std::int64_t> value = delay
			? delay->asInt()
			: std::nullopt;
		if (value) {
			result.delay = *value;
		}
		return result;
	}
	const Json::Value *error = item.find("error");
	const std::string *text = error ? error->asString() : nullptr;
	if (text) {
		result.error = *text;
	}
	return result;
}

// Pings one chunk with one pingBatch call. A server whose config cannot be
// built fails without a call, and the other servers still run.
[[nodiscard]] std::vector<PingResult> PingChunk(
		const Invoker &invoker,
		const std::vector<ProxyState::Server> &chunk,
		const PingSettings &settings) {
	std::vector<PingResult> results(chunk.size());
	std::vector<std::size_t> valid;
	std::vector<std::string> configs;
	for (std::size_t index = 0; index < chunk.size(); ++index) {
		std::optional<std::string> config = PingConfig(chunk[index].outbound);
		if (!config) {
			results[index].error = std::string(kErrorInvalidConfig);
			continue;
		}
		valid.push_back(index);
		configs.push_back(std::move(*config));
	}
	if (valid.empty()) {
		return results;
	}

	const std::optional<Json::Value> data = Call(
		invoker,
		"pingBatch",
		PingPayload(configs, settings));
	if (!data) {
		for (const std::size_t index : valid) {
			results[index].error = std::string(kErrorPingBatch);
		}
		return results;
	}
	const Json::Value *list = data->find("results");
	const std::vector<Json::Value> *items = list ? list->asArray() : nullptr;
	for (std::size_t position = 0; position < valid.size(); ++position) {
		if (!items || position >= items->size()) {
			continue;
		}
		results[valid[position]] = ReadEntry((*items)[position]);
	}
	return results;
}

} // namespace

PingHelper::PingHelper(Runtime &runtimeRef, Invoker libraryInvoker)
: runtime(runtimeRef)
, invoker(std::move(libraryInvoker)) {
}

std::vector<PingResult> PingHelper::check(
		const ProxyState::State &state,
		const std::vector<ProxyState::Server> &servers,
		const PingChunkListener &listener) {
	std::lock_guard<std::mutex> checkLock(checkMutex);
	const PingSettings settings = Settings(state.advanced);

	// pingBatch refuses while a managed core runs, so the core is stopped
	// first. A disabled state stops the core and starts nothing.
	ProxyState::State stopped = state;
	stopped.enabled = false;
	runtime.restart(stopped);

	std::vector<PingResult> results(servers.size());
	for (std::size_t offset = 0;
			offset < servers.size();
			offset += settings.chunkSize) {
		const std::size_t end = std::min(
			offset + settings.chunkSize,
			servers.size());
		std::vector<ProxyState::Server> chunk;
		for (std::size_t index = offset; index < end; ++index) {
			chunk.push_back(servers[index]);
		}
		const std::vector<PingResult> chunkResults = PingChunk(
			invoker,
			chunk,
			settings);
		for (std::size_t index = 0; index < chunkResults.size(); ++index) {
			results[offset + index] = chunkResults[index];
		}
		if (listener && !listener(offset, chunkResults)) {
			break;
		}
		if (settings.pauseMillis > 0 && end < servers.size()) {
			std::this_thread::sleep_for(
				std::chrono::milliseconds(settings.pauseMillis));
		}
	}

	// The saved state comes back, so the core runs again when it is enabled.
	runtime.restart(state);
	return results;
}

} // namespace Impulse::Xray
