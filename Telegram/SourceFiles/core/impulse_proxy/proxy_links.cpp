#include "core/impulse_proxy/proxy_links.h"

#include "mtproto/impulse/core/hash.h"
#include "mtproto/impulse/core/proxy_input.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Impulse::Xray {
namespace {

constexpr std::string_view kMethod = "convertShareLinksToXrayJson";
constexpr std::string_view kProtocolVless = "vless";
constexpr std::string_view kNoValidOutbound = "no valid outbound";
constexpr std::string_view kLinksPrefix = "vless://";
constexpr std::string_view kDefaultNetwork = "tcp";
constexpr std::string_view kDefaultSecurity = "none";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::size_t kIdLength = 24;

// A JSON string literal. Quotes, backslashes and control characters are
// escaped; other bytes are copied as they are.
[[nodiscard]] std::string Quoted(std::string_view text) {
	std::string out = "\"";
	for (const char symbol : text) {
		const unsigned char code = static_cast<unsigned char>(symbol);
		if (symbol == '"') {
			out += "\\\"";
		} else if (symbol == '\\') {
			out += "\\\\";
		} else if (code < 0x20) {
			out += "\\u00";
			out.push_back(kHexDigits[static_cast<std::size_t>(code >> 4)]);
			out.push_back(kHexDigits[static_cast<std::size_t>(code & 0x0F)]);
		} else {
			out.push_back(symbol);
		}
	}
	out.push_back('"');
	return out;
}

// Compact JSON of a parsed value, members in the order they were read.
void AppendValue(std::string &out, const Json::Value &value) {
	if (value.kind() == Json::Kind::Null) {
		out += "null";
		return;
	}
	if (value.kind() == Json::Kind::Bool) {
		out += value.asBool().value_or(false) ? "true" : "false";
		return;
	}
	if (value.kind() == Json::Kind::Int) {
		out += std::to_string(value.asInt().value_or(0));
		return;
	}
	if (value.kind() == Json::Kind::Double) {
		char buffer[64];
		const std::to_chars_result written = std::to_chars(
			buffer,
			buffer + sizeof(buffer),
			value.asDouble().value_or(0.));
		out.append(buffer, static_cast<std::size_t>(written.ptr - buffer));
		return;
	}
	if (value.kind() == Json::Kind::String) {
		const std::string *text = value.asString();
		out += Quoted(text ? std::string_view(*text) : std::string_view());
		return;
	}
	if (value.kind() == Json::Kind::Array) {
		const std::vector<Json::Value> *items = value.asArray();
		out.push_back('[');
		for (std::size_t index = 0; items && index < items->size(); ++index) {
			if (index > 0) {
				out.push_back(',');
			}
			AppendValue(out, (*items)[index]);
		}
		out.push_back(']');
		return;
	}
	const std::vector<Json::Value::Member> *members = value.asObject();
	out.push_back('{');
	for (std::size_t index = 0; members && index < members->size(); ++index) {
		if (index > 0) {
			out.push_back(',');
		}
		out += Quoted((*members)[index].first);
		out.push_back(':');
		AppendValue(out, (*members)[index].second);
	}
	out.push_back('}');
}

[[nodiscard]] std::string Lowered(std::string_view text) {
	std::string out(text);
	for (char &symbol : out) {
		if (symbol >= 'A' && symbol <= 'Z') {
			symbol = static_cast<char>(symbol - 'A' + 'a');
		}
	}
	return out;
}

// True when the trimmed line starts with vless:// in any letter case.
[[nodiscard]] bool IsLinkLine(std::string_view line) {
	return Lowered(line.substr(0, kLinksPrefix.size())) == kLinksPrefix;
}

// The request {"text":"..."} of the convert call.
[[nodiscard]] std::string ConvertPayload(std::string_view text) {
	return "{\"text\":" + Quoted(text) + "}";
}

// A string member of an object. The fallback when the member is missing;
// nullopt when the member is present but is not a string.
[[nodiscard]] std::optional<std::string> StringMember(
		const Json::Value *object,
		std::string_view key,
		std::string_view fallback) {
	const Json::Value *member = object ? object->find(key) : nullptr;
	if (!member) {
		return std::string(fallback);
	}
	const std::string *text = member->asString();
	if (!text) {
		return std::nullopt;
	}
	return *text;
}

// The first 24 hex characters of the SHA-256 of the text.
[[nodiscard]] std::string ShortDigest(std::string_view text) {
	const Sha256Digest digest = Impulse::Sha256(text);
	std::string result;
	result.reserve(kIdLength);
	for (std::size_t index = 0; index < kIdLength / 2; ++index) {
		result.push_back(kHexDigits[static_cast<std::size_t>(digest[index] >> 4)]);
		result.push_back(kHexDigits[static_cast<std::size_t>(digest[index] & 0x0F)]);
	}
	return result;
}

} // namespace

