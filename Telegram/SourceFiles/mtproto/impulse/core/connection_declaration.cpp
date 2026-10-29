#include "mtproto/impulse/core/connection_declaration.h"

#include "mtproto/impulse/core/transcoder.h"
#include "mtproto/impulse/core/wire.h"

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

} // namespace Impulse
