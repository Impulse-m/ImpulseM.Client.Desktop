#include "mtproto/impulse/core/envelope.h"

#include "mtproto/impulse/core/json.h"

#include <limits>
#include <optional>
#include <utility>

namespace Impulse {
namespace {

constexpr std::int64_t kInt32Max = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

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
	if (symbol == '+' || symbol == '-') {
		return 62;
	}
	if (symbol == '/' || symbol == '_') {
		return 63;
	}
	return -1;
}

[[nodiscard]] bool IsBase64Space(char symbol) {
	return symbol == ' '
		|| symbol == '\t'
		|| symbol == '\r'
		|| symbol == '\n'
		|| symbol == '\f';
}

// Accepts the standard and the URL-safe alphabet, ignores whitespace and
// stops at the first '=' padding. Returns nullopt for any other symbol and
// for a single trailing symbol, which carries no whole byte.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> DecodeBase64(
		std::string_view text) {
	auto result = std::vector<std::uint8_t>();
	result.reserve(text.size() / 4 * 3 + 3);
	auto bits = std::uint32_t(0);
	auto count = 0;
	for (const char symbol : text) {
		if (symbol == '=') {
			break;
		}
		if (IsBase64Space(symbol)) {
			continue;
		}
		const auto value = Base64Value(symbol);
		if (value < 0) {
			return std::nullopt;
		}
		bits = (bits << 6) | std::uint32_t(value);
		++count;
		if (count == 4) {
			result.push_back(std::uint8_t(bits >> 16));
			result.push_back(std::uint8_t(bits >> 8));
			result.push_back(std::uint8_t(bits));
			bits = 0;
			count = 0;
		}
	}
	if (count == 1) {
		return std::nullopt;
	}
	if (count == 2) {
		bits <<= 12;
		result.push_back(std::uint8_t(bits >> 16));
	}
	if (count == 3) {
		bits <<= 6;
		result.push_back(std::uint8_t(bits >> 16));
		result.push_back(std::uint8_t(bits >> 8));
	}
	return result;
}

// Reads an integer member. A missing member is an error when required,
// and reads as 0 otherwise. A present member must be an integer in [min, max].
[[nodiscard]] std::optional<std::int64_t> IntMember(
		const Json::Value &object,
		std::string_view key,
		bool required,
		std::int64_t min,
		std::int64_t max) {
	const auto *member = object.find(key);
	if (!member) {
		if (required) {
			return std::nullopt;
		}
		return std::int64_t(0);
	}
	const auto value = member->asInt();
	if (!value || *value < min || *value > max) {
		return std::nullopt;
	}
	return *value;
}

[[nodiscard]] Envelope ParseFrame(const Json::Value &object) {
	const auto type = IntMember(object, "type", true, 1, kInt32Max);
	const auto date = IntMember(object, "date", true, 1, kInt32Max);
	const auto *dataMember = object.find("data");
	const auto *dataText = dataMember ? dataMember->asString() : nullptr;
	auto body = std::optional<std::vector<std::uint8_t>>();
	if (dataText) {
		body = DecodeBase64(*dataText);
	}
	if (!type || !date || !body || body->empty()) {
		return Envelope::Undecodable();
	}
	const auto pts = IntMember(object, "pts", false, 0, kInt64Max);
	const auto ptsCount = IntMember(object, "ptsCount", false, 0, kInt32Max);
	if (!pts || !ptsCount) {
		return Envelope::Undecodable();
	}
	const auto hasPts = object.find("pts") != nullptr
		|| object.find("ptsCount") != nullptr;
	const auto typeValue = std::int32_t(*type);
	const auto dateValue = std::int32_t(*date);
	if (typeValue == kUpdatesTooLongType) {
		return Envelope::UpdatesTooLong(dateValue, std::move(*body));
	}
	if (!hasPts) {
		return Envelope::PtsLess(typeValue, dateValue, std::move(*body));
	}
	if (*pts == 0 && *ptsCount == 0) {
		return Envelope::Unnumbered(typeValue, dateValue, std::move(*body));
	}
	return Envelope::Numbered(
		typeValue,
		*pts,
		std::int32_t(*ptsCount),
		dateValue,
		std::move(*body));
}

[[nodiscard]] Envelope ParseLegacy(const std::vector<Json::Value> &items) {
	if (items.empty()) {
		return Envelope::Undecodable();
	}
	for (const auto &item : items) {
		if (item.kind() != Json::Kind::Object) {
			return Envelope::Undecodable();
		}
	}
	return Envelope::LegacySessionRevoked();
}

} // namespace

Envelope Envelope::PtsLess(
		std::int32_t type,
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto) {
	auto result = Envelope();
	result.kind = Kind::PtsLess;
	result.type = type;
	result.date = date;
	result.updatesProto = std::move(updatesProto);
	return result;
}

Envelope Envelope::Numbered(
		std::int32_t type,
		std::int64_t pts,
		std::int32_t ptsCount,
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto) {
	auto result = Envelope();
	result.kind = Kind::Numbered;
	result.type = type;
	result.pts = pts;
	result.ptsCount = ptsCount;
	result.date = date;
	result.updatesProto = std::move(updatesProto);
	return result;
}

Envelope Envelope::Unnumbered(
		std::int32_t type,
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto) {
	auto result = Envelope();
	result.kind = Kind::Unnumbered;
	result.type = type;
	result.date = date;
	result.updatesProto = std::move(updatesProto);
	return result;
}

Envelope Envelope::UpdatesTooLong(
		std::int32_t date,
		std::vector<std::uint8_t> updatesProto) {
	auto result = Envelope();
	result.kind = Kind::UpdatesTooLong;
	result.type = kUpdatesTooLongType;
	result.date = date;
	result.updatesProto = std::move(updatesProto);
	return result;
}

Envelope Envelope::LegacySessionRevoked() {
	auto result = Envelope();
	result.kind = Kind::LegacySessionRevoked;
	return result;
}

Envelope Envelope::Undecodable() {
	return Envelope();
}

Envelope ParseEnvelope(std::string_view text) {
	const auto parsed = Json::parse(text);
	if (!parsed.value) {
		return Envelope::Undecodable();
	}
	if (const auto *items = parsed.value->asArray()) {
		return ParseLegacy(*items);
	}
	if (parsed.value->kind() != Json::Kind::Object) {
		return Envelope::Undecodable();
	}
	return ParseFrame(*parsed.value);
}

} // namespace Impulse