LinkConversion ConvertLinks(
		const Invoker &invoker,
		std::string_view text) {
	LinkConversion result;
	const std::string method(kMethod);
	const Json::ParseResult parsed = Json::parse(
		invoker(method, ConvertPayload(text)));
	const std::string malformed = method + ": malformed response";
	if (!parsed.value || parsed.value->kind() != Json::Kind::Object) {
		result.failure = malformed;
		return result;
	}
	const Json::Value *success = parsed.value->find("success");
	const std::optional<bool> ok = success ? success->asBool() : std::nullopt;
	if (!ok) {
		result.failure = malformed;
		return result;
	}
	if (!*ok) {
		const Json::Value *error = parsed.value->find("error");
		const std::string *message = error ? error->asString() : nullptr;
		result.failure = method
			+ ": "
			+ (message ? *message : std::string("unknown error"));
		return result;
	}

	const Json::Value *data = parsed.value->find("data");
	const Json::Value *outbounds = data ? data->find("outbounds") : nullptr;
	const std::vector<Json::Value> *items = outbounds
		? outbounds->asArray()
		: nullptr;
	if (!items) {
		result.failure = malformed;
		return result;
	}
	for (const Json::Value &item : *items) {
		std::optional<ProxyState::Server> server = ServerFromOutbound(item);
		if (server) {
			result.servers.push_back(std::move(*server));
		} else {
			++result.skipped;
		}
	}
	return result;
}

std::vector<ProxyState::Server> ConvertShareLines(
		const Invoker &invoker,
		std::string_view text) {
	std::vector<ProxyState::Server> servers;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t newline = text.find('\n', start);
		const std::size_t end = (newline == std::string_view::npos)
			? text.size()
			: newline;
		const std::string_view line = ProxyInput::Trim(
			text.substr(start, end - start));
		if (IsLinkLine(line)) {
			LinkConversion conversion = ConvertLinks(invoker, line);
			if (conversion.failure.empty()) {
				for (ProxyState::Server &server : conversion.servers) {
					server.shareLink = std::string(line);
					servers.push_back(std::move(server));
				}
			}
		}
		if (newline == std::string_view::npos) {
			break;
		}
		start = newline + 1;
	}
	return servers;
}

std::optional<std::string> SubscriptionBodyError(
		const LinkConversion &conversion) {
	if (!conversion.failure.empty()) {
		if (Lowered(conversion.failure).find(kNoValidOutbound)
				!= std::string::npos) {
			return ProxyInput::NoLinksError(0);
		}
		return std::string(ProxyInput::kErrorInvalidSubscription);
	}
	if (conversion.servers.empty()) {
		return ProxyInput::NoLinksError(conversion.skipped);
	}
	return std::nullopt;
}

std::optional<ProxyState::Server> ServerFromOutbound(
		const Json::Value &outbound) {
	if (outbound.kind() != Json::Kind::Object) {
		return std::nullopt;
	}
	const Json::Value *protocol = outbound.find("protocol");
	const std::string *protocolName = protocol ? protocol->asString() : nullptr;
	if (!protocolName || *protocolName != kProtocolVless) {
		return std::nullopt;
	}

	// The first upstream of the outbound, when it has one, carries the
	// address and the port.
	const Json::Value *settings = outbound.find("settings");
	if (settings && settings->kind() != Json::Kind::Object) {
		settings = nullptr;
	}
	const Json::Value *endpoint = settings;
	const Json::Value *vnext = settings ? settings->find("vnext") : nullptr;
	const std::vector<Json::Value> *upstreams = vnext ? vnext->asArray() : nullptr;
	if (upstreams && !upstreams->empty()) {
		endpoint = &(*upstreams)[0];
		if (endpoint->kind() != Json::Kind::Object) {
			return std::nullopt;
		}
	}
	if (!endpoint) {
		return std::nullopt;
	}

	const std::optional<std::string> host = StringMember(endpoint, "address", "");
	if (!host || host->empty()) {
		return std::nullopt;
	}
	const Json::Value *portValue = endpoint->find("port");
	const std::optional<std::int64_t> port = portValue
		? portValue->asInt()
		: std::nullopt;
	if (!port
			|| *port <= 0
			|| *port > std::numeric_limits<int>::max()) {
		return std::nullopt;
	}

	const Json::Value *stream = outbound.find("streamSettings");
	if (stream && stream->kind() != Json::Kind::Object) {
		stream = nullptr;
	}
	const std::optional<std::string> network = StringMember(
		stream,
		"network",
		kDefaultNetwork);
	const std::optional<std::string> security = StringMember(
		stream,
		"security",
		kDefaultSecurity);
	const std::optional<std::string> tag = StringMember(&outbound, "tag", "");
	if (!network || !security || !tag) {
		return std::nullopt;
	}

	std::string outboundJson;
	AppendValue(outboundJson, outbound);

	ProxyState::Server server;
	server.id = ShortDigest(std::string_view(outboundJson));
	const std::string_view trimmedTag = ProxyInput::Trim(*tag);
	server.name = trimmedTag.empty() ? *host : std::string(trimmedTag);
	server.host = *host;
	server.port = static_cast<int>(*port);
	server.network = *network;
	server.security = *security;
	server.outbound = std::move(outboundJson);
	return server;
}

} // namespace Impulse::Xray
