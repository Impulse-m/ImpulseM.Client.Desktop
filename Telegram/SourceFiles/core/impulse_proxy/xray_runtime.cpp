#include "core/impulse_proxy/xray_runtime.h"

#include "mtproto/impulse/core/json.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Impulse::Xray {
namespace {

constexpr std::string_view kLoopbackHost = "127.0.0.1";
constexpr std::string_view kProxyTag = "proxy";
constexpr std::string_view kFragmentTag = "fragment";
constexpr std::string_view kHttpTag = "http-in";
constexpr std::string_view kSocksTag = "socks-in";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kFragmentOff = "off";
constexpr std::string_view kFragmentClassic = "classic";
constexpr std::string_view kFragmentTlsHello = "tlshello";
constexpr std::string_view kDnsCloudflare = "cloudflare";
constexpr std::string_view kDnsGoogle = "google";
constexpr std::string_view kDnsCustom = "custom";
constexpr std::string_view kCloudflareDoh = "https://1.1.1.1/dns-query";
constexpr std::string_view kGoogleDoh = "https://8.8.8.8/dns-query";
constexpr std::string_view kHttpsPrefix = "https://";
constexpr std::string_view kVisionFlow = "xtls-rprx-vision";
constexpr std::string_view kUseIpv4 = "UseIPv4";
constexpr std::size_t kUserBytes = 16;
constexpr std::size_t kPasswordBytes = 24;
constexpr std::int64_t kMaxPort = 65535;

using Member = Json::Value::Member;

// Keys of nested object members, outermost first.
using Path = std::vector<std::string_view>;

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

// The fragmentation of the run config: mode is "classic" or "finalmask".
struct Fragment {
	std::string mode;
	std::string packets;
	std::string length;
	std::string interval;
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

[[nodiscard]] Json::Value Text(std::string_view value) {
	return Json::Value::String(std::string(value));
}

[[nodiscard]] Member Entry(
		std::string_view key,
		Json::Value value) {
	return Member(std::string(key), std::move(value));
}

[[nodiscard]] bool IsObject(const Json::Value *value) {
	return value && value->kind() == Json::Kind::Object;
}

// The object with the key set. An existing member is replaced in place, a new
// one is appended, as the Android builder adds properties.
[[nodiscard]] Json::Value WithMember(
		const Json::Value &object,
		std::string_view key,
		Json::Value value) {
	std::vector<Member> members;
	if (const std::vector<Member> *existing = object.asObject()) {
		members = *existing;
	}
	for (Member &member : members) {
		if (member.first == key) {
			member.second = std::move(value);
			return Json::Value::Object(std::move(members));
		}
	}
	members.push_back(Entry(key, std::move(value)));
	return Json::Value::Object(std::move(members));
}

// Sets the member at the end of the path. An object on the way is created
// when it is absent or not an object, as the Android builder creates them.
[[nodiscard]] Json::Value PutAt(
		const Json::Value &object,
		const Path &path,
		std::size_t depth,
		Json::Value value) {
	const std::string_view key = path[depth];
	if (depth + 1 == path.size()) {
		return WithMember(object, key, std::move(value));
	}
	const Json::Value *found = object.find(key);
	const Json::Value child = IsObject(found)
		? *found
		: Json::Value::Object(std::vector<Member>());
	return WithMember(
		object,
		key,
		PutAt(child, path, depth + 1, std::move(value)));
}

[[nodiscard]] Json::Value Put(
		const Json::Value &object,
		const Path &path,
		Json::Value value) {
	return PutAt(object, path, 0, std::move(value));
}

// True when the whole path leads to an object.
[[nodiscard]] bool HasObjectAt(
		const Json::Value &root,
		const Path &path) {
	const Json::Value *node = &root;
	for (const std::string_view key : path) {
		node = node->find(key);
		if (!node) {
			return false;
		}
	}
	return node->kind() == Json::Kind::Object;
}

// Adds the entries to the streamSettings.sockopt of the outbound.
[[nodiscard]] Json::Value WithSockopt(
		Json::Value outbound,
		const std::vector<Member> &entries) {
	for (const Member &entry : entries) {
		outbound = Put(
			outbound,
			{"streamSettings", "sockopt", entry.first},
			entry.second);
	}
	return outbound;
}

[[nodiscard]] bool IsVisionFlow(const Json::Value *flow) {
	const std::string *text = flow ? flow->asString() : nullptr;
	return text && text->starts_with(kVisionFlow);
}

// Vision flow is incompatible with mux. Checks settings.flow and the flow of
// every vnext user.
[[nodiscard]] bool UsesVision(const Json::Value &outbound) {
	const Json::Value *settings = outbound.find("settings");
	if (!IsObject(settings)) {
		return false;
	}
	if (IsVisionFlow(settings->find("flow"))) {
		return true;
	}
	const Json::Value *vnext = settings->find("vnext");
	const std::vector<Json::Value> *endpoints = vnext ? vnext->asArray() : nullptr;
	if (!endpoints) {
		return false;
	}
	for (const Json::Value &endpoint : *endpoints) {
		const Json::Value *users = endpoint.find("users");
		const std::vector<Json::Value> *list = users ? users->asArray() : nullptr;
		if (!list) {
			continue;
		}
		for (const Json::Value &user : *list) {
			if (IsVisionFlow(user.find("flow"))) {
				return true;
			}
		}
	}
	return false;
}

void AppendDouble(
		std::string &out,
		double number) {
	char buffer[64];
	const std::to_chars_result result = std::to_chars(
		buffer,
		buffer + sizeof(buffer),
		number);
	if (result.ec == std::errc()) {
		out.append(buffer, result.ptr);
	} else {
		out.push_back('0');
	}
}

void AppendJson(
		std::string &out,
		const Json::Value &value) {
	switch (value.kind()) {
	case Json::Kind::Null:
		out += "null";
		break;
	case Json::Kind::Bool:
		out += (value.asBool().value_or(false) ? "true" : "false");
		break;
	case Json::Kind::Int:
		out += std::to_string(value.asInt().value_or(0));
		break;
	case Json::Kind::Double:
		AppendDouble(out, value.asDouble().value_or(0.));
		break;
	case Json::Kind::String: {
		const std::string *text = value.asString();
		out += JsonString(text ? *text : std::string());
		break;
	}
	case Json::Kind::Array: {
		out.push_back('[');
		const std::vector<Json::Value> *items = value.asArray();
		if (items) {
			for (std::size_t index = 0; index < items->size(); ++index) {
				if (index > 0) {
					out.push_back(',');
				}
				AppendJson(out, (*items)[index]);
			}
		}
		out.push_back(']');
		break;
	}
	case Json::Kind::Object: {
		out.push_back('{');
		const std::vector<Member> *members = value.asObject();
		if (members) {
			for (std::size_t index = 0; index < members->size(); ++index) {
				if (index > 0) {
					out.push_back(',');
				}
				out += JsonString((*members)[index].first);
				out.push_back(':');
				AppendJson(out, (*members)[index].second);
			}
		}
		out.push_back('}');
		break;
	}
	}
}

[[nodiscard]] std::string WriteJson(const Json::Value &value) {
	std::string out;
	AppendJson(out, value);
	return out;
}

[[nodiscard]] int HexValue(char symbol) {
	if (symbol >= '0' && symbol <= '9') {
		return symbol - '0';
	}
	if (symbol >= 'a' && symbol <= 'f') {
		return symbol - 'a' + 10;
	}
	if (symbol >= 'A' && symbol <= 'F') {
		return symbol - 'A' + 10;
	}
	return -1;
}

// The URL decoding of Java URLDecoder: '+' is a space and %XX is a byte. A
// malformed escape fails the whole value.
[[nodiscard]] std::optional<std::string> UrlDecode(std::string_view text) {
	std::string result;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const char symbol = text[index];
		if (symbol == '+') {
			result.push_back(' ');
		} else if (symbol == '%') {
			if (index + 2 >= text.size()) {
				return std::nullopt;
			}
			const int high = HexValue(text[index + 1]);
			const int low = HexValue(text[index + 2]);
			if (high < 0 || low < 0) {
				return std::nullopt;
			}
			result.push_back(static_cast<char>(high * 16 + low));
			index += 2;
		} else {
			result.push_back(symbol);
		}
	}
	return result;
}

// Splits on every separator. Empty parts are kept, as Java split with a
// negative limit keeps them.
[[nodiscard]] std::vector<std::string> Split(
		std::string_view text,
		char separator) {
	std::vector<std::string> parts;
	std::size_t start = 0;
	while (true) {
		const std::size_t found = text.find(separator, start);
		if (found == std::string_view::npos) {
			parts.emplace_back(text.substr(start));
			return parts;
		}
		parts.emplace_back(text.substr(start, found - start));
		start = found + 1;
	}
}

// Java trim: removes every character up to and including the space.
[[nodiscard]] std::string Trim(std::string_view text) {
	std::size_t first = 0;
	std::size_t last = text.size();
	while (first < last && static_cast<unsigned char>(text[first]) <= ' ') {
		++first;
	}
	while (last > first && static_cast<unsigned char>(text[last - 1]) <= ' ') {
		--last;
	}
	return std::string(text.substr(first, last - first));
}

[[nodiscard]] std::string Lowercase(std::string value) {
	for (char &symbol : value) {
		if (symbol >= 'A' && symbol <= 'Z') {
			symbol = static_cast<char>(symbol - 'A' + 'a');
		}
	}
	return value;
}

// Digits only, into 0..65535; -1 when invalid.
[[nodiscard]] int ParseBounded(std::string_view text) {
	if (text.empty() || text.size() > 5) {
		return -1;
	}
	int value = 0;
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
	const std::size_t dash = value.find('-');
	if (dash == std::string_view::npos) {
		return ParseBounded(value) >= 0;
	}
	const int low = ParseBounded(value.substr(0, dash));
	const int high = ParseBounded(value.substr(dash + 1));
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
	const std::vector<std::string> parts = Split(value, '.');
	if (parts.size() != 4) {
		return false;
	}
	for (const std::string &part : parts) {
		if (part.empty() || part.size() > 3) {
			return false;
		}
		int number = 0;
		for (const char symbol : part) {
			if (symbol < '0' || symbol > '9') {
				return false;
			}
			number = number * 10 + (symbol - '0');
		}
		if (number > 255) {
			return false;
		}
	}
	return true;
}

// The URL-decoded value of a query parameter of a share link. nullopt when
// the parameter is absent or its value is malformed.
[[nodiscard]] std::optional<std::string> QueryParameter(
		std::string_view link,
		std::string_view name) {
	const std::size_t hash = link.find('#');
	const std::string_view withoutFragment = hash == std::string_view::npos
		? link
		: link.substr(0, hash);
	const std::size_t question = withoutFragment.find('?');
	if (question == std::string_view::npos) {
		return std::nullopt;
	}
	const std::vector<std::string> pairs = Split(
		withoutFragment.substr(question + 1),
		'&');
	for (const std::string &pair : pairs) {
		const std::size_t equals = pair.find('=');
		if (equals == std::string::npos || equals == 0) {
			continue;
		}
		if (std::string_view(pair).substr(0, equals) == name) {
			return UrlDecode(std::string_view(pair).substr(equals + 1));
		}
	}
	return std::nullopt;
}

// The fragmentation to apply, or nullopt for none. A global mode wins.
// Otherwise a link parameter fragment=length,interval,packets is honoured as
// classic fragmentation, when fragmentFromLink is on.
[[nodiscard]] std::optional<Fragment> EffectiveFragment(
		const ProxyState::Server &server,
		const ProxyState::Advanced &advanced) {
	if (advanced.fragmentMode != kFragmentOff) {
		return Fragment{
			advanced.fragmentMode,
			advanced.fragmentPackets,
			advanced.fragmentLength,
			advanced.fragmentInterval,
		};
	}
	if (!advanced.fragmentFromLink || !server.shareLink) {
		return std::nullopt;
	}
	const std::optional<std::string> value = QueryParameter(
		*server.shareLink,
		"fragment");
	if (!value) {
		return std::nullopt;
	}
	const std::vector<std::string> parts = Split(*value, ',');
	if (parts.size() < 3) {
		return std::nullopt;
	}
	const std::string length = Trim(parts[0]);
	const std::string interval = Trim(parts[1]);
	const std::string packets = Lowercase(Trim(parts[2]));
	if (!IsRange(length) || !IsRange(interval) || !IsPackets(packets)) {
		return std::nullopt;
	}
	return Fragment{
		std::string(kFragmentClassic),
		packets,
		length,
		interval,
	};
}

// The DNS server to use, or nullopt for the system resolver.
[[nodiscard]] std::optional<std::string> EffectiveDnsServer(
		const ProxyState::Advanced &advanced) {
	if (advanced.dnsMode == kDnsCloudflare) {
		return std::string(kCloudflareDoh);
	}
	if (advanced.dnsMode == kDnsGoogle) {
		return std::string(kGoogleDoh);
	}
	if (advanced.dnsMode == kDnsCustom && IsDnsServer(advanced.dnsCustom)) {
		return advanced.dnsCustom;
	}
	return std::nullopt;
}

// The proxy outbound with the tag and the advanced settings applied, followed
// by the freedom dialer of the classic fragmentation. nullopt when the stored
// outbound is not a JSON object.
[[nodiscard]] std::optional<std::vector<Json::Value>> AdvancedOutbounds(
		const ProxyState::Server &server,
		const ProxyState::Advanced &advanced) {
	const Json::ParseResult parsed = Json::parse(server.outbound);
	if (!parsed.value || parsed.value->kind() != Json::Kind::Object) {
		return std::nullopt;
	}
	Json::Value proxy = WithMember(*parsed.value, "tag", Text(kProxyTag));
	const std::optional<Fragment> fragment = EffectiveFragment(server, advanced);
	const bool classic = fragment && fragment->mode == kFragmentClassic;

	if (!advanced.fingerprint.empty()) {
		if (HasObjectAt(proxy, {"streamSettings", "tlsSettings"})) {
			proxy = Put(
				proxy,
				{"streamSettings", "tlsSettings", "fingerprint"},
				Text(advanced.fingerprint));
		}
		if (HasObjectAt(proxy, {"streamSettings", "realitySettings"})) {
			proxy = Put(
				proxy,
				{"streamSettings", "realitySettings", "fingerprint"},
				Text(advanced.fingerprint));
		}
	}
	if (advanced.muxEnabled && !UsesVision(proxy)) {
		proxy = WithMember(
			proxy,
			"mux",
			Json::Value::Object(std::vector<Member>{
				Entry("enabled", Json::Value::Bool(true)),
				Entry("concurrency", Json::Value::Int(advanced.muxConcurrency)),
			}));
	}

	std::optional<Json::Value> freedom;
	if (classic) {
		const Json::Value fragmentJson = Json::Value::Object(std::vector<Member>{
			Entry("packets", Text(fragment->packets)),
			Entry("length", Text(fragment->length)),
			Entry("interval", Text(fragment->interval)),
		});
		freedom = Json::Value::Object(std::vector<Member>{
			Entry("tag", Text(kFragmentTag)),
			Entry("protocol", Text("freedom")),
			Entry("settings", Json::Value::Object(std::vector<Member>{
				Entry("fragment", fragmentJson),
			})),
		});
		proxy = Put(
			proxy,
			{"streamSettings", "sockopt", "dialerProxy"},
			Text(kFragmentTag));
	} else if (fragment) {
		const Json::Value settings = Json::Value::Object(std::vector<Member>{
			Entry("packets", Text(fragment->packets)),
			Entry("lengths", Json::Value::Array(std::vector<Json::Value>{
				Text(fragment->length),
			})),
			Entry("delays", Json::Value::Array(std::vector<Json::Value>{
				Text(fragment->interval),
			})),
		});
		const Json::Value mask = Json::Value::Object(std::vector<Member>{
			Entry("type", Text("fragment")),
			Entry("settings", settings),
		});
		const Json::Value finalMask = Json::Value::Object(std::vector<Member>{
			Entry("tcp", Json::Value::Array(std::vector<Json::Value>{mask})),
		});
		proxy = Put(proxy, {"streamSettings", "finalmask"}, finalMask);
	}

	// The TCP options go on the dialing outbound: the freedom dialer in
	// classic mode, the proxy otherwise.
	std::vector<Member> sockopt;
	if (advanced.keepAliveIdle > 0) {
		sockopt.push_back(Entry(
			"tcpKeepAliveIdle",
			Json::Value::Int(advanced.keepAliveIdle)));
	}
	if (advanced.keepAliveInterval > 0) {
		sockopt.push_back(Entry(
			"tcpKeepAliveInterval",
			Json::Value::Int(advanced.keepAliveInterval)));
	}
	if (advanced.tcpFastOpen) {
		sockopt.push_back(Entry("tcpFastOpen", Json::Value::Bool(true)));
	}
	if (advanced.tcpMaxSeg > 0) {
		sockopt.push_back(Entry(
			"tcpMaxSeg",
			Json::Value::Int(advanced.tcpMaxSeg)));
	}
	if (EffectiveDnsServer(advanced)) {
		if (classic) {
			freedom = Put(*freedom, {"settings", "domainStrategy"}, Text(kUseIpv4));
		} else {
			sockopt.push_back(Entry("domainStrategy", Text(kUseIpv4)));
		}
	}

	if (classic) {
		*freedom = WithSockopt(*freedom, sockopt);
		return std::vector<Json::Value>{proxy, *freedom};
	}
	return std::vector<Json::Value>{WithSockopt(proxy, sockopt)};
}

// The run config: the two loopback inbounds with the same credentials, the
// proxy outbound with the advanced settings, the DNS object when a DNS server
// is set, and routing of both inbounds to the proxy. The log is quiet and
// records no access. nullopt when the stored outbound is not a JSON object.
[[nodiscard]] std::optional<std::string> BuildConfig(
		const ProxyState::Server &server,
		const ProxyState::Advanced &advanced,
		const Ports &ports,
		const Credentials &credentials) {
	const std::optional<std::vector<Json::Value>> outbounds = AdvancedOutbounds(
		server,
		advanced);
	if (!outbounds) {
		return std::nullopt;
	}
	const Json::Value accounts = Json::Value::Array(std::vector<Json::Value>{
		Json::Value::Object(std::vector<Member>{
			Entry("user", Text(credentials.user)),
			Entry("pass", Text(credentials.password)),
		}),
	});
	const Json::Value httpInbound = Json::Value::Object(std::vector<Member>{
		Entry("tag", Text(kHttpTag)),
		Entry("listen", Text(kLoopbackHost)),
		Entry("port", Json::Value::Int(ports.httpPort)),
		Entry("protocol", Text("http")),
		Entry("settings", Json::Value::Object(std::vector<Member>{
			Entry("accounts", accounts),
			Entry("allowTransparent", Json::Value::Bool(false)),
		})),
	});
	const Json::Value socksInbound = Json::Value::Object(std::vector<Member>{
		Entry("tag", Text(kSocksTag)),
		Entry("listen", Text(kLoopbackHost)),
		Entry("port", Json::Value::Int(ports.socksPort)),
		Entry("protocol", Text("socks")),
		Entry("settings", Json::Value::Object(std::vector<Member>{
			Entry("auth", Text("password")),
			Entry("accounts", accounts),
			Entry("udp", Json::Value::Bool(true)),
			Entry("ip", Text(kLoopbackHost)),
		})),
	});

	std::vector<Member> members;
	members.push_back(Entry("log", Json::Value::Object(std::vector<Member>{
		Entry("loglevel", Text("error")),
		Entry("access", Text("none")),
	})));
	members.push_back(Entry("inbounds", Json::Value::Array(std::vector<Json::Value>{
		httpInbound,
		socksInbound,
	})));
	members.push_back(Entry("outbounds", Json::Value::Array(*outbounds)));
	const std::optional<std::string> dnsServer = EffectiveDnsServer(advanced);
	if (dnsServer) {
		members.push_back(Entry("dns", Json::Value::Object(std::vector<Member>{
			Entry("servers", Json::Value::Array(std::vector<Json::Value>{
				Text(*dnsServer),
			})),
			Entry("queryStrategy", Text(kUseIpv4)),
		})));
	}
	members.push_back(Entry("routing", Json::Value::Object(std::vector<Member>{
		Entry("domainStrategy", Text("AsIs")),
		Entry("rules", Json::Value::Array(std::vector<Json::Value>{
			Json::Value::Object(std::vector<Member>{
				Entry("type", Text("field")),
				Entry("inboundTag", Json::Value::Array(std::vector<Json::Value>{
					Text(kHttpTag),
					Text(kSocksTag),
				})),
				Entry("outboundTag", Text(kProxyTag)),
			}),
		})),
	})));
	return WriteJson(Json::Value::Object(std::move(members)));
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
		*server,
		state.advanced,
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
