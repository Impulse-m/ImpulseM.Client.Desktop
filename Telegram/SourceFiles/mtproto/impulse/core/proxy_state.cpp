#include "mtproto/impulse/core/proxy_state.h"

#include "mtproto/impulse/core/json.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Impulse::ProxyState {
namespace {

inline constexpr auto kFragmentOff = std::string_view("off");
inline constexpr auto kFragmentClassic = std::string_view("classic");
inline constexpr auto kFragmentFinalMask = std::string_view("finalmask");
inline constexpr auto kFragmentTlsHello = std::string_view("tlshello");
inline constexpr auto kFragmentDefaultLength = std::string_view("100-200");
inline constexpr auto kFragmentDefaultInterval = std::string_view("10-20");

inline constexpr auto kDnsSystem = std::string_view("system");
inline constexpr auto kDnsCloudflare = std::string_view("cloudflare");
inline constexpr auto kDnsGoogle = std::string_view("google");
inline constexpr auto kDnsCustom = std::string_view("custom");

inline constexpr auto kPingBatch = std::string_view("batch");
inline constexpr auto kPingSequential = std::string_view("sequential");
inline constexpr auto kPingDefaultUrl = std::string_view("https://www.gstatic.com/generate_204");
inline constexpr auto kHttpsPrefix = std::string_view("https://");

inline constexpr auto kSecurityNone = std::string_view("none");

inline constexpr int kDefaultMuxConcurrency = 8;
inline constexpr int kDefaultPingTimeoutSeconds = 5;
inline constexpr int kDefaultIntervalHours = 24;

inline constexpr auto kFingerprints = std::array<std::string_view, 10>{
	"chrome",
	"firefox",
	"safari",
	"ios",
	"android",
	"edge",
	"360",
	"qq",
	"random",
	"randomized",
};

[[nodiscard]] bool IsKnownSecurity(std::string_view value) {
	return value == "none" || value == "tls" || value == "reality";
}

[[nodiscard]] bool IsFingerprint(std::string_view value) {
	for (const auto candidate : kFingerprints) {
		if (candidate == value) {
			return true;
		}
	}
	return false;
}

// Digits only, into 0..65535; -1 when invalid.
[[nodiscard]] int ParseBounded(std::string_view text) {
	if (text.empty() || text.size() > 5) {
		return -1;
	}
	auto value = 0;
	for (const char symbol : text) {
		if (symbol < '0' || symbol > '9') {
			return -1;
		}
		value = value * 10 + (symbol - '0');
	}
	return value <= 65535 ? value : -1;
}

// "N" or "N-M" with 0 <= N <= M <= 65535.
[[nodiscard]] bool IsRange(std::string_view value) {
	const auto dash = value.find('-');
	if (dash == std::string_view::npos) {
		return ParseBounded(value) >= 0;
	}
	const auto low = ParseBounded(value.substr(0, dash));
	const auto high = ParseBounded(value.substr(dash + 1));
	return low >= 0 && high >= 0 && low <= high;
}

[[nodiscard]] bool IsPackets(std::string_view value) {
	return value == kFragmentTlsHello || IsRange(value);
}

// An https:// URL without spaces, or an IPv4 address.
[[nodiscard]] bool IsDnsServer(std::string_view value) {
	if (value.substr(0, kHttpsPrefix.size()) == kHttpsPrefix) {
		return value.size() > kHttpsPrefix.size()
			&& value.find(' ') == std::string_view::npos;
	}
	auto parts = 0;
	auto digits = 0;
	auto number = 0;
	for (std::size_t i = 0; i <= value.size(); ++i) {
		if (i == value.size() || value[i] == '.') {
			if (digits == 0 || digits > 3 || number > 255) {
				return false;
			}
			++parts;
			digits = 0;
			number = 0;
			continue;
		}
		if (value[i] < '0' || value[i] > '9') {
			return false;
		}
		number = number * 10 + (value[i] - '0');
		++digits;
	}
	return parts == 4;
}

void Sanitize(Advanced &advanced) {
	if (advanced.fragmentMode != kFragmentOff
		&& advanced.fragmentMode != kFragmentClassic
		&& advanced.fragmentMode != kFragmentFinalMask) {
		advanced.fragmentMode = std::string(kFragmentOff);
	}
	if (!IsPackets(advanced.fragmentPackets)) {
		advanced.fragmentPackets = std::string(kFragmentTlsHello);
	}
	if (!IsRange(advanced.fragmentLength)) {
		advanced.fragmentLength = std::string(kFragmentDefaultLength);
	}
	if (!IsRange(advanced.fragmentInterval)) {
		advanced.fragmentInterval = std::string(kFragmentDefaultInterval);
	}
	if (!advanced.fingerprint.empty() && !IsFingerprint(advanced.fingerprint)) {
		advanced.fingerprint.clear();
	}
	if (advanced.muxConcurrency < 1 || advanced.muxConcurrency > 128) {
		advanced.muxConcurrency = kDefaultMuxConcurrency;
	}
	if (advanced.keepAliveIdle < 0 || advanced.keepAliveIdle > 86400) {
		advanced.keepAliveIdle = 0;
	}
	if (advanced.keepAliveInterval < 0 || advanced.keepAliveInterval > 86400) {
		advanced.keepAliveInterval = 0;
	}
	if (advanced.tcpMaxSeg != 0
		&& (advanced.tcpMaxSeg < 64 || advanced.tcpMaxSeg > 1460)) {
		advanced.tcpMaxSeg = 0;
	}
	if (advanced.dnsMode != kDnsSystem
		&& advanced.dnsMode != kDnsCloudflare
		&& advanced.dnsMode != kDnsGoogle
		&& advanced.dnsMode != kDnsCustom) {
		advanced.dnsMode = std::string(kDnsSystem);
	}
	if (!advanced.dnsCustom.empty() && !IsDnsServer(advanced.dnsCustom)) {
		advanced.dnsCustom.clear();
	}
	if (advanced.dnsMode == kDnsCustom && advanced.dnsCustom.empty()) {
		advanced.dnsMode = std::string(kDnsSystem);
	}
	if (advanced.pingMode != kPingBatch && advanced.pingMode != kPingSequential) {
		advanced.pingMode = std::string(kPingBatch);
	}
	if (advanced.pingPauseMillis < 0 || advanced.pingPauseMillis > 10000) {
		advanced.pingPauseMillis = 0;
	}
	if (advanced.pingTimeoutSeconds < 1 || advanced.pingTimeoutSeconds > 30) {
		advanced.pingTimeoutSeconds = kDefaultPingTimeoutSeconds;
	}
	if (std::string_view(advanced.pingUrl).substr(0, kHttpsPrefix.size()) != kHttpsPrefix
		|| advanced.pingUrl.size() <= kHttpsPrefix.size()) {
		advanced.pingUrl = std::string(kPingDefaultUrl);
	}
}

void RepairToggles(
		bool &useForTransport,
		bool &useForCalls) {
	if (!useForTransport && !useForCalls) {
		useForTransport = true;
	}
}

// Reading helpers. A value of the wrong JSON type counts as missing.

[[nodiscard]] std::optional<std::string> ReadOptionalString(
		const Json::Value &object,
		std::string_view key) {
	const auto *value = object.find(key);
	if (value == nullptr) {
		return std::nullopt;
	}
	if (const auto *text = value->asString()) {
		return *text;
	}
	return std::nullopt;
}

[[nodiscard]] std::string ReadString(
		const Json::Value &object,
		std::string_view key,
		std::string fallback) {
	auto value = ReadOptionalString(object, key);
	return value ? std::move(*value) : std::move(fallback);
}

[[nodiscard]] bool ReadBool(
		const Json::Value &object,
		std::string_view key,
		bool fallback) {
	const auto *value = object.find(key);
	if (value == nullptr) {
		return fallback;
	}
	const auto flag = value->asBool();
	return flag ? *flag : fallback;
}

[[nodiscard]] std::int64_t ReadInt64(
		const Json::Value &object,
		std::string_view key,
		std::int64_t fallback) {
	const auto *value = object.find(key);
	if (value == nullptr) {
		return fallback;
	}
	const auto number = value->asInt();
	return number ? *number : fallback;
}

[[nodiscard]] int ReadInt(
		const Json::Value &object,
		std::string_view key,
		int fallback) {
	const auto number = ReadInt64(object, key, fallback);
	if (number < std::numeric_limits<int>::min()
		|| number > std::numeric_limits<int>::max()) {
		return fallback;
	}
	return static_cast<int>(number);
}

[[nodiscard]] const std::vector<Json::Value> *ReadArray(
		const Json::Value &object,
		std::string_view key) {
	const auto *value = object.find(key);
	return value == nullptr ? nullptr : value->asArray();
}

[[nodiscard]] std::optional<Server> DecodeServer(const Json::Value &item) {
	if (item.kind() != Json::Kind::Object) {
		return std::nullopt;
	}
	const auto id = ReadOptionalString(item, "id");
	const auto name = ReadOptionalString(item, "name");
	const auto host = ReadOptionalString(item, "host");
	const auto network = ReadOptionalString(item, "network");
	const auto security = ReadOptionalString(item, "security");
	const auto outbound = ReadOptionalString(item, "outbound");
	auto port = std::optional<std::int64_t>();
	if (const auto *portValue = item.find("port")) {
		port = portValue->asInt();
	}
	if (!id || !name || !host || !network || !security || !outbound || !port) {
		return std::nullopt;
	}
	if (*port < std::numeric_limits<int>::min() || *port > std::numeric_limits<int>::max()) {
		return std::nullopt;
	}
	auto server = Server();
	server.id = *id;
	server.name = *name;
	server.host = *host;
	server.port = static_cast<int>(*port);
	server.network = *network;
	server.security = IsKnownSecurity(*security)
		? *security
		: std::string(kSecurityNone);
	server.outbound = *outbound;
	server.shareLink = ReadOptionalString(item, "shareLink");
	return server;
}

[[nodiscard]] std::vector<Server> DecodeServers(const std::vector<Json::Value> *array) {
	auto servers = std::vector<Server>();
	if (array == nullptr) {
		return servers;
	}
	for (const auto &element : *array) {
		if (auto server = DecodeServer(element)) {
			servers.push_back(std::move(*server));
		}
	}
	return servers;
}

[[nodiscard]] SubscriptionMeta DecodeMeta(const Json::Value *element) {
	auto meta = SubscriptionMeta();
	if (element == nullptr || element->kind() != Json::Kind::Object) {
		return meta;
	}
	meta.title = ReadOptionalString(*element, "title");
	meta.upload = ReadInt64(*element, "upload", meta.upload);
	meta.download = ReadInt64(*element, "download", meta.download);
	meta.total = ReadInt64(*element, "total", meta.total);
	meta.expire = ReadInt64(*element, "expire", meta.expire);
	meta.updateIntervalHours = ReadInt(*element, "interval", kDefaultIntervalHours);
	return meta;
}

[[nodiscard]] std::optional<Subscription> DecodeSubscription(const Json::Value &item) {
	if (item.kind() != Json::Kind::Object) {
		return std::nullopt;
	}
	const auto id = ReadOptionalString(item, "id");
	const auto url = ReadOptionalString(item, "url");
	if (!id || !url) {
		return std::nullopt;
	}
	auto subscription = Subscription();
	subscription.id = *id;
	subscription.url = *url;
	subscription.meta = DecodeMeta(item.find("meta"));
	subscription.updatedAt = ReadInt64(item, "updatedAt", 0);
	subscription.lastError = ReadOptionalString(item, "lastError");
	subscription.skipped = ReadInt(item, "skipped", 0);
	subscription.servers = DecodeServers(ReadArray(item, "servers"));
	return subscription;
}

[[nodiscard]] Advanced DecodeAdvanced(const Json::Value *element) {
	auto advanced = Advanced();
	if (element != nullptr && element->kind() == Json::Kind::Object) {
		advanced.fragmentMode = ReadString(*element, "fragmentMode", advanced.fragmentMode);
		advanced.fragmentPackets = ReadString(*element, "fragmentPackets", advanced.fragmentPackets);
		advanced.fragmentLength = ReadString(*element, "fragmentLength", advanced.fragmentLength);
		advanced.fragmentInterval = ReadString(*element, "fragmentInterval", advanced.fragmentInterval);
		advanced.fragmentFromLink = ReadBool(*element, "fragmentFromLink", advanced.fragmentFromLink);
		advanced.fingerprint = ReadString(*element, "fingerprint", advanced.fingerprint);
		advanced.muxEnabled = ReadBool(*element, "muxEnabled", advanced.muxEnabled);
		advanced.muxConcurrency = ReadInt(*element, "muxConcurrency", advanced.muxConcurrency);
		advanced.keepAliveIdle = ReadInt(*element, "keepAliveIdle", advanced.keepAliveIdle);
		advanced.keepAliveInterval = ReadInt(*element, "keepAliveInterval", advanced.keepAliveInterval);
		advanced.tcpFastOpen = ReadBool(*element, "tcpFastOpen", advanced.tcpFastOpen);
		advanced.tcpMaxSeg = ReadInt(*element, "tcpMaxSeg", advanced.tcpMaxSeg);
		advanced.dnsMode = ReadString(*element, "dnsMode", advanced.dnsMode);
		advanced.dnsCustom = ReadString(*element, "dnsCustom", advanced.dnsCustom);
		advanced.pingMode = ReadString(*element, "pingMode", advanced.pingMode);
		advanced.pingPauseMillis = ReadInt(*element, "pingPauseMillis", advanced.pingPauseMillis);
		advanced.pingTimeoutSeconds = ReadInt(*element, "pingTimeoutSeconds", advanced.pingTimeoutSeconds);
		advanced.pingUrl = ReadString(*element, "pingUrl", advanced.pingUrl);
		advanced.pingSkipWhileConnected = ReadBool(*element, "pingSkipWhileConnected", advanced.pingSkipWhileConnected);
	}
	Sanitize(advanced);
	return advanced;
}

// Writing helpers. Each returns a complete JSON fragment.

[[nodiscard]] std::string Quoted(std::string_view text) {
	constexpr auto hex = std::string_view("0123456789abcdef");
	auto out = std::string("\"");
	for (const char symbol : text) {
		const auto code = static_cast<unsigned char>(symbol);
		if (symbol == '"') {
			out += "\\\"";
		} else if (symbol == '\\') {
			out += "\\\\";
		} else if (symbol == '\n') {
			out += "\\n";
		} else if (symbol == '\r') {
			out += "\\r";
		} else if (symbol == '\t') {
			out += "\\t";
		} else if (code < 0x20) {
			out += "\\u00";
			out += hex[(code >> 4) & 0xF];
			out += hex[code & 0xF];
		} else {
			out += symbol;
		}
	}
	out += '"';
	return out;
}

[[nodiscard]] std::string Member(
		std::string_view key,
		const std::string &value) {
	return Quoted(key) + ':' + value;
}

[[nodiscard]] std::string Container(
		char open,
		char close,
		const std::vector<std::string> &items) {
	auto out = std::string(1, open);
	for (auto i = std::size_t(0); i < items.size(); ++i) {
		if (i > 0) {
			out += ',';
		}
		out += items[i];
	}
	out += close;
	return out;
}

[[nodiscard]] std::string Boolean(bool value) {
	return value ? "true" : "false";
}

[[nodiscard]] std::string EncodeServer(const Server &server) {
	auto members = std::vector<std::string>{
		Member("id", Quoted(server.id)),
		Member("name", Quoted(server.name)),
		Member("host", Quoted(server.host)),
		Member("port", std::to_string(server.port)),
		Member("network", Quoted(server.network)),
		Member("security", Quoted(server.security)),
		Member("outbound", Quoted(server.outbound)),
	};
	if (server.shareLink) {
		members.push_back(Member("shareLink", Quoted(*server.shareLink)));
	}
	return Container('{', '}', members);
}

[[nodiscard]] std::string EncodeServers(const std::vector<Server> &servers) {
	auto items = std::vector<std::string>();
	items.reserve(servers.size());
	for (const auto &server : servers) {
		items.push_back(EncodeServer(server));
	}
	return Container('[', ']', items);
}

[[nodiscard]] std::string EncodeMeta(const SubscriptionMeta &meta) {
	auto members = std::vector<std::string>();
	if (meta.title) {
		members.push_back(Member("title", Quoted(*meta.title)));
	}
	members.push_back(Member("upload", std::to_string(meta.upload)));
	members.push_back(Member("download", std::to_string(meta.download)));
	members.push_back(Member("total", std::to_string(meta.total)));
	members.push_back(Member("expire", std::to_string(meta.expire)));
	members.push_back(Member("interval", std::to_string(meta.updateIntervalHours)));
	return Container('{', '}', members);
}

[[nodiscard]] std::string EncodeSubscription(const Subscription &subscription) {
	auto members = std::vector<std::string>{
		Member("id", Quoted(subscription.id)),
		Member("url", Quoted(subscription.url)),
		Member("updatedAt", std::to_string(subscription.updatedAt)),
		Member("skipped", std::to_string(subscription.skipped)),
	};
	if (subscription.lastError) {
		members.push_back(Member("lastError", Quoted(*subscription.lastError)));
	}
	members.push_back(Member("meta", EncodeMeta(subscription.meta)));
	members.push_back(Member("servers", EncodeServers(subscription.servers)));
	return Container('{', '}', members);
}

[[nodiscard]] std::string EncodeAdvanced(const Advanced &advanced) {
	auto members = std::vector<std::string>{
		Member("fragmentMode", Quoted(advanced.fragmentMode)),
		Member("fragmentPackets", Quoted(advanced.fragmentPackets)),
		Member("fragmentLength", Quoted(advanced.fragmentLength)),
		Member("fragmentInterval", Quoted(advanced.fragmentInterval)),
		Member("fragmentFromLink", Boolean(advanced.fragmentFromLink)),
		Member("fingerprint", Quoted(advanced.fingerprint)),
		Member("muxEnabled", Boolean(advanced.muxEnabled)),
		Member("muxConcurrency", std::to_string(advanced.muxConcurrency)),
		Member("keepAliveIdle", std::to_string(advanced.keepAliveIdle)),
		Member("keepAliveInterval", std::to_string(advanced.keepAliveInterval)),
		Member("tcpFastOpen", Boolean(advanced.tcpFastOpen)),
		Member("tcpMaxSeg", std::to_string(advanced.tcpMaxSeg)),
		Member("dnsMode", Quoted(advanced.dnsMode)),
		Member("dnsCustom", Quoted(advanced.dnsCustom)),
		Member("pingMode", Quoted(advanced.pingMode)),
		Member("pingPauseMillis", std::to_string(advanced.pingPauseMillis)),
		Member("pingTimeoutSeconds", std::to_string(advanced.pingTimeoutSeconds)),
		Member("pingUrl", Quoted(advanced.pingUrl)),
		Member("pingSkipWhileConnected", Boolean(advanced.pingSkipWhileConnected)),
	};
	return Container('{', '}', members);
}

} // namespace

