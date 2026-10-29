#include "mtproto/impulse/core/proxy_input.h"

#include "mtproto/impulse/core/hash.h"

#include <cstdint>
#include <limits>

namespace Impulse::ProxyInput {
namespace {

constexpr std::string_view kHttpPrefix = "http://";
constexpr std::string_view kHttpsPrefix = "https://";
constexpr std::string_view kLinksPrefix = "vless://";
constexpr std::string_view kBase64Prefix = "base64:";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] bool IsTrimSymbol(char symbol) {
	return static_cast<unsigned char>(symbol) <= ' ';
}

[[nodiscard]] bool IsWhitespaceSymbol(char symbol) {
	return symbol == ' '
		|| (symbol >= '\t' && symbol <= '\r')
		|| (symbol >= 0x1C && symbol <= 0x1F);
}

[[nodiscard]] bool ContainsWhitespace(std::string_view text) {
	for (const char symbol : text) {
		if (IsWhitespaceSymbol(symbol)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] char LowerAscii(char symbol) {
	return (symbol >= 'A' && symbol <= 'Z')
		? static_cast<char>(symbol - 'A' + 'a')
		: symbol;
}

[[nodiscard]] bool StartsWithIgnoreCase(
		std::string_view text,
		std::string_view prefix) {
	if (text.size() < prefix.size()) {
		return false;
	}
	for (auto index = std::size_t(0); index < prefix.size(); ++index) {
		if (LowerAscii(text[index]) != LowerAscii(prefix[index])) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool EqualsIgnoreCase(
		std::string_view text,
		std::string_view other) {
	return text.size() == other.size()
		&& StartsWithIgnoreCase(text, other);
}

[[nodiscard]] int Base64Value(char symbol) {
	if (symbol >= 'A' && symbol <= 'Z') {
		return symbol - 'A';
	}
	if (symbol >= 'a' && symbol <= 'z') {
		return symbol - 'a' + 26;
	}
	if (symbol >= '0' && symbol <= '9') {
		return symbol - '0' + 52;
	}
	if (symbol == '+') {
		return 62;
	}
	if (symbol == '/') {
		return 63;
	}
	return -1;
}

[[nodiscard]] char ByteSymbol(std::uint32_t value) {
	return static_cast<char>(static_cast<std::uint8_t>(value & 0xFF));
}

// Standard alphabet. Whitespace is skipped, decoding stops at the first '='
// padding. Returns nullopt for any other symbol and for a single trailing
// symbol, which carries no whole byte.
[[nodiscard]] std::optional<std::string> DecodeBase64(std::string_view text) {
	auto result = std::string();
	result.reserve(text.size() / 4 * 3 + 3);
	auto bits = std::uint32_t(0);
	auto count = 0;
	for (const char symbol : text) {
		if (symbol == '=') {
			break;
		}
		if (IsWhitespaceSymbol(symbol)) {
			continue;
		}
		const auto value = Base64Value(symbol);
		if (value < 0) {
			return std::nullopt;
		}
		bits = (bits << 6) | std::uint32_t(value);
		++count;
		if (count == 4) {
			result.push_back(ByteSymbol(bits >> 16));
			result.push_back(ByteSymbol(bits >> 8));
			result.push_back(ByteSymbol(bits));
			bits = 0;
			count = 0;
		}
	}
	if (count == 1) {
		return std::nullopt;
	}
	if (count == 2) {
		bits <<= 12;
		result.push_back(ByteSymbol(bits >> 16));
	} else if (count == 3) {
		bits <<= 6;
		result.push_back(ByteSymbol(bits >> 16));
		result.push_back(ByteSymbol(bits >> 8));
	}
	return result;
}

// Decimal with an optional sign. Nullopt for an empty digit string, any other
// symbol or a value outside int64.
[[nodiscard]] std::optional<std::int64_t> ParseDecimal(std::string_view text) {
	auto negative = false;
	auto digits = text;
	if (!digits.empty() && (digits.front() == '+' || digits.front() == '-')) {
		negative = digits.front() == '-';
		digits.remove_prefix(1);
	}
	if (digits.empty()) {
		return std::nullopt;
	}
	const auto limit = std::uint64_t(std::numeric_limits<std::int64_t>::max())
		+ (negative ? 1 : 0);
	auto magnitude = std::uint64_t(0);
	for (const char symbol : digits) {
		if (symbol < '0' || symbol > '9') {
			return std::nullopt;
		}
		const auto digit = std::uint64_t(symbol - '0');
		if (magnitude > (limit - digit) / 10) {
			return std::nullopt;
		}
		magnitude = magnitude * 10 + digit;
	}
	return negative
		? static_cast<std::int64_t>(~magnitude + 1)
		: static_cast<std::int64_t>(magnitude);
}

[[nodiscard]] std::optional<std::string> ParseTitle(
		std::optional<std::string_view> header) {
	if (!header) {
		return std::nullopt;
	}
	const auto value = Trim(*header);
	if (value.empty()) {
		return std::nullopt;
	}
	if (value.starts_with(kBase64Prefix)) {
		const auto bytes = DecodeBase64(value.substr(kBase64Prefix.size()));
		if (!bytes) {
			return std::nullopt;
		}
		const auto decoded = Trim(*bytes);
		if (decoded.empty()) {
			return std::nullopt;
		}
		return std::string(decoded);
	}
	return std::string(value);
}

[[nodiscard]] std::int64_t UserInfoValue(
		std::optional<std::string_view> header,
		std::string_view key) {
	if (!header) {
		return -1;
	}
	auto rest = *header;
	while (true) {
		const auto split = rest.find(';');
		const auto part = (split == std::string_view::npos)
			? rest
			: rest.substr(0, split);
		const auto equals = part.find('=');
		if (equals != std::string_view::npos) {
			const auto name = Trim(part.substr(0, equals));
			if (EqualsIgnoreCase(name, key)) {
				const auto parsed = ParseDecimal(Trim(part.substr(equals + 1)));
				return parsed ? *parsed : -1;
			}
		}
		if (split == std::string_view::npos) {
			return -1;
		}
		rest.remove_prefix(split + 1);
	}
}

[[nodiscard]] int UpdateInterval(std::optional<std::string_view> header) {
	if (!header) {
		return kDefaultUpdateIntervalHours;
	}
	const auto parsed = ParseDecimal(Trim(*header));
	if (!parsed
		|| *parsed > std::numeric_limits<std::int32_t>::max()
		|| *parsed < std::numeric_limits<std::int32_t>::min()) {
		return kDefaultUpdateIntervalHours;
	}
	return *parsed < 1 ? 1 : static_cast<int>(*parsed);
}

} // namespace

std::string_view Trim(std::string_view text) {
	auto begin = std::size_t(0);
	auto end = text.size();
	while (begin < end && IsTrimSymbol(text[begin])) {
		++begin;
	}
	while (end > begin && IsTrimSymbol(text[end - 1])) {
		--end;
	}
	return text.substr(begin, end - begin);
}

InputKind Detect(std::string_view text) {
	const auto value = Trim(text);
	if (value.empty()) {
		return InputKind::Unknown;
	}
	if ((StartsWithIgnoreCase(value, kHttpsPrefix)
			|| StartsWithIgnoreCase(value, kHttpPrefix))
		&& !ContainsWhitespace(value)) {
		return InputKind::Subscription;
	}
	auto rest = value;
	while (true) {
		const auto newline = rest.find('\n');
		const auto line = Trim((newline == std::string_view::npos)
			? rest
			: rest.substr(0, newline));
		if (StartsWithIgnoreCase(line, kLinksPrefix)) {
			return InputKind::Links;
		}
		if (newline == std::string_view::npos) {
			return InputKind::Unknown;
		}
		rest.remove_prefix(newline + 1);
	}
}

std::string SubscriptionId(std::string_view url) {
	const auto digest = Sha256(Trim(url));
	auto result = std::string();
	result.reserve(kSubscriptionIdLength);
	for (auto index = std::size_t(0); index < kSubscriptionIdLength / 2; ++index) {
		result.push_back(kHexDigits[digest[index] >> 4]);
		result.push_back(kHexDigits[digest[index] & 0x0F]);
	}
	return result;
}

std::string_view StripBom(std::string_view text) {
	return text.starts_with(kUtf8Bom)
		? text.substr(kUtf8Bom.size())
		: text;
}

bool BodyWithinCap(std::size_t bytes) {
	return bytes <= kMaxSubscriptionBodyBytes;
}

std::string HttpError(int code) {
	return std::string(kErrorHttp) + ":" + std::to_string(code);
}

std::string NoLinksError(int skipped) {
	return skipped > 0
		? std::string(kErrorNoLinks) + ":" + std::to_string(skipped)
		: std::string(kErrorNoLinks);
}

SubscriptionMeta ParseMeta(
		std::optional<std::string_view> profileTitle,
		std::optional<std::string_view> userInfo,
		std::optional<std::string_view> updateInterval) {
	auto result = SubscriptionMeta();
	result.title = ParseTitle(profileTitle);
	result.upload = UserInfoValue(userInfo, "upload");
	result.download = UserInfoValue(userInfo, "download");
	result.total = UserInfoValue(userInfo, "total");
	result.expire = UserInfoValue(userInfo, "expire");
	result.updateIntervalHours = UpdateInterval(updateInterval);
	return result;
}

} // namespace Impulse::ProxyInput
