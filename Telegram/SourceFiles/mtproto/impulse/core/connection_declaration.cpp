#include "mtproto/impulse/core/connection_declaration.h"

#include "mtproto/impulse/core/transcoder.h"
#include "mtproto/impulse/core/wire.h"

#include <algorithm>

namespace Impulse {
namespace {

constexpr std::uint32_t kInitConnectionId = 0xc1cd5ea9u;
constexpr std::uint32_t kInvokeWithLayerId = 0xda9b0d0du;
constexpr std::uint32_t kInvokeWithoutUpdatesId = 0xbf9459b7u;
constexpr std::uint32_t kInvokeWithTakeoutId = 0xaca9fd2eu;

// Field numbers of ConnectionDeclaration.
constexpr std::uint32_t kApiIdField = 1;
constexpr std::uint32_t kDeviceModelField = 2;
constexpr std::uint32_t kSystemVersionField = 3;
constexpr std::uint32_t kAppVersionField = 4;
constexpr std::uint32_t kSystemLangCodeField = 5;
constexpr std::uint32_t kLangPackField = 6;
constexpr std::uint32_t kLangCodeField = 7;

// Field number of the declaration inside the initConnection method payload.
constexpr std::uint32_t kDeclarationField = 1;

[[nodiscard]] std::string ToString(std::span<const std::uint8_t> bytes) {
	return std::string(
		reinterpret_cast<const char *>(bytes.data()),
		bytes.size());
}

// The UTF-16 code units of one UTF-8 character, given its lead byte. A four
// byte character is a surrogate pair.
[[nodiscard]] std::size_t Utf16UnitsOf(unsigned char lead) {
	return (lead >= 0xF0) ? 2 : 1;
}

// The byte length of one UTF-8 character, given its lead byte. A byte that is
// not a lead byte counts as one character, so a broken value is still cut.
[[nodiscard]] std::size_t Utf8LengthOf(unsigned char lead) {
	if (lead >= 0xF0) {
		return 4;
	} else if (lead >= 0xE0) {
		return 3;
	} else if (lead >= 0xC0) {
		return 2;
	}
	return 1;
}

[[nodiscard]] bool IsSpace(char symbol) {
	return symbol == ' ' || symbol == '\t' || symbol == '\r' || symbol == '\n';
}

// The value trimmed at both ends and cut to maxUnits UTF-16 code units. The
// cut keeps whole characters and never leaves a trailing space.
[[nodiscard]] std::string FitField(const std::string &value, std::size_t maxUnits) {
	std::size_t first = 0;
	while (first < value.size() && IsSpace(value[first])) {
		++first;
	}
	std::size_t last = value.size();
	while (last > first && IsSpace(value[last - 1])) {
		--last;
	}

	std::string result;
	std::size_t units = 0;
	std::size_t index = first;
	while (index < last) {
		const unsigned char lead = static_cast<unsigned char>(value[index]);
		const std::size_t length = std::min(Utf8LengthOf(lead), last - index);
		const std::size_t cost = Utf16UnitsOf(lead);
		if (units + cost > maxUnits) {
			break;
		}
		result.append(value, index, length);
		units += cost;
		index += length;
	}
	while (!result.empty() && IsSpace(result.back())) {
		result.pop_back();
	}
	return result;
}

} // namespace

std::optional<ConnectionDeclaration> ReadConnectionDeclaration(
		std::span<const std::uint8_t> tl) {
	try {
		TlReader reader(tl);
		for (;;) {
			const std::uint32_t id = static_cast<std::uint32_t>(
				reader.readInt32());
			if (id == kInvokeWithLayerId) {
				reader.readInt32();
			} else if (id == kInvokeWithoutUpdatesId) {
				continue;
			} else if (id == kInvokeWithTakeoutId) {
				reader.readInt64();
			} else if (id == kInitConnectionId) {
				break;
			} else {
				return std::nullopt;
			}
		}

		// The flags come first; the proxy and params fields follow the
		// strings and are not read here.
		static_cast<void>(reader.readInt32());
		ConnectionDeclaration declaration;
		declaration.apiId = reader.readInt32();
		declaration.deviceModel = ToString(reader.readBytes());
		declaration.systemVersion = ToString(reader.readBytes());
		declaration.appVersion = ToString(reader.readBytes());
		declaration.systemLangCode = ToString(reader.readBytes());
		declaration.langPack = ToString(reader.readBytes());
		declaration.langCode = ToString(reader.readBytes());
		return declaration;
	} catch (const TranscodeError &) {
		return std::nullopt;
	}
}

std::vector<std::uint8_t> EncodeConnectionDeclaration(
		const ConnectionDeclaration &declaration) {
	ProtoWriter message;
	message.writeSignedVarintField(kApiIdField, declaration.apiId);
	message.writeStringField(kDeviceModelField, declaration.deviceModel);
	message.writeStringField(kSystemVersionField, declaration.systemVersion);
	message.writeStringField(kAppVersionField, declaration.appVersion);
	message.writeStringField(kSystemLangCodeField, declaration.systemLangCode);
	message.writeStringField(kLangPackField, declaration.langPack);
	message.writeStringField(kLangCodeField, declaration.langCode);

	ProtoWriter payload;
	payload.writeMessageField(kDeclarationField, message);
	return payload.takeBytes();
}

ConnectionDeclaration ClampConnectionDeclaration(
		ConnectionDeclaration declaration) {
	declaration.deviceModel = FitField(
		declaration.deviceModel,
		kDeviceModelMaxLength);
	declaration.systemVersion = FitField(
		declaration.systemVersion,
		kSystemVersionMaxLength);
	declaration.appVersion = FitField(
		declaration.appVersion,
		kAppVersionMaxLength);
	declaration.systemLangCode = FitField(
		declaration.systemLangCode,
		kSystemLangCodeMaxLength);
	declaration.langPack = FitField(
		declaration.langPack,
		kLangPackMaxLength);
	declaration.langCode = FitField(
		declaration.langCode,
		kLangCodeMaxLength);
	return declaration;
}

} // namespace Impulse