std::string Encode(const State &state) {
	auto useForTransport = state.useForTransport;
	auto useForCalls = state.useForCalls;
	RepairToggles(useForTransport, useForCalls);

	auto members = std::vector<std::string>{
		Member("v", std::to_string(kVersion)),
		Member("enabled", Boolean(state.enabled)),
		Member("useForTransport", Boolean(useForTransport)),
		Member("useForCalls", Boolean(useForCalls)),
	};
	if (state.selectedId) {
		members.push_back(Member("selectedId", Quoted(*state.selectedId)));
	}
	members.push_back(Member("manual", EncodeServers(state.manual)));

	auto subscriptions = std::vector<std::string>();
	subscriptions.reserve(state.subscriptions.size());
	for (const auto &subscription : state.subscriptions) {
		subscriptions.push_back(EncodeSubscription(subscription));
	}
	members.push_back(Member("subscriptions", Container('[', ']', subscriptions)));
	members.push_back(Member("advanced", EncodeAdvanced(state.advanced)));
	return Container('{', '}', members);
}

State Decode(std::string_view text) {
	const auto parsed = Json::parse(text);
	if (!parsed.value || parsed.value->kind() != Json::Kind::Object) {
		return State();
	}
	const auto &root = *parsed.value;

	auto state = State();
	state.enabled = ReadBool(root, "enabled", state.enabled);
	state.useForTransport = ReadBool(root, "useForTransport", state.useForTransport);
	state.useForCalls = ReadBool(root, "useForCalls", state.useForCalls);
	RepairToggles(state.useForTransport, state.useForCalls);
	state.selectedId = ReadOptionalString(root, "selectedId");
	state.manual = DecodeServers(ReadArray(root, "manual"));

	if (const auto *subscriptions = ReadArray(root, "subscriptions")) {
		for (const auto &element : *subscriptions) {
			if (auto subscription = DecodeSubscription(element)) {
				state.subscriptions.push_back(std::move(*subscription));
			}
		}
	}
	state.advanced = DecodeAdvanced(root.find("advanced"));
	return state;
}

} // namespace Impulse::ProxyState
